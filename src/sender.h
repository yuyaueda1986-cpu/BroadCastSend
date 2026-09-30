#ifndef BCS_SENDER_H
#define BCS_SENDER_H

#include <netinet/in.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "config.h"

typedef struct {
    int fd;
    unsigned ifindex;
    int mtu;                 /* 取得できなければ0 */
    int sndbuf;              /* SO_SNDBUFの実効値。取得できなければ0 */
    struct in_addr netmask;
    struct sockaddr_in local; /* bind後の送信元アドレス・ポート */
    struct sockaddr_in dst;
    struct iovec iov;
    struct msghdr msg;
    union {
        char buf[CMSG_SPACE(sizeof(struct in_pktinfo))];
        struct cmsghdr align;
    } cbuf;
} sender_t;

/* インターフェースと送信元を検証し、送信用ソケットを準備する。 */
int sender_open(sender_t *s, const config_t *cfg, const uint8_t *payload, size_t len,
                char *err, size_t errlen);

/* 1個のデータグラムを非ブロッキングで送信する。成功時0、失敗時errnoを返す。
 * EINTRは*stop_flagが立っていない限り同じ試行を再開し、終了要求時だけEINTRを返す。 */
int sender_send(sender_t *s, volatile const sig_atomic_t *stop_flag);

void sender_close(sender_t *s);

/* 送信を継続できる一時的な失敗か */
int sender_errno_is_transient(int e);

#endif
