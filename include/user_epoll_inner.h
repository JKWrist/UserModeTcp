#ifndef __USER_EPOLL_INNER_H__
#define __USER_EPOLL_INNER_H__


#include "user_socket.h"
#include "user_epoll.h"
#include "user_buffer.h"
#include "user_header.h"


typedef struct _user_epoll_stat {
	uint64_t calls;
	uint64_t waits;
	uint64_t wakes;

	uint64_t issued;
	uint64_t registered;
	uint64_t invalidated;
	uint64_t handled;
} user_epoll_stat;

typedef struct _user_epoll_event_int {
	user_epoll_event ev;
	int sockid;
} user_epoll_event_int;

typedef enum {
	USR_EVENT_QUEUE = 0,
	USR_SHADOW_EVENT_QUEUE = 1,
	USER_EVENT_QUEUE = 2
} user_event_queue_type;


typedef struct _user_event_queue {
	user_epoll_event_int *events;
	int start;
	int end;
	int size;
	int num_events;
} user_event_queue;

typedef struct _user_epoll {
	user_event_queue *usr_queue;
	user_event_queue *usr_shadow_queue;
	user_event_queue *queue;

	uint8_t waiting;
	user_epoll_stat stat;

	pthread_cond_t epoll_cond;
	pthread_mutex_t epoll_lock;
} user_epoll;

int user_epoll_add_event(user_epoll *ep, int queue_type, struct _user_socket_map *socket, uint32_t event);
int user_close_epoll_socket(int epid);
int user_epoll_flush_events(uint32_t cur_ts);


#if USER_ENABLE_EPOLL_RB

struct epitem {
	RB_ENTRY(epitem) rbn;
	LIST_ENTRY(epitem) rdlink;
	int rdy; //exist in list 
	
	int sockfd;
	struct epoll_event event; 
};

static int sockfd_cmp(struct epitem *ep1, struct epitem *ep2) {
	if (ep1->sockfd < ep2->sockfd) return -1;
	else if (ep1->sockfd == ep2->sockfd) return 0;
	return 1;
}


RB_HEAD(_epoll_rb_socket, epitem);
RB_GENERATE_STATIC(_epoll_rb_socket, epitem, rbn, sockfd_cmp);

typedef struct _epoll_rb_socket ep_rb_tree;


struct eventpoll {
	ep_rb_tree rbr;
	int rbcnt;
	
	LIST_HEAD( ,epitem) rdlist;
	int rdnum;

	int waiting;

	pthread_mutex_t mtx; //rbtree update
	pthread_spinlock_t lock; //rdlist update
	
	pthread_cond_t cond; //block for event
	pthread_mutex_t cdmtx; //mutex for cond
	
};


int epoll_event_callback(struct eventpoll *ep, int sockid, uint32_t event);



#endif



#endif



