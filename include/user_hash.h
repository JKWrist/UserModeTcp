#ifndef __USER_HASH_H__
#define __USER_HASH_H__


#include <stdint.h>

#include "user_queue.h"
//#include "user_tcp.h"


#define NUM_BINS_FLOWS		131072
#define NUM_BINS_LISTENERS	1024
#define TCP_AR_CNT			3

#if 0
typedef struct hash_bucket_head {
	struct _user_tcp_stream *tqh_first;
	struct _user_tcp_stream **tqh_last;
} hash_buchet_head;
#else

#define HASH_BUCKET_ENTRY(type)	\
	struct {					\
		struct type *tqh_first;	\
		struct type **tqh_last;	\
	}

#endif

typedef HASH_BUCKET_ENTRY(_user_tcp_stream) hash_bucket_head;
typedef HASH_BUCKET_ENTRY(_user_tcp_listener) list_bucket_head;

typedef struct _user_hashtable {
	uint8_t ht_count;
	uint32_t bins;
#if 0
	union {
		TAILQ_ENTRY(_user_tcp_stream) *ht_stream;
		TAILQ_ENTRY(_user_tcp_listener) *ht_listener;
	};
#else
	union {
		hash_bucket_head *ht_stream;
		list_bucket_head *ht_listener;
	};
#endif
	unsigned int (*hashfn)(const void *);
	int (*eqfn)(const void *, const void *);
} user_hashtable;


void *ListenerHTSearch(user_hashtable *ht, const void *it);
void *StreamHTSearch(user_hashtable *ht, const void *it);
int ListenerHTInsert(user_hashtable *ht, void *it);
int StreamHTInsert(user_hashtable *ht, void *it);
void *StreamHTRemove(user_hashtable *ht, void *it);


unsigned int HashFlow(const void *f);
int EqualFlow(const void *f1, const void *f2);
unsigned int HashListener(const void *l);
int EqualListener(const void *l1, const void *l2);

user_hashtable *CreateHashtable(unsigned int (*hashfn) (const void *), // key function
		int (*eqfn) (const void*, const void *),            // equality
		int bins);
void DestroyHashtable(user_hashtable *ht);




#endif