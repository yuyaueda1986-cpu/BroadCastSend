#define _GNU_SOURCE
#include <arpa/inet.h>
#include "setup.h"
#include "testutil.h"

static char err[1024];

static nic_t make_nic(const char *name, const char *addr, const char *bc)
{
    nic_t nic = {.flags = IFF_UP | IFF_BROADCAST | IFF_RUNNING, .prefix = 24};
    snprintf(nic.name, sizeof(nic.name), "%s", name);
    inet_pton(AF_INET, addr, &nic.address);
    inet_pton(AF_INET, bc, &nic.broadcast);
    inet_pton(AF_INET, "255.255.255.0", &nic.netmask);
    return nic;
}

static void test_inventory(void)
{
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = inet_addr("192.0.2.1")};
    struct sockaddr_in mask = {.sin_family = AF_INET, .sin_addr.s_addr = inet_addr("255.255.255.0")};
    struct sockaddr_in bc = {.sin_family = AF_INET, .sin_addr.s_addr = inet_addr("192.0.2.255")};
    struct ifaddrs ifa = {.ifa_name = "eth0", .ifa_flags = IFF_UP | IFF_BROADCAST,
        .ifa_addr = (struct sockaddr *)&addr, .ifa_netmask = (struct sockaddr *)&mask};
    ifa.ifa_broadaddr = (struct sockaddr *)&bc;
    nic_t nic;
    CHECK_EQ(nic_from_ifaddr(&ifa, &nic), 1);
    CHECK_EQ(nic.prefix, 24);
    CHECK_EQ(nic.broadcast.s_addr, bc.sin_addr.s_addr);
    ifa.ifa_flags &= ~IFF_UP;
    CHECK_EQ(nic_from_ifaddr(&ifa, &nic), 0);
    ifa.ifa_flags = IFF_UP | IFF_LOOPBACK;
    CHECK_EQ(nic_from_ifaddr(&ifa, &nic), 0);
    ifa.ifa_flags = IFF_UP | IFF_POINTOPOINT;
    CHECK_EQ(nic_from_ifaddr(&ifa, &nic), 0);
    ifa.ifa_flags = IFF_UP | IFF_BROADCAST;
    mask.sin_addr.s_addr = inet_addr("255.255.255.254");
    CHECK_EQ(nic_from_ifaddr(&ifa, &nic), 0);
    mask.sin_addr.s_addr = inet_addr("255.255.255.255");
    CHECK_EQ(nic_from_ifaddr(&ifa, &nic), 0);
    mask.sin_addr.s_addr = inet_addr("255.0.255.0");
    CHECK_EQ(nic_from_ifaddr(&ifa, &nic), 0);
    ifa.ifa_addr = NULL;
    CHECK_EQ(nic_from_ifaddr(&ifa, &nic), 0);
}

static void test_selection(void)
{
    nic_t nics[] = {make_nic("eth0", "192.0.2.1", "192.0.2.255"),
                    make_nic("eth0", "198.51.100.1", "198.51.100.255"),
                    make_nic("eth1", "203.0.113.1", "203.0.113.255")};
    config_t cfg;
    config_init(&cfg);
    FILE *out = tmpfile();
    CHECK(out != NULL);
    if (!out) return;
    CHECK_EQ(setup_network(&cfg, nics, 3, NULL, out, err, sizeof(err)), -1);
    CHECK_CONTAINS(err, "選択が必要");
    char answers[] = "invalid\n0\n4\n-1\n2\n";
    FILE *in = fmemopen(answers, strlen(answers), "r");
    CHECK_EQ(setup_network(&cfg, nics, 3, in, out, err, sizeof(err)), 0);
    CHECK(strcmp(cfg.interface, "eth0") == 0);
    CHECK_EQ(cfg.source_ip.s_addr, inet_addr("198.51.100.1"));
    CHECK_EQ(cfg.broadcast_ip.s_addr, inet_addr("198.51.100.255"));
    fclose(in);

    config_init(&cfg);
    CHECK_EQ(config_set(&cfg, K_INTERFACE, "eth1", "", err, sizeof(err)), 0);
    CHECK_EQ(setup_network(&cfg, nics, 3, NULL, out, err, sizeof(err)), 0);
    CHECK_EQ(cfg.source_ip.s_addr, inet_addr("203.0.113.1"));
    CHECK_EQ(cfg.broadcast_ip.s_addr, inet_addr("203.0.113.255"));
    /* Already configured values must constrain selection, never be replaced. */
    config_init(&cfg);
    CHECK_EQ(config_set(&cfg, K_INTERFACE, "eth0", "", err, sizeof(err)), 0);
    CHECK_EQ(setup_network(&cfg, nics, 3, NULL, out, err, sizeof(err)), -1);
    CHECK_EQ(config_set(&cfg, K_SOURCE_IP, "203.0.113.1", "", err, sizeof(err)), 0);
    CHECK_EQ(setup_network(&cfg, nics, 3, NULL, out, err, sizeof(err)), -1);
    CHECK_CONTAINS(err, "ありません");
    config_init(&cfg);
    CHECK_EQ(config_set(&cfg, K_BROADCAST_IP, "198.51.100.255", "", err, sizeof(err)), 0);
    char one[] = "1\n";
    in = fmemopen(one, strlen(one), "r");
    CHECK_EQ(setup_network(&cfg, nics, 3, in, out, err, sizeof(err)), 0);
    CHECK_EQ(cfg.source_ip.s_addr, inet_addr("198.51.100.1"));
    fclose(in);
    config_init(&cfg);
    in = tmpfile();
    CHECK_EQ(setup_network(&cfg, nics, 1, in, out, err, sizeof(err)), -1);
    CHECK_CONTAINS(err, "入力が終了");
    fclose(in);
    CHECK_EQ(setup_network(&cfg, nics, 0, NULL, out, err, sizeof(err)), -1);
    fclose(out);
}

static void test_prompts(void)
{
    char payload[512];
    write_text("p.hex", "00 ff", payload, sizeof(payload));
    config_t cfg;
    config_init(&cfg);
    CHECK_EQ(config_set(&cfg, K_DESTINATION_PORT, "12345", "", err, sizeof(err)), 0);
    char answers[2048];
    snprintf(answers, sizeof(answers), "\nwrong\nhex\n/no/such/payload\n%s\n99\n1000\n2\n0\n1\n0\n-\n", payload);
    FILE *in = fmemopen(answers, strlen(answers), "r"), *out = tmpfile();
    CHECK_EQ(setup_missing(&cfg, in, out, err, sizeof(err)), 0);
    CHECK_EQ(cfg.destination_port, 12345);
    CHECK_EQ(cfg.source_port, 0);
    CHECK_EQ(cfg.period_us, 1000);
    CHECK_EQ(cfg.packets_per_cycle, 2);
    CHECK_EQ(cfg.duration_sec, 0);
    CHECK_EQ(cfg.max_attempts, 1);
    CHECK_EQ(cfg.stats_interval_sec, 0);
    CHECK(cfg.stats_path[0] == 0);
    CHECK(CONFIG_HAS(&cfg, K_STATS_FILE));
    CHECK(strcmp(cfg.payload_path, payload) == 0);
    fclose(in); fclose(out);
}

static void test_zero_prompts(void)
{
    config_t cfg;
    config_init(&cfg);
    CHECK_EQ(config_set(&cfg, K_SOURCE_PORT, "0", "", err, sizeof(err)), 0);
    CHECK_EQ(config_set(&cfg, K_DESTINATION_PORT, "50000", "", err, sizeof(err)), 0);
    char answers[] = "\n0\n65508\n128\n\n\n\n\n\n\n";
    FILE *in = fmemopen(answers, strlen(answers), "r"), *out = tmpfile();
    CHECK_EQ(setup_missing(&cfg, in, out, err, sizeof(err)), 0);
    CHECK_EQ(cfg.payload_format, PAYLOAD_ZERO);
    CHECK_EQ(cfg.payload_size, 128);
    CHECK(!CONFIG_HAS(&cfg, K_PAYLOAD_FILE));
    rewind(out);
    char transcript[8192] = {0};
    size_t length = fread(transcript, 1, sizeof(transcript) - 1, out);
    CHECK(length > 0 && !ferror(out));
    CHECK(strstr(transcript, "ペイロードファイル (") == NULL);
    CHECK_CONTAINS(transcript, "0データのサイズ");
    CHECK_CONTAINS(transcript, "size: 1〜65507");
    fclose(in); fclose(out);
}

int main(void)
{
    make_tmpdir();
    test_inventory();
    test_selection();
    test_prompts();
    test_zero_prompts();
    remove_tmpdir();
    if (failures) return 1;
    puts("test_setup: OK");
    return 0;
}
