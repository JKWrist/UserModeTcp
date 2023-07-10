#ifndef __USER_EPOLL_H__
#define __USER_EPOLL_H__

#include <stdint.h>
#include "user_config.h"

typedef enum
{
    USER_EPOLLNONE = 0x0000,
    USER_EPOLLIN = 0x0001,
    USER_EPOLLPRI = 0x0002,
    USER_EPOLLOUT = 0x0004,
    USER_EPOLLRDNORM = 0x0040,
    USER_EPOLLRDBAND = 0x0080,
    USER_EPOLLWRNORM = 0x0100,
    USER_EPOLLWRBAND = 0x0200,
    USER_EPOLLMSG = 0x0400,
    USER_EPOLLERR = 0x0008,
    USER_EPOLLHUP = 0x0010,
    USER_EPOLLRDHUP = 0x2000,
    USER_EPOLLONESHOT = (1 << 30),
    USER_EPOLLET = (1 << 31)

} user_epoll_type;


typedef enum
{
    USER_EPOLL_CTL_ADD = 1,
    USER_EPOLL_CTL_DEL = 2,
    USER_EPOLL_CTL_MOD = 3,
} user_epoll_op;


typedef union _user_epoll_data
{
    void *ptr;
    int sockid;
    uint32_t u32;
    uint64_t u64;
} user_epoll_data;

typedef struct
{
    uint32_t events;
    uint64_t data;
} user_epoll_event;


int user_epoll_create(int size);

int user_epoll_ctl(int epid, int op, int sockid, user_epoll_event *event);

int user_epoll_wait(int epid, user_epoll_event *events, int maxevents, int timeout);


#if USER_ENABLE_EPOLL_RB

enum EPOLL_EVENTS
{
    EPOLLNONE = 0x0000,
    EPOLLIN = 0x0001,
    EPOLLPRI = 0x0002,
    EPOLLOUT = 0x0004,
    EPOLLRDNORM = 0x0040,
    EPOLLRDBAND = 0x0080,
    EPOLLWRNORM = 0x0100,
    EPOLLWRBAND = 0x0200,
    EPOLLMSG = 0x0400,
    EPOLLERR = 0x0008,
    EPOLLHUP = 0x0010,
    EPOLLRDHUP = 0x2000,
    EPOLLONESHOT = (1 << 30),
    EPOLLET = (1 << 31)

};

#define EPOLL_CTL_ADD    1
#define EPOLL_CTL_DEL    2
#define EPOLL_CTL_MOD    3

typedef union epoll_data
{
    void *ptr;
    int fd;
    uint32_t u32;
    uint64_t u64;
} epoll_data_t;

struct epoll_event
{
    uint32_t events;
    epoll_data_t data;
};

int epoll_create(int size);
int epoll_ctl(int epid, int op, int sockid, struct epoll_event *event);
int epoll_wait(int epid, struct epoll_event *events, int maxevents, int timeout);
int user_epoll_close_socket(int epid);

#endif //USER_ENABLE_EPOLL_RB

#endif