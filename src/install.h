/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Installs with the system's background transfer service (BGFT), as its own installer
 * does: for add-ons, whose record in addcont.db only the system may write. */
#pragma once

#include <stdint.h>

/* Starts installing the pkg at path (as the system names it: /user/data/..., /mnt/usb0/...)
 * for user, shown as name. Returns the BGFT task id, or -1 (msg says why). The install
 * runs in the system: its result shows in /user/addcont. */
int install_pkg(const char *path, const char *name, int user, char *msg, unsigned long n);

/* Closes the installer service, when it was opened: the next payload in this process
 * finds it closed. */
void install_close(void);

/* Called by install.c for every positive task id the system returns, even when registration
 * or start later fails: the id is occupied for the rest of the payload run. */
void bgft_task_id_taken(uint32_t id);
