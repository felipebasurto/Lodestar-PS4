/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * System-state durability and snapshot tests (specs/system-state.md SYS-1, SYS-2;
 * specs/ps4-native.md P4N-33). */
#include "dbsnap.h"
#include "durable.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_pass, g_fail;

#define CHECK(cond, msg)                                                           \
    do {                                                                           \
        if (cond) {                                                                \
            g_pass++;                                                              \
        } else {                                                                   \
            g_fail++;                                                              \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg));      \
        }                                                                          \
    } while (0)

static int read_str(const char *path, char *out, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = 0;
    return (int)n;
}

static int count_tmp_files(const char *dir) {
    DIR *d = opendir(dir);
    if (!d)
        return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (strstr(e->d_name, ".tmp."))
            n++;
    closedir(d);
    return n;
}

static void test_sys1_snapshot_policy(void) {
    /* Failed query never publishes. */
    CHECK(db_snapshot_publish(0, 0, 0, 10, 0) == 0, "failed read rejected");
    CHECK(db_snapshot_publish(0, 1, 10, 10, 0) == 0, "failed read with prior rejected");

    /* Empty-but-successful read never publishes (SYS-1 / H3). */
    CHECK(db_snapshot_publish(1, 0, 0, 0, 0) == 0, "empty first read rejected");
    CHECK(db_snapshot_publish(1, 1, 10, 0, 0) == 0, "empty subsequent read rejected");
    CHECK(db_snapshot_publish(1, 1, 10, 0, 1) == 0, "empty journal read rejected");

    /* Hot journal rejects first read and any shrunk read. */
    CHECK(db_snapshot_publish(1, 0, 0, 20, 1) == 0, "first read with journal rejected");
    CHECK(db_snapshot_publish(1, 1, 20, 19, 1) == 0, "shrunk read with journal rejected");
    CHECK(db_snapshot_publish(1, 1, 20, 20, 1) == 1, "unchanged count with journal allowed");
    CHECK(db_snapshot_publish(1, 1, 20, 21, 1) == 1, "grown count with journal allowed");

    /* Clean (no journal) non-empty reads publish, including genuine uninstalls. */
    CHECK(db_snapshot_publish(1, 0, 0, 1, 0) == 1, "clean non-empty first read allowed");
    CHECK(db_snapshot_publish(1, 1, 20, 19, 0) == 1, "clean genuine uninstall shrink allowed");
}

static void test_sys2_durable_replace(void) {
    char dir[] = "/tmp/lodestar-sysstate-XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "mkdtemp");
    char target[256];
    snprintf(target, sizeof(target), "%s/config.txt", dir);

    /* Fresh creation. */
    const char *v1 = "ver=3\nbatch=1\n";
    CHECK(durable_replace(target, v1, strlen(v1), 0644) == 0, "fresh durable_replace succeeds");
    char buf[128];
    CHECK(read_str(target, buf, sizeof(buf)) == (int)strlen(v1) && !strcmp(buf, v1),
          "fresh file content matches");
    CHECK(count_tmp_files(dir) == 0, "no leftover .tmp.* after fresh write");
    struct stat st1;
    CHECK(stat(target, &st1) == 0 && (st1.st_mode & 0777) == 0644, "fresh file mode applied");

    /* Atomic overwrite produces a new inode and replaces whole content. */
    const char *v2 = "ver=3\nbatch=0\norganize=0\n";
    CHECK(durable_replace(target, v2, strlen(v2), 0600) == 0, "overwrite durable_replace succeeds");
    struct stat st2;
    CHECK(stat(target, &st2) == 0 && (st2.st_mode & 0777) == 0600, "overwrite mode applied");
    CHECK(st2.st_ino != st1.st_ino, "overwrite replaced via rename (new inode, not O_TRUNC)");
    CHECK(read_str(target, buf, sizeof(buf)) == (int)strlen(v2) && !strcmp(buf, v2),
          "overwritten content matches");
    CHECK(count_tmp_files(dir) == 0, "no leftover .tmp.* after overwrite");

    /* Zero-length replace succeeds cleanly. */
    CHECK(durable_replace(target, "", 0, 0644) == 0, "zero-length durable_replace succeeds");
    CHECK(stat(target, &st2) == 0 && st2.st_size == 0, "zero-length file has size 0");

    /* Restore v2, then force failure (rename over a directory fails with EISDIR). */
    CHECK(durable_replace(target, v2, strlen(v2), 0644) == 0, "restore v2");
    char bad_dir[256];
    snprintf(bad_dir, sizeof(bad_dir), "%s/subdir", dir);
    CHECK(mkdir(bad_dir, 0755) == 0, "mkdir subdir");
    CHECK(durable_replace(bad_dir, "nope", 4, 0644) == -1, "rename onto directory fails");
    CHECK(count_tmp_files(dir) == 0, "failed durable_replace cleans up temp file");

    /* Non-existent parent directory fails without touching existing target. */
    char missing[256];
    snprintf(missing, sizeof(missing), "%s/no-such-dir/config.txt", dir);
    CHECK(durable_replace(missing, "x", 1, 0644) == -1, "missing parent fails");
    CHECK(durable_replace(NULL, "x", 1, 0644) == -1 && errno == EINVAL, "NULL path rejected");
    CHECK(durable_replace(target, NULL, 5, 0644) == -1 && errno == EINVAL, "NULL data with len>0 rejected");
    CHECK(read_str(target, buf, sizeof(buf)) == (int)strlen(v2) && !strcmp(buf, v2),
          "existing target remains untouched after rejected calls");

    rmdir(bad_dir);
    unlink(target);
    rmdir(dir);
}

int main(void) {
    test_sys1_snapshot_policy();
    test_sys2_durable_replace();
    printf("sysstate_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}
