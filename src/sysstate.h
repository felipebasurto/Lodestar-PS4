/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Interruption model for mount, register, delete and scan. Host tests walk every
 * step. Outcomes that depend on ShellCore are labeled as such in docs/hazards. */
#pragma once

#include <string.h>

#include "delete.h"

typedef enum {
    SYS_FATE_STAY = 0, /* process still at this step */
    SYS_FATE_DEATH,    /* crash, kill, GoldHEN reload: memory gone, nullfs remains */
    SYS_FATE_REBOOT,   /* power cycle: nullfs is gone, files remain */
    SYS_FATE_REBUILD   /* reboot plus a rebuilt app.db: external rows dropped */
} sys_fate_t;

typedef enum {
    SYS_BYTES_ABSENT = 0,
    SYS_BYTES_OLD,
    SYS_BYTES_TRUNC, /* visible short prefix; durable replace never produces this */
    SYS_BYTES_NEW
} sys_bytes_t;

/* step 0: not started. step 1: bytes accepted but not committed. step 2: committed.
 * existed is 1 when a regular file was already at the path. */
static inline sys_bytes_t sys_replace_visible(int durable, int existed, int step) {
    if (step >= 2)
        return SYS_BYTES_NEW;
    if (step <= 0 || durable)
        return existed ? SYS_BYTES_OLD : SYS_BYTES_ABSENT;
    return SYS_BYTES_TRUNC;
}

/* 1 when a crash at this uncommitted step can leave a temp sibling. */
static inline int sys_replace_leaves_temp(int durable, int step) {
    return durable && step == 1;
}

/* A pair is one generation: both new, or neither new. A short file is never consistent. */
static inline int sys_pair_consistent(sys_bytes_t a, sys_bytes_t b) {
    if (a == SYS_BYTES_TRUNC || b == SYS_BYTES_TRUNC)
        return 0;
    return (a == SYS_BYTES_NEW) == (b == SYS_BYTES_NEW);
}

typedef enum {
    SYS_REG_IDLE = 0,
    SYS_REG_META_TEMP,   /* param.sfo temp written, not renamed */
    SYS_REG_META,        /* appmeta, priv param.sfo and playgo committed; no row */
    SYS_REG_LIC_RIF,     /* rif committed, idx not yet */
    SYS_REG_LIC,         /* rif and idx both committed */
    SYS_REG_HOLD,        /* hold symlink in place */
    SYS_REG_MOUNTED,     /* nullfs up; /user/app/TID was mkdir'd first */
    SYS_REG_MASKED,      /* /user/app shows only the batch */
    SYS_REG_INSTALLING,  /* AppInstallAll in flight; row not assumed yet */
    SYS_REG_ROW_MASKED,  /* system row exists; still masked */
    SYS_REG_UNMASKED,    /* row exists; mask down; no .src yet */
    SYS_REG_MARKER,      /* .src written */
    SYS_REG_N
} sys_reg_step_t;

typedef struct {
    sys_bytes_t meta, rif, idx;
    int hold, mounted, masked, db_row;
    int marker_src, marker_removed;
    int library, usb_pkg, saves;
    int playgo, appmeta, empty_user_dir;
    int db_written; /* Lodestar never writes system databases */
    int orphan_temp;
} sys_obs_t;

static inline void sys_obs_external(sys_obs_t *o) {
    memset(o, 0, sizeof(*o));
    o->library = 1;
    o->usb_pkg = 1;
    o->saves = 1;
    /* A dump often shares the 19-character content-id prefix with a real install. */
    o->rif = SYS_BYTES_OLD;
    o->idx = SYS_BYTES_OLD;
}

static inline void sys_apply_fate(sys_obs_t *o, sys_fate_t fate) {
    if (fate == SYS_FATE_REBOOT || fate == SYS_FATE_REBUILD) {
        o->mounted = 0;
        o->masked = 0;
    }
    /* Inference, not a console measurement: DbRecovery drops rows that existed only
     * because AppInstallAll saw a nullfs. The pkg and the metadata files stay. */
    if (fate == SYS_FATE_REBUILD)
        o->db_row = 0;
}

static inline void sys_register_at(sys_obs_t *o, int step) {
    sys_obs_external(o);
    if (step >= SYS_REG_META_TEMP) {
        o->orphan_temp = 1;
        o->meta = sys_replace_visible(1, 0, 1);
    }
    if (step >= SYS_REG_META) {
        o->orphan_temp = 0;
        o->meta = SYS_BYTES_NEW;
        o->appmeta = 1;
        o->playgo = 1;
    }
    if (step >= SYS_REG_LIC_RIF)
        o->rif = SYS_BYTES_NEW;
    if (step >= SYS_REG_LIC)
        o->idx = SYS_BYTES_NEW;
    if (step >= SYS_REG_HOLD)
        o->hold = 1;
    if (step >= SYS_REG_MOUNTED) {
        o->mounted = 1;
        o->empty_user_dir = 1;
    }
    if (step >= SYS_REG_MASKED)
        o->masked = 1;
    if (step >= SYS_REG_ROW_MASKED)
        o->db_row = 1;
    if (step >= SYS_REG_UNMASKED)
        o->masked = 0;
    if (step >= SYS_REG_MARKER)
        o->marker_src = 1;
}

static inline sys_obs_t sys_register_obs(int step, sys_fate_t fate) {
    sys_obs_t o;
    sys_register_at(&o, step);
    sys_apply_fate(&o, fate);
    return o;
}

static inline const char *sys_reg_name(int step) {
    static const char *const names[] = {
        "idle", "meta-temp", "meta", "lic-rif", "lic", "hold", "mounted", "masked", "installing", "row-masked",
        "unmasked", "marker"};
    if (step < 0 || step >= SYS_REG_N)
        return "?";
    return names[step];
}

typedef enum {
    SYS_DEL_IDLE = 0,
    SYS_DEL_INTENT,           /* GameWillDelete only; memory */
    SYS_DEL_UNMOUNTED,        /* detached; still registered */
    SYS_DEL_UNBOUND_ZERO,     /* some AppUnInstallMainExecute ret=0; title not checked */
    SYS_DEL_CONFIRMED_ABSENT, /* fresh read: was registered, now absent */
    SYS_DEL_BOUND_ZERO,       /* ret=0 names this title */
    SYS_DEL_TIMEOUT,
    SYS_DEL_N
} sys_del_step_t;

/* What the current decision does. Binding the result to a title is the caller's job;
 * delete_pending_decision does not receive a title id (DEL-6). */
static inline int sys_delete_commits_now(int step) {
    switch (step) {
    case SYS_DEL_UNBOUND_ZERO:
    case SYS_DEL_BOUND_ZERO:
        return delete_pending_decision(0, 1, 0, 0, 0) == 1;
    case SYS_DEL_CONFIRMED_ABSENT:
        return delete_pending_decision(-1, 2, 1, 0, 0) == 1;
    case SYS_DEL_TIMEOUT:
        return delete_pending_decision(-1, 1, 1, 0, DELETE_PENDING_MAX_SEC) == 1;
    default:
        return 0;
    }
}

/* Commit only for a confirmed absence transition or a zero result that names this title. */
static inline int sys_delete_commit_should(int step) {
    return step == SYS_DEL_CONFIRMED_ABSENT || step == SYS_DEL_BOUND_ZERO;
}

/* organized: the pkg lives in <root>/ps4/<TID>, so committing deletes that folder. */
static inline sys_obs_t sys_delete_obs(int step, int organized, sys_fate_t fate) {
    sys_obs_t o;
    sys_obs_external(&o);
    o.db_row = 1;
    o.mounted = step == SYS_DEL_IDLE || step == SYS_DEL_INTENT;
    if (sys_delete_commits_now(step) && organized) {
        o.library = 0;
        o.usb_pkg = 0;
    }
    if (sys_delete_commits_now(step) && !organized)
        o.marker_removed = 1;
    sys_apply_fate(&o, fate);
    return o;
}

/* Current scan gate: a ready read is enough. The corrupt marker is ignored. */
static inline int sys_scan_may_register(int db_ready, int corrupt_marker) {
    (void)corrupt_marker;
    return db_ready;
}

static inline int sys_scan_should_register(int db_ready, int corrupt_marker) {
    return db_ready && !corrupt_marker;
}

/* Any non-dot directory entry currently counts as a finished internal add-on. */
static inline int sys_addcont_installed_now(int any_nondot_entry) {
    return any_nondot_entry;
}

static inline int sys_addcont_installed_should(int regular_ac_pkg, int size_matches) {
    return regular_ac_pkg && size_matches;
}
