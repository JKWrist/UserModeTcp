#ifndef __USER_BUFFER_H__
#define __USER_BUFFER_H__

#include <stdint.h>
#include <sys/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include "user_queue.h"
#include "user_tree.h"
#include "user_mempool.h"

enum rb_caller
{
    AT_APP,
    AT_MTCP
};


#define MAX(a, b) ((a)>(b)?(a):(b))
#define MIN(a, b) ((a)<(b)?(a):(b))

/*----------------------------------------------------------------------------*/
typedef struct _user_sb_manager
{
    size_t chunk_size;
    uint32_t cur_num;
    uint32_t cnum;
    struct _user_mempool *mp;
    struct _user_sb_queue *freeq;

} user_sb_manager;

typedef struct _user_send_buffer
{
    unsigned char *data;
    unsigned char *head;

    uint32_t head_off;
    uint32_t tail_off;
    uint32_t len;
    uint64_t cum_len;
    uint32_t size;

    uint32_t head_seq;
    uint32_t init_seq;
} user_send_buffer;

#ifndef _INDEX_TYPE_
#define _INDEX_TYPE_
typedef uint32_t index_type;
typedef int32_t signed_index_type;
#endif


typedef struct _user_sb_queue
{
    index_type _capacity;
    volatile index_type _head;
    volatile index_type _tail;

    user_send_buffer *volatile *_q;
} user_sb_queue;

#define NextIndex(sq, i)    (i != sq->_capacity ? i + 1: 0)
#define PrevIndex(sq, i)    (i != 0 ? i - 1: sq->_capacity)
#define MemoryBarrier(buf, idx)    __asm__ volatile("" : : "m" (buf), "m" (idx))


/** rb frag queue **/

typedef struct _user_rb_frag_queue
{
    index_type _capacity;
    volatile index_type _head;
    volatile index_type _tail;

    struct _user_fragment_ctx *volatile *_q;
} user_rb_frag_queue;


/** ring buffer **/

typedef struct _user_fragment_ctx
{
    uint32_t seq;
    uint32_t len: 31,
            is_calloc: 1;
    struct _user_fragment_ctx *next;
} user_fragment_ctx;

typedef struct _user_ring_buffer
{
    u_char *data;
    u_char *head;

    uint32_t head_offset;
    uint32_t tail_offset;

    int merged_len;
    uint64_t cum_len;
    int last_len;
    int size;

    uint32_t head_seq;
    uint32_t init_seq;

    user_fragment_ctx *fctx;
} user_ring_buffer;

typedef struct _user_rb_manager
{
    size_t chunk_size;
    uint32_t cur_num;
    uint32_t cnum;

    user_mempool *mp;
    user_mempool *frag_mp;

    user_rb_frag_queue *free_fragq;
    user_rb_frag_queue *free_fragq_int;

} user_rb_manager;


typedef struct _user_stream_queue
{
    index_type _capacity;
    volatile index_type _head;
    volatile index_type _tail;

    struct _user_tcp_stream *volatile *_q;
} user_stream_queue;

typedef struct _user_stream_queue_int
{
    struct _user_tcp_stream **array;
    int size;

    int first;
    int last;
    int count;

} user_stream_queue_int;


user_sb_manager *user_sbmanager_create(size_t chunk_size, uint32_t cnum);
user_rb_manager *RBManagerCreate(size_t chunk_size, uint32_t cnum);
user_stream_queue *CreateStreamQueue(int capacity);


user_stream_queue_int *CreateInternalStreamQueue(int size);
void DestroyInternalStreamQueue(user_stream_queue_int *sq);


user_send_buffer *SBInit(user_sb_manager *sbm, uint32_t init_seq);

void   SBFree(   user_sb_manager *sbm, user_send_buffer *buf);
size_t SBPut(    user_sb_manager *sbm, user_send_buffer *buf, const void *data, size_t len);
int    SBEnqueue(user_sb_queue *sq,    user_send_buffer *buf);
size_t SBRemove( user_sb_manager *sbm, user_send_buffer *buf, size_t len);
size_t RBRemove( user_rb_manager *rbm, user_ring_buffer *buf, size_t len, int option);
int    RBPut(    user_rb_manager *rbm, user_ring_buffer *buf, void *data, uint32_t len, uint32_t cur_seq);
void  RBFree(    user_rb_manager *rbm, user_ring_buffer *buf);

int StreamInternalEnqueue(user_stream_queue_int *sq, struct _user_tcp_stream *stream);

struct _user_tcp_stream *StreamInternalDequeue(user_stream_queue_int *sq);


/*** ******************************** sb queue ******************************** ***/
user_sb_queue *CreateSBQueue(int capacity);

int StreamQueueIsEmpty(user_stream_queue *sq);


user_send_buffer *SBDequeue(user_sb_queue *sq);
user_ring_buffer *RBInit(user_rb_manager *rbm, uint32_t init_seq);


struct _user_tcp_stream *StreamDequeue(user_stream_queue *sq);
int StreamEnqueue(user_stream_queue *sq, struct _user_tcp_stream *stream);

void DestroyStreamQueue(user_stream_queue *sq);

#endif



