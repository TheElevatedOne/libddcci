#include "ddcci.h"

/* Linked against both the static archive and the shared library by `make test`.
 * Does not open a device. */
int main(void)
{
    if (!ddcci_strerror(DDCCI_OK))
        return 1;
    if (ddcci_open(-1, NULL) != DDCCI_ERR_INVALID_ARG)
        return 1;
    if (ddcci_open_path(NULL, NULL) != DDCCI_ERR_INVALID_ARG)
        return 1;
    if (ddcci_open_connector(NULL, NULL) != DDCCI_ERR_INVALID_ARG)
        return 1;
    if (ddcci_parse_edid(NULL, 0, NULL) != DDCCI_ERR_INVALID_ARG)
        return 1;
    if (ddcci_parse_capabilities(NULL, NULL) != DDCCI_ERR_INVALID_ARG)
        return 1;
    if (ddcci_find_displays(NULL, NULL) != DDCCI_ERR_INVALID_ARG)
        return 1;
    if (ddcci_save_settings(NULL) != DDCCI_ERR_INVALID_ARG)
        return 1;
    if (ddcci_get_vcp(NULL, 0x10, NULL) != DDCCI_ERR_INVALID_ARG)
        return 1;
    if (ddcci_set_vcp(NULL, 0x10, 1) != DDCCI_ERR_INVALID_ARG)
        return 1;
    ddcci_set_sleep_scale(NULL, 0.5);
    ddcci_close(NULL);
    ddcci_free_info_list(NULL);
    return 0;
}
