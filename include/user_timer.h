#ifndef __USER_TIMER_H__
#define __USER_TIMER_H__

#include "user_tcp.h"
#include "user_queue.h"

#include <stdint.h>

#define RTO_HASH		3000

typedef struct _user_rto_hashstore {
	uint32_t rto_now_idx;
	uint32_t rto_now_ts;
	TAILQ_HEAD(rto_head, _user_tcp_stream) rto_list[RTO_HASH+1];
} user_rto_hashstore;

user_rto_hashstore *InitRTOHashstore(void);



#endif