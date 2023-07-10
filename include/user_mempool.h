#ifndef __USER_MEMPOOL_H__
#define __USER_MEMPOOL_H__

enum
{
    MEM_NORMAL,
    MEM_HUGEPAGE
};

typedef struct _user_mem_chunk
{
    int mc_free_chunks;
    struct _user_mem_chunk *next;
} user_mem_chunk;

typedef struct _user_mempool
{
    u_char *mp_startptr;
    user_mem_chunk *mp_freeptr;
    int mp_free_chunks;
    int mp_total_chunks;
    int mp_chunk_size;
    int mp_type;
} user_mempool;

user_mempool *user_mempool_create(int chunk_size, size_t total_size, int is_hugepage);
void          user_mempool_destory(user_mempool *mp);
void *        user_mempool_alloc(  user_mempool *mp);
void          user_mempool_free(   user_mempool *mp, void *p);

#endif