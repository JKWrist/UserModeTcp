#ifndef __USER_ADDR_H__
#define __USER_ADDR_H__

#include "user_queue.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <netinet/in.h>


#define USER_MIN_PORT            1025
#define USER_MAX_PORT            65535

#ifndef INPORT_ANY
#define INPORT_ANY    (uint16_t)0
#endif

typedef struct _user_addr_entry
{
    struct sockaddr_in addr;
    TAILQ_ENTRY(_user_addr_entry) addr_link;
} user_addr_entry;

typedef struct _user_addr_map
{
    user_addr_entry *addrmap[USER_MAX_PORT];
} user_addr_map;

typedef struct _user_addr_pool
{
    user_addr_entry *pool;
    user_addr_map *mapper;

    uint32_t addr_base;

    int num_addr;
    int num_entry;
    int num_free;
    int num_used;

    pthread_mutex_t lock;
    TAILQ_HEAD(, _user_addr_entry) free_list;
    TAILQ_HEAD(, _user_addr_entry) used_list;
} user_addr_pool;


user_addr_pool *CreateAddressPool(in_addr_t addr_base, int num_addr);

user_addr_pool *CreateAddressPoolPerCore(int core, int num_queues,
                                         in_addr_t saddr_base, int num_addr, in_addr_t daddr, in_port_t dport);

void DestroyAddressPool(user_addr_pool *ap);

int FetchAddress(user_addr_pool *ap, int core, int num_queues,
                 const struct sockaddr_in *daddr, struct sockaddr_in *saddr);

int FetchAddressPerCore(user_addr_pool *ap, int core, int num_queues,
                        const struct sockaddr_in *daddr, struct sockaddr_in *saddr);

int FreeAddress(user_addr_pool *ap, const struct sockaddr_in *addr);


int GetRSSCPUCore(in_addr_t sip, in_addr_t dip,
                  in_port_t sp, in_port_t dp, int num_queues, uint8_t endian_check);

#endif