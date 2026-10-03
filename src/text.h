#ifndef BCS_TEXT_H
#define BCS_TEXT_H

#include <stddef.h>
#include <stdio.h>

/* Source and internal messages are UTF-8. Only I/O boundaries convert text. */
int text_init(char *err, size_t errlen);
const char *text_encoding(const char *name);
int text_convert(const char *input, const char *from, const char *to, char **out);
int text_decode(const char *input, const char *encoding, char **out,
                char *err, size_t errlen);
int text_input(const char *input, char **out, char *err, size_t errlen);
int text_native_path(const char *utf8, char *out, size_t size);
const char *text_display_path(const char *path);
int ui_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int ui_fprintf(FILE *fp, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif
