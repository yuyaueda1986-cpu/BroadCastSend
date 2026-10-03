#define _GNU_SOURCE
#include "payload.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include "text.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* hexファイルは空白・改行を含むため、データ上限より大きい読込み上限を設ける。 */
#define HEX_FILE_MAX (16u * 1024u * 1024u)

static int read_file(const char *path, size_t limit, uint8_t **buf_out, size_t *len_out,
                     int *too_large, char *err, size_t errlen)
{
    *too_large = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        snprintf(err, errlen, "%s: 開けません: %s", text_display_path(path), strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        snprintf(err, errlen, "%s: 通常ファイルではありません", text_display_path(path));
        close(fd);
        return -1;
    }
    /* 上限+1バイトまで読み、上限超過を検出する。 */
    uint8_t *buf = malloc(limit + 1);
    if (buf == NULL) {
        snprintf(err, errlen, "%s: メモリー不足", text_display_path(path));
        close(fd);
        return -1;
    }
    size_t len = 0;
    while (len < limit + 1) {
        ssize_t n = read(fd, buf + len, limit + 1 - len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            snprintf(err, errlen, "%s: 読込みエラー: %s", text_display_path(path), strerror(errno));
            free(buf);
            close(fd);
            return -1;
        }
        if (n == 0)
            break;
        len += (size_t)n;
    }
    close(fd);
    if (len > limit)
        *too_large = 1;
    *buf_out = buf;
    *len_out = len;
    return 0;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

int payload_decode_hex(const char *path, const char *text, size_t textlen, payload_t *out,
                       char *err, size_t errlen)
{
    uint8_t *data = malloc(BCS_PAYLOAD_MAX);
    if (data == NULL) {
        snprintf(err, errlen, "%s: メモリー不足", text_display_path(path));
        return -1;
    }
    size_t n = 0;
    int line = 1, col = 0;
    int hi = -1, hi_line = 0, hi_col = 0;

    /* A text-file BOM is metadata, never part of a hex payload. Binary stays exact. */
    if (textlen >= 3 && !memcmp(text, "\xef\xbb\xbf", 3)) {
        text += 3;
        textlen -= 3;
    }

    for (size_t i = 0; i < textlen; i++) {
        char c = text[i];
        col++;
        if (is_space(c)) {
            if (hi >= 0) {
                snprintf(err, errlen, "%s:%d:%d: 不完全なバイトです（16進数1桁）", text_display_path(path), hi_line, hi_col);
                goto fail;
            }
            if (c == '\n') {
                line++;
                col = 0;
            }
            continue;
        }
        int v = hexval(c);
        if (v < 0) {
            if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7f)
                snprintf(err, errlen, "%s:%d:%d: 16進数ではない文字です: '%c'", text_display_path(path), line, col, c);
            else
                snprintf(err, errlen, "%s:%d:%d: 16進数ではない文字です: 0x%02X", text_display_path(path), line, col,
                         (unsigned char)c);
            goto fail;
        }
        if (hi < 0) {
            hi = v;
            hi_line = line;
            hi_col = col;
            continue;
        }
        if (n >= BCS_PAYLOAD_MAX) {
            snprintf(err, errlen, "%s:%d:%d: データが最大%uバイトを超えています", text_display_path(path), line, col,
                     BCS_PAYLOAD_MAX);
            goto fail;
        }
        data[n++] = (uint8_t)((hi << 4) | v);
        hi = -1;
    }
    if (hi >= 0) {
        snprintf(err, errlen, "%s:%d:%d: 不完全なバイトです（16進数1桁）", text_display_path(path), hi_line, hi_col);
        goto fail;
    }
    if (n == 0) {
        snprintf(err, errlen, "%s: データが空です（1バイト以上必要）", text_display_path(path));
        goto fail;
    }
    out->data = data;
    out->len = n;
    return 0;
fail:
    free(data);
    return -1;
}

int payload_load(const char *path, payload_format_t format, payload_t *out, char *err, size_t errlen)
{
    out->data = NULL;
    out->len = 0;
    if (format != PAYLOAD_BINARY && format != PAYLOAD_HEX) {
        snprintf(err, errlen, "ファイルのペイロード形式はbinaryまたはhexを指定してください");
        return -1;
    }
    uint8_t *buf;
    size_t len;
    int too_large;

    if (format == PAYLOAD_BINARY) {
        if (read_file(path, BCS_PAYLOAD_MAX, &buf, &len, &too_large, err, errlen) < 0)
            return -1;
        if (too_large) {
            snprintf(err, errlen, "%s: データが最大%uバイトを超えています", text_display_path(path), BCS_PAYLOAD_MAX);
            free(buf);
            return -1;
        }
        if (len == 0) {
            snprintf(err, errlen, "%s: データが空です（1バイト以上必要）", text_display_path(path));
            free(buf);
            return -1;
        }
        out->data = buf;
        out->len = len;
        return 0;
    }

    if (read_file(path, HEX_FILE_MAX, &buf, &len, &too_large, err, errlen) < 0)
        return -1;
    if (too_large) {
        snprintf(err, errlen, "%s: hexファイルが大きすぎます（最大%uバイト）", text_display_path(path), HEX_FILE_MAX);
        free(buf);
        return -1;
    }
    int rc = payload_decode_hex(path, (const char *)buf, len, out, err, errlen);
    free(buf);
    return rc;
}

int payload_prepare(const config_t *cfg, payload_t *out, char *err, size_t errlen)
{
    if (cfg->payload_format != PAYLOAD_ZERO)
        return payload_load(cfg->payload_path, cfg->payload_format, out, err, errlen);

    out->data = NULL;
    out->len = 0;
    if (cfg->payload_size < 1 || cfg->payload_size > BCS_PAYLOAD_MAX) {
        snprintf(err, errlen, "0データのsizeは1〜%uバイトを指定してください", BCS_PAYLOAD_MAX);
        return -1;
    }
    out->data = calloc(cfg->payload_size, 1);
    if (!out->data) {
        snprintf(err, errlen, "0データの生成: メモリー不足");
        return -1;
    }
    out->len = cfg->payload_size;
    return 0;
}

void payload_free(payload_t *p)
{
    free(p->data);
    p->data = NULL;
    p->len = 0;
}
