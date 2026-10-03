#define _GNU_SOURCE
#include "sender.h"
#include "setup.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdio.h>
#include "text.h"
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int check_interface(sender_t *s, const config_t *cfg, char *err, size_t errlen)
{
    struct ifaddrs *list;
    if (getifaddrs(&list) < 0) {
        snprintf(err, errlen, "getifaddrs: %s", strerror(errno));
        return -1;
    }

    int found_if = 0, found_addr = 0, rc = -1;
    unsigned flags = 0;
    const struct ifaddrs *selected = NULL;
    char src[INET_ADDRSTRLEN], bc[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &cfg->source_ip, src, sizeof(src));
    inet_ntop(AF_INET, &cfg->broadcast_ip, bc, sizeof(bc));

    for (struct ifaddrs *ifa = list; ifa != NULL; ifa = ifa->ifa_next) {
        if (strcmp(ifa->ifa_name, cfg->interface) != 0)
            continue;
        found_if = 1;
        flags = ifa->ifa_flags;
        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        const struct sockaddr_in *a = (const struct sockaddr_in *)ifa->ifa_addr;
        if (a->sin_addr.s_addr != cfg->source_ip.s_addr)
            continue;
        found_addr = 1;
        selected = ifa;
        if (ifa->ifa_netmask != NULL)
            s->netmask = ((const struct sockaddr_in *)ifa->ifa_netmask)->sin_addr;
        break;
    }

    if (!found_if) {
        snprintf(err, errlen, "インターフェース'%s'がありません", cfg->interface);
        goto out;
    }
    if (!found_addr) {
        snprintf(err, errlen, "インターフェース'%s'にIPv4アドレス%sが設定されていません",
                 cfg->interface, src);
        goto out;
    }
    if (!(flags & IFF_UP)) {
        snprintf(err, errlen, "インターフェース'%s'がUP状態ではありません", cfg->interface);
        goto out;
    }
    if (!(flags & IFF_BROADCAST)) {
        snprintf(err, errlen, "インターフェース'%s'はブロードキャストに対応していません", cfg->interface);
        goto out;
    }
    if (!(flags & IFF_RUNNING))
        ui_fprintf(stderr, "警告: インターフェース'%s'がRUNNING状態ではありません（リンクダウンの可能性）\n",
                cfg->interface);

    nic_t nic;
    if (!selected || !nic_from_ifaddr(selected, &nic)) {
        snprintf(err, errlen, "インターフェース'%s'のIPv4には送信可能なブロードキャストアドレスがありません", cfg->interface);
        goto out;
    }
    if (nic.broadcast.s_addr != cfg->broadcast_ip.s_addr) {
        char exp[INET_ADDRSTRLEN], mask[INET_ADDRSTRLEN];
        struct in_addr e = nic.broadcast;
        inet_ntop(AF_INET, &e, exp, sizeof(exp));
        inet_ntop(AF_INET, &s->netmask, mask, sizeof(mask));
        snprintf(err, errlen, "broadcast_ip %s が %s/%s のブロードキャストアドレス %s と一致しません",
                 bc, src, mask, exp);
        goto out;
    }

    s->ifindex = if_nametoindex(cfg->interface);
    if (s->ifindex == 0) {
        snprintf(err, errlen, "if_nametoindex(%s): %s", cfg->interface, strerror(errno));
        goto out;
    }
    rc = 0;
out:
    freeifaddrs(list);
    return rc;
}

int sender_open(sender_t *s, const config_t *cfg, const uint8_t *payload, size_t len,
                char *err, size_t errlen)
{
    memset(s, 0, sizeof(*s));
    s->fd = -1;

    if (check_interface(s, cfg, err, errlen) < 0)
        return -1;

    s->fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP);
    if (s->fd < 0) {
        snprintf(err, errlen, "socket: %s", strerror(errno));
        return -1;
    }

    int on = 1;
    if (setsockopt(s->fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)) < 0) {
        snprintf(err, errlen, "setsockopt(SO_BROADCAST): %s", strerror(errno));
        goto fail;
    }
    /* IP分割を許可する（DFビットを立てない） */
    int pmtu = IP_PMTUDISC_DONT;
    if (setsockopt(s->fd, IPPROTO_IP, IP_MTU_DISCOVER, &pmtu, sizeof(pmtu)) < 0) {
        snprintf(err, errlen, "setsockopt(IP_MTU_DISCOVER): %s", strerror(errno));
        goto fail;
    }

    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port = htons(cfg->source_port),
        .sin_addr = cfg->source_ip,
    };
    if (bind(s->fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        snprintf(err, errlen, "bind(%s:%u): %s", inet_ntoa(cfg->source_ip), cfg->source_port,
                 strerror(errno));
        goto fail;
    }
    socklen_t sl = sizeof(s->local);
    if (getsockname(s->fd, (struct sockaddr *)&s->local, &sl) < 0) {
        snprintf(err, errlen, "getsockname: %s", strerror(errno));
        goto fail;
    }

    socklen_t ol = sizeof(s->sndbuf);
    if (getsockopt(s->fd, SOL_SOCKET, SO_SNDBUF, &s->sndbuf, &ol) < 0)
        s->sndbuf = 0;

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", cfg->interface);
    if (ioctl(s->fd, SIOCGIFMTU, &ifr) == 0)
        s->mtu = ifr.ifr_mtu;

    s->dst.sin_family = AF_INET;
    s->dst.sin_port = htons(cfg->destination_port);
    s->dst.sin_addr = cfg->broadcast_ip;

    s->iov.iov_base = (void *)payload;
    s->iov.iov_len = len;

    /* IP_PKTINFOで送信インターフェースと送信元アドレスを明示する */
    memset(&s->cbuf, 0, sizeof(s->cbuf));
    s->msg.msg_name = &s->dst;
    s->msg.msg_namelen = sizeof(s->dst);
    s->msg.msg_iov = &s->iov;
    s->msg.msg_iovlen = 1;
    s->msg.msg_control = s->cbuf.buf;
    s->msg.msg_controllen = sizeof(s->cbuf.buf);
    struct cmsghdr *cm = CMSG_FIRSTHDR(&s->msg);
    cm->cmsg_level = IPPROTO_IP;
    cm->cmsg_type = IP_PKTINFO;
    cm->cmsg_len = CMSG_LEN(sizeof(struct in_pktinfo));
    struct in_pktinfo *pi = (struct in_pktinfo *)CMSG_DATA(cm);
    pi->ipi_ifindex = (int)s->ifindex;
    pi->ipi_spec_dst = cfg->source_ip;
    return 0;

fail:
    close(s->fd);
    s->fd = -1;
    return -1;
}

int sender_send(sender_t *s, volatile const sig_atomic_t *stop_flag)
{
    for (;;) {
        ssize_t n = sendmsg(s->fd, &s->msg, MSG_DONTWAIT);
        if (n >= 0) {
            /* UDPは一括送信だが、念のため部分送信を失敗として扱う */
            return ((size_t)n == s->iov.iov_len) ? 0 : EMSGSIZE;
        }
        if (errno == EINTR && !*stop_flag)
            continue;
        return errno;
    }
}

void sender_close(sender_t *s)
{
    if (s->fd >= 0)
        close(s->fd);
    s->fd = -1;
}

int sender_errno_is_transient(int e)
{
    return e == EAGAIN || e == EWOULDBLOCK || e == ENOBUFS;
}
