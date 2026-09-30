#define _GNU_SOURCE
#include <arpa/inet.h>

#include "config.h"
#include "testutil.h"

static const char VALID[] =
    "# コメント\n"
    "; セミコロンのコメント\n"
    "\n"
    "[network]\n"
    "interface = eth0\n"
    "  source_ip   =   192.168.10.10  \n"
    "source_port = 0\n"
    "broadcast_ip = 192.168.10.255\n"
    "destination_port = 50000\n"
    "\n"
    "[payload]\n"
    "format = hex\n"
    "file = payload.hex\n"
    "\n"
    "[send]\n"
    "period_us = 100\n"
    "packets_per_cycle = 8\n"
    "duration_sec = 60\n"
    "max_attempts = 0\n"
    "\n"
    "[stats]\n"
    "interval_sec = 1\n"
    "file = /abs/sender.csv\n";

static char path[512];
static char err[1024];
static config_t cfg;

static int load_text(const char *text)
{
    write_text("test.ini", text, path, sizeof(path));
    err[0] = '\0';
    return config_load(path, &cfg, err, sizeof(err));
}

/* VALIDの1行をreplaceで置き換えたテキストを読み込む */
static int load_replaced(const char *line, const char *replace)
{
    static char buf[8192];
    const char *p = strstr(VALID, line);
    if (p == NULL) {
        fprintf(stderr, "test bug: '%s' not in VALID\n", line);
        exit(2);
    }
    snprintf(buf, sizeof(buf), "%.*s%s%s", (int)(p - VALID), VALID, replace, p + strlen(line));
    return load_text(buf);
}

static void test_valid(void)
{
    CHECK_EQ(load_text(VALID), 0);
    CHECK(strcmp(cfg.interface, "eth0") == 0);
    CHECK_EQ(cfg.source_ip.s_addr, inet_addr("192.168.10.10"));
    CHECK_EQ(cfg.broadcast_ip.s_addr, inet_addr("192.168.10.255"));
    CHECK_EQ(cfg.source_port, 0);
    CHECK_EQ(cfg.destination_port, 50000);
    CHECK_EQ(cfg.payload_format, PAYLOAD_HEX);
    CHECK_EQ(cfg.period_us, 100);
    CHECK_EQ(cfg.packets_per_cycle, 8);
    CHECK_EQ(cfg.duration_sec, 60);
    CHECK_EQ(cfg.max_attempts, 0);
    CHECK_EQ(cfg.stats_interval_sec, 1);
    /* 相対パスはコンフィグのディレクトリ基準、絶対パスはそのまま */
    char expect[512];
    snprintf(expect, sizeof(expect), "%s/payload.hex", tmpdir);
    CHECK(strcmp(cfg.payload_path, expect) == 0);
    CHECK(strcmp(cfg.stats_path, "/abs/sender.csv") == 0);
}

static void test_optional_defaults(void)
{
    const char *minimal =
        "[network]\ninterface=eth0\nsource_ip=10.0.0.1\nsource_port=0\n"
        "broadcast_ip=10.0.0.255\ndestination_port=1\n"
        "[payload]\nformat=binary\nfile=p.bin\n"
        "[send]\nperiod_us=100\npackets_per_cycle=1\n";
    CHECK_EQ(load_text(minimal), 0);
    CHECK_EQ(cfg.duration_sec, 0);
    CHECK_EQ(cfg.max_attempts, 0);
    CHECK_EQ(cfg.stats_interval_sec, 1);
    CHECK_EQ(cfg.stats_path[0], '\0');
    CHECK_EQ(cfg.payload_format, PAYLOAD_BINARY);
}

static void expect_error(const char *line, const char *replace, const char *msg_part)
{
    CHECK_EQ(load_replaced(line, replace), -1);
    CHECK_CONTAINS(err, msg_part);
}

static void test_errors(void)
{
    /* 行番号付きのエラー */
    expect_error("period_us = 100\n", "period_us = 99\n", "test.ini:16:");
    expect_error("period_us = 100\n", "period_us = 99\n", "period_us");
    expect_error("period_us = 100\n", "period_us = 3600000001\n", "period_us");
    expect_error("period_us = 100\n", "period_us = -100\n", "period_us");
    expect_error("period_us = 100\n", "period_us = 1e3\n", "period_us");
    expect_error("period_us = 100\n", "period_us = 100 # comment\n", "period_us");
    expect_error("period_us = 100\n", "period_us = 99999999999999999999999\n", "period_us");
    expect_error("packets_per_cycle = 8\n", "packets_per_cycle = 0\n", "packets_per_cycle");
    expect_error("destination_port = 50000\n", "destination_port = 0\n", "destination_port");
    expect_error("destination_port = 50000\n", "destination_port = 65536\n", "destination_port");
    expect_error("source_port = 0\n", "source_port = 70000\n", "source_port");
    expect_error("  source_ip   =   192.168.10.10  \n", "source_ip = 192.168.10\n", "source_ip");
    expect_error("broadcast_ip = 192.168.10.255\n", "broadcast_ip = host\n", "broadcast_ip");
    expect_error("format = hex\n", "format = text\n", "format");
    expect_error("interface = eth0\n", "interface = 0123456789abcdef\n", "interface");
    expect_error("interval_sec = 1\n", "interval_sec = 86401\n", "interval_sec");
    /* 未定義キー・重複キー・空値・構文 */
    expect_error("max_attempts = 0\n", "max_attempts = 0\nunknown = 1\n", "未定義のキー");
    expect_error("max_attempts = 0\n", "max_attempts = 0\nperiod_us = 200\n", "重複");
    expect_error("max_attempts = 0\n", "max_attempts =\n", "値が空");
    expect_error("max_attempts = 0\n", "max_attempts 0\n", "形式ではありません");
    expect_error("[stats]\n", "[statistics]\n", "未定義のセクション");
    expect_error("[stats]\n", "[stats\n", "セクション行");
    /* セクションが違えば同名のキーは別物（stats.fileとpayload.file）。別セクションのキーは未定義扱い */
    expect_error("max_attempts = 0\n", "max_attempts = 0\nformat = hex\n", "未定義のキー");
    /* 必須項目の欠落 */
    expect_error("broadcast_ip = 192.168.10.255\n", "", "必須項目がありません: [network] broadcast_ip");
    expect_error("period_us = 100\n", "", "必須項目がありません: [send] period_us");
}

static void test_key_before_section(void)
{
    CHECK_EQ(load_text("interface = eth0\n[network]\n"), -1);
    CHECK_CONTAINS(err, "セクションの前");
}

static void test_nul_in_line(void)
{
    static const char data[] = "[network]\ninterface = et\0h0\n";
    write_file("nul.ini", data, sizeof(data) - 1, path, sizeof(path));
    CHECK_EQ(config_load(path, &cfg, err, sizeof(err)), -1);
    CHECK_CONTAINS(err, "NUL");
}

static void test_crlf(void)
{
    char buf[8192];
    size_t j = 0;
    for (const char *p = VALID; *p && j < sizeof(buf) - 2; p++) {
        if (*p == '\n')
            buf[j++] = '\r';
        buf[j++] = *p;
    }
    buf[j] = '\0';
    CHECK_EQ(load_text(buf), 0);
    CHECK_EQ(cfg.period_us, 100);
}

static void test_missing_file(void)
{
    CHECK_EQ(config_load("/nonexistent/x.ini", &cfg, err, sizeof(err)), -1);
    CHECK_CONTAINS(err, "開けません");
}

static void test_resolve_relative(void)
{
    char out[256];
    CHECK_EQ(resolve_relative("dir/sub/a.ini", "p.hex", out, sizeof(out)), 0);
    CHECK(strcmp(out, "dir/sub/p.hex") == 0);
    CHECK_EQ(resolve_relative("a.ini", "p.hex", out, sizeof(out)), 0);
    CHECK(strcmp(out, "p.hex") == 0);
    CHECK_EQ(resolve_relative("dir/a.ini", "/x/p.hex", out, sizeof(out)), 0);
    CHECK(strcmp(out, "/x/p.hex") == 0);
    CHECK_EQ(resolve_relative("dir/a.ini", "p.hex", out, 5), -1);
}

int main(void)
{
    make_tmpdir();
    test_valid();
    test_optional_defaults();
    test_errors();
    test_key_before_section();
    test_nul_in_line();
    test_crlf();
    test_missing_file();
    test_resolve_relative();
    remove_tmpdir();
    if (failures) {
        fprintf(stderr, "test_config: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_config: OK\n");
    return 0;
}
