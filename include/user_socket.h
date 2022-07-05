#ifndef __USER_SOCKET_H__
#define __USER_SOCKET_H__


#include "user_buffer.h"
#include "user_tcp.h"
#include "user_config.h"

#include <pthread.h>


typedef struct _user_socket_map {
	int id;
	int socktype;
	uint32_t opts;

	struct sockaddr_in s_addr;

	union {
		struct _user_tcp_stream *stream;
		struct _user_tcp_listener *listener;
#if USER_ENABLE_EPOLL_RB
		void *ep;
#else
		struct _user_epoll *ep;
#endif
		//struct pipe *pp;
	};

	uint32_t epoll;
	uint32_t events;
	uint64_t ep_data;

	TAILQ_ENTRY(_user_socket_map) free_smap_link;
} user_socket_map; //__attribute__((packed)) 


enum user_socket_opts{
	USER_TCP_NONBLOCK = 0x01,
	USER_TCP_ADDR_BIND = 0x02,
};

user_socket_map *user_allocate_socket(int socktype, int need_lock);
void user_free_socket(int sockid, int need_lock);
user_socket_map *user_get_socket(int sockid);


/*
 * rebuild socket module for support 10M
 */
#if USER_ENABLE_SOCKET_C10M


struct _user_socket {
	int id;	
	int socktype;

	uint32_t opts;
	struct sockaddr_in s_addr;

	union {
		struct _user_tcp_stream *stream;
		struct _user_tcp_listener *listener;
		void *ep;
	};
	struct _user_socket_table *socktable;
};


struct _user_socket_table {
	size_t max_fds;
	int cur_idx;
	struct _user_socket **sockfds;
	unsigned char *open_fds;
	pthread_spinlock_t lock;
};

struct _user_socket* user_socket_allocate(int socktype);

void user_socket_free(int sockid);

struct _user_socket* user_socket_get(int sockid);

struct _user_socket_table * user_socket_init_fdtable(void);

int user_socket_close_listening(int sockid);

int user_socket_close_stream(int sockid);



#endif

#endif


