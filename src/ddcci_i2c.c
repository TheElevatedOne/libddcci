#define _GNU_SOURCE

#include "ddcci_priv.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <unistd.h>

int ddcci_i2c_open(const char *path, int *err_out)
{
    int fd;
    unsigned long funcs = 0;

    if (err_out)
        *err_out = 0;
    if (!path) {
        if (err_out)
            *err_out = EINVAL;
        return -1;
    }

    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        if (err_out)
            *err_out = errno;
        return -1;
    }

    /* Confirms this fd is an i2c-dev node. Do not call I2C_TIMEOUT or
     * I2C_RETRIES: both write the adapter's global timeout/retry count
     * and change kernel and other processes' behaviour on this bus. */
    if (ioctl(fd, I2C_FUNCS, &funcs) < 0) {
        if (err_out)
            *err_out = errno;
        close(fd);
        return -1;
    }
    (void)funcs;
    return fd;
}

static ddcci_status_t map_errno(int e)
{
    switch (e) {
    case EAGAIN:
#if EAGAIN != EWOULDBLOCK
    case EWOULDBLOCK:
#endif
    case EBUSY:
        return DDCCI_ERR_BUSY;
    case ETIMEDOUT:
        return DDCCI_ERR_TIMEOUT;
    case ENXIO:
    case ENODEV:
    case EREMOTEIO:
        return DDCCI_ERR_NO_DEVICE;
    default:
        return DDCCI_ERR_IO;
    }
}

static int rdwr_is_unsupported(int err)
{
    return err == EOPNOTSUPP || err == ENOTTY || err == EPROTONOSUPPORT;
}

static ddcci_status_t bus_lock(int fd)
{
    while (flock(fd, LOCK_EX) != 0) {
        if (errno != EINTR)
            return DDCCI_ERR_BUSY;
    }
    return DDCCI_OK;
}

static void bus_unlock(int fd)
{
    if (fd >= 0) {
        while (flock(fd, LOCK_UN) != 0 && errno == EINTR)
            ;
    }
}

static ddcci_status_t rdwr_msgs(struct ddcci_display *d, struct i2c_msg *msgs,
                                __u32 nmsgs)
{
    struct i2c_rdwr_ioctl_data rdwr;

    if (d->use_rdwr < 0)
        return DDCCI_ERR_UNSUPPORTED;

    rdwr.msgs = msgs;
    rdwr.nmsgs = nmsgs;
    if (ioctl(d->fd, I2C_RDWR, &rdwr) < 0) {
        int err = errno;

        if (rdwr_is_unsupported(err)) {
            d->use_rdwr = -1;
            return DDCCI_ERR_UNSUPPORTED;
        }
        return map_errno(err);
    }
    d->use_rdwr = 1;
    return DDCCI_OK;
}

static ddcci_status_t slave_select(int fd, uint8_t addr)
{
    if (ioctl(fd, I2C_SLAVE, addr) == 0)
        return DDCCI_OK;
    /* FORCE detaches a kernel client bound to this address. Only worth it
     * when the address is genuinely busy — never as the first attempt. */
    if (errno == EBUSY && ioctl(fd, I2C_SLAVE_FORCE, addr) == 0)
        return DDCCI_OK;
    return map_errno(errno);
}

static ddcci_status_t slave_write(int fd, uint8_t addr, const uint8_t *buf, size_t n)
{
    ddcci_status_t st;
    ssize_t w;

    st = slave_select(fd, addr);
    if (st != DDCCI_OK)
        return st;
    w = write(fd, buf, n);
    if (w < 0)
        return map_errno(errno);
    if ((size_t)w != n)
        return DDCCI_ERR_IO;
    return DDCCI_OK;
}

static ddcci_status_t slave_read(int fd, uint8_t addr, uint8_t *buf, size_t n)
{
    ddcci_status_t st;
    ssize_t r;

    st = slave_select(fd, addr);
    if (st != DDCCI_OK)
        return st;
    r = read(fd, buf, n);
    if (r < 0)
        return map_errno(errno);
    if (r == 0)
        return DDCCI_ERR_NO_DEVICE;
    /* Short reads leave the tail zeroed by the caller. */
    if ((size_t)r < n)
        memset(buf + r, 0, n - (size_t)r);
    return DDCCI_OK;
}

static ddcci_status_t i2c_write(struct ddcci_display *d, uint8_t addr,
                                const uint8_t *buf, size_t n)
{
    uint8_t tmp[DDCCI_FRAME_MAX];
    struct i2c_msg msg;
    ddcci_status_t st;

    if (n == 0 || n > sizeof(tmp))
        return DDCCI_ERR_INVALID_ARG;
    memcpy(tmp, buf, n);
    memset(&msg, 0, sizeof(msg));
    msg.addr = addr;
    msg.flags = 0;
    msg.len = (uint16_t)n;
    msg.buf = tmp;

    st = rdwr_msgs(d, &msg, 1);
    if (st == DDCCI_OK)
        return DDCCI_OK;
    if (st != DDCCI_ERR_UNSUPPORTED)
        return st;
    return slave_write(d->fd, addr, tmp, n);
}

static ddcci_status_t i2c_read(struct ddcci_display *d, uint8_t addr,
                               uint8_t *buf, size_t n)
{
    struct i2c_msg msg;
    ddcci_status_t st;

    if (n == 0 || n > 65535u)
        return DDCCI_ERR_INVALID_ARG;
    memset(&msg, 0, sizeof(msg));
    msg.addr = addr;
    msg.flags = I2C_M_RD;
    msg.len = (uint16_t)n;
    msg.buf = buf;

    st = rdwr_msgs(d, &msg, 1);
    if (st == DDCCI_OK)
        return DDCCI_OK;
    if (st != DDCCI_ERR_UNSUPPORTED)
        return st;
    return slave_read(d->fd, addr, buf, n);
}

static void sleep_until(long long deadline_ms)
{
    int spins = 0;

    while (spins++ < 1000) {
        long long now = ddcci_mono_ms();
        long long left;

        if (now == 0 || now >= deadline_ms)
            return;
        left = deadline_ms - now;
        if (left > 10000)
            left = 10000;
        ddcci_sleep_ms((unsigned)left);
    }
}

static void wait_gap(struct ddcci_display *d)
{
    long long now;

    if (d->next_cmd_ms <= 0)
        return;
    now = ddcci_mono_ms();
    if (now > 0 && d->next_cmd_ms > now)
        ddcci_sleep_ms((unsigned)(d->next_cmd_ms - now));
}

static void schedule_gap(struct ddcci_display *d, long long from_ms, unsigned gap_unscaled)
{
    long long ready;
    long long now = ddcci_mono_ms();

    if (from_ms <= 0)
        from_ms = now;
    ready = from_ms + (long long)ddcci_scale_ms(d, gap_unscaled);
    if (ready < now)
        ready = now;
    if (ready > d->next_cmd_ms)
        d->next_cmd_ms = ready;
}

static void adapt_success(struct ddcci_display *d, int kind, int first_try,
                          unsigned elapsed_ms)
{
    unsigned *wait = (kind == DDCCI_KIND_CAPS) ? &d->caps_wait_ms : &d->get_wait_ms;
    unsigned *streak = (kind == DDCCI_KIND_CAPS) ? &d->caps_streak : &d->get_streak;

    if (!first_try) {
        unsigned unscaled;

        *streak = 0;
        unscaled = d->scale_num
            ? (unsigned)(((unsigned long)elapsed_ms * 100ul) / d->scale_num)
            : elapsed_ms;
        if (unscaled < DDCCI_WAIT_MIN_MS)
            unscaled = DDCCI_WAIT_MIN_MS;
        if (unscaled > DDCCI_WAIT_MAX_MS)
            unscaled = DDCCI_WAIT_MAX_MS;
        if (unscaled > *wait)
            *wait = unscaled;
        return;
    }

    if (*streak < 100u)
        (*streak)++;
    /* Two clean first-try replies before shaving the wait, so one lucky
     * fast answer does not push the next command into null-message retries. */
    if (*streak >= 2u && *wait > DDCCI_WAIT_MIN_MS) {
        unsigned step = 3u;

        if (*wait - DDCCI_WAIT_MIN_MS < step)
            *wait = DDCCI_WAIT_MIN_MS;
        else
            *wait -= step;
    }
}

static void adapt_failure(struct ddcci_display *d, int kind)
{
    unsigned *wait = (kind == DDCCI_KIND_CAPS) ? &d->caps_wait_ms : &d->get_wait_ms;
    unsigned *streak = (kind == DDCCI_KIND_CAPS) ? &d->caps_streak : &d->get_streak;
    unsigned bumped = *wait + (*wait / 2u) + 5u;

    *streak = 0;
    if (bumped > DDCCI_WAIT_MAX_MS)
        bumped = DDCCI_WAIT_MAX_MS;
    if (bumped > *wait)
        *wait = bumped;
}

static void note_no_ddc(struct ddcci_display *d)
{
    if (!d->ddc_ok) {
        d->ddc_checked = true;
        d->ddc_ok = false;
    }
}

static void note_ddc_ok(struct ddcci_display *d)
{
    d->ddc_checked = true;
    d->ddc_ok = true;
}

ddcci_status_t ddcci_transact(struct ddcci_display *d, const uint8_t *w, size_t wn,
                              uint8_t *r, size_t rcap, size_t read_n, size_t *rgot,
                              int kind)
{
    ddcci_status_t st = DDCCI_ERR_IO;
    int tries = 0;
    int grew = 0;

    if (!d || d->fd < 0 || !w || wn == 0 || wn > DDCCI_FRAME_MAX)
        return DDCCI_ERR_INVALID_ARG;
    if (r && (rcap < 3 || read_n < 3))
        return DDCCI_ERR_INVALID_ARG;
    if (read_n > rcap)
        read_n = rcap;

    /* Settle time belongs to the previous command. Sleep before taking the
     * bus so another process can use it during our gap. */
    wait_gap(d);

    if (!r) {
        unsigned gap = (kind == DDCCI_KIND_SAVE) ? DDCCI_SAVE_GAP_MS : DDCCI_SET_GAP_MS;

        st = bus_lock(d->fd);
        if (st != DDCCI_OK)
            return st;
        st = i2c_write(d, DDCCI_ADDR_DDC, w, wn);
        bus_unlock(d->fd);
        if (st == DDCCI_OK) {
            note_ddc_ok(d);
            schedule_gap(d, ddcci_mono_ms(), gap);
            return DDCCI_OK;
        }
        if (st == DDCCI_ERR_NO_DEVICE) {
            note_no_ddc(d);
            return DDCCI_ERR_NO_DDC;
        }
        return st;
    }

    while (tries < DDCCI_RETRY_MAX) {
        long long t_write;
        unsigned budget, first;
        int spins = 0;
        int saw_null = 0;
        int bad_frames = 0;
        int grow_now = 0;

        if (tries > 0)
            adapt_failure(d, kind);

        st = bus_lock(d->fd);
        if (st != DDCCI_OK)
            return st;

        st = i2c_write(d, DDCCI_ADDR_DDC, w, wn);
        if (st != DDCCI_OK) {
            bus_unlock(d->fd);
            if (st == DDCCI_ERR_NO_DEVICE) {
                note_no_ddc(d);
                return DDCCI_ERR_NO_DDC;
            }
            tries++;
            continue;
        }

        t_write = ddcci_mono_ms();
        budget = ddcci_scale_ms(d, kind == DDCCI_KIND_CAPS
                                   ? DDCCI_CAPS_BUDGET_MS : DDCCI_GET_BUDGET_MS);
        first = ddcci_scale_ms(d, kind == DDCCI_KIND_CAPS
                                  ? d->caps_wait_ms : d->get_wait_ms);
        if (first > budget)
            first = budget;
        if (t_write > 0)
            sleep_until(t_write + (long long)first);
        else
            ddcci_sleep_ms(first);

        st = DDCCI_ERR_TIMEOUT;
        while (spins < DDCCI_READ_SPIN_MAX) {
            size_t flen = 0;
            bool is_null = false;
            long long now;
            unsigned elapsed;

            spins++;
            memset(r, 0, read_n);
            st = i2c_read(d, DDCCI_ADDR_DDC, r, read_n);
            if (st != DDCCI_OK)
                break;

            if (ddcci_frame_split(r, read_n, &flen, &is_null) && !is_null) {
                unsigned wall;

                if (rgot)
                    *rgot = flen;
                now = ddcci_mono_ms();
                wall = (t_write > 0 && now > t_write)
                    ? (unsigned)(now - t_write) : first;
                adapt_success(d, kind, spins == 1 && !saw_null, wall);
                note_ddc_ok(d);
                schedule_gap(d, t_write > 0 ? t_write : now, 40u);
                bus_unlock(d->fd);
                return DDCCI_OK;
            }

            if (is_null)
                saw_null = 1;
            else if (r[0] == DDCCI_DISP_WRITE && (r[1] & DDCCI_LENGTH_FLAG) != 0) {
                size_t declared = (size_t)(r[1] & 0x7Fu);
                size_t need = 2u + declared + 1u;

                if (!grew && need > read_n && need <= rcap && need <= DDCCI_READ_MAX) {
                    read_n = need;
                    grow_now = 1;
                    grew = 1;
                    break;
                }
                bad_frames++;
            } else {
                bad_frames++;
            }

            now = ddcci_mono_ms();
            elapsed = (t_write > 0 && now > t_write) ? (unsigned)(now - t_write) : budget;
            if (elapsed >= budget || (!saw_null && bad_frames >= 2)) {
                st = saw_null ? DDCCI_ERR_TIMEOUT : DDCCI_ERR_CHECKSUM;
                break;
            }
            ddcci_sleep_ms(ddcci_scale_ms(d, DDCCI_NULL_STEP_MS));
        }

        bus_unlock(d->fd);
        if (grow_now)
            continue; /* rewrite once with a buffer large enough for the length byte */
        /* Spinning out on null / garbage leaves st == OK from the last read. */
        if (st == DDCCI_OK)
            st = saw_null ? DDCCI_ERR_TIMEOUT : DDCCI_ERR_CHECKSUM;
        tries++;
        if (st == DDCCI_ERR_NO_DEVICE)
            continue;
    }

    return st;
}

static ddcci_status_t edid_chunk(struct ddcci_display *d, uint8_t offset,
                                 uint8_t *dst, uint16_t n)
{
    uint8_t off = offset;
    struct i2c_msg msgs[2];
    ddcci_status_t st;

    memset(dst, 0, n);
    memset(msgs, 0, sizeof(msgs));
    msgs[0].addr = DDCCI_ADDR_EDID;
    msgs[0].flags = 0;
    msgs[0].len = 1;
    msgs[0].buf = &off;
    msgs[1].addr = DDCCI_ADDR_EDID;
    msgs[1].flags = I2C_M_RD;
    msgs[1].len = n;
    msgs[1].buf = dst;

    /* Repeated start: the EEPROM address pointer is only reliable when the
     * offset write and the read share one I2C transaction. */
    st = rdwr_msgs(d, msgs, 2);
    if (st == DDCCI_OK)
        return DDCCI_OK;
    if (st != DDCCI_ERR_UNSUPPORTED)
        return st;

    st = slave_write(d->fd, DDCCI_ADDR_EDID, &off, 1);
    if (st != DDCCI_OK)
        return st;
    return slave_read(d->fd, DDCCI_ADDR_EDID, dst, n);
}

static int edid_sum_ok(const uint8_t *block)
{
    unsigned sum = 0;
    int i;

    for (i = 0; i < 128; i++)
        sum += block[i];
    return (sum & 0xFFu) == 0;
}

static ddcci_status_t edid_read_block(struct ddcci_display *d, uint8_t *raw,
                                      unsigned base)
{
    unsigned off = 0;
    unsigned prefer = d->edid_chunk ? d->edid_chunk : DDCCI_EDID_CHUNK;

    while (off < 128u) {
        unsigned remaining = 128u - off;
        unsigned requested = prefer > remaining ? remaining : prefer;
        unsigned try = requested;
        int reduced = 0;
        int attempts = 0;
        ddcci_status_t st = DDCCI_ERR_IO;

        while (try >= 8u && attempts < 3) {
            st = edid_chunk(d, (uint8_t)(base + off), raw + base + off,
                            (uint16_t)try);
            if (st == DDCCI_OK)
                break;
            if (st == DDCCI_ERR_NO_DEVICE)
                return st;
            reduced = 1;
            attempts++;
            try /= 2u;
        }
        if (st != DDCCI_OK)
            return st;
        /* Remember a smaller size only when a full attempt failed, so the
         * next chunk does not pay a kernel timeout on DP-AUX's 16-byte cap. */
        if (reduced && try >= 8u)
            d->edid_chunk = prefer = try;
        off += try;
    }
    return DDCCI_OK;
}

ddcci_status_t ddcci_read_edid_i2c(struct ddcci_display *d, ddcci_edid *out)
{
    uint8_t raw[DDCCI_EDID_LEN_MAX];
    ddcci_status_t st;
    size_t len = DDCCI_EDID_LEN_MIN;

    if (!d || d->fd < 0 || !out)
        return DDCCI_ERR_INVALID_ARG;

    memset(raw, 0, sizeof(raw));
    st = bus_lock(d->fd);
    if (st != DDCCI_OK)
        return st;

    st = edid_read_block(d, raw, 0);
    /* Extension count is only meaningful when block 0's checksum matches.
     * A garbage count would otherwise cost another slow I2C read. */
    if (st == DDCCI_OK && edid_sum_ok(raw) && raw[126] >= 1 && raw[126] <= 3) {
        if (edid_read_block(d, raw, 128) == DDCCI_OK)
            len = DDCCI_EDID_LEN_MAX;
    }
    bus_unlock(d->fd);
    if (st != DDCCI_OK)
        return st;

    /* Some panels ignore DDC/CI if it follows EDID with no gap. */
    schedule_gap(d, ddcci_mono_ms(), DDCCI_EDID_GAP_MS);
    return ddcci_parse_edid(raw, len, out);
}
