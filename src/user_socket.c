#include "user_epoll_inner.h"
#include "user_header.h"
#include "user_socket.h"

#include <hugetlbfs.h>
#include <pthread.h>
#include <errno.h>

extern user_tcp_manager *user_get_tcp_manager(void);

user_socket_map *user_allocate_socket(int socktype, int need_lock)
{
    user_tcp_manager *tcp = user_get_tcp_manager();
    if (tcp == NULL)
    {
        assert(0);
        return NULL;
    }

    if (need_lock)
    {
        pthread_mutex_lock(&tcp->ctx->smap_lock);
    }

    user_socket_map *socket = NULL;
    while (socket == NULL)
    {
        socket = TAILQ_FIRST(&tcp->free_smap);
        if (!socket)
        {
            if (need_lock)
            {
                pthread_mutex_unlock(&tcp->ctx->smap_lock);
            }
            printf("The concurrent sockets are at maximum.\n");
            return NULL;
        }
        TAILQ_REMOVE(&tcp->free_smap, socket, free_smap_link);

        if (socket->events)
        {
            printf("There are still not invalidate events remaining.\n");
            TAILQ_INSERT_TAIL(&tcp->free_smap, socket, free_smap_link);
            socket = NULL;
        }
    }

    if (need_lock)
    {
        pthread_mutex_unlock(&tcp->ctx->smap_lock);
    }
    socket->socktype = socktype;
    socket->opts = 0;
    socket->stream = NULL;
    socket->epoll = 0;
    socket->events = 0;
    memset(&socket->s_addr, 0, sizeof(struct sockaddr_in));
    memset(&socket->ep_data, 0, sizeof(user_epoll_data));

    return socket;
}


void user_free_socket(int sockid, int need_lock)
{

    user_tcp_manager *tcp = user_get_tcp_manager();
    user_socket_map *socket = &tcp->smap[sockid];

    if (socket->socktype == USER_TCP_SOCK_UNUSED)
    {
        return;
    }
    socket->socktype = USER_TCP_SOCK_UNUSED;
    socket->socktype = USER_EPOLLNONE;
    socket->events = 0;

    if (need_lock)
    {
        pthread_mutex_lock(&tcp->ctx->smap_lock);
    }
    tcp->smap[sockid].stream = NULL;
    TAILQ_INSERT_TAIL(&tcp->free_smap, socket, free_smap_link);

    if (need_lock)
    {
        pthread_mutex_unlock(&tcp->ctx->smap_lock);
    }
}


user_socket_map *user_get_socket(int sockid)
{
#if 1
    if (sockid < 0 || sockid >= USER_MAX_CONCURRENCY)
    {
        errno = EBADF;
        return NULL;
    }
#endif
    user_tcp_manager *tcp = user_get_tcp_manager();
    user_socket_map *socket = &tcp->smap[sockid];

    return socket;
}


/*
 * socket fd need to support 10M, so rebuild socket module.
 * 
 */

#if USER_ENABLE_SOCKET_C10M

struct _user_socket_table * user_socket_allocate_fdtable(void)
{
    //user_tcp_manager *tcp = user_get_tcp_manager();
    //if (tcp == NULL) return NULL;

    struct _user_socket_table *sock_table = (struct _user_socket_table*)calloc(1, sizeof(struct _user_socket_table));
    if (sock_table == NULL)
    {
        errno = -ENOMEM;
        return NULL;
    }

    size_t total_size = USER_SOCKFD_NR * sizeof(struct _user_socket *);
#if 0 //(USER_SOCKFD_NR > 1024)
    sock_table->sockfds = (struct _user_socket **)get_huge_pages(total_size, GHP_DEFAULT);
    if (sock_table->sockfds == NULL)
    {
        errno = -ENOMEM;
        free(sock_table);
        return NULL;
    }
#else
    int res = posix_memalign((void **)&sock_table->sockfds, getpagesize(), total_size);
    if (res != 0)
    {
        errno = -ENOMEM;
        free(sock_table);
        return NULL;
    }
#endif

    sock_table->max_fds = (USER_SOCKFD_NR % USER_BITS_PER_BYTE ? USER_SOCKFD_NR / USER_BITS_PER_BYTE + 1 : USER_SOCKFD_NR / USER_BITS_PER_BYTE);

    sock_table->open_fds = (unsigned char*)calloc(sock_table->max_fds, sizeof(unsigned char));
    if (sock_table->open_fds == NULL)
    {
        errno = -ENOMEM;
#if 0 //(USER_SOCKFD_NR > 1024)
        free_huge_pages(sock_table->sockfds);
#else
        free(sock_table->sockfds);
#endif
        free(sock_table);
        return NULL;
    }

    if (pthread_spin_init(&sock_table->lock, PTHREAD_PROCESS_SHARED))
    {
        errno = -EINVAL;
        free(sock_table->open_fds);
#if 0 //(USER_SOCKFD_NR > 1024)
        free_huge_pages(sock_table->sockfds);
#else
        free(sock_table->sockfds);
#endif
        free(sock_table);

        return NULL;
    }

    //tcp->fdtable = sock_table;

    return sock_table;
}


void user_socket_free_fdtable(struct _user_socket_table *fdtable)
{
    pthread_spin_destroy(&fdtable->lock);
    free(fdtable->open_fds);

#if (USER_SOCKFD_NR > 1024)
    free_huge_pages(fdtable->sockfds);
#else
    free(fdtable->sockfds);
#endif

    free(fdtable);
}


/*
 * singleton should use CAS
 */
struct _user_socket_table *user_socket_get_fdtable(void)
{
#if 0
    if (fdtable == NULL)
    {
        fdtable = user_socket_allocate_table();
    }
    return fdtable;
#else
    user_tcp_manager *tcp = user_get_tcp_manager();

    return tcp->fdtable;
#endif
}

struct _user_socket_table * user_socket_init_fdtable(void)
{
    return user_socket_allocate_fdtable();
}


int user_socket_find_id(unsigned char *fds, int start, size_t max_fds)
{

    size_t i = 0;
    for (i = start;i < max_fds;i ++)
    {
        if (fds[i] != 0xFF)
        {
            break;
        }
    }
    if (i == max_fds) return -1;

    int j = 0;
    char byte = fds[i];
    while (byte % 2)
    {
        byte /= 2;
        j ++;
    }

    return i * USER_BITS_PER_BYTE + j;
}

char user_socket_unuse_id(unsigned char *fds, size_t idx)
{
    int i = idx / USER_BITS_PER_BYTE;
    int j = idx % USER_BITS_PER_BYTE;

    char byte = 0x01 << j;
    fds[i] &= ~byte;

    return fds[i];
}

int user_socket_set_start(size_t idx)
{
    return idx / USER_BITS_PER_BYTE;
}

char user_socket_use_id(unsigned char *fds, size_t idx)
{
    int i = idx / USER_BITS_PER_BYTE;
    int j = idx % USER_BITS_PER_BYTE;

    char byte = 0x01 << j;

    fds[i] |= byte;

    return fds[i];
}


struct _user_socket* user_socket_allocate(int socktype)
{
    struct _user_socket *s = (struct _user_socket*)calloc(1, sizeof(struct _user_socket));
    if (s == NULL)
    {
        errno = -ENOMEM;
        return NULL;
    }

    struct _user_socket_table *sock_table = user_socket_get_fdtable();


    pthread_spin_lock(&sock_table->lock);

    s->id = user_socket_find_id(sock_table->open_fds, sock_table->cur_idx, sock_table->max_fds);
    if (s->id == -1)
    {
        pthread_spin_unlock(&sock_table->lock);
        errno = -ENFILE;
        return NULL;
    }

    sock_table->cur_idx = user_socket_set_start(s->id);
    char byte = user_socket_use_id(sock_table->open_fds, s->id);

    sock_table->sockfds[s->id] = s;
    user_trace_socket("user_socket_allocate --> user_socket_use_id : %x\n", byte);
    pthread_spin_unlock(&sock_table->lock);

    s->socktype = socktype;
    s->opts = 0;
    s->socktable = sock_table;
    s->stream = NULL;

    memset(&s->s_addr, 0, sizeof(struct sockaddr_in));
    UNUSED(byte);
    return s;
}

void user_socket_free(int sockid)
{
    struct _user_socket_table *sock_table = user_socket_get_fdtable();
    struct _user_socket *s = sock_table->sockfds[sockid];
    sock_table->sockfds[sockid] = NULL;

    pthread_spin_lock(&sock_table->lock);

    char byte = user_socket_unuse_id(sock_table->open_fds, sockid);

    sock_table->cur_idx = user_socket_set_start(sockid);
    user_trace_socket("user_socket_free --> user_socket_unuse_id : %x, %d\n",
        byte, sock_table->cur_idx);

    pthread_spin_unlock(&sock_table->lock);

    free(s);

    UNUSED(byte);
    user_trace_socket("user_socket_free --> Exit\n");

    return ;
}

struct _user_socket* user_socket_get(int sockid)
{
    struct _user_socket_table *sock_table = user_socket_get_fdtable();
    if(sock_table == NULL)
        return NULL;

    return sock_table->sockfds[sockid];
}

int user_socket_close_stream(int sockid)
{
    user_tcp_manager *tcp = user_get_tcp_manager();
    if (!tcp) return -1;

    struct _user_socket *s = user_socket_get(sockid);
    if (s == NULL) return -1;

    user_tcp_stream *cur_stream = s->stream;
    if (!cur_stream)
    {
        user_trace_api("Socket %d: stream does not exist.\n", sockid);
        errno = ENOTCONN;
        return -1;
    }

    if (cur_stream->closed)
    {
        user_trace_api("Socket %d (Stream %u): already closed stream\n",
                sockid, cur_stream->id);
        return 0;
    }
    cur_stream->closed = 1;

    user_trace_api("Stream %d: closing the stream.\n", cur_stream->id);
    cur_stream->s = NULL;

    if (cur_stream->state == USER_TCP_CLOSED)
    {
        printf("Stream %d at TCP_ST_CLOSED. destroying the stream.\n",
                cur_stream->id);

        StreamEnqueue(tcp->destroyq, cur_stream);
        tcp->wakeup_flag = 1;

        return 0;
    }
    else if (cur_stream->state == USER_TCP_SYN_SENT)
    {

        StreamEnqueue(tcp->destroyq, cur_stream);
        tcp->wakeup_flag = 1;

        return -1;
    }
    else if (cur_stream->state != USER_TCP_ESTABLISHED &&
               cur_stream->state != USER_TCP_CLOSE_WAIT)
    {
        user_trace_api("Stream %d at state %d\n",
                cur_stream->id, cur_stream->state);
        errno = -EBADF;
        return -1;
    }

    cur_stream->snd->on_closeq = 1;
    int ret = StreamEnqueue(tcp->closeq, cur_stream);
    tcp->wakeup_flag = 1;

    if (ret < 0)
    {
        user_trace_api("(NEVER HAPPEN) Failed to enqueue the stream to close.\n");
        errno = EAGAIN;
        return -1;
    }

    return 0;
}

int user_socket_close_listening(int sockid)
{
    user_tcp_manager *tcp = user_get_tcp_manager();
    if (!tcp) return -1;

    struct _user_socket *s = user_socket_get(sockid);
    if (s == NULL) return -1;

    struct _user_tcp_listener *listener = s->listener;
    if (!listener)
    {
        errno = EINVAL;
        return -1;
    }

    if (listener->acceptq)
    {
        DestroyStreamQueue(listener->acceptq);
        listener->acceptq = NULL;
    }

    pthread_mutex_lock(&listener->accept_lock);
    pthread_cond_signal(&listener->accept_cond);
    pthread_mutex_unlock(&listener->accept_lock);

    pthread_cond_destroy(&listener->accept_cond);
    pthread_mutex_destroy(&listener->accept_lock);

    free(listener);
    s->listener = NULL;

    return 0;
}

#endif
