#ifndef BCS_SETUP_H
#define BCS_SETUP_H

#include <ifaddrs.h>
#include <stdio.h>
#include "config.h"

typedef struct {
    char name[IF_NAMESIZE];
    struct in_addr address, netmask, broadcast;
    unsigned flags;
    unsigned prefix;
} nic_t;

/* Returns 1 for a usable broadcast IPv4 address, 0 otherwise. */
int nic_from_ifaddr(const struct ifaddrs *ifa, nic_t *nic);
int nic_discover(nic_t **items, size_t *count, char *err, size_t errlen);
/* Separate inventory and selection so tests do not depend on the host's NICs. */
int setup_network(config_t *cfg, const nic_t *items, size_t count, FILE *input,
                  FILE *output, char *err, size_t errlen);
int setup_missing(config_t *cfg, FILE *input, FILE *output, char *err, size_t errlen);

#endif
