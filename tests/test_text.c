#define _GNU_SOURCE
#include "config.h"
#include "text.h"
#include "testutil.h"

static void test_encodings(void)
{
    char err[1024], *out = NULL;
    /* Known bytes, independent of the conversion function under test. */
    const char *utf8 = "日本語";
    const char euc[] = "\xc6\xfc\xcb\xdc\xb8\xec";
    const char sjis[] = "\x93\xfa\x96\x7b\x8c\xea";
    CHECK_EQ(text_decode(utf8, "auto", &out, err, sizeof(err)), 0);
    CHECK(out && !strcmp(out, utf8)); free(out);
    CHECK_EQ(text_decode(euc, "EUC", &out, err, sizeof(err)), 0);
    CHECK(out && !strcmp(out, utf8)); free(out);
    CHECK_EQ(text_decode(sjis, "SJIS", &out, err, sizeof(err)), 0);
    CHECK(out && !strcmp(out, utf8)); free(out);
    CHECK_EQ(text_decode(sjis, "auto", &out, err, sizeof(err)), 0);
    CHECK(out && !strcmp(out, utf8)); free(out);
    CHECK_EQ(text_decode("\xa4\xa2", "auto", &out, err, sizeof(err)), -1);
    CHECK_CONTAINS(err, "--config-encoding");
    CHECK_EQ(text_decode("\x8f", "auto", &out, err, sizeof(err)), -1);
    CHECK_EQ(text_decode("\xc0\xaf", "UTF-8", &out, err, sizeof(err)), -1);
    CHECK_EQ(text_decode("\xed\xa0\x80", "UTF-8", &out, err, sizeof(err)), -1);

    const char *encodings[] = {"UTF-8", "EUC-JP", "SJIS"};
    const char *expected[] = {utf8, euc, sjis};
    for (size_t i = 0; i < 3; i++) {
        setenv("BCS_TERMINAL_ENCODING", encodings[i], 1);
        CHECK_EQ(text_init(err, sizeof(err)), 0);
        FILE *fp = tmpfile();
        CHECK(fp != NULL);
        if (!fp) continue;
        CHECK(ui_fprintf(fp, "%s", utf8) > 0);
        rewind(fp);
        char buf[128] = {0};
        size_t len = fread(buf, 1, sizeof(buf), fp);
        CHECK_EQ(len, strlen(expected[i]));
        CHECK(!memcmp(buf, expected[i], len));
        fclose(fp);
        CHECK_EQ(text_input(expected[i], &out, err, sizeof(err)), 0);
        CHECK(out && !strcmp(out, utf8)); free(out);
    }
    unsetenv("BCS_TERMINAL_ENCODING");
    CHECK_EQ(text_init(err, sizeof(err)), 0);
}

static void test_partial_and_bom(void)
{
    char path[512], err[1024];
    config_t cfg;
    write_text("partial.ini", "\xef\xbb\xbf[network]\r\ninterface = \r\nsource_port = 0\r\n", path, sizeof(path));
    CHECK_EQ(config_load_partial(path, "auto", &cfg, err, sizeof(err)), 0);
    CHECK(!CONFIG_HAS(&cfg, K_INTERFACE));
    CHECK(CONFIG_HAS(&cfg, K_SOURCE_PORT));
    CHECK_EQ(config_require(&cfg, path, err, sizeof(err)), -1);
    CHECK_EQ(config_load_partial("/no/such/config.ini", "auto", &cfg, err, sizeof(err)), -2);
    write_text("dup.ini", "[network]\ninterface=\ninterface=eth0\n", path, sizeof(path));
    CHECK_EQ(config_load_partial(path, "auto", &cfg, err, sizeof(err)), -1);
    CHECK_CONTAINS(err, "重複");
    write_text("encoded.ini", "# \xc6\xfc\xcb\xdc\xb8\xec\n[payload]\nfile=\x93\xfa\x96\x7b\x8c\xea.hex\n", path, sizeof(path));
    CHECK_EQ(config_load_partial(path, "auto", &cfg, err, sizeof(err)), 0);
    CHECK_CONTAINS(cfg.payload_path, "日本語.hex");
}

int main(void)
{
    make_tmpdir();
    test_encodings();
    test_partial_and_bom();
    remove_tmpdir();
    if (failures) return 1;
    puts("test_text: OK");
    return 0;
}
