// SPDX-License-Identifier: GPL-3.0-or-later
#include "shaode/sleep.h"
#include <wlr/util/log.h>

#if SHAODE_SDBUS_SYSTEMD
#include <systemd/sd-bus.h>
#elif SHAODE_SDBUS_ELOGIND
#include <elogind/sd-bus.h>
#elif SHAODE_SDBUS_BASU
#include <basu/sd-bus.h>
#endif

#if SHAODE_SDBUS_SYSTEMD || SHAODE_SDBUS_ELOGIND || SHAODE_SDBUS_BASU
#include <errno.h>
#include <fcntl.h>
#include <string.h>

int sh_sleep_inhibit(void) {
    sd_bus *bus = NULL;
    sd_bus_message *reply = NULL;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int fd = -1;
    int ret = sd_bus_open_system(&bus);
    if (ret < 0) {
        wlr_log(WLR_ERROR, "Cannot keep the machine awake: no system bus: %s", strerror(-ret));
        return -1;
    }
    // This runs on VT switches, so a stuck logind must not freeze the desktop for long.
    sd_bus_set_method_call_timeout(bus, 2 * 1000 * 1000);
    ret = sd_bus_call_method(bus, "org.freedesktop.login1", "/org/freedesktop/login1",
                             "org.freedesktop.login1.Manager", "Inhibit", &error, &reply, "ssss",
                             "sleep", "shaoDe", "The desktop is on screen", "block");
    int held;
    if (ret < 0) {
        wlr_log(WLR_ERROR, "Cannot keep the machine awake: %s", error.message);
    } else if ((ret = sd_bus_message_read(reply, "h", &held)) < 0) {
        wlr_log(WLR_ERROR, "Cannot keep the machine awake: %s", strerror(-ret));
    } else if ((fd = fcntl(held, F_DUPFD_CLOEXEC, 3)) < 0) { // The reply owns `held`.
        wlr_log(WLR_ERROR, "Cannot keep the machine awake: %s", strerror(errno));
    }
    sd_bus_error_free(&error);
    sd_bus_message_unref(reply);
    sd_bus_flush_close_unref(bus);
    return fd;
}
#else
int sh_sleep_inhibit(void) {
    return -1;
}
#endif
