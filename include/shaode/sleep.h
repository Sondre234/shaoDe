// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/* Keeps the machine awake while a standalone session is on screen. Another desktop on a
 * different VT sees no input while shaoDe is in front, so its idle daemon (hypridle, swayidle)
 * would otherwise suspend the machine under the user. */

/* Takes a logind "sleep" block inhibitor. Returns the file descriptor that holds it (close it
 * to release the inhibitor), or -1 when logind is unreachable or shaoDe was built without
 * sd-bus. */
int sh_sleep_inhibit(void);
