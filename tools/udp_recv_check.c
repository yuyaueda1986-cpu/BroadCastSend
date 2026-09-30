/*
 * 送信内容の検証用受信ツール。既存受信プログラムの代わりではなく、
 * 受信したUDPデータ部がペイロードファイルとバイト単位で一致するかを確認する。
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "payload.h"
#include "timeutil.h"

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void usage(FILE *fp, const char *prog)
{
    fprintf(fp,
            "使い方: %s --port PORT --payload FILE --format hex|binary\n"
            "          [--bind ADDR] [--count N] [--idle-timeout SEC] [--rcvbuf BYTES]\n"
            "\n"
            "  --bind ADDR          受信アドレス（既定 0.0.0.0。ブロードキャスト受信には0.0.0.0を使用）\n"
            "  --count N            N個受信したら終了（既定 0=無制限）\n"
            "  --idle-timeout SEC   最初の受信後、SEC秒受信がなければ終了（既定 0=無制限）\n"
            "  --rcvbuf BYTES       SO_RCVBUFを指定する（既定はOSの値）\n",
            prog);
}

int main(int argc, char **argv)
{
    long port = -1, idle = 0, rcvbuf = 0;
    unsigned long long count = 0;
    const char *payload_path = NULL, *fmt = NULL, *bind_addr = "0.0.0.0";

    static const struct option opts[] = {
        { "port", required_argument, NULL, 'p' },
        { "payload", required_argument, NULL, 'f' },
        { "format", required_argument, NULL, 'F' },
        { "bind", required_argument, NULL, 'b' },
        { "count", required_argument, NULL, 'n' },
        { "idle-timeout", required_argument, NULL, 't' },
        { "rcvbuf", required_argument, NULL, 'r' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    int c;
    while ((c = getopt_long(argc, argv, "p:f:F:b:n:t:r:h", opts, NULL)) != -1) {
        switch (c) {
        case 'p': port = strtol(optarg, NULL, 10); break;
        case 'f': payload_path = optarg; break;
        case 'F': fmt = optarg; break;
        case 'b': bind_addr = optarg; break;
        case 'n': count = strtoull(optarg, NULL, 10); break;
        case 't': idle = strtol(optarg, NULL, 10); break;
        case 'r': rcvbuf = strtol(optarg, NULL, 10); break;
        case 'h': usage(stdout, argv[0]); return 0;
        default: usage(stderr, argv[0]); return 1;
        }
    }
    if (port < 1 || port > 65535 || payload_path == NULL || fmt == NULL ||
        (strcmp(fmt, "hex") != 0 && strcmp(fmt, "binary") != 0)) {
        usage(stderr, argv[0]);
        return 1;
    }

    char err[1024];
    payload_t pl;
    if (payload_load(payload_path, strcmp(fmt, "hex") == 0 ? PAYLOAD_HEX : PAYLOAD_BINARY, &pl,
                     err, sizeof(err)) < 0) {
        fprintf(stderr, "ペイロードエラー: %s\n", err);
        return 1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
    if (fd < 0) {
        perror("socket");
        return 2;
    }
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &on, sizeof(on));
    if (rcvbuf > 0 && setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(int)) < 0)
        perror("setsockopt(SO_RCVBUF)");

    struct sockaddr_in local = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    if (inet_pton(AF_INET, bind_addr, &local.sin_addr) != 1) {
        fprintf(stderr, "--bind: IPv4アドレスではありません: %s\n", bind_addr);
        return 1;
    }
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        perror("bind");
        return 2;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    printf("listening %s:%ld, expected %zu bytes\n", bind_addr, port, pl.len);
    fflush(stdout);

    static uint8_t buf[65536];
    uint64_t received = 0, match = 0, size_mismatch = 0, content_mismatch = 0, truncated = 0;
    int reported_first = 0;

    while (!g_stop && (count == 0 || received < count)) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int timeout_ms = (idle > 0 && received > 0) ? (int)(idle * 1000) : 1000;
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }
        if (pr == 0) {
            if (idle > 0 && received > 0)
                break;
            continue;
        }

        struct sockaddr_in from;
        char cbuf[CMSG_SPACE(sizeof(struct in_pktinfo))];
        struct iovec iov = { .iov_base = buf, .iov_len = sizeof(buf) };
        struct msghdr msg = {
            .msg_name = &from, .msg_namelen = sizeof(from),
            .msg_iov = &iov, .msg_iovlen = 1,
            .msg_control = cbuf, .msg_controllen = sizeof(cbuf),
        };
        ssize_t n = recvmsg(fd, &msg, MSG_TRUNC);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("recvmsg");
            break;
        }
        received++;
        if (msg.msg_flags & MSG_TRUNC)
            truncated++;

        if (!reported_first) {
            char src[INET_ADDRSTRLEN] = "?", dst[INET_ADDRSTRLEN] = "?";
            int ifindex = -1;
            inet_ntop(AF_INET, &from.sin_addr, src, sizeof(src));
            for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
                if (cm->cmsg_level == IPPROTO_IP && cm->cmsg_type == IP_PKTINFO) {
                    struct in_pktinfo *pi = (struct in_pktinfo *)CMSG_DATA(cm);
                    inet_ntop(AF_INET, &pi->ipi_addr, dst, sizeof(dst));
                    ifindex = pi->ipi_ifindex;
                }
            }
            printf("first datagram: from %s:%u to %s:%ld ifindex=%d size=%zd\n", src,
                   ntohs(from.sin_port), dst, port, ifindex, n);
            fflush(stdout);
            reported_first = 1;
        }

        if ((size_t)n != pl.len)
            size_mismatch++;
        else if (memcmp(buf, pl.data, pl.len) != 0)
            content_mismatch++;
        else
            match++;
    }

    printf("received=%" PRIu64 " match=%" PRIu64 " size_mismatch=%" PRIu64
           " content_mismatch=%" PRIu64 " truncated=%" PRIu64 "\n",
           received, match, size_mismatch, content_mismatch, truncated);
    close(fd);
    payload_free(&pl);
    return (received > 0 && received == match) ? 0 : 3;
}
