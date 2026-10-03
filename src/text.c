#define _GNU_SOURCE
#include "text.h"

#include <errno.h>
#include <iconv.h>
#include <langinfo.h>
#include <limits.h>
#include <locale.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *terminal_encoding = "UTF-8";
static const char *filename_encoding = "UTF-8";

const char *text_encoding(const char *name)
{
    if (!name) return NULL;
    if (!strcasecmp(name, "UTF-8") || !strcasecmp(name, "UTF8")) return "UTF-8";
    if (!strcasecmp(name, "EUC-JP") || !strcasecmp(name, "EUCJP") ||
        !strcasecmp(name, "EUC")) return "EUC-JP";
    if (!strcasecmp(name, "SJIS") || !strcasecmp(name, "SHIFT_JIS") ||
        !strcasecmp(name, "SHIFT-JIS") || !strcasecmp(name, "CP932")) return "CP932";
    if (!strcasecmp(name, "ASCII") || !strcasecmp(name, "ANSI_X3.4-1968")) return "ASCII";
    return NULL;
}

int text_convert(const char *input, const char *from, const char *to, char **out)
{
    *out = NULL;
    size_t left = strlen(input);
    if (left > (SIZE_MAX - 1) / 4) return -1;
    size_t room = left * 4 + 1;
    char *buf = malloc(room);
    if (!buf) return -1;
    iconv_t cd = iconv_open(to, from);
    if (cd == (iconv_t)-1) { free(buf); return -1; }
    char *src = (char *)input, *dst = buf;
    room--;
    size_t rc = iconv(cd, &src, &left, &dst, &room);
    if (rc != 0 || iconv(cd, NULL, NULL, &dst, &room) == (size_t)-1) {
        iconv_close(cd);
        free(buf);
        return -1;
    }
    *dst = '\0';
    iconv_close(cd);
    *out = buf;
    return 0;
}

int text_decode(const char *input, const char *encoding, char **out,
                char *err, size_t errlen)
{
    *out = NULL;
    if (encoding && strcasecmp(encoding, "auto")) {
        const char *enc = text_encoding(encoding);
        if (enc && text_convert(input, enc, "UTF-8", out) == 0) return 0;
        snprintf(err, errlen, "文字コード%sとして読めません（不正な文字または未対応の文字コード）", encoding);
        return -1;
    }
    /* ASCII is identical in all supported encodings; valid UTF-8 takes priority. */
    if (text_convert(input, "UTF-8", "UTF-8", out) == 0) return 0;
    char *euc = NULL, *sjis = NULL;
    int a = text_convert(input, "EUC-JP", "UTF-8", &euc);
    int b = text_convert(input, "CP932", "UTF-8", &sjis);
    if (a == 0 && b == 0 && strcmp(euc, sjis)) {
        free(euc); free(sjis);
        snprintf(err, errlen, "文字コードを判別できません。--config-encoding EUC-JP または SJIS を指定してください");
        return -1;
    }
    if (a == 0) { free(sjis); *out = euc; return 0; }
    if (b == 0) { *out = sjis; return 0; }
    snprintf(err, errlen, "UTF-8 / EUC-JP / SJIS の有効な文字列ではありません");
    return -1;
}

int text_init(char *err, size_t errlen)
{
    /* Keep numeric/diagnostic libc output in C: CSV must always use decimal dots. */
    const char *loc = setlocale(LC_CTYPE, "");
    const char *detected = loc ? text_encoding(nl_langinfo(CODESET)) : NULL;
    terminal_encoding = detected && strcmp(detected, "ASCII") ? detected : "UTF-8";
    filename_encoding = terminal_encoding;
    const char *env = getenv("BCS_TERMINAL_ENCODING");
    if (env && !(terminal_encoding = text_encoding(env))) {
        snprintf(err, errlen, "Invalid BCS_TERMINAL_ENCODING: use UTF-8, EUC-JP, SJIS or ASCII");
        terminal_encoding = "UTF-8";
        return -1;
    }
    env = getenv("BCS_FILENAME_ENCODING");
    if (env && !(filename_encoding = text_encoding(env))) {
        snprintf(err, errlen, "Invalid BCS_FILENAME_ENCODING: use UTF-8, EUC-JP or SJIS");
        filename_encoding = "UTF-8";
        return -1;
    }
    const char *encodings[] = {terminal_encoding, filename_encoding};
    for (size_t i = 0; i < sizeof(encodings) / sizeof(encodings[0]); i++) {
        iconv_t to = iconv_open(encodings[i], "UTF-8");
        iconv_t from = iconv_open("UTF-8", encodings[i]);
        int valid = to != (iconv_t)-1 && from != (iconv_t)-1;
        if (to != (iconv_t)-1) iconv_close(to);
        if (from != (iconv_t)-1) iconv_close(from);
        if (!valid) {
            snprintf(err, errlen, "Character conversion unavailable: %s; install the glibc iconv modules", encodings[i]);
            return -1;
        }
    }
    return 0;
}

int text_input(const char *input, char **out, char *err, size_t errlen)
{
    return text_decode(input, terminal_encoding, out, err, errlen);
}

int text_native_path(const char *utf8, char *out, size_t size)
{
    char *native;
    if (text_convert(utf8, "UTF-8", filename_encoding, &native) < 0) return -1;
    size_t n = strlen(native);
    if (n < size) memcpy(out, native, n + 1);
    free(native);
    return n < size ? 0 : -1;
}

const char *text_display_path(const char *path)
{
    int saved_errno = errno;
    /* A small ring allows several path arguments in the same diagnostic. */
    static char buffers[4][PATH_MAX * 4];
    static unsigned next;
    char *decoded;
    if (text_convert(path, filename_encoding, "UTF-8", &decoded) < 0) {
        errno = saved_errno;
        return path;
    }
    char *out = buffers[next++ % 4];
    snprintf(out, sizeof(buffers[0]), "%s", decoded);
    free(decoded);
    errno = saved_errno;
    return out;
}

static int ui_vfprintf(FILE *fp, const char *fmt, va_list ap)
{
    char *utf8;
    int len = vasprintf(&utf8, fmt, ap);
    if (len < 0) return -1;
    iconv_t cd = iconv_open(terminal_encoding, "UTF-8");
    if (cd == (iconv_t)-1) { free(utf8); return -1; }
    char *src = utf8;
    size_t left = (size_t)len;
    int failed = 0;
    while (left) {
        char buf[1024], *dst = buf;
        size_t room = sizeof(buf);
        size_t rc = iconv(cd, &src, &left, &dst, &room);
        int e = errno;
        size_t n = sizeof(buf) - room;
        if (fwrite(buf, 1, n, fp) != n) { failed = 1; break; }
        if (rc != (size_t)-1) continue;
        if (e == E2BIG) continue;
        /* Never emit malformed target bytes or silently drop unrepresentable text. */
        if (fprintf(fp, "\\x%02X", (unsigned char)*src) < 0) { failed = 1; break; }
        src++; left--;
    }
    iconv_close(cd);
    free(utf8);
    return failed ? -1 : len;
}

int ui_fprintf(FILE *fp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int rc = ui_vfprintf(fp, fmt, ap);
    va_end(ap);
    return rc;
}

int ui_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int rc = ui_vfprintf(stdout, fmt, ap);
    va_end(ap);
    return rc;
}
