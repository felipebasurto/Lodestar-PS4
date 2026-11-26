/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Unit tests for the parts that run without a console: crypto, the install file
 * builders and the param.sfo parser. Build and run with: bash test/run.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "autorun.h"
#include "bgftinfo.h"
#include "bgftscan.h"
#include "delete.h"
#include "dlcvec.h"
#include "build.h"
#include "check.h"
#include "crypto.h"
#include "emu.h"
#include "lang.h"
#include "mem.h"
#include "pkgmake.h"
#include "origin.h"
#include "pkg.h"
#include "mountplan.h"
#include "retention.h"
#include "restore.h"
#include "scanreq.h"
#include "stage.h"
#include "webnet.h"
#include "pdb_template.h"

static int g_failed, g_checks;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        g_checks++;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            g_failed++;                                                                                                \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                   \
        }                                                                                                              \
    } while (0)

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

static void hex(const char *s, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++)
        sscanf(s + 2 * i, "%2hhx", &out[i]);
}

/* A pkg with only the fields the builders use. */
static void fake_pkg(pkg_t *p, const char *content_id, uint64_t size) {
    memset(p, 0, sizeof(*p));
    snprintf(p->content_id, sizeof(p->content_id), "%s", content_id);
    memcpy(p->title_id, content_id + 7, 9);
    p->pkg_size = size;
    for (int i = 0; i < 32; i++)
        p->digest[i] = (uint8_t)(0xa0 + i);
}

static void test_crypto(void) {
    printf("crypto\n");
    uint8_t h[32], want[32];
    sha256("abc", 3, h);
    hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", want, 32);
    CHECK(!memcmp(h, want, 32));
    sha256("", 0, h);
    hex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", want, 32);
    CHECK(!memcmp(h, want, 32));
    /* the same digest fed in pieces */
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, "a", 1);
    sha256_update(&c, "bc", 2);
    sha256_final(&c, h);
    hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", want, 32);
    CHECK(!memcmp(h, want, 32));
    CHECK(crc32c("123456789", 9) == 0xe3069283u);
    /* NIST SP 800-38A F.2.2 AES-128-CBC decrypt (2 blocks) */
    uint8_t key[16], iv[16], ct[32], pt[32];
    hex("2b7e151628aed2a6abf7158809cf4f3c", key, 16);
    hex("000102030405060708090a0b0c0d0e0f", iv, 16);
    hex("7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2", ct, 32);
    hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51", pt, 32);
    aes128_cbc_decrypt(key, iv, ct, sizeof(ct));
    CHECK(!memcmp(ct, pt, sizeof(pt)));
}

static void test_app_json(void) {
    printf("app.json / ac.json\n");
    pkg_t p;
    fake_pkg(&p, "UP0000-CUSA00001_00-TESTGAME00000000", 1234567);
    buf_t b = {0};
    build_app_json(&b, &p, "/mnt/usb0/ps4/CUSA00001/app.pkg");
    const char *want = "{\"numberOfSplitFiles\":1,\"originalFileSize\":1234567,\"packageDigest\":"
                       "\"A0A1A2A3A4A5A6A7A8A9AAABACADAEAFB0B1B2B3B4B5B6B7B8B9BABBBCBDBEBF\",\"pieces\":[{\"fileOffset\":0,"
                       "\"fileSize\":1234567,\"url\":\"\\/mnt\\/usb0\\/ps4\\/CUSA00001\\/app.pkg\"}]}";
    CHECK(b.len == strlen(want) && !memcmp(b.data, want, b.len));
    buf_free(&b);
}

static void check_pbm(const pkg_t *p, int type) {
    buf_t b = {0};
    build_app_pbm(&b, p, "01.02", type);
    uint64_t blocks = (p->pkg_size + 0xffff) / 0x10000;
    size_t bitmap = (size_t)((blocks + 7) / 8);
    CHECK(b.len == 0x100 + bitmap + 32);
    CHECK(!memcmp(b.data, "pdbm", 4));
    CHECK(!memcmp(b.data + 4, p->title_id, 9));
    CHECK(!memcmp(b.data + 0x0e, "01.02", 5));
    CHECK(le32(b.data + 0x1e) == (uint32_t)type);
    CHECK(le64(b.data + 0x22) == blocks);
    CHECK(!memcmp(b.data + 0x60, p->digest, 32));
    CHECK(le32(b.data + 0xac) == bitmap);
    /* all blocks present; bits past the last block stay clear */
    CHECK(b.data[0x100] == 0xff);
    if (blocks % 8)
        CHECK(b.data[0x100 + bitmap - 1] == (uint8_t)(0xff << (8 - blocks % 8)));
    uint8_t h[32];
    sha256(b.data, b.len - 32, h);
    CHECK(!memcmp(b.data + b.len - 32, h, 32));
    buf_free(&b);
}

static void test_pbm(void) {
    printf("app.pbm / patch.pbm / ac.pbm\n");
    pkg_t p;
    fake_pkg(&p, "UP0000-CUSA00001_00-TESTGAME00000000", 82706432); /* whole blocks */
    check_pbm(&p, PBM_APP);
    check_pbm(&p, PBM_PATCH);
    fake_pkg(&p, "UP0000-CUSA00001_00-TESTADDON0000001", 524288 + 100); /* partial last block */
    check_pbm(&p, PBM_ADDCONT);
    check_pbm(&p, PBM_LICENSE);
}

/* Records of a pdb: u32 version, then (u32 key, u32 len, u32 len, data), CRC32C trailer. */
typedef struct {
    uint32_t key, len;
    const uint8_t *data;
} rec_t;

static int parse_pdb(const uint8_t *d, size_t n, rec_t *out, int max) {
    int c = 0;
    size_t off = 4;
    while (off + 12 <= n - 4 && c < max) {
        out[c].key = le32(d + off);
        out[c].len = le32(d + off + 4);
        out[c].data = d + off + 12;
        off += 12 + out[c].len;
        c++;
    }
    return off == n - 4 ? c : -1;
}

static const rec_t *find(const rec_t *r, int n, uint32_t key) {
    for (int i = 0; i < n; i++)
        if (r[i].key == key)
            return &r[i];
    return NULL;
}

static void test_ac_pdb(void) {
    printf("add-on PlayGo record\n");
    pkg_t p;
    fake_pkg(&p, "UP0000-CUSA00001_00-TESTADDON0000001", 31129600);
    const char *url = "/mnt/usb0/ps4/CUSA00001/addcont/CUSA00001/TESTADDON0000001/ac.pkg";
    buf_t json = {0}, pdb = {0};
    build_app_json(&json, &p, url);
    build_ac_pdb(&pdb, &p, url, &json);
    static rec_t got[256], tmpl[256];
    int n = parse_pdb(pdb.data, pdb.len, got, 256);
    int nt = parse_pdb(k_pdb_addcont_template, sizeof(k_pdb_addcont_template), tmpl, 256);
    CHECK(n > 0 && n == nt);
    for (int i = 0; n > 0 && i < n && i < nt; i++)
        CHECK(got[i].key == tmpl[i].key); /* same records in the same order as the installer */
    CHECK(le32(pdb.data) == le32(k_pdb_addcont_template));
    CHECK(le32(pdb.data + pdb.len - 4) == crc32c(pdb.data, pdb.len - 4));
    const rec_t *r;
    CHECK((r = find(got, n, 0xf4241)) && r->len == 36 && !memcmp(r->data, p.content_id, 36));
    CHECK((r = find(got, n, 0xf42a6)) && r->len == strlen(url) && !memcmp(r->data, url, r->len));
    CHECK((r = find(got, n, 0xf42a7)) && r->len == json.len && !memcmp(r->data, json.data, r->len));
    for (uint32_t k = 0xf4246; k <= 0xf4249; k++)
        CHECK((r = find(got, n, k)) && r->len == 8 && le64(r->data) == p.pkg_size);
    CHECK((r = find(got, n, 0xf431b)) && r->len == 53 && !memcmp(r->data, "0000000000000000-", 17) &&
          !memcmp(r->data + 17, p.content_id, 36));
    buf_free(&json);
    buf_free(&pdb);
}

/* PS4 BGFT tasks: u32 0, then records to the end of the file (no trailer). */
static int parse_task(const uint8_t *d, size_t n, rec_t *out, int max) {
    int c = 0;
    size_t off = 4;
    while (off + 12 <= n && c < max) {
        out[c].key = le32(d + off);
        out[c].len = le32(d + off + 4);
        out[c].data = d + off + 12;
        off += 12 + out[c].len;
        c++;
    }
    return off == n ? c : -1;
}

static int rec_is(const rec_t *r, int n, uint32_t key, const void *data, size_t len) {
    const rec_t *f = find(r, n, key);
    return f && f->len == len && !memcmp(f->data, data, len);
}

static void test_bgft_task(void) {
    printf("BGFT game task\n");
    pkg_t app, pat;
    fake_pkg(&app, "UP0000-CUSA00001_00-TESTGAME00000000", 271384576);
    fake_pkg(&pat, "UP0000-CUSA00001_00-TESTGAME00000000", 21430272);
    const char *url = "/mnt/usb0/ps4/CUSA00001/app.pkg", *purl = "/mnt/usb0/ps4/CUSA00001/patch.pkg";
    buf_t json = {0}, pjson = {0};
    build_app_json(&json, &app, url);
    build_app_json(&pjson, &pat, purl);
    patch_ref_t pr = {&pat, NULL, 0, "01.14", purl, &pjson};
    static rec_t r[128];
    uint8_t v[8];
    for (int with_patch = 0; with_patch < 2; with_patch++)
        for (int d1 = 0; d1 < 2; d1++) {
            bgft_task_t t = {d1, 0x13520001, 0x00e31e2713ddac1eull, 0xf092, &app, "app.pkg", url, &json,
                             with_patch ? &pr : NULL};
            buf_t b = {0};
            build_bgft_task(&b, &t);
            int n = parse_task(b.data, b.len, r, 128);
            CHECK(n == (with_patch ? 47 : 41));
            CHECK(le32(b.data) == 0);
            CHECK(r[0].key == 0x95 && le32(r[0].data) == 0x13520001);
            /* the creation time sits where place_bgft reads it back */
            CHECK(le32(b.data + 152) == 0x76 && le64(b.data + 164) == 0x00e31e2713ddac1eull);
            CHECK(rec_is(r, n, 0x74, app.content_id, 36));
            CHECK(rec_is(r, n, 0x7c, "CUSA00001", 9));
            CHECK(rec_is(r, n, 0x1388, url, strlen(url)));
            CHECK(rec_is(r, n, 0x1389, "/user/app/CUSA00001/app.pkg", 27));
            CHECK(rec_is(r, n, 0x1390, json.data, json.len));
            CHECK(find(r, n, 0x1a3) && le64(find(r, n, 0x1a3)->data) == app.pkg_size);
            uint64_t total = app.pkg_size + (with_patch ? pat.pkg_size : 0);
            CHECK(find(r, n, 0x7a) && le64(find(r, n, 0x7a)->data) == total);
            CHECK(find(r, n, 0x7b) && le64(find(r, n, 0x7b)->data) == total);
            CHECK(find(r, n, 0x1f4) && le32(find(r, n, 0x1f4)->data) == (with_patch ? 2u : 1u));
            CHECK(find(r, n, 0x1a4) && le32(find(r, n, 0x1a4)->data) == 0xf092);
            CHECK(find(r, n, 0x9a) && le64(find(r, n, 0x9a)->data) == (d1 ? 0x8000000000000000ull : 0));
            memset(v, 0, 8);
            v[7] = 0x10;
            CHECK(rec_is(r, n, 0x193, v, 8));
            CHECK(!with_patch == !find(r, n, 0x13a9));
            if (with_patch) {
                CHECK(rec_is(r, n, 0x13a8, purl, strlen(purl)));
                CHECK(rec_is(r, n, 0x13a9, "/user/patch/CUSA00001/patch.pkg", 31));
                CHECK(rec_is(r, n, 0x13b0, pjson.data, pjson.len));
                CHECK(rec_is(r, n, 0x84, "01.14", 5));
            }
            buf_free(&b);
        }
    buf_free(&json);
    buf_free(&pjson);
}

static void slot(uint8_t *lic, uint8_t v) { memset(lic, v, 0x400); }

static int idx_count(const buf_t *idx) { return (int)le32(idx->data + 4); }

static const uint8_t *idx_label(const buf_t *idx, int i) { return idx->data + 0x20 + i * 0x30; }

static void test_license(void) {
    printf("license files (rif/idx)\n");
    static uint8_t a[0x400], b[0x400], c[0x400];
    slot(a, 0x11);
    slot(b, 0x22);
    slot(c, 0x33);
    const char *game = "UP0000-CUSA00001_00-TESTGAME00000000", *dlc = "UP0000-CUSA00001_00-TESTADDON0000001";

    buf_t r1 = {0}, i1 = {0};
    build_license(&r1, &i1, NULL, 0, NULL, 0, game, a);
    CHECK(r1.len == 0x800 && !memcmp(r1.data, "rifa", 4) && !memcmp(r1.data + 4, game, 19));
    CHECK(!memcmp(r1.data + 0x400, a, 0x400));
    CHECK(i1.len == 0x50 && !memcmp(i1.data, "ridx", 4) && idx_count(&i1) == 1);
    CHECK(!memcmp(i1.data + 9, game, 19) && !memcmp(idx_label(&i1, 0), game + 20, 16));
    CHECK(le64(idx_label(&i1, 0) + 0x10) == 0x400 && le64(idx_label(&i1, 0) + 0x18) == 0x400);

    /* an add-on gets a second slot, the game keeps its own */
    buf_t r2 = {0}, i2 = {0};
    build_license(&r2, &i2, r1.data, r1.len, i1.data, i1.len, dlc, b);
    CHECK(r2.len == 0xc00 && idx_count(&i2) == 2 && i2.len == 0x80);
    CHECK(!memcmp(r2.data + 0x400, a, 0x400) && !memcmp(r2.data + 0x800, b, 0x400));
    CHECK(!memcmp(idx_label(&i2, 1), dlc + 20, 16) && le64(idx_label(&i2, 1) + 0x10) == 0x800);

    /* writing the game again replaces its slot and keeps the add-on */
    buf_t r3 = {0}, i3 = {0};
    build_license(&r3, &i3, r2.data, r2.len, i2.data, i2.len, game, c);
    CHECK(r3.len == 0xc00 && idx_count(&i3) == 2);
    CHECK(!memcmp(r3.data + 0x400, c, 0x400) && !memcmp(r3.data + 0x800, b, 0x400));

    /* files of another content ID prefix, or damaged ones, are replaced */
    buf_t r4 = {0}, i4 = {0};
    build_license(&r4, &i4, r2.data, r2.len, i2.data, i2.len, "UP0000-CUSA00002_00-OTHERGAME0000000", a);
    CHECK(r4.len == 0x800 && idx_count(&i4) == 1);
    buf_t r5 = {0}, i5 = {0};
    build_license(&r5, &i5, r2.data, 0x500, i2.data, i2.len, dlc, b); /* rif shorter than its slots */
    CHECK(r5.len == 0x800 && idx_count(&i5) == 1);

    buf_t *all[] = {&r1, &i1, &r2, &i2, &r3, &i3, &r4, &i4, &r5, &i5};
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
        buf_free(all[i]);
}

/* A param.sfo with one string and one integer. */
static size_t make_sfo(uint8_t *d) {
    memset(d, 0, 256);
    const char keys[] = "TITLE\0ATTRIBUTE\0";
    uint32_t ko = 20 + 2 * 16, dof = ko + (uint32_t)sizeof(keys);
    memcpy(d, "\0PSF", 4);
    d[4] = 1, d[5] = 1;
    d[8] = (uint8_t)ko, d[12] = (uint8_t)dof, d[16] = 2;
    uint8_t *e = d + 20;
    e[0] = 0, e[2] = 0x04, e[3] = 0x02, e[4] = 6, e[8] = 8, e[12] = 0; /* TITLE: utf8, "Title\0" */
    e += 16;
    e[0] = 6, e[2] = 0x04, e[3] = 0x04, e[4] = 4, e[8] = 4, e[12] = 8; /* ATTRIBUTE: int32 at 8 */
    memcpy(d + ko, keys, sizeof(keys));
    memcpy(d + dof, "Title", 6);
    d[dof + 8] = 0x34, d[dof + 9] = 0x12;
    return dof + 12;
}

static void test_sfo(void) {
    printf("param.sfo\n");
    uint8_t d[256];
    size_t n = make_sfo(d);
    sfo_t s;
    CHECK(sfo_parse(d, n, &s) == 0 && s.count == 2);
    const sfo_item_t *t = sfo_get(&s, "TITLE"), *a = sfo_get(&s, "ATTRIBUTE");
    CHECK(t && !t->is_int && !strcmp(t->sval, "Title"));
    CHECK(a && a->is_int && a->ival == 0x1234);
    CHECK(!sfo_get(&s, "CATEGORY"));
    /* decision strings are bounded: a long unknown/title value is truncated evidence,
     * never an unbounded allocation or a missing terminator */
    n = make_sfo(d);
    d[20 + 4] = 120; /* TITLE's value length, including its NUL */
    memset(d + (size_t)d[12], 'a', 119);
    d[(size_t)d[12] + 119] = 0;
    n = (size_t)d[12] + 120;
    CHECK(sfo_parse(d, n, &s) == 0);
    t = sfo_get(&s, "TITLE");
    CHECK(t && strlen(t->sval) == sizeof(t->sval) - 1);
    d[1] = 'X'; /* not an SFO */
    CHECK(sfo_parse(d, n, &s) != 0);
}


/* REG-1/REG-2 (specs/registration.md): registration distills only decision keys and
 * preserves the difference between absent keys, empty strings and wrong value types. */
static void test_pkg_meta(void) {
    printf("registration metadata distillation\n");
    sfo_t s;
    memset(&s, 0, sizeof(s));
    s.count = 5;
    snprintf(s.items[0].key, sizeof(s.items[0].key), "TITLE");
    snprintf(s.items[0].sval, sizeof(s.items[0].sval), "Game");
    snprintf(s.items[1].key, sizeof(s.items[1].key), "VERSION");
    snprintf(s.items[1].sval, sizeof(s.items[1].sval), "01.04");
    snprintf(s.items[2].key, sizeof(s.items[2].key), "APP_VER");
    s.items[2].is_int = 1; /* wrong type: present, but not valid string metadata */
    s.items[2].ival = 7;
    snprintf(s.items[3].key, sizeof(s.items[3].key), "SYSTEM_VER");
    s.items[3].is_int = 1;
    s.items[3].ival = 0x09008000;
    snprintf(s.items[4].key, sizeof(s.items[4].key), "CATEGORY");
    snprintf(s.items[4].sval, sizeof(s.items[4].sval), "ac");
    pkg_meta_t m;
    sfo_meta(&s, &m);
    CHECK(m.has_title && m.title_valid && !strcmp(m.title, "Game"));
    CHECK(m.has_version && m.version_valid && !strcmp(m.version, "01.04"));
    CHECK(m.has_app_ver && !m.app_ver_valid); /* absence and wrong type stay different */
    CHECK(m.has_system_ver && m.system_ver_valid && m.system_ver == 0x09008000);
    CHECK(m.has_category && !strcmp(m.category, "ac"));
    /* decision widths are bounded, but a too-long malformed category cannot become valid */
    snprintf(s.items[4].sval, sizeof(s.items[4].sval), "ac-long-malformed");
    sfo_meta(&s, &m);
    CHECK(m.has_category && strcmp(m.category, "ac"));
    snprintf(s.items[4].sval, sizeof(s.items[4].sval), "gdOxxxx");
    sfo_meta(&s, &m);
    CHECK(m.has_category && strcmp(m.category, "gdO"));
    snprintf(s.items[4].sval, sizeof(s.items[4].sval), "ac");
    memset(&s, 0, sizeof(s));
    sfo_meta(&s, &m);
    CHECK(!m.has_title && !m.title_valid && !m.has_version && !m.version_valid);
    CHECK(!m.has_app_ver && !m.app_ver_valid && !m.has_system_ver && !m.system_ver_valid);
    CHECK(sizeof(pkg_meta_t) < 160);
}

static void touch(const char *path) {
    FILE *f = fopen(path, "w");
    if (f)
        fclose(f);
}

/* PKG-1 (specs/pkg.md): the reader keeps the entries a pkg really has; the worst-case
 * entry cap does not make every open pkg carry a fixed 512-entry table. */
static void test_pkg_reader(void) {
    printf("pkg reader memory\n");
    char root[] = "/tmp/ps4mount-pkgreader-XXXXXX", p[256];
    if (!mkdtemp(root)) {
        CHECK(0);
        return;
    }
    snprintf(p, sizeof(p), "%s/x.pkg", root);
    make_pkg(p, "UP0000-CUSA00001_00-TESTGAME00000000", "gd", 0x09008000, 1, 0x100000, 0x100000);
    pkg_t pkg;
    CHECK(pkg_open(&pkg, p) == 0);
    CHECK(pkg.nentries == 2 && pkg.entries != NULL);
    size_t len = 0;
    uint8_t *sfo = pkg_entry(&pkg, 0x1000, &len);
    CHECK(sfo != NULL && len > 20);
    mem_free(sfo);
    pkg_close(&pkg);
    CHECK(pkg.entries == NULL && pkg.nentries == 0);
    /* memory regression bounds: the measured library has 11..54 entries; these objects
     * used to reserve 512 entries (pkg) and 512-byte strings (sfo) for every parse */
    CHECK(sizeof(pkg_entry_t) == 32);
    CHECK(sizeof(pkg_t) < 512);
    CHECK(sizeof(sfo_t) < 20480);
    unlink(p);
    rmdir(root);
}

static void test_check(void) {
    printf("pkg check\n");
    char root[] = "/tmp/ps4mount-test-XXXXXX", p[256];
    if (!mkdtemp(root)) {
        CHECK(0);
        return;
    }
    snprintf(p, sizeof(p), "%s/x.pkg", root);
    const char *cid = "UP0000-CUSA00001_00-TESTGAME00000000";
    pkg_check_t c;

    make_pkg(p, cid, "gd", 0x09008000, 1, 0x100000, 0x100000);
    CHECK(pkg_check(p, 0x13520001, &c) == 0 && c.ok && c.fake && c.kind == PKG_KIND_GAME && !c.problem[0]);
    CHECK(!strcmp(c.content_id, cid) && !strcmp(c.title_id, "CUSA00001") && !strcmp(c.title, "Test Game"));
    CHECK(!strcmp(c.app_ver, "01.04") && !strcmp(c.version, "01.00") && c.system_ver == 0x09008000);
    CHECK(c.pkg_size == 0x100000);

    /* only the start of a big pkg: everything is known already */
    make_pkg(p, cid, "gp", 0x09008000, 1, 30ull << 30, 0x4000);
    CHECK(pkg_check(p, 0x13520001, &c) == 0 && c.ok && c.kind == PKG_KIND_UPDATE && c.pkg_size == 30ull << 30);

    make_pkg(p, cid, "ac", 0, 1, 0x4000, 0x4000);
    CHECK(pkg_check(p, 0x13520001, &c) == 0 && c.ok && c.kind == PKG_KIND_ADDON);

    /* built for a newer firmware than the console's */
    make_pkg(p, cid, "gd", 0x13500000, 1, 0x4000, 0x4000);
    CHECK(pkg_check(p, 0x12520001, &c) == 0 && c.ok && strstr(c.warning, "firmware 13.50") &&
          strstr(c.warning, "12.52") && strstr(c.warning, "backport"));
    CHECK(pkg_check(p, 0x13500001, &c) == 0 && c.ok && !c.warning[0]); /* same firmware */
    CHECK(pkg_check(p, 0, &c) == 0 && c.ok && !c.warning[0]);          /* no firmware check */

    make_pkg(p, cid, "gd", 0x09008000, 0, 0x4000, 0x4000);
    CHECK(pkg_check(p, 0x13520001, &c) == 0 && !c.ok && !c.fake && strstr(c.problem, "fake"));
    CHECK(!c.installable); /* not fake: nothing to offer */

    /* homebrew ports and apps have title IDs of their own */
    make_pkg(p, "IV0000-SSBU00001_00-SSBURECOMPPS4000", "gd", 0, 1, 0x4000, 0x4000);
    CHECK(pkg_check(p, 0x13520001, &c) == 0 && !c.ok && c.kind == PKG_KIND_GAME && strstr(c.problem, "homebrew app") &&
          strstr(c.problem, "SSBU00001") && c.installable); /* a few KB: an app, for the installer */
    CHECK(homebrew_small("SSBU00001", 600ull << 20) && !homebrew_small("SSBU00001", 5ull << 30) &&
          !homebrew_small("CUSA00001", 1 << 20)); /* a port is big; CUSA games are never held back */
    /* PS1/PS2/PSP Classic fPKGs (Sony prefixes) under 1 GB are games, not small homebrew apps */
    CHECK(!homebrew_small("SLUS20071", 600ull << 20) && !homebrew_small("SCUS94163", 400ull << 20) &&
          !homebrew_small("ULUS10041", 300ull << 20) && !homebrew_small("NPUJ00001", 200ull << 20));
    CHECK(homebrew_small("PCSX20007", 50ull << 20)); /* emulator app: installable homebrew */
    /* GET /api/games "platform" (idea 10: the panel's filter): public, the panel reads it */
    CHECK(!strcmp(game_platform("CUSA00001"), "ps4") && !strcmp(game_platform("SLUS20071"), "classic") &&
          !strcmp(game_platform("NPUJ00001"), "classic") && !strcmp(game_platform("SMWR00001"), "homebrew") &&
          !strcmp(game_platform("PCSX20007"), "homebrew"));
    CHECK(pkg_cat_is_game("gd") && pkg_cat_is_game("gdO") && pkg_cat_is_game("gdo") && !pkg_cat_is_game("gdc") &&
          !pkg_cat_is_game("gp") && !pkg_cat_is_game("ac"));
    make_pkg(p, "UP9000-SLUS20071_00-SLUS200710000001", "gdO", 0x05050000, 1, 600ull << 20, 0x4000);
    CHECK(pkg_check(p, 0x13520001, &c) == 0 && c.ok && c.kind == PKG_KIND_GAME && !strcmp(c.title_id, "SLUS20071"));
    /* multi-chunk bitmap (> 4096 bytes) in build_app_pbm */
    {
        pkg_t big = {0};
        snprintf(big.title_id, sizeof(big.title_id), "CUSA00001");
        big.pkg_size = (4ull << 30) + 0x10000ull; /* 65537 blocks -> 8193 bitmap bytes, last byte 0x80 */
        buf_t b = {0};
        build_app_pbm(&b, &big, "", 0);
        CHECK(b.len == 0x100 + 8193 + 32);
        CHECK(b.data[0x100] == 0xff && b.data[0x100 + 4096] == 0xff && b.data[0x100 + 8191] == 0xff &&
              b.data[0x100 + 8192] == 0x80);
        buf_free(&b);
    }
    CHECK(is_game_id("CUSA00001") && is_game_id("SSBU00001") && is_game_id("PCSX20007"));
    CHECK(!is_game_id("NPXS00001") && !is_game_id("PPSA00001") && !is_game_id("cusa00001") && !is_game_id("CUSA0001") &&
          !is_game_id("CUSA000011") && !is_game_id("CUS100001") && !is_game_id("CUSA0000X") && !is_game_id(""));
    CHECK(game_id_at("SSBU00001.src") && !game_id_at("SSBU0001"));

    make_pkg(p, "UP0000-NPXS00001_00-TESTGAME00000000", "gd", 0, 1, 0x4000, 0x4000);
    CHECK(pkg_check(p, 0x13520001, &c) == 0 && !c.ok && strstr(c.problem, "not a game") && strstr(c.problem, "NPXS00001"));
    /* the same refusal in Spanish, with the same argument in it */
    g_lang = LANG_ES;
    CHECK(pkg_check(p, 0x13520001, &c) == 0 && !c.ok && strstr(c.problem, "no un juego") && strstr(c.problem, "NPXS00001"));
    lang_set(LANG_EN);

    make_pkg(p, cid, "gdc", 0, 1, 0x4000, 0x4000);
    CHECK(pkg_check(p, 0x13520001, &c) == 0 && !c.ok && c.kind == PKG_KIND_OTHER);

    /* cut before param.sfo */
    make_pkg(p, cid, "gd", 0, 1, 0x100000, 0x1800);
    CHECK(pkg_check(p, 0x13520001, &c) == 0 && !c.ok && strstr(c.problem, "param.sfo"));

    FILE *o = fopen(p, "wb");
    fputs("not a pkg at all", o);
    fclose(o);
    CHECK(pkg_check(p, 0x13520001, &c) == -1);

    char v[16];
    ver_text(0x13520001, v, sizeof(v));
    CHECK(!strcmp(v, "13.52"));
    ver_text(0x05050000, v, sizeof(v));
    CHECK(!strcmp(v, "5.05"));
    CHECK(fw_runs(0x09008000, 0x13520001) && !fw_runs(0x13500000, 0x13020001) && fw_runs(0x13500000, 0x13520001));

    unlink(p);
    rmdir(root);
}

static void test_origin(void) {
    printf("origin records and restored list\n");
    char root[] = "/tmp/ps4mount-test-XXXXXX", game[128], list[128], out[256], p[256];
    if (!mkdtemp(root)) {
        CHECK(0);
        return;
    }
    snprintf(game, sizeof(game), "%s/ps4", root);
    mkdir(game, 0777);
    snprintf(game, sizeof(game), "%s/ps4/CUSA00001", root);
    mkdir(game, 0777);

    CHECK(origin_find(game, "app.pkg", out, sizeof(out)) != 0); /* nothing recorded yet */
    CHECK(origin_add(game, "app.pkg", "games/My Game [v1.00].pkg") == 0);
    CHECK(origin_add(game, "patch.pkg", "Update 1.01.pkg") == 0);
    CHECK(origin_add(game, "addcont/CUSA00001/TESTADDON0000001/ac.pkg", "dlc/Pack.pkg") == 0);
    CHECK(origin_find(game, "app.pkg", out, sizeof(out)) == 0 && !strcmp(out, "games/My Game [v1.00].pkg"));
    CHECK(origin_find(game, "addcont/CUSA00001/TESTADDON0000001/ac.pkg", out, sizeof(out)) == 0 &&
          !strcmp(out, "dlc/Pack.pkg"));
    /* a newer update in the same place: the last record wins, the old one is kept aside */
    CHECK(origin_add(game, "replaced/patch-1.pkg", "Update 1.01.pkg") == 0);
    CHECK(origin_add(game, "patch.pkg", "Update 1.02.pkg") == 0);
    CHECK(origin_find(game, "patch.pkg", out, sizeof(out)) == 0 && !strcmp(out, "Update 1.02.pkg"));
    CHECK(origin_find(game, "replaced/patch-1.pkg", out, sizeof(out)) == 0 && !strcmp(out, "Update 1.01.pkg"));
    CHECK(origin_find(game, "patch", out, sizeof(out)) != 0); /* whole names only */
    CHECK(origin_add(game, "bad\tname", "x.pkg") != 0);

    /* free names in the original folder */
    snprintf(p, sizeof(p), "%s/Game.pkg", root);
    CHECK(origin_free_path(root, "Game.pkg", out, sizeof(out)) == 0 && !strcmp(out, p));
    touch(p);
    snprintf(p, sizeof(p), "%s/Game (1).pkg", root);
    CHECK(origin_free_path(root, "Game.pkg", out, sizeof(out)) == 0 && !strcmp(out, p));
    touch(p);
    snprintf(p, sizeof(p), "%s/Game (2).pkg", root);
    CHECK(origin_free_path(root, "Game.pkg", out, sizeof(out)) == 0 && !strcmp(out, p));
    snprintf(p, sizeof(p), "%s/v1.0/noext", root);
    CHECK(origin_free_path(root, "v1.0/noext", out, sizeof(out)) == 0 && !strcmp(out, p));

    /* restored pkgs are matched by path and size */
    snprintf(list, sizeof(list), "%s/restored.txt", root);
    CHECK(!restored_has(list, "Game.pkg", 100));
    CHECK(restored_add(list, "Game.pkg", 100) == 0);
    CHECK(restored_add(list, "games/Other.pkg", 5000000000ull) == 0);
    CHECK(restored_has(list, "Game.pkg", 100));
    CHECK(restored_has(list, "games/Other.pkg", 5000000000ull));
    CHECK(!restored_has(list, "Game.pkg", 101));    /* changed */
    CHECK(!restored_has(list, "Game2.pkg", 100));   /* renamed */
    CHECK(!restored_has(list, "x/Game.pkg", 100));  /* moved */

    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    CHECK(system(cmd) == 0);
}

static int ini_ours(const char *s) { return autorun_is_ours(s, strlen(s)); }

/* autorun_edit_ini(raw, on) must give want, byte for byte (raw NULL: empty). */
static void ini_result(const char *raw, const char *path, int on, const char *want) {
    buf_t b = {0};
    CHECK(autorun_edit_ini((const uint8_t *)raw, raw ? strlen(raw) : 0, path, on, &b) == 0);
    size_t wl = strlen(want);
    int same = b.len == wl && !memcmp(b.data ? b.data : (const uint8_t *)"", want, wl);
    CHECK(same);
    if (!same)
        printf("    wanted %zu bytes \"%s\", got %zu\n", wl, want, b.len);
    buf_free(&b);
}

static size_t slurp(const char *path, char *out, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = 0;
    return n;
}

static void test_autorun(void) {
    printf("autorun ini\n");
    const char *me = "/data/payloads/lodestar-ps4.bin";

    /* the matcher: our file, any path and case; not comments nor look-alikes. The legacy
     * ps4mount names count too: AutoRun replaces them rather than listing both. */
    CHECK(ini_ours("/data/payloads/lodestar-ps4.bin = 1"));
    CHECK(ini_ours("/data/GoldHEN/payloads/lodestar-ps4.elf=1"));
    CHECK(ini_ours("lodestar-ps4.bin"));
    CHECK(ini_ours("/x/LODESTAR-PS4.BIN\r"));
    CHECK(ini_ours("/data/payloads/ps4mount-ps4.bin = 1"));
    CHECK(ini_ours("/data/GoldHEN/payloads/ps4mount-ps4.elf=1"));
    CHECK(ini_ours("ps4mount-ps4.bin"));
    CHECK(ini_ours("/x/PS4MOUNT-PS4.BIN\r"));
    CHECK(ini_ours("/x/ps4mount-ps4.bin\t= 1"));
    CHECK(ini_ours("  ps4mount-ps4.bin = 1"));
    CHECK(!ini_ours("# /data/payloads/ps4mount-ps4.bin = 1"));
    CHECK(!ini_ours("   ; ps4mount-ps4.elf was here"));
    CHECK(!ini_ours("/x/myps4mount-ps4.bin = 1"));
    CHECK(!ini_ours("/x/ps4mount-ps4.binfoo = 1"));
    CHECK(!ini_ours("/x/ps4mount-ps4.bin.bak = 1"));
    CHECK(!ini_ours("/x/other.bin = 1"));
    CHECK(!ini_ours("backup = /x/ps4mount-ps4.bin"));
    CHECK(!ini_ours("/x/ps4mount-ps4.bin/other.bin = 1"));
    CHECK(!ini_ours("[AutoRun]"));

    /* on with nothing there: a new section; off: nothing */
    ini_result(NULL, me, 1, "[AutoRun]\n/data/payloads/lodestar-ps4.bin = 1\n");
    ini_result("", me, 1, "[AutoRun]\n/data/payloads/lodestar-ps4.bin = 1\n");
    ini_result(NULL, NULL, 0, "");
    ini_result("", NULL, 0, "");
    {
        buf_t b = {0};
        CHECK(autorun_edit_ini(NULL, 0, NULL, 1, &b) == -1);
        CHECK(autorun_edit_ini(NULL, 0, "", 1, &b) == -1);
        CHECK(b.len == 0);
        buf_free(&b);
    }

    /* an existing [AutoRun]: ours goes right under the header, the rest stays */
    ini_result("[AutoRun]\n/data/payloads/other.bin = 1\n", me, 1,
               "[AutoRun]\n/data/payloads/lodestar-ps4.bin = 1\n/data/payloads/other.bin = 1\n");

    /* [autorun] any case, spaces round it, CRLF endings */
    ini_result("  [AuToRun]  \r\n/data/other/x.bin = 1\r\n", me, 1,
               "  [AuToRun]  \r\n/data/payloads/lodestar-ps4.bin = 1\n/data/other/x.bin = 1\r\n");

    /* an older entry, any path, case or extension: replaced, not duplicated */
    ini_result("[AutoRun]\n/data/payloads/PS4Mount-PS4.bin = 1\n"
               "/data/GoldHEN/payloads/ps4mount-ps4.elf = 1\n"
               "/data/payloads/other.bin = 1\n",
               me, 1,
               "[AutoRun]\n/data/payloads/lodestar-ps4.bin = 1\n/data/payloads/other.bin = 1\n");

    /* other sections and comments stay verbatim; ours goes even from another section */
    ini_result("# mine\n[Other]\n/foo.bin = 1\n/old/ps4mount-ps4.bin = 1\n"
               "[AutoRun]\n# /data/ps4mount-ps4.bin = 1\n   ; ps4mount-ps4.elf\n/x.bin = 1\n",
               me, 1,
               "# mine\n[Other]\n/foo.bin = 1\n[AutoRun]\n/data/payloads/lodestar-ps4.bin = 1\n"
               "# /data/ps4mount-ps4.bin = 1\n   ; ps4mount-ps4.elf\n/x.bin = 1\n");

    /* off: ours goes, everything else stays */
    ini_result("[AutoRun]\n/data/payloads/ps4mount-ps4.bin = 1\n"
               "/data/GoldHEN/payloads/PS4MOUNT-PS4.ELF = 1\n"
               "/data/payloads/other.bin = 1\n[Other]\n# c\n",
               NULL, 0,
               "[AutoRun]\n/data/payloads/other.bin = 1\n[Other]\n# c\n");

    /* a last line without a line end stays without one; a bare header gets one */
    ini_result("[AutoRun]\n/x.bin = 1", NULL, 0, "[AutoRun]\n/x.bin = 1");
    ini_result("[AutoRun]", me, 1, "[AutoRun]\n/data/payloads/lodestar-ps4.bin = 1\n");
    ini_result("tail", me, 1, "tail\n\n[AutoRun]\n/data/payloads/lodestar-ps4.bin = 1\n");
    ini_result("[X]\n", me, 1, "[X]\n\n[AutoRun]\n/data/payloads/lodestar-ps4.bin = 1\n");

    /* applying it twice changes nothing the second time */
    {
        static const char *const raws[] = {"# c\n[Other]\n/o.bin = 1\n[AutoRun]\n/old/ps4mount-ps4.bin = 1\n/p.bin = 1\n",
                                           "/x.bin = 1\n"};
        for (size_t i = 0; i < sizeof(raws) / sizeof(raws[0]); i++) {
            buf_t a = {0}, b = {0};
            CHECK(autorun_edit_ini((const uint8_t *)raws[i], strlen(raws[i]), me, 1, &a) == 0);
            CHECK(autorun_edit_ini(a.data, a.len, me, 1, &b) == 0);
            CHECK(a.len == b.len && !memcmp(a.data, b.data, a.len));
            buf_free(&a);
            buf_free(&b);
        }
    }

    /* the file is replaced whole: tmp + rename, nothing left behind */
    {
        char root[] = "/tmp/ps4mount-autorun-XXXXXX", ini[256], tmp[256], got[64];
        if (!mkdtemp(root)) {
            CHECK(0);
            return;
        }
        snprintf(ini, sizeof(ini), "%s/payloads.ini", root);
        snprintf(tmp, sizeof(tmp), "%s/payloads.ini.tmp", root);
        CHECK(autorun_write_atomic(ini, "first\n", 6) == 0);
        CHECK(access(tmp, F_OK) != 0);
        CHECK(slurp(ini, got, sizeof(got)) == 6 && !memcmp(got, "first\n", 6));
        CHECK(autorun_write_atomic(ini, "second, longer\n", 15) == 0);
        CHECK(access(tmp, F_OK) != 0);
        CHECK(slurp(ini, got, sizeof(got)) == 15 && !memcmp(got, "second, longer\n", 15));
        CHECK(autorun_write_atomic("/nonexistent-ps4mount-dir/f.ini", "x", 1) == -1);
        char cmd[300];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
        CHECK(system(cmd) == 0);
    }
}

static void test_lang(void) {
    printf("languages\n");
    CHECK(lang_current() == LANG_EN && !strcmp(TR("Yes", "Sí"), "Yes"));
    lang_set(LANG_ES);
    CHECK(!strcmp(TR("Yes", "Sí"), "Sí"));
    CHECK(!strcmp(TR("Only English"), "Only English")); /* a language a text lacks: English */
    lang_set(LANG_COUNT);                            /* one with no column anywhere */
    CHECK(!strcmp(TR("Yes", "Sí"), "Yes"));
    g_lang = LANG_EN;
    CHECK(lang_from_console(3) == LANG_ES && lang_from_console(20) == LANG_ES);
    CHECK(lang_from_console(1) == LANG_EN && lang_from_console(7) == LANG_EN && lang_from_console(-1) == LANG_EN);
    CHECK(lang_from_tag("es") == LANG_ES && lang_from_tag("en") == LANG_EN && lang_from_tag("auto") == -1);
    CHECK(!strcmp(lang_tag(LANG_ES), "es") && !strcmp(lang_tag(-1), "en"));
}

/* STORE-1, STORE-3, STORE-4, STORE-5 (specs/storage.md): when PS4Mount may delete its own copy of a
 * pkg sent from the panel. A wrong row here deletes a file the system still needs, or leaves
 * gigabytes on the console's drive. */
static void test_stage(void) {
    printf("storage: when a staged copy may be deleted\n");
    static const struct {
        const char *name;
        long age;
        int receiving;
        stage_action_t want;
    } rows[] = {
        /* a whole pkg: left alone for 10 minutes, then deleted only if the system has the app, and
         * deleted whatever the system says once it is 3 days old */
        {"game.pkg", 0, 0, STAGE_KEEP},
        {"game.pkg", 599, 0, STAGE_KEEP},
        {"game.pkg", 600, 0, STAGE_VERIFY},
        {"game.pkg", 3 * 86400 - 1, 0, STAGE_VERIFY},
        {"game.pkg", 3 * 86400, 0, STAGE_DELETE_ABANDONED},
        {"game.pkg", 3 * 86400, 1, STAGE_DELETE_ABANDONED}, /* another pkg arriving does not protect it */
        {"game.pkg", -5, 0, STAGE_KEEP},                     /* the clock went back: never deleted */
        /* the part being received: only when nothing is being received, and not at once */
        {".ps4mount-upload.part", 7199, 0, STAGE_KEEP},
        {".ps4mount-upload.part", 7200, 0, STAGE_DELETE_PART},
        {".ps4mount-upload.part", 7200, 1, STAGE_KEEP},
        {"lodestar-panel.part", 100 * 86400, 1, STAGE_KEEP},
        {"lodestar-panel.part", 7200, 0, STAGE_DELETE_PART},
        {"ps4mount-panel.part", 7200, 0, STAGE_DELETE_PART},
        /* anything else in the folder is not PS4Mount's to delete */
        {"notes.txt", 100 * 86400, 0, STAGE_KEEP},
        {"game.pkg.bak", 100 * 86400, 0, STAGE_KEEP},
        {"other-tool.part", 100 * 86400, 0, STAGE_KEEP},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        stage_action_t got = stage_action(rows[i].name, rows[i].age, rows[i].receiving);
        if (got != rows[i].want)
            printf("  \"%s\" age %ld receiving %d: %d, wanted %d\n", rows[i].name, rows[i].age, rows[i].receiving, got,
                   rows[i].want);
        CHECK(got == rows[i].want);
    }

    /* STORE-3: every piece of positive install evidence is required; removing any one keeps the copy. */
    CHECK(stage_installed_ok(1, 1, 1, 1, 1234567, 1234567));
    CHECK(!stage_installed_ok(0, 1, 1, 1, 1234567, 1234567)); /* malformed title id */
    CHECK(!stage_installed_ok(1, 0, 1, 1, 1234567, 1234567)); /* mounted or absent, not an internal install */
    CHECK(!stage_installed_ok(1, 1, 0, 1, 1234567, 1234567)); /* no confirmed fresh database row */
    CHECK(!stage_installed_ok(1, 1, 1, 0, 1234567, 1234567)); /* app.pkg missing or not regular */
    CHECK(!stage_installed_ok(1, 1, 1, 1, 1234567, 1234568)); /* size mismatch */
    CHECK(!stage_installed_ok(1, 1, 1, 1, 0, 0));             /* zero bytes is not a whole app */

    char root[96], target[160];
    CHECK(stage_install_paths(PKG_KIND_GAME, "CUSA00001", "UP0000-CUSA00001_00-GAME000000000000",
                              root, sizeof(root), target, sizeof(target)) == 0);
    CHECK(!strcmp(root, "/user/app/CUSA00001") && !strcmp(target, "/user/app/CUSA00001/app.pkg"));
    CHECK(stage_install_paths(PKG_KIND_UPDATE, "CUSA00001", "UP0000-CUSA00001_00-GAME000000000000",
                              root, sizeof(root), target, sizeof(target)) == 0);
    CHECK(!strcmp(root, "/user/patch/CUSA00001") && !strcmp(target, "/user/patch/CUSA00001/patch.pkg"));
    CHECK(stage_install_paths(PKG_KIND_ADDON, "CUSA00001", "UP0000-CUSA00001_00-ADDON00000000000",
                              root, sizeof(root), target, sizeof(target)) == 0);
    CHECK(!strcmp(root, "/user/addcont/CUSA00001") &&
          !strcmp(target, "/user/addcont/CUSA00001/ADDON00000000000/ac.pkg"));
    CHECK(stage_install_paths(PKG_KIND_OTHER, "CUSA00001", "UP0000-CUSA00001_00-GAME000000000000",
                              root, sizeof(root), target, sizeof(target)));
    CHECK(stage_install_paths(PKG_KIND_GAME, "cusa00001", "UP0000-CUSA00001_00-GAME000000000000",
                              root, sizeof(root), target, sizeof(target)));
    CHECK(stage_install_paths(PKG_KIND_ADDON, "CUSA00002", "UP0000-CUSA00001_00-ADDON00000000000",
                              root, sizeof(root), target, sizeof(target))); /* content id is for another game */
    CHECK(stage_install_paths(PKG_KIND_ADDON, "CUSA00001", "UP0000-CUSA00001_00-ADDON0000000000",
                              root, sizeof(root), target, sizeof(target))); /* 35-byte content id */
    CHECK(stage_install_paths(PKG_KIND_ADDON, "CUSA00001", "UP0000-CUSA00001_00-ADDON00/0000000",
                              root, sizeof(root), target, sizeof(target))); /* slash is not path evidence */
}


/* DEL-1 (specs/delete.md): the console-delete path may remove only the exact organized
 * library folder of the game. Anything else is user data or foreign data. */
static void test_delete_target(void) {
    printf("delete: which folder a console delete may remove\n");
    static const struct {
        const char *root, *tid, *dir;
        int ok;
    } rows[] = {
        {"/mnt/usb0", "CUSA00001", "/mnt/usb0/ps4/CUSA00001", 1},
        {"/mnt/ext0", "SLUS20071", "/mnt/ext0/ps4/SLUS20071", 1},
        {"/mnt/usb0", "CUSA00001", "/mnt/usb0/ps4/CUSA00001/", 0},
        {"/mnt/usb0", "CUSA00001", "/mnt/usb0/ps4/cusa00001", 0},
        {"/mnt/usb0", "CUSA00001", "/mnt/usb0/Games/CUSA00001/app.pkg", 0},
        {"/mnt/usb0", "CUSA00001", "/mnt/usb0/ps4/CUSA00002", 0},
        {"/mnt/usb1", "CUSA00001", "/mnt/usb0/ps4/CUSA00001", 0},
        {"/mnt/usb0", "CUSA0001", "/mnt/usb0/ps4/CUSA0001", 0},
        {"/mnt/usb0", "NPXS00001", "/mnt/usb0/ps4/NPXS00001", 0},
        {"/mnt/usb0", "../..", "/mnt/usb0/ps4/../..", 0},
        {NULL, "CUSA00001", "/mnt/usb0/ps4/CUSA00001", 0},
        {"/mnt/usb0", NULL, "/mnt/usb0/ps4/CUSA00001", 0},
        {"/mnt/usb0", "CUSA00001", NULL, 0},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        int got = delete_target_ok(rows[i].root, rows[i].tid, rows[i].dir);
        if (got != rows[i].ok)
            printf("  %s %s %s: %d, wanted %d\n", rows[i].root, rows[i].tid, rows[i].dir, got, rows[i].ok);
        CHECK(got == rows[i].ok);
    }
}

/* DEL-3 (specs/delete.md): the two PS4 delete flows open and keep the delete window;
 * unrelated screens close it. */
static void test_delete_scene(void) {
    printf("delete: which scenes open or keep the delete window\n");
    static const struct {
        const char *scene;
        int kind;
    } rows[] = {
        {"ConfirmContentDeleteScene", 2},
        {"storage_data#application", 2},
        {"storage_data#application_delete", 2}, /* the delete page contains the list page's prefix */
        {"Parent_ContentDeleteScene", 1},
        {"storage management", 1},
        {"OptionMenu : Delete", 1},
        {"Parent_NormalStack", 1},
        {"AppDeleteConfirmScene", 2}, /* inherited PS5 names: harmless, not PS4 evidence */
        {"DeleteDialogScreen", 2},
        {"ReactPopupMenu", 1},
        {"game scene CUSA00001", 0},
        {"main scene", 0},
        {"PSMT00001", 0},
        {"", 0},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        int got = delete_scene_kind(rows[i].scene);
        if (got != rows[i].kind)
            printf("  scene %s: %d, wanted %d\n", rows[i].scene, got, rows[i].kind);
        CHECK(got == rows[i].kind);
    }
}




/* DEL-4 (specs/delete.md): window state decides whether an uninstall-looking kernel event is
 * a user delete. The table includes the ordering cases that previously caused data loss. */
static void test_delete_window(void) {
    printf("delete: when kernel uninstall events are user deletes\n");
    static const struct {
        int kind, open, want;
    } trans[] = {
        {2, 0, 2}, {2, 1, 2}, /* confirmed screens open or refresh the window */
        {1, 1, 1},            /* dialogs keep it open */
        {1, 0, 0},            /* a dialog alone does not open it */
        {0, 1, -1},           /* an unrelated scene closes it */
        {0, 0, 0},
    };
    for (size_t i = 0; i < sizeof(trans) / sizeof(trans[0]); i++) {
        int got = delete_window_transition(trans[i].kind, trans[i].open);
        if (got != trans[i].want)
            printf("  kind %d open %d: %d, wanted %d\n", trans[i].kind, trans[i].open, got, trans[i].want);
        CHECK(got == trans[i].want);
    }
    CHECK(delete_user_event_ok(1, 1));
    CHECK(!delete_user_event_ok(0, 1));
    CHECK(!delete_user_event_ok(1, 0));
    CHECK(!delete_user_event_ok(0, 0));
    CHECK(delete_playgo_event_kind(1, 1, 0) == 2);
    CHECK(delete_playgo_event_kind(1, 1, 1) == 2); /* the base content id is not treated as an add-on */
    CHECK(delete_playgo_event_kind(1, 0, 1) == 1);
    CHECK(delete_playgo_event_kind(1, 0, 0) == 0);
    CHECK(delete_playgo_event_kind(0, 1, 1) == 0); /* panel/system uninstall work is not a user delete */
    CHECK(delete_content_id_plain("UP0000-CUSA00001_00-TESTGAME00000000"));
    CHECK(!delete_content_id_plain("UP0000-CUSA00001_00-../GAME"));
    CHECK(!delete_content_id_plain("UP0000/CUSA00001"));
    CHECK(!delete_content_id_plain(""));

    /* DEL-6: intent waits; only explicit success or a confirmed registered-to-absent
     * transition commits. Failure and age can only roll generated pending state back. */
    static const struct {
        int report, db, seen;
        long since_report, since_intent, want;
    } pend[] = {
        {-1, 0, 0, 0, 0, 0},  /* no evidence yet */
        {-1, 1, 0, 0, 0, 0},  /* still registered is not failure proof */
        {-1, 2, 0, 0, 0, 0},  /* absence without a seen transition may be pre-existing */
        {-1, 2, 1, 0, 0, 1},  /* normal successful PS4 delete writes no result line */
        {0, 0, 0, 0, 0, 1},   /* explicit zero result is positive commit evidence */
        {0, 1, 0, 0, 0, 1},
        {1, 0, 0, 0, 0, 0},   /* let the database settle after a nonzero result */
        {1, 0, 0, DELETE_RESULT_SETTLE_SEC - 1, 0, 0},
        {1, 0, 0, DELETE_RESULT_SETTLE_SEC, 0, -1},
        {1, 2, 1, DELETE_RESULT_SETTLE_SEC, 0, 1}, /* confirmed transition wins over a confusing rc */
        {1, 2, 0, DELETE_RESULT_SETTLE_SEC, 0, -1},
        {-1, 0, 0, 0, DELETE_PENDING_MAX_SEC - 1, 0},
        {-1, 0, 0, 0, DELETE_PENDING_MAX_SEC, -1}, /* expiry is rollback only */
        {-1, 1, 1, 0, DELETE_PENDING_MAX_SEC, -1},
    };
    for (size_t i = 0; i < sizeof(pend) / sizeof(pend[0]); i++) {
        int got = delete_pending_decision(pend[i].report, pend[i].db, pend[i].seen, pend[i].since_report, pend[i].since_intent);
        if (got != pend[i].want)
            printf("  pending row %zu: %d, wanted %ld\n", i, got, pend[i].want);
        CHECK(got == pend[i].want);
    }
}

/* DEL-5 (specs/delete.md): a valid title id is not ownership. The panel may remove only
 * a title with PS4Mount evidence, and a real internal install always wins over stale state. */
static void test_remove_admission(void) {
    printf("delete: panel removal ownership\n");
    CHECK(remove_owned_ok(0, 1, 0, 0));
    CHECK(remove_owned_ok(0, 0, 1, 0));
    CHECK(remove_owned_ok(0, 0, 0, 1));
    CHECK(remove_owned_ok(0, 1, 1, 1));
    CHECK(!remove_owned_ok(0, 0, 0, 0));
    CHECK(!remove_owned_ok(1, 1, 1, 1));
    CHECK(!remove_owned_ok(1, 0, 0, 0));
    CHECK(remove_detached_ok(0, 0, 0));
    CHECK(!remove_detached_ok(1, 0, 0));
    CHECK(!remove_detached_ok(0, 1, 0));
    CHECK(!remove_detached_ok(0, 0, 1));
}

/* P4N-13 (specs/ps4-native.md): the BGFT parser is strict about the external PDB frame and
 * extracts only the canonical launch metadata used by scans. */
static void test_bgft_info(void) {
    printf("BGFT game task parser\n");
    pkg_t app;
    fake_pkg(&app, "UP0000-CUSA00001_00-TESTGAME00000000", 271384576);
    const char *url = "/mnt/usb0/ps4/CUSA00001/app.pkg";
    buf_t json = {0}, b = {0};
    build_app_json(&json, &app, url);
    bgft_task_t t = {0, 0x13520001, 0x00e31e2713ddac1eull, 0xf092, &app, "app.pkg", url, &json, NULL};
    build_bgft_task(&b, &t);
    bgft_info_t info;
    CHECK(bgft_task_parse(b.data, b.len, &info) == 0);
    CHECK(info.records == 41);
    CHECK(info.has_title && !strcmp(info.title_id, "CUSA00001"));
    CHECK(info.has_url && info.url_len == strlen(url) && !memcmp(info.url, url, info.url_len));
    CHECK(info.error_present && info.error == 0);
    CHECK(bgft_info_url_is(&info, url));
    CHECK(!bgft_info_url_is(&info, "/mnt/usb0/ps4/CUSA00001/other.pkg"));
    bgft_info_t longer = info;
    longer.url_len = strlen(url) + 1; /* a trailing NUL is a different length-delimited value */
    CHECK(!bgft_info_url_is(&longer, url));
    char legacy_tid[10] = "";
    CHECK(bgft_legacy_title_id(b.data, b.len, legacy_tid));
    CHECK(!strcmp(legacy_tid, "CUSA00001"));

    uint8_t *bad = mem_alloc(b.len);
    CHECK(bad != NULL);
    memcpy(bad, b.data, b.len);
    bad[0] = 1;
    CHECK(bgft_task_parse(bad, b.len, &info) == -1);
    memcpy(bad, b.data, b.len);
    bad[8] ^= 1; /* first length no longer equals the repeated length */
    CHECK(bgft_task_parse(bad, b.len, &info) == -1);
    memcpy(bad, b.data, b.len);
    CHECK(bgft_task_parse(bad, b.len - 1, &info) == -1);
    mem_free(bad);

    /* Canonical keys present with the wrong width are malformed, not absent. */
    static const uint8_t bad_error_width[] = {
        0, 0, 0, 0,
        0x68, 0, 0, 0, 3, 0, 0, 0, 3, 0, 0, 0,
        1, 2, 3,
    };
    CHECK(bgft_task_parse(bad_error_width, sizeof(bad_error_width), &info) == -1);
    static const uint8_t bad_title_width[] = {
        0, 0, 0, 0,
        0x7c, 0, 0, 0, 8, 0, 0, 0, 8, 0, 0, 0,
        'C', 'U', 'S', 'A', '0', '0', '0', '1',
    };
    CHECK(bgft_task_parse(bad_title_width, sizeof(bad_title_width), &info) == -1);
    buf_free(&json);
    buf_free(&b);
}


static void write_bin(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f) {
        CHECK(fwrite(data, 1, len, f) == len);
        CHECK(fclose(f) == 0);
    }
}

static void test_dlcvec(void) {
    printf("registration add-on array growth\n");
    CHECK(dlcvec_next_cap(0, 0, 64) == 10); /* ten 1600-byte descriptors still fit one page */
    CHECK(dlcvec_next_cap(9, 10, 64) == 10);
    CHECK(dlcvec_next_cap(10, 10, 64) == 20);
    CHECK(dlcvec_next_cap(20, 20, 64) == 40);
    CHECK(dlcvec_next_cap(40, 40, 64) == 64);
    CHECK(dlcvec_next_cap(64, 64, 64) == -1);
    CHECK(dlcvec_next_cap(3, 0, 4) == 4); /* a smaller caller cap is never crossed */
}

/* P4N-13 integration (specs/ps4-native.md): one directory load answers title, URL and error
 * queries for every task, including a malformed legacy task, without rereading PDB files. */
static void test_bgft_scan(void) {
    printf("BGFT scan snapshot\n");
    char root[] = "/tmp/ps4mount-bgftscan-XXXXXX";
    if (!mkdtemp(root)) {
        CHECK(0);
        return;
    }
    pkg_t app, app2, app3;
    fake_pkg(&app, "UP0000-CUSA00001_00-TESTGAME00000000", 271384576);
    fake_pkg(&app2, "UP0000-CUSA00002_00-TESTGAME00000000", 271384576);
    fake_pkg(&app3, "UP0000-CUSA00004_00-TESTGAME00000000", 271384576);
    const char *url = "/mnt/usb0/ps4/CUSA00001/app.pkg";
    buf_t json = {0}, one = {0}, two = {0}, three = {0};
    build_app_json(&json, &app, url);
    bgft_task_t t = {0, 0x13520001, 1, 0xf000, &app, "app.pkg", url, &json, NULL};
    bgft_task_t t2 = {0, 0x13520001, 1, 0xf002, &app2, "app.pkg", url, &json, NULL};
    bgft_task_t t3 = {0, 0x13520001, 1, 0xf004, &app3, "app.pkg", url, &json, NULL};
    build_bgft_task(&one, &t);
    build_bgft_task(&two, &t2);
    build_bgft_task(&three, &t3);
    /* put a nonzero error in the second task's canonical error record, walking the PDB
     * frame rather than relying on where its key bytes happen to occur */
    int patched = 0;
    for (size_t off = 4; off + 12 <= two.len;) {
        uint32_t key = le32(two.data + off), n = le32(two.data + off + 4);
        if (n != le32(two.data + off + 8) || n > two.len - off - 12)
            break;
        if (key == 0x68 && n == 4) {
            two.data[off + 12] = 0x19;
            patched = 1;
            break;
        }
        off += 12 + n;
    }
    CHECK(patched);
    char dir[256], file[320];
    snprintf(dir, sizeof(dir), "%s/0000f001", root);
    CHECK(mkdir(dir, 0777) == 0);
    snprintf(file, sizeof(file), "%s/d0.pdb", dir);
    write_bin(file, one.data, one.len);
    snprintf(dir, sizeof(dir), "%s/0000f003", root);
    CHECK(mkdir(dir, 0777) == 0);
    snprintf(file, sizeof(file), "%s/d0.pdb", dir);
    write_bin(file, two.data, two.len);
    const char legacy[] = "xx/user/app/CUSA00003/app.pkg yy";
    snprintf(dir, sizeof(dir), "%s/0000f005", root);
    CHECK(mkdir(dir, 0777) == 0);
    snprintf(file, sizeof(file), "%s/d0.pdb", dir);
    write_bin(file, legacy, sizeof(legacy) - 1);
    snprintf(dir, sizeof(dir), "%s/0000f000", root); /* install task id: outside the game range */
    CHECK(mkdir(dir, 0777) == 0);
    bgftscan_t scan;
    bgftscan_init(&scan);
    bgftscan_load(&scan, root, 0xf001, 0xffff);
    CHECK(scan.usable);
    CHECK(scan.count == 3);
    /* occupancy is physical directory evidence, not successful parsing: the install id
     * 0xf000 has no d0.pdb here but still makes its pair unavailable */
    CHECK(bgftscan_id_used(&scan, 0xf000));
    CHECK(bgftscan_id_used(&scan, 0xf001));
    CHECK(!bgftscan_id_used(&scan, 0xf002));
    /* BGFT-4: installer ids taken after the snapshot are occupied even before their task
     * directories exist. 0xf07b therefore must not be handed to a game. */
    bgft_taken_t taken = {0};
    CHECK(bgftscan_alloc_id(&scan, &taken, root, 0xf001, 0xffff) == 0xf007);
    for (uint32_t id = 0xf078; id <= 0xf07c; id++)
        bgft_taken_add(&taken, id);
    CHECK(bgftscan_alloc_id(&scan, &taken, root, 0xf001, 0xffff) == 0xf07f);
    scan.usable = 0; /* same answer from direct directory evidence plus reservations */
    CHECK(bgftscan_alloc_id(&scan, &taken, root, 0xf001, 0xffff) == 0xf07f);
    scan.usable = 1;
    const bgftscan_entry_t *e = bgftscan_find(&scan, "CUSA00001");
    CHECK(e && e->id == 0xf001 && !e->malformed);
    CHECK(bgftscan_names(e, url));
    CHECK(!bgftscan_names(e, "/mnt/usb0/ps4/CUSA00001/other.pkg"));
    CHECK(e && e->error == 0);
    e = bgftscan_find(&scan, "CUSA00002");
    CHECK(e && e->id == 0xf003 && e->error == 0x19);
    e = bgftscan_find(&scan, "CUSA00003");
    CHECK(e && e->malformed && e->error != 0);
    CHECK(!bgftscan_names(e, "/user/app/CUSA00003/app.pkg"));
    CHECK(!bgftscan_find(&scan, "CUSA00009"));
    /* BGFT-1/2: bytes written during the epoch are indexed synchronously, and removal by
     * task id drops only that task. */
    CHECK(bgftscan_update(&scan, 0xf007, one.data, one.len));
    CHECK(scan.count == 4);
    CHECK(bgftscan_id_used(&scan, 0xf007));
    e = bgftscan_find(&scan, "CUSA00001");
    CHECK(e && e->id == 0xf001); /* the first task with that title remains first */
    bgftscan_remove_id(&scan, 0xf007);
    CHECK(scan.count == 3);
    CHECK(!bgftscan_id_used(&scan, 0xf007));
    CHECK(bgftscan_update(&scan, 0xf001, three.data, three.len));
    e = bgftscan_find(&scan, "CUSA00004");
    CHECK(e && e->id == 0xf001 && !e->malformed);
    CHECK(!bgftscan_find(&scan, "CUSA00001"));
    bgftscan_free(&scan);
    CHECK(scan.entries == NULL && scan.count == 0 && !scan.usable);

    /* enough extra valid tasks to cross the scanner's initial allocation growth boundary */
    for (int i = 0; i < 20; i++) {
        snprintf(dir, sizeof(dir), "%s/0000f1%02x", root, i * 2 + 1);
        CHECK(mkdir(dir, 0777) == 0);
        snprintf(file, sizeof(file), "%s/d0.pdb", dir);
        write_bin(file, one.data, one.len);
    }
    bgftscan_load(&scan, root, 0xf001, 0xffff);
    CHECK(scan.usable);
    CHECK(scan.count == 23);
    CHECK(scan.capacity >= scan.count);
    bgftscan_free(&scan);

    char cmd[384];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    CHECK(system(cmd) == 0);
    buf_free(&json);
    buf_free(&one);
    buf_free(&two);
    buf_free(&three);
}

/* PANEL-4 (specs/panel.md): the unauthenticated panel's write boundary is the exact set of
 * local IPv4 networks. These rows include every mask boundary. */
static void test_web_net(void) {
    printf("panel: which network peers may write\n");
#define IP(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
    static const struct {
        uint32_t addr;
        int ok;
    } rows[] = {
        {IP(9, 255, 255, 255), 0}, {IP(10, 0, 0, 0), 1}, {IP(10, 255, 255, 255), 1}, {IP(11, 0, 0, 0), 0},
        {IP(126, 255, 255, 255), 0}, {IP(127, 0, 0, 1), 1}, {IP(128, 0, 0, 0), 0},
        {IP(172, 15, 255, 255), 0}, {IP(172, 16, 0, 0), 1}, {IP(172, 31, 255, 255), 1}, {IP(172, 32, 0, 0), 0},
        {IP(192, 167, 255, 255), 0}, {IP(192, 168, 0, 1), 1}, {IP(192, 168, 255, 255), 1}, {IP(192, 169, 0, 0), 0},
        {IP(169, 253, 255, 255), 0}, {IP(169, 254, 0, 1), 1}, {IP(169, 255, 0, 0), 0},
        {IP(100, 63, 255, 255), 0}, {IP(100, 64, 0, 0), 1}, {IP(100, 127, 255, 255), 1}, {IP(100, 128, 0, 0), 0},
        {IP(8, 8, 8, 8), 0}, {IP(0, 0, 0, 0), 0}, {IP(255, 255, 255, 255), 0},
    };
#undef IP
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        int got = web_net_ok(rows[i].addr);
        if (got != rows[i].ok)
            printf("  0x%08x: %d, wanted %d\n", rows[i].addr, got, rows[i].ok);
        CHECK(got == rows[i].ok);
    }
}


/* MNT-1, MNT-2, MNT-3, MNT-4 (specs/mount.md): what proves a nullfs layer is PS4Mount's,
 * and when an observed stack may be replaced, detached or must be left alone. */
static void test_mountplan(void) {
    printf("mount ownership and fail-closed mount decisions\n");
    char tid[10];
    CHECK(mount_target_kind("/user/app/CUSA00001", tid) == MOUNT_TARGET_APP && !strcmp(tid, "CUSA00001"));
    CHECK(mount_target_kind("/user/patch/CUSA00001", tid) == MOUNT_TARGET_PATCH);
    CHECK(mount_target_kind("/user/addcont/CUSA00001", tid) == MOUNT_TARGET_ADDCONT);
    CHECK(mount_target_kind("/user/app/CUSA00001/extra", NULL) == MOUNT_TARGET_NONE);
    CHECK(mount_target_kind("/user/app/cusa00001", NULL) == MOUNT_TARGET_NONE);
    CHECK(mount_target_kind("/data/PCSX2/games", NULL) == MOUNT_TARGET_NONE);

    const char *target = "/user/app/CUSA00001";
    const char *expected = "/system_data/ps4mount/app/CUSA00001";
    CHECK(mount_layer_owner(target, expected, "nullfs", expected, NULL) == MOUNT_OWNER_EXACT);
    CHECK(mount_layer_owner(target, expected, "nullfs", "/mnt/usb0/ps4/CUSA00001", "/mnt/usb0/ps4/CUSA00001") ==
          MOUNT_OWNER_LEGACY);
    CHECK(mount_layer_owner("/user/patch/CUSA00001", expected, "nullfs", "/mnt/usb0/ps4/CUSA00001",
                            "/mnt/usb0/ps4/CUSA00001") == MOUNT_OWNER_LEGACY);
    CHECK(mount_layer_owner("/user/addcont/CUSA00001", expected, "nullfs", "/mnt/usb0/ps4/CUSA00001",
                            "/mnt/usb0/ps4/CUSA00001") == MOUNT_OWNER_FOREIGN);
    CHECK(mount_layer_owner(target, expected, "nullfs", "/mnt/usb0/ps4/CUSA00001", NULL) == MOUNT_OWNER_FOREIGN);
    CHECK(mount_layer_owner(target, expected, "nullfs", "/mnt/usb0/ps4/CUSA00001", "/mnt/usb0/ps4/CUSA00002") ==
          MOUNT_OWNER_FOREIGN);
    CHECK(mount_layer_owner(target, expected, "nullfs", "/system_data/ps4mount/app/CUSA00002", NULL) ==
          MOUNT_OWNER_FOREIGN);
    CHECK(mount_layer_owner(target, expected, "exfat", expected, NULL) == MOUNT_OWNER_FOREIGN);
    CHECK(mount_layer_owner("/user/app/CUSA00001/extra", expected, "nullfs", expected, NULL) == MOUNT_OWNER_FOREIGN);
    CHECK(!mount_owner_owned(MOUNT_OWNER_FOREIGN));
    CHECK(mount_owner_owned(MOUNT_OWNER_EXACT));
    CHECK(mount_owner_owned(MOUNT_OWNER_LEGACY));

    static const struct {
        int table, layers, top, exact, owned, all, prepare, detach;
    } rows[] = {
        /* no observation: never mutate */
        {0, 0, 0, 0, 0, 0, MOUNT_PREPARE_UNKNOWN, MOUNT_STEP_UNKNOWN},
        {0, 1, 1, 1, 1, 1, MOUNT_PREPARE_UNKNOWN, MOUNT_STEP_UNKNOWN},
        /* empty table */
        {1, 0, 0, 0, 0, 1, MOUNT_PREPARE_MOUNT, MOUNT_STEP_DONE},
        /* a live top missing from the table is inconsistency, not absence */
        {1, 0, 1, 0, 1, 1, MOUNT_PREPARE_UNKNOWN, MOUNT_STEP_UNKNOWN},
        {1, 0, 1, 1, 1, 1, MOUNT_PREPARE_UNKNOWN, MOUNT_STEP_UNKNOWN},
        /* exact live top is already mounted */
        {1, 1, 1, 1, 1, 1, MOUNT_PREPARE_ALREADY, MOUNT_STEP_UNMOUNT},
        /* live owned non-exact top may be replaced or detached */
        {1, 1, 1, 0, 1, 1, MOUNT_PREPARE_UNMOUNT, MOUNT_STEP_UNMOUNT},
        /* live foreign top blocks even when lower layers are owned */
        {1, 2, 1, 0, 0, 0, MOUNT_PREPARE_REFUSE, MOUNT_STEP_REFUSE},
        /* a foreign lower layer makes replacement impossible: routine mount refuses without
         * disturbing the owned top, while delete still strips owned exposure first */
        {1, 2, 1, 0, 1, 0, MOUNT_PREPARE_REFUSE, MOUNT_STEP_UNMOUNT},
        /* invisible top: safe only when every visible layer is owned */
        {1, 1, 0, 0, 0, 1, MOUNT_PREPARE_UNMOUNT, MOUNT_STEP_UNMOUNT},
        {1, 2, 0, 0, 0, 0, MOUNT_PREPARE_REFUSE, MOUNT_STEP_REFUSE},
        /* exact beneath a foreign live top is not reachable and does not count as mounted */
        {1, 2, 1, 0, 0, 0, MOUNT_PREPARE_REFUSE, MOUNT_STEP_REFUSE},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        int prepare = mount_prepare_step(rows[i].table, rows[i].layers, rows[i].top, rows[i].exact, rows[i].owned,
                                         rows[i].all);
        int detach = mount_detach_step(rows[i].table, rows[i].layers, rows[i].top, rows[i].owned, rows[i].all);
        if (prepare != rows[i].prepare || detach != rows[i].detach)
            printf("  row %zu: prepare %d wanted %d, detach %d wanted %d\n", i, prepare, rows[i].prepare, detach,
                   rows[i].detach);
        CHECK(prepare == rows[i].prepare);
        CHECK(detach == rows[i].detach);
    }
}


/* DEL-7 (specs/delete.md): .removed filenames are state input, and only a valid marker's
 * own embedded title id may decide which DLC retention it clears. */
static void test_retention_marks(void) {
    printf("retention: which .removed markers belong to a title\n");
    char tid[10];
    CHECK(retention_marker("CUSA00001.removed", tid) == RETENTION_MARK_BASE && !strcmp(tid, "CUSA00001"));
    CHECK(retention_marker("UP0000-CUSA00001_00-ADDON00000000000.removed", tid) == RETENTION_MARK_DLC &&
          !strcmp(tid, "CUSA00001"));
    CHECK(retention_marker("EP0001-CUSA00002_00-ADDON00000000000.removed", tid) == RETENTION_MARK_DLC &&
          !strcmp(tid, "CUSA00002"));
    CHECK(retention_clear_dlc_for_tid("UP0000-CUSA00001_00-ADDON00000000000.removed", "CUSA00001"));
    CHECK(!retention_clear_dlc_for_tid("UP0000-CUSA00001_00-ADDON00000000000.removed", "CUSA00002"));
    CHECK(!retention_clear_dlc_for_tid("CUSA00001.removed", "CUSA00001")); /* base markers are not DLC */
    CHECK(!retention_clear_dlc_for_tid("UP0000-CUSA00001_00-ADDON000/00000.removed", "CUSA00001"));
    static const char *bad[] = {
        "cusa00001.removed", "CUSA00001.removedx", "xCUSA00001.removed", "CUSA00001.remove",
        "UP0000-CUSA00001_00-ADDON0000000000.removed",  /* 35-byte content id */
        "UP0000-CUSA00001_00-ADDON000000000000.removed", /* 37-byte content id */
        "UP0000-CUSA00001_00-ADDON00000000..removed",
        "UP0000-CUSA00001_00-ADDON000/00000.removed",
        "UP0000-cusa00001_00-ADDON0000000000.removed",
        "", ".removed",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (retention_marker(bad[i], tid) != RETENTION_MARK_NONE)
            printf("  bad marker accepted: %s\n", bad[i]);
        CHECK(retention_marker(bad[i], tid) == RETENTION_MARK_NONE);
    }
}


/* REST-2/3/4 (specs/restore.md): state cleanup follows the per-title uninstall commit, and
 * origin.txt paths are external input that must stay relative to the drive root. */
static void test_restore_plan(void) {
    printf("restore: commit ordering and original path admission\n");
    CHECK(!restore_clear_state_ok(0, 0));  /* never unregistered */
    CHECK(!restore_clear_state_ok(0, 1));  /* failed before the commit boundary */
    CHECK(restore_clear_state_ok(1, 0));   /* unregistered, every move that existed succeeded */
    CHECK(!restore_clear_state_ok(1, 1));  /* unregistered but a pkg could not move: stay suppressed */
    static const char *good[] = {
        "Game.pkg", "games/My Game [v1.00].pkg", "a/.hidden/file.pkg", "d1/d2/d3/update.pkg",
    };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++)
        CHECK(restore_orig_path_ok(good[i]));
    static const char *bad[] = {
        "", "/Game.pkg", "\\Game.pkg", "a\\Game.pkg", "../Game.pkg", "a/../Game.pkg",
        "./Game.pkg", "a/./Game.pkg", ".", "..", "a//Game.pkg", "a/", "/",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (restore_orig_path_ok(bad[i]))
            printf("  unsafe restore path accepted: %s\n", bad[i]);
        CHECK(!restore_orig_path_ok(bad[i]));
    }
}

static void test_emu(void) {
    printf("emulators: which names are mounted over the console's folders\n");
    CHECK(emu_mount_step(1, 1, 1, 0, 0, 0, 0) == EMU_STEP_ALREADY);
    CHECK(emu_mount_step(1, 1, 0, 1, 0, 0, 0) == EMU_STEP_MKDIR_MOUNT);
    CHECK(emu_mount_step(1, 1, 0, 0, 0, 1, 1) == EMU_STEP_MOUNT);
    CHECK(emu_mount_step(1, 1, 0, 0, 0, 1, 0) == EMU_STEP_REFUSE); /* console files are never hidden */
    CHECK(emu_mount_step(1, 1, 0, 0, 0, 0, 0) == EMU_STEP_REFUSE); /* not a real folder */
    CHECK(emu_mount_step(1, 1, -1, 0, 0, 1, 1) == EMU_STEP_REFUSE); /* foreign mount */
    CHECK(emu_mount_step(1, 1, 0, 0, 1, 0, 0) == EMU_STEP_REFUSE); /* unknown stat error is not absence */
    CHECK(emu_mount_step(1, 0, 0, 1, 0, 0, 0) == EMU_STEP_REFUSE); /* start the app once first */
    CHECK(emu_mount_step(0, 1, 0, 1, 0, 0, 0) == EMU_STEP_REFUSE);

    CHECK(emu_mount_owned("/mnt/usb0", "/mnt/usb0/PS4Mount/emulators/PCSX2/games", "/data/PCSX2/games"));
    CHECK(emu_mount_owned("/mnt/ext1", "/mnt/ext1/PS4Mount/emulators/retro/bios", "/data/retro/bios"));
    CHECK(!emu_mount_owned("/mnt/usb0", "/mnt/usb0/PS4Mount/emulators/PCSX2/games", "/data/PCSX2/roms"));
    CHECK(!emu_mount_owned("/mnt/usb0", "/mnt/usb0x/PS4Mount/emulators/PCSX2/games", "/data/PCSX2/games"));
    CHECK(!emu_mount_owned("/mnt/usb0", "/mnt/usb0/PS4Mount/emulators/PCSX2/games/extra", "/data/PCSX2/games"));
    CHECK(!emu_mount_owned("/mnt/usb0", "/mnt/usb0/PS4Mount/emulators/PCSX2/games", "/user/data/PCSX2/games"));

    CHECK(emu_stale_unmount_ok(0, 0, 1));  /* source gone, single layer */
    CHECK(emu_stale_unmount_ok(0, 1, 1));  /* disconnected/dead layer */
    CHECK(!emu_stale_unmount_ok(1, 0, 1)); /* live source */
    CHECK(!emu_stale_unmount_ok(0, 0, 0)); /* table cannot prove the layer count */
    CHECK(!emu_stale_unmount_ok(0, 0, 2)); /* another layer is above or below */
    static const struct {
        const char *name;
        int ok;
    } names[] = {
        {"PS4SX2", 1}, {"roms", 1}, {"my roms_2.0-b", 1}, {"a", 1},
        {"", 0}, {".", 0}, {"..", 0}, {".hidden", 0},
        {"a/b", 0}, {"../x", 0}, {"/etc", 0}, {"a\\b", 0}, {"a\nb", 0}, {"caf\xc3\xa9", 0},
        {"123456789012345678901234567890123456789012345678", 1},  /* 48 */
        {"1234567890123456789012345678901234567890123456789", 0}, /* 49 */
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (emu_name_ok(names[i].name) != names[i].ok)
            printf("  name \"%s\" wanted %d\n", names[i].name, names[i].ok);
        CHECK(emu_name_ok(names[i].name) == names[i].ok);
    }
    char out[128];
    CHECK(emu_target("PS4SX2", "roms", out, sizeof(out)) == 0 && !strcmp(out, "/data/PS4SX2/roms"));
    CHECK(emu_target("PS4SX2", "..", out, sizeof(out)) == -1);
    CHECK(emu_target("..", "roms", out, sizeof(out)) == -1);
    /* the folders PS4Mount and the system use, in any case */
    const char *reserved[] = {"PS4Mount", "ps4mount", "PS4", "pkg", "GoldHEN", "goldhen", "payloads", "PAYLOADS"};
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++)
        CHECK(emu_target(reserved[i], "roms", out, sizeof(out)) == -1);
    CHECK(emu_target("PS4Mounts", "roms", out, sizeof(out)) == 0); /* only the exact names */
    char tiny[16];
    CHECK(emu_target("PS4SX2", "roms", tiny, sizeof(tiny)) == -1); /* does not fit: not cut */
    /* the folder the games searches leave alone, and nothing else */
    CHECK(emu_is_dir("/mnt/usb0/PS4Mount/emulators"));
    CHECK(emu_is_dir("/mnt/usb0/ps4mount/Emulators"));
    CHECK(!emu_is_dir("/mnt/usb0/PS4Mount/emulators2"));
    CHECK(!emu_is_dir("/mnt/usb0/PS4Mount/Games/emulators"));
    CHECK(!emu_is_dir("PS4Mount/emulators/PS4SX2"));
    CHECK(!emu_is_dir(""));
}


/* WEBAPI-4 (specs/webapi.md): scan requests arriving during a scan survive end-of-scan
 * scheduling, and manual requests are consumed exactly once unless the scan never ran. */
static void test_scanreq(void) {
    printf("scan requests: cross-thread intent ordering\n");
    CHECK(scanreq_period(0) == SCANREQ_IDLE_SECONDS);
    CHECK(scanreq_period(1) == SCANREQ_GAMEPLAY_SECONDS);
    CHECK(SCANREQ_IDLE_SECONDS == 10 && SCANREQ_GAMEPLAY_SECONDS == 120); /* P4N-20 */
    scanreq_t q;
    scanreq_init(&q);
    CHECK(scanreq_due(&q, 0)); /* the first scan is due at start */
    int manual = -1, hard = -1;
    unsigned seq = scanreq_begin(&q, &manual, &hard);
    CHECK(manual == 0 && hard == 0);
    scanreq_finish(&q, seq, 10, 10);
    CHECK(!scanreq_due(&q, 19) && scanreq_due(&q, 20));

    scanreq_init(&q);
    seq = scanreq_begin(&q, &manual, &hard);
    scanreq_finish(&q, seq, 10, scanreq_period(1));
    CHECK(!scanreq_due(&q, 129) && scanreq_due(&q, 130)); /* P4N-20: gameplay cadence */

    scanreq_request(&q, 1);
    CHECK(scanreq_due(&q, 1));
    seq = scanreq_begin(&q, &manual, &hard);
    CHECK(manual == 1);
    seq = scanreq_begin(&q, &manual, &hard);
    CHECK(manual == 0); /* consumed by one epoch only */

    scanreq_init(&q);
    scanreq_request(&q, 1);
    seq = scanreq_begin(&q, &manual, &hard);
    scanreq_request(&q, 0); /* an upload finishes while the scan is still walking */
    scanreq_finish(&q, seq, 30, 10);
    CHECK(scanreq_due(&q, 30)); /* not overwritten by routine scheduling */

    scanreq_init(&q);
    scanreq_request(&q, 1);
    seq = scanreq_begin(&q, &manual, &hard);
    scanreq_keep_request(&q, manual, hard); /* app.db unreadable: the scan never ran */
    scanreq_finish(&q, seq, 40, 10);
    CHECK(!scanreq_due(&q, 41)); /* no immediate retry loop */
    seq = scanreq_begin(&q, &manual, &hard);
    CHECK(manual == 1); /* but the next scheduled scan is still fresh/manual */

    scanreq_init(&q);
    scanreq_request_hard(&q);
    CHECK(scanreq_due(&q, 1));
    seq = scanreq_begin(&q, &manual, &hard);
    CHECK(manual == 1 && hard == 1); /* owner invalidates inside this epoch */
    seq = scanreq_begin(&q, &manual, &hard);
    CHECK(manual == 0 && hard == 0); /* consumed once */

    scanreq_init(&q);
    scanreq_request_hard(&q);
    seq = scanreq_begin(&q, &manual, &hard);
    scanreq_keep_request(&q, manual, hard); /* app.db unreadable: the epoch never ran */
    scanreq_finish(&q, seq, 50, 10);
    seq = scanreq_begin(&q, &manual, &hard);
    CHECK(manual == 1 && hard == 1); /* hard invalidation is not lost either */
}

int main(void) {
    test_crypto();
    test_app_json();
    test_pbm();
    test_ac_pdb();
    test_bgft_task();
    test_bgft_info();
    test_dlcvec();
    test_bgft_scan();
    test_pkg_reader();
    test_check();
    test_lang();
    test_license();
    test_sfo();
    test_pkg_meta();
    test_origin();
    test_autorun();
    test_stage();
    test_delete_target();
    test_delete_scene();
    test_delete_window();
    test_remove_admission();
    test_web_net();
    test_mountplan();
    test_retention_marks();
    test_restore_plan();
    test_emu();
    test_scanreq();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
