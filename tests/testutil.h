#ifndef BCS_TESTUTIL_H
#define BCS_TESTUTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

#define CHECK_EQ(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "%s:%d: %s == %lld, expected %s == %lld\n", __FILE__, __LINE__, #a, _a, #b, _b); \
        failures++; \
    } \
} while (0)

#define CHECK_CONTAINS(s, sub) do { \
    if (strstr((s), (sub)) == NULL) { \
        fprintf(stderr, "%s:%d: '%s' does not contain '%s'\n", __FILE__, __LINE__, (s), (sub)); \
        failures++; \
    } \
} while (0)

static char tmpdir[256];

static void make_tmpdir(void)
{
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/bcs_test_XXXXXX");
    const char *base = getenv("TMPDIR");
    if (base != NULL)
        snprintf(tmpdir, sizeof(tmpdir), "%s/bcs_test_XXXXXX", base);
    if (mkdtemp(tmpdir) == NULL) {
        perror("mkdtemp");
        exit(2);
    }
}

static void remove_tmpdir(void)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
    if (system(cmd) != 0)
        fprintf(stderr, "warning: failed to remove %s\n", tmpdir);
}

/* tmpdir/name にlen バイトを書き込み、パスをpathに返す */
static void write_file(const char *name, const void *data, size_t len, char *path, size_t pathlen)
{
    snprintf(path, pathlen, "%s/%s", tmpdir, name);
    FILE *fp = fopen(path, "wb");
    if (fp == NULL || fwrite(data, 1, len, fp) != len) {
        perror(path);
        exit(2);
    }
    fclose(fp);
}

static void write_text(const char *name, const char *text, char *path, size_t pathlen)
{
    write_file(name, text, strlen(text), path, pathlen);
}

#endif
