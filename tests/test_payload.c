#define _GNU_SOURCE
#include "payload.h"
#include "testutil.h"

static char path[512];
static char err[1024];

static void test_binary_with_nul(void)
{
    static const uint8_t data[] = { 0x00, 0x01, 0xff, 0x00, 0x0a, 0x0d, 0x00 };
    write_file("p.bin", data, sizeof(data), path, sizeof(path));
    payload_t p;
    CHECK_EQ(payload_load(path, PAYLOAD_BINARY, &p, err, sizeof(err)), 0);
    CHECK_EQ(p.len, sizeof(data));
    CHECK(memcmp(p.data, data, sizeof(data)) == 0);
    payload_free(&p);
}

static void test_hex_matches_binary(void)
{
    static const uint8_t data[] = { 0x00, 0x01, 0xff, 0xab, 0xcd, 0xef, 0x10 };
    write_text("p.hex", "00 01 FF\nab\tCD  ef\r\n10\n", path, sizeof(path));
    payload_t p;
    CHECK_EQ(payload_load(path, PAYLOAD_HEX, &p, err, sizeof(err)), 0);
    CHECK_EQ(p.len, sizeof(data));
    CHECK(memcmp(p.data, data, sizeof(data)) == 0);
    payload_free(&p);

    /* 区切りなしの連続表記も受け付ける */
    write_text("p2.hex", "0001FFabCDef10", path, sizeof(path));
    CHECK_EQ(payload_load(path, PAYLOAD_HEX, &p, err, sizeof(err)), 0);
    CHECK_EQ(p.len, sizeof(data));
    CHECK(memcmp(p.data, data, sizeof(data)) == 0);
    payload_free(&p);
}

static void expect_hex_error(const char *text, const char *msg_part)
{
    payload_t p;
    write_text("bad.hex", text, path, sizeof(path));
    CHECK_EQ(payload_load(path, PAYLOAD_HEX, &p, err, sizeof(err)), -1);
    CHECK_CONTAINS(err, msg_part);
}

static void test_hex_errors(void)
{
    expect_hex_error("01 0g", "bad.hex:1:5: 16進数ではない文字です: 'g'");
    expect_hex_error("01\n2 34", "bad.hex:2:1: 不完全なバイト");
    expect_hex_error("01 02 3", "bad.hex:1:7: 不完全なバイト");
    expect_hex_error("0x01", "16進数ではない文字");
    expect_hex_error("01,02", "','");
    expect_hex_error("# comment\n01", "'#'");
    expect_hex_error("", "データが空");
    expect_hex_error(" \n\t\n", "データが空");
}

static void test_empty_binary(void)
{
    payload_t p;
    write_file("empty.bin", "", 0, path, sizeof(path));
    CHECK_EQ(payload_load(path, PAYLOAD_BINARY, &p, err, sizeof(err)), -1);
    CHECK_CONTAINS(err, "データが空");
}

static void test_size_boundary(void)
{
    uint8_t *big = malloc(BCS_PAYLOAD_MAX + 1);
    for (size_t i = 0; i < BCS_PAYLOAD_MAX + 1; i++)
        big[i] = (uint8_t)(i * 7);
    payload_t p;

    write_file("max.bin", big, BCS_PAYLOAD_MAX, path, sizeof(path));
    CHECK_EQ(payload_load(path, PAYLOAD_BINARY, &p, err, sizeof(err)), 0);
    CHECK_EQ(p.len, 65507);
    CHECK(memcmp(p.data, big, BCS_PAYLOAD_MAX) == 0);
    payload_free(&p);

    write_file("over.bin", big, BCS_PAYLOAD_MAX + 1, path, sizeof(path));
    CHECK_EQ(payload_load(path, PAYLOAD_BINARY, &p, err, sizeof(err)), -1);
    CHECK_CONTAINS(err, "65507");

    /* hexでも同じ境界 */
    size_t hexcap = (BCS_PAYLOAD_MAX + 1) * 3 + 1;
    char *hex = malloc(hexcap);
    for (size_t n = BCS_PAYLOAD_MAX; n <= BCS_PAYLOAD_MAX + 1; n++) {
        size_t j = 0;
        for (size_t i = 0; i < n; i++)
            j += (size_t)sprintf(hex + j, "%02x%c", big[i], (i % 16 == 15) ? '\n' : ' ');
        write_file("size.hex", hex, j, path, sizeof(path));
        int rc = payload_load(path, PAYLOAD_HEX, &p, err, sizeof(err));
        if (n == BCS_PAYLOAD_MAX) {
            CHECK_EQ(rc, 0);
            CHECK_EQ(p.len, 65507);
            CHECK(memcmp(p.data, big, BCS_PAYLOAD_MAX) == 0);
            payload_free(&p);
        } else {
            CHECK_EQ(rc, -1);
            CHECK_CONTAINS(err, "65507");
        }
    }
    free(hex);
    free(big);
}

static void test_not_regular_file(void)
{
    payload_t p;
    CHECK_EQ(payload_load(tmpdir, PAYLOAD_BINARY, &p, err, sizeof(err)), -1);
    CHECK_CONTAINS(err, "通常ファイル");
    CHECK_EQ(payload_load("/nonexistent/p.bin", PAYLOAD_BINARY, &p, err, sizeof(err)), -1);
    CHECK_CONTAINS(err, "開けません");
}

int main(void)
{
    make_tmpdir();
    test_binary_with_nul();
    test_hex_matches_binary();
    test_hex_errors();
    test_empty_binary();
    test_size_boundary();
    test_not_regular_file();
    remove_tmpdir();
    if (failures) {
        fprintf(stderr, "test_payload: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_payload: OK\n");
    return 0;
}
