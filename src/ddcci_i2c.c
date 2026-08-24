#define _GNU_SOURCE

#include "ddcci_priv.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef I2C_SLAVE_FORCE
#define I2C_SLAVE_FORCE 0x0706
#endif
#ifndef I2C_TIMEOUT
#define I2C_TIMEOUT 0x0702
#endif
#ifndef I2C_RETRIES
#define I2C_RETRIES 0x0701
#endif

int ddcci_i2c_open(const char *path)
{
    int fd;
    unsigned long funcs = 0;

    fd = open(path, O_RDWR);
    if (fd < 0)
        return -1;

    /* Keep probes from hanging on dead buses. Units are 10 ms. */
    (void)ioctl(fd, I2C_TIMEOUT, 10);
    (void)ioctl(fd, I2C_RETRIES, 2);
    (void)ioctl(fd, I2C_FUNCS, &funcs);
    return fd;
}

static ddcci_status_t map_errno(int e)
{
    switch (e) {
    case EAGAIN:
    case EBUSY:
        return DDCCI_ERR_BUSY;
    case ETIMEDOUT:
        return DDCCI_ERR_TIMEOUT;
    case ENXIO:
    case ENODEV:
    case EREMOTEIO:
        return DDCCI_ERR_NO_DEVICE;
    case EACCES:
    case EPERM:
        return DDCCI_ERR_IO;
    default:
        return DDCCI_ERR_IO;
    }
}

static ddcci_status_t rdwr_one(int fd, uint8_t addr, uint16_t flags,
                               uint8_t *buf, size_t n)
{
    struct i2c_msg msg;
    struct i2c_rdwr_ioctl_data rdwr;

    memset(&msg, 0, sizeof(msg));
    msg.addr = addr;
    msg.flags = flags;
    msg.len = (uint16_t)n;
    msg.buf = buf;

    memset(&rdwr, 0, sizeof(rdwr));
    rdwr.msgs = &msg;
    rdwr.nmsgs = 1;

    if (ioctl(fd, I2C_RDWR, &rdwr) < 0)
        return map_errno(errno);
    return DDCCI_OK;
}

static ddcci_status_t slave_rw(int fd, uint8_t addr, int writing,
                               uint8_t *buf, size_t n, size_t *got)
{
    if (ioctl(fd, I2C_SLAVE, addr) < 0) {
        if (ioctl(fd, I2C_SLAVE_FORCE, addr) < 0)
            return map_errno(errno);
    }

    if (writing) {
        ssize_t w = write(fd, buf, n);
        if (w < 0)
            return map_errno(errno);
        if ((size_t)w != n)
            return DDCCI_ERR_IO;
        if (got)
            *got = (size_t)w;
        return DDCCI_OK;
    } else {
        ssize_t r = read(fd, buf, n);
        if (r < 0)
            return map_errno(errno);
        if (r == 0)
            return DDCCI_ERR_NO_DEVICE;
        if (got)
            *got = (size_t)r;
        return DDCCI_OK;
    }
}

ddcci_status_t ddcci_i2c_write(int fd, uint8_t addr, const uint8_t *buf, size_t n)
{
    ddcci_status_t st;
    uint8_t tmp[DDCCI_FRAME_MAX];

    if (fd < 0 || !buf || n == 0 || n > DDCCI_FRAME_MAX)
        return DDCCI_ERR_INVALID_ARG;

    memcpy(tmp, buf, n);
    st = rdwr_one(fd, addr, 0, tmp, n);
    if (st == DDCCI_OK)
        return st;
    return slave_rw(fd, addr, 1, tmp, n, NULL);
}

ddcci_status_t ddcci_i2c_read(int fd, uint8_t addr, uint8_t *buf, size_t n,
                              size_t *got)
{
    ddcci_status_t st;
    size_t local = 0;

    if (fd < 0 || !buf || n == 0)
        return DDCCI_ERR_INVALID_ARG;

    st = rdwr_one(fd, addr, I2C_M_RD, buf, n);
    if (st == DDCCI_OK) {
        if (got)
            *got = n;
        return st;
    }
    st = slave_rw(fd, addr, 0, buf, n, &local);
    if (got)
        *got = local;
    return st;
}

ddcci_status_t ddcci_transact(ddcci_display *d, const uint8_t *w, size_t wn,
                              uint8_t *r, size_t rmax, size_t *rgot,
                              unsigned wait_ms)
{
    ddcci_status_t st;
    int attempt;

    if (!d || d->fd < 0 || !w || wn == 0)
        return DDCCI_ERR_INVALID_ARG;

    for (attempt = 0; attempt < DDCCI_RETRIES; attempt++) {
        if (attempt)
            ddcci_sleep_ms(DDCCI_WAIT_GET_MS);

        st = ddcci_i2c_write(d->fd, DDCCI_ADDR_DDC, w, wn);
        if (st != DDCCI_OK)
            continue;

        if (!r) {
            ddcci_sleep_ms(wait_ms);
            return DDCCI_OK;
        }

        ddcci_sleep_ms(wait_ms);
        st = ddcci_i2c_read(d->fd, DDCCI_ADDR_DDC, r, rmax, rgot);
        if (st != DDCCI_OK)
            continue;
        if (rgot && *rgot >= 3 && ddcci_read_checksum_ok(r, *rgot))
            return DDCCI_OK;
        /* Some adapters pad the read; trim to declared length + header + chk. */
        if (rgot && *rgot >= 3) {
            size_t declared = (size_t)(r[1] & 0x7Fu);
            size_t need = 2 + declared + 1;
            if (need <= *rgot && ddcci_read_checksum_ok(r, need)) {
                *rgot = need;
                return DDCCI_OK;
            }
        }
        st = DDCCI_ERR_CHECKSUM;
    }
    return st;
}

ddcci_status_t ddcci_read_edid_i2c(int fd, ddcci_edid *out)
{
    uint8_t raw[DDCCI_EDID_LEN_MAX];
    uint8_t offset = 0;
    ddcci_status_t st;
    size_t got = 0;
    size_t want = DDCCI_EDID_LEN_MIN;

    memset(raw, 0, sizeof(raw));

    st = ddcci_i2c_write(fd, DDCCI_ADDR_EDID, &offset, 1);
    if (st != DDCCI_OK)
        return st;

    st = ddcci_i2c_read(fd, DDCCI_ADDR_EDID, raw, want, &got);
    if (st != DDCCI_OK)
        return st;
    if (got < DDCCI_EDID_LEN_MIN)
        return DDCCI_ERR_PARSE;

    if (raw[126] > 0) {
        /* Retry a full 256-byte read for the CEA extension block. */
        offset = 0;
        (void)ddcci_i2c_write(fd, DDCCI_ADDR_EDID, &offset, 1);
        if (ddcci_i2c_read(fd, DDCCI_ADDR_EDID, raw, DDCCI_EDID_LEN_MAX, &got) == DDCCI_OK
            && got >= DDCCI_EDID_LEN_MAX)
            want = DDCCI_EDID_LEN_MAX;
        else
            want = DDCCI_EDID_LEN_MIN;
    }

    return ddcci_parse_edid(raw, want, out);
}
