//实现封装的 socket api
#include "user_buffer.h"
#include "user_header.h"
#include "user_tcp.h"
#include "user_api.h"
#include "user_epoll.h"
#include "user_socket.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

extern user_tcp_manager *user_get_tcp_manager(void);

#if 0
static int user_peek_for_user(user_tcp_stream *cur_stream, char *buf, int len) {
	user_tcp_recv *rcv = cur_stream->rcv;

	int copylen = MIN(rcv->recvbuf->merged_len, len);
	if (copylen <= 0) {
		errno = EAGAIN;
		return -1;
	}
	memcpy(buf, rcv->recvbuf->head, copylen);

	return copylen;
}
#endif

static int user_copy_to_user(user_tcp_stream *cur_stream, char *buf, int len) {
	user_tcp_manager *tcp = user_get_tcp_manager();
	if (tcp == NULL) return -1;
	
	user_tcp_recv *rcv = cur_stream->rcv;

	int copylen = MIN(rcv->recvbuf->merged_len, len);
	if (copylen < 0) {
		errno = EAGAIN;
		return -1;
	} else if (copylen == 0){
		errno = 0;
		return 0;
	}
	//rcv --> data increase
	//uint32_t prev_rcv_wnd = rcv->rcv_wnd;

	memcpy(buf, rcv->recvbuf->head, copylen);

	RBRemove(tcp->rbm_rcv, rcv->recvbuf, copylen, AT_APP);
	rcv->rcv_wnd = rcv->recvbuf->size - rcv->recvbuf->merged_len;

	//printf("size:%d, merged_len:%d offset %d, %s\n",rcv->recvbuf->size, rcv->recvbuf->merged_len,
	//	rcv->recvbuf->head_offset, rcv->recvbuf->data);

	if (cur_stream->need_wnd_adv) {
		if (rcv->rcv_wnd > cur_stream->snd->eff_mss) {
			if (!cur_stream->snd->on_ackq) {
				cur_stream->snd->on_ackq = 1;
				StreamEnqueue(tcp->ackq, cur_stream);

				cur_stream->need_wnd_adv = 0;
				tcp->wakeup_flag = 0;
			}
		}
	}

	return copylen;
}

static int user_copy_from_user(user_tcp_stream *cur_stream, const char *buf, int len) {
	user_tcp_manager *tcp = user_get_tcp_manager();
	if (tcp == NULL) return -1;
	
	user_tcp_send *snd = cur_stream->snd;
	
	int sndlen = MIN((int)snd->snd_wnd, len);
	if (sndlen <= 0) {
		errno = EAGAIN;
		return -1;
	}

	if (!snd->sndbuf) {
		snd->sndbuf = SBInit(tcp->rbm_snd, snd->iss + 1);
		if (!snd->sndbuf) {
			cur_stream->close_reason = TCP_NO_MEM;
			errno = ENOMEM;
			return -1;
		}
	}

	int ret = SBPut(tcp->rbm_snd, snd->sndbuf, buf, sndlen);
	assert(ret == sndlen);
	if (ret <= 0) {
		user_trace_api("SBPut failed. reason: %d (sndlen: %u, len: %u\n", 
				ret, sndlen, snd->sndbuf->len);
		errno = EAGAIN;
		return -1;
	}

	snd->snd_wnd = snd->sndbuf->size - snd->sndbuf->len;
	if (snd->snd_wnd <= 0) {
		user_trace_api("%u Sending buffer became full!! snd_wnd: %u\n", 
				cur_stream->id, snd->snd_wnd);
	}

	return ret;
}

static int user_close_stream_socket(int sockid) {
	user_tcp_manager *tcp = user_get_tcp_manager();
	if (!tcp) return -1;

	user_tcp_stream *cur_stream = tcp->smap[sockid].stream;
	if (!cur_stream) {
		user_trace_api("Socket %d: stream does not exist.\n", sockid);
		errno = ENOTCONN;
		return -1;
	}

	if (cur_stream->closed) {
		user_trace_api("Socket %d (Stream %u): already closed stream\n", 
				sockid, cur_stream->id);
		return 0;
	}
	cur_stream->closed = 1;
	
	user_trace_api("Stream %d: closing the stream.\n", cur_stream->id);
	cur_stream->socket = NULL;

	if (cur_stream->state == USER_TCP_CLOSED) {
		printf("Stream %d at TCP_ST_CLOSED. destroying the stream.\n", 
				cur_stream->id);
		
		StreamEnqueue(tcp->destroyq, cur_stream);
		tcp->wakeup_flag = 1;
		
		return 0;
	} else if (cur_stream->state == USER_TCP_SYN_SENT) {
		
		StreamEnqueue(tcp->destroyq, cur_stream);		
		tcp->wakeup_flag = 1;
		
		return -1;
	} else if (cur_stream->state != USER_TCP_ESTABLISHED &&
			   cur_stream->state != USER_TCP_CLOSE_WAIT) {
		user_trace_api("Stream %d at state %d\n", 
				cur_stream->id, cur_stream->state);
		errno = EBADF;
		return -1;
	}

	cur_stream->snd->on_closeq = 1;
	int ret = StreamEnqueue(tcp->closeq, cur_stream);
	tcp->wakeup_flag = 1;

	if (ret < 0) {
		user_trace_api("(NEVER HAPPEN) Failed to enqueue the stream to close.\n");
		errno = EAGAIN;
		return -1;
	}

	return 0;
}

static int user_close_listening_socket(int sockid) {

	user_tcp_manager *tcp = user_get_tcp_manager();
	if (!tcp) return -1;

	struct _user_tcp_listener *listener = tcp->smap[sockid].listener;
	if (!listener) {
		errno = EINVAL;
		return -1;
	}

	if (listener->acceptq) {
		DestroyStreamQueue(listener->acceptq);
		listener->acceptq = NULL;
	}

	pthread_mutex_lock(&listener->accept_lock);
	pthread_cond_signal(&listener->accept_cond);
	pthread_mutex_unlock(&listener->accept_lock);

	pthread_cond_destroy(&listener->accept_cond);
	pthread_mutex_destroy(&listener->accept_lock);

	free(listener);
	tcp->smap[sockid].listener = NULL;

	return 0;
}


int user_socket(int domain, int type, int protocol) {

	
	if (domain != AF_INET) {
		errno = EAFNOSUPPORT;
		return -1;
	}
	if (type == SOCK_STREAM) {
		type = USER_TCP_SOCK_STREAM;
	} else {
		errno = EINVAL;
		return -1;
	}

	user_socket_map *socket = user_allocate_socket(type, 0);
	if (!socket) {
		errno = ENFILE;
		return -1;
	}

	return socket->id;
}

int user_bind(int sockid, const struct sockaddr *addr, socklen_t addrlen) {

	user_tcp_manager *tcp = user_get_tcp_manager();
	if (!tcp) return -1;

	if (sockid < 0 || sockid >= USER_MAX_CONCURRENCY) {
		errno = EBADF;
		return -1;
	}

	if (tcp->smap[sockid].socktype == USER_TCP_SOCK_UNUSED) {
		user_trace_api("Invalid socket id: %d\n", sockid);
		errno = EBADF;
		return -1;
	}

	if (tcp->smap[sockid].socktype != USER_TCP_SOCK_STREAM &&
		tcp->smap[sockid].socktype != USER_TCP_SOCK_LISTENER) {
		user_trace_api("Not a stream socket id: %d\n", sockid);
		errno = ENOTSOCK;
		return -1;
	}

	if (!addr) {
		user_trace_api("Socket %d: empty address!\n", sockid);
		errno = EINVAL;
		return -1;
	}

	if (tcp->smap[sockid].opts & USER_TCP_ADDR_BIND) {
		user_trace_api("Socket %d: adress already bind for this socket.\n", sockid);
		errno = EINVAL;
		return -1;
	}

	if (addr->sa_family != AF_INET || addrlen < sizeof(struct sockaddr_in)) {
		user_trace_api("Socket %d: invalid argument!\n", sockid);
		errno = EINVAL;
		return -1;
	}

	struct sockaddr_in *addr_in = (struct sockaddr_in*)addr;
	tcp->smap[sockid].s_addr = *addr_in;
	tcp->smap[sockid].opts |= USER_TCP_ADDR_BIND;

	return 0;
}

int user_listen(int sockid, int backlog) {
	user_tcp_manager *tcp = user_get_tcp_manager();
	if (!tcp) return -1;

	if (sockid < 0 || sockid >= USER_MAX_CONCURRENCY) {
		errno = EBADF;
		return -1;
	}
	if (tcp->smap[sockid].socktype == USER_TCP_SOCK_UNUSED) {
		user_trace_api("Socket %d: invalid argument!\n", sockid);
		errno = EBADF;
		return -1;
	}
	if (tcp->smap[sockid].socktype == USER_TCP_SOCK_STREAM) {
		tcp->smap[sockid].socktype = USER_TCP_SOCK_LISTENER;
	}
	if (tcp->smap[sockid].socktype != USER_TCP_SOCK_LISTENER) {
		user_trace_api("Not a listening socket. id: %d\n", sockid);
		errno = ENOTSOCK;
		return -1;
	}

	if (ListenerHTSearch(tcp->listeners, &tcp->smap[sockid].s_addr.sin_port)) {
		errno = EADDRINUSE;
		return -1;
	}
	
	user_tcp_listener *listener = (user_tcp_listener*)calloc(1, sizeof(user_tcp_listener));
	if (!listener) {
		return -1;
	}

	listener->sockid = sockid;
	listener->backlog = backlog;
	listener->socket = &tcp->smap[sockid];

	if (pthread_cond_init(&listener->accept_cond, NULL)) {
		user_trace_api("pthread_cond_init of ctx->accept_cond\n");
		free(listener);
		return -1;
	}

	if (pthread_mutex_init(&listener->accept_lock, NULL)) {
		user_trace_api("pthread_mutex_init of ctx->accept_lock\n");
		free(listener);
		return -1;
	}

	listener->acceptq = CreateStreamQueue(backlog);
	if (!listener->acceptq) {
		free(listener);
		errno = ENOMEM;
		return -1;
	}

	tcp->smap[sockid].listener = listener;
	ListenerHTInsert(tcp->listeners, listener);

	return 0;
}

int user_accept(int sockid, struct sockaddr *addr, socklen_t *addrlen) {

	user_tcp_manager *tcp = user_get_tcp_manager();
	if (!tcp) return -1;

	if (sockid < 0 || sockid >= USER_MAX_CONCURRENCY) {
		errno = EBADF;
		return -1;
	}

	if (tcp->smap[sockid].socktype != USER_TCP_SOCK_LISTENER) {
		errno = EINVAL;
		return -1;
	}

	user_tcp_listener *listener = tcp->smap[sockid].listener;
	user_tcp_stream *accepted = StreamDequeue(listener->acceptq);
	if (!accepted) {
		if (listener->socket->opts & USER_TCP_NONBLOCK) {
			errno = EAGAIN;
			return -1;
		} else {
			pthread_mutex_lock(&listener->accept_lock);
			while (accepted == NULL && ((accepted = StreamDequeue(listener->acceptq)) == NULL)) {
				pthread_cond_wait(&listener->accept_cond, &listener->accept_lock);

				//printf("");
				if (tcp->ctx->done || tcp->ctx->exit) {
					pthread_mutex_unlock(&listener->accept_lock);
					errno = EINTR;
					return -1;
				}
			}
			pthread_mutex_unlock(&listener->accept_lock);
		}
	}

	user_socket_map *socket = NULL;
	if (!accepted->socket) {
		socket = user_allocate_socket(USER_TCP_SOCK_STREAM, 0);
		if (!socket) {
			user_trace_api("Failed to create new socket!\n");
			/* TODO: destroy the stream */
			errno = ENFILE;
			return -1;
		}

		socket->stream = accepted;
		accepted->socket = socket;

		socket->s_addr.sin_family = AF_INET;
		socket->s_addr.sin_port = accepted->dport;
		socket->s_addr.sin_addr.s_addr = accepted->daddr;
	}
	if (!(listener->socket->epoll & USER_EPOLLET) &&
		!StreamQueueIsEmpty(listener->acceptq)) {
		user_epoll_add_event(tcp->ep, USR_SHADOW_EVENT_QUEUE, listener->socket, USER_EPOLLIN);		
	}
	user_trace_api("Stream %d accepted.\n", accepted->id);

	if (addr && addrlen) {
		struct sockaddr_in *addr_in = (struct sockaddr_in *)addr;
		addr_in->sin_family = AF_INET;
		addr_in->sin_port = accepted->dport;
		addr_in->sin_addr.s_addr = accepted->daddr;
		*addrlen = sizeof(struct sockaddr_in);
	}

	return accepted->socket->id;
}


ssize_t user_recv(int sockid, char *buf, size_t len, int flags) {
	user_tcp_manager *tcp = user_get_tcp_manager();
	if (!tcp) return -1;

	if (sockid < 0 || sockid >= USER_MAX_CONCURRENCY) {
		errno = EBADF;
		return -1;
	}

	user_socket_map *socket = &tcp->smap[sockid];
	if (socket->socktype == USER_TCP_SOCK_UNUSED) {
		errno = EINVAL;
		return -1;
	}
	
	if (socket->socktype != USER_TCP_SOCK_STREAM) {
		errno = ENOTSOCK;
		return -1;
	}
	
	/* stream should be in ESTABLISHED, FIN_WAIT_1, FIN_WAIT_2, CLOSE_WAIT */
	user_tcp_stream *cur_stream = socket->stream;
	if (!cur_stream ||
		!(cur_stream->state == USER_TCP_ESTABLISHED ||
		  cur_stream->state == USER_TCP_CLOSE_WAIT ||
		  cur_stream->state == USER_TCP_FIN_WAIT_1 ||
		  cur_stream->state == USER_TCP_FIN_WAIT_2)) {
		errno = ENOTCONN;
		return -1;
	}
	
	user_tcp_recv *rcv = cur_stream->rcv;
	if (cur_stream->state == USER_TCP_CLOSE_WAIT) {
		if (!rcv->recvbuf) return 0;
		if (rcv->recvbuf->merged_len == 0) return 0;
	}

	if (socket->opts & USER_TCP_NONBLOCK) {
		if (!rcv->recvbuf || rcv->recvbuf->merged_len == 0) {
			errno = EAGAIN;
			return -1;
		}
	}
	
	//SBUF_LOCK(&rcv->read_lock);
	pthread_mutex_lock(&rcv->read_lock);
#if USER_ENABLE_BLOCKING

	if (!(socket->opts & USER_TCP_NONBLOCK)) {
		
		while (!rcv->recvbuf || rcv->recvbuf->merged_len == 0) {
			if (!cur_stream || cur_stream->state != USER_TCP_ESTABLISHED) {
				pthread_mutex_unlock(&rcv->read_lock);
				
				if (rcv->recvbuf->merged_len == 0) { //disconnect
					errno = 0;
					return 0;
				} else {
					errno = EINTR;
					return -1;
				}
			}
			
			pthread_cond_wait(&rcv->read_cond, &rcv->read_lock);
		}
	}
#endif

	int ret = 0;
	switch (flags) {
		case 0: {
			ret = user_copy_to_user(cur_stream, buf, len);
			break;
		}
		default: {
			pthread_mutex_unlock(&rcv->read_lock);
			ret = -1;
			errno = EINVAL;
			return ret;
		}		
	}

	int event_remaining = 0;
	if (socket->epoll & USER_EPOLLIN) {
		if (!(socket->epoll & USER_EPOLLET) && rcv->recvbuf->merged_len > 0) {
			event_remaining = 1;
		}
	}

	if (cur_stream->state == USER_TCP_CLOSE_WAIT && 
	    rcv->recvbuf->merged_len == 0 && ret > 0) {
		event_remaining = 1;
	}
	
	pthread_mutex_unlock(&rcv->read_lock);


	if (event_remaining) {
		if (socket->epoll) {
			user_epoll_add_event(tcp->ep, USR_SHADOW_EVENT_QUEUE, socket, USER_EPOLLIN);
#if USER_ENABLE_BLOCKING
		} else if (!(socket->opts & USER_TCP_NONBLOCK)) {
			if (!cur_stream->on_rcv_br_list) {
				cur_stream->on_rcv_br_list = 1;
				TAILQ_INSERT_TAIL(&tcp->rcv_br_list, cur_stream, rcv->rcv_br_link);
				tcp->rcv_br_list_cnt ++;
			}
		}
#endif
	}

	user_trace_api("Stream %d: mtcp_recv() returning %d\n", cur_stream->id, ret);
    return ret;
}

ssize_t user_send(int sockid, const char *buf, size_t len) {
	user_tcp_manager *tcp = user_get_tcp_manager();
	if (!tcp) return -1;

	if (sockid < 0 || sockid >= USER_MAX_CONCURRENCY) {
		errno = EBADF;
		return -1;
	}
	
	user_socket_map *socket = &tcp->smap[sockid];
	if (socket->socktype == USER_TCP_SOCK_UNUSED) {
		errno = EINVAL;
		return -1;
	}
	if (socket->socktype != USER_TCP_SOCK_STREAM) {
		errno = ENOTSOCK;
		return -1;
	}
	/* stream should be in ESTABLISHED, FIN_WAIT_1, FIN_WAIT_2, CLOSE_WAIT */
	user_tcp_stream *cur_stream = socket->stream;
	if (!cur_stream ||
		!(cur_stream->state == USER_TCP_ESTABLISHED ||
		  cur_stream->state == USER_TCP_CLOSE_WAIT)) {
		errno = ENOTCONN;
		return -1;
	}

	if (len <= 0) {
		if (socket->opts & USER_TCP_NONBLOCK) {
			errno = EAGAIN;
			return -1;
		} else {
			return 0;
		}
	}

	user_tcp_send *snd = cur_stream->snd;

	pthread_mutex_lock(&snd->write_lock);

#if USER_ENABLE_BLOCKING
	if (!(socket->opts & USER_TCP_NONBLOCK)) {
		while (snd->snd_wnd <= 0) {
			if (!cur_stream || cur_stream->state != USER_TCP_ESTABLISHED) {
				pthread_mutex_unlock(&snd->write_lock);
				errno = EINTR;
				return -1;
			}
			
			pthread_cond_wait(&snd->write_cond, &snd->write_lock);
		}
	}
#endif
	int ret = user_copy_from_user(cur_stream, buf, len);
	pthread_mutex_unlock(&snd->write_lock);

	user_trace_api("user_copy_from_user --> %d, %d\n", 
		snd->on_sendq, snd->on_send_list);
	if (ret > 0 && !(snd->on_sendq || snd->on_send_list)) {
		snd->on_sendq = 1;
		StreamEnqueue(tcp->sendq, cur_stream);
		tcp->wakeup_flag = 1;
	}

	if (ret == 0 && (socket->opts & USER_TCP_NONBLOCK)) {
		ret = -1;
		errno = EAGAIN;
	}

	if (snd->snd_wnd > 0) {
		if ((socket->epoll & USER_EPOLLOUT) && !(socket->epoll & USER_EPOLLET)) {
			user_epoll_add_event(tcp->ep, USR_SHADOW_EVENT_QUEUE, socket, USER_EPOLLOUT);
#if USER_ENABLE_BLOCKING
		} else if (!(socket->opts & USER_TCP_NONBLOCK)) {
			if (!cur_stream->on_snd_br_list) {
				cur_stream->on_snd_br_list = 1;
				TAILQ_INSERT_TAIL(&tcp->snd_br_list, cur_stream, snd->snd_br_link);
				tcp->snd_br_list_cnt ++;
			}
#endif
		}
	}
	
	user_trace_api("Stream %d: mtcp_write() returning %d\n", cur_stream->id, ret);
	return ret;
}

int user_close(int sockid) {
	user_tcp_manager *tcp = user_get_tcp_manager();
	if (!tcp) return -1;

	if (sockid < 0 || sockid >= USER_MAX_CONCURRENCY) {
		errno = EBADF;
		return -1;
	}

	user_socket_map *socket = &tcp->smap[sockid];
	if (socket->socktype == USER_TCP_SOCK_UNUSED) {
		errno = EINVAL;
		return -1;
	}
	user_trace_api("Socket %d: mtcp_close called.\n", sockid);

	int ret = -1;
	switch (tcp->smap[sockid].socktype) {
		case USER_TCP_SOCK_STREAM: {
			ret = user_close_stream_socket(sockid);
			break;
		}
		case USER_TCP_SOCK_LISTENER: {
			ret = user_close_listening_socket(sockid);
			break;
		}
		case USER_TCP_SOCK_EPOLL: {
			ret = user_close_epoll_socket(sockid);
			break;
		}
		default: {
			errno = EINVAL;
			ret = -1;
			break;
		}
	}

	user_free_socket(sockid, 0);
	return ret;
}

#if 0
int user_connect(int sockid, const struct sockaddr *addr, socklen_t addrlen) {
	user_tcp_manager *tcp = user_get_tcp_manager();

	if (!tcp) {
		return -1;
	}

	if (sockid < 0 || sockid >= USER_MAX_CONCURRENCY) {
		errno = EBADF;
		return -1;
	}

	if (tcp->smap[sockid].socktype != USER_TCP_SOCK_UNUSED) {
		errno = EINVAL;
		return -1;
	}

	if (tcp->smap[sockid].socktype != USER_TCP_SOCK_STREAM) {
		errno = ENOTSOCK;
		return -1;
	}

	if (!addr) {
		printf("Socket %d: empty address!\n", sockid);
		errno = EFAULT;
		return -1;
	}

	if (addr->sa_family != AF_INET || addrlen < sizeof(struct sockaddr_in)) {
		printf("Socket %d: invalid argument!\n", sockid);
		errno = EAFNOSUPPORT;
		return -1;
	}

	user_socket_map *socket = &tcp->smap[sockid];
	if (socket->stream) {
		printf("Socket %d: stream already exist!\n", sockid);
		if (socket->stream->state >= TCP_ST_ESTABLISHED) {
			errno = EISCONN;
		} else {
			errno = EALREADY;
		}
		return -1;
	}

	struct sockaddr_in *addr_in = (struct sockaddr_in*)addr;
	in_addr_t dip = addr_in->sin_addr.s_addr;
	in_port_t dport = addr_in->sin_port;

	if ((socket->opts & USER_TCP_ADDR_BIND) && 
		socket->s_addr.sin_port != INPORT_ANY &&
		socket->s_addr.sin_addr.s_addr != INADDR_ANY) {

	}
}
#endif


#if USER_ENABLE_POSIX_API


int socket(int domain, int type, int protocol) {
	
	if (domain != AF_INET) {
		errno = EAFNOSUPPORT;
		return -1;
	}
	if (type == SOCK_STREAM) {
		type = USER_TCP_SOCK_STREAM;
	} else {
		errno = EINVAL;
		return -1;
	}

	struct _user_socket *socket = user_socket_allocate(type);
	if (!socket) {
		errno = ENFILE;
		return -1;
	}

	return socket->id;
}

int bind(int sockid, const struct sockaddr *addr, socklen_t addrlen) {
	user_tcp_manager *tcp = user_get_tcp_manager();
	if (!tcp) return -1;

	if (sockid < 0) {
		errno = EBADF;
		return -1;
	}

	if (tcp->fdtable == NULL) {
		errno = EBADF;
		return -1;
	}

	user_trace_api(" Enter Bind \n");
	struct _user_socket *s = tcp->fdtable->sockfds[sockid];
	if (s == NULL) {
		errno = EBADF;
		return -1;
	}

	if (s->socktype == USER_TCP_SOCK_UNUSED) {
		user_trace_api("Invalid socket id: %d\n", sockid);
		errno = EBADF;
		return -1;
	}

	if (s->socktype != USER_TCP_SOCK_STREAM &&
		s->socktype != USER_TCP_SOCK_LISTENER) {
		user_trace_api("Not a stream socket id: %d\n", sockid);
		errno = ENOTSOCK;
		return -1;
	}

	if (!addr) {
		user_trace_api("Socket %d: empty address!\n", sockid);
		errno = EINVAL;
		return -1;
	}

	if (s->opts & USER_TCP_ADDR_BIND) {
		user_trace_api("Socket %d: adress already bind for this socket.\n", sockid);
		errno = EINVAL;
		return -1;
	}

	if (addr->sa_family != AF_INET || addrlen < sizeof(struct sockaddr_in)) {
		user_trace_api("Socket %d: invalid argument!\n", sockid);
		errno = EINVAL;
		return -1;
	}

	struct sockaddr_in *addr_in = (struct sockaddr_in*)addr;
	s->s_addr= *addr_in;
	s->opts |= USER_TCP_ADDR_BIND;

	return 0;
}


int listen(int sockid, int backlog) {
	user_tcp_manager *tcp = user_get_tcp_manager();
	if (!tcp) return -1;

	if (sockid < 0) {
		errno = -EBADF;
		return -1;
	}

	if (tcp->fdtable == NULL) {
		errno = -EBADF;
		return -1;
	}
	user_trace_api(" Enter listen\n");
	struct _user_socket *s = tcp->fdtable->sockfds[sockid];
	if (s == NULL) {
		errno = -EBADF;
		return -1;
	}

	user_trace_api(" Enter listen 1111\n");
	if (s->socktype == USER_TCP_SOCK_UNUSED) {
		user_trace_api("Socket %d: invalid argument!\n", sockid);
		errno = -EBADF;
		return -1;
	}
	if (s->socktype == USER_TCP_SOCK_STREAM) {
		s->socktype = USER_TCP_SOCK_LISTENER;
	}
	if (s->socktype != USER_TCP_SOCK_LISTENER) {
		user_trace_api("Not a listening socket. id: %d\n", sockid);
		errno = -ENOTSOCK;
		return -1;
	}

	if (ListenerHTSearch(tcp->listeners, &s->s_addr.sin_port)) {
		errno = EADDRINUSE;
		return -1;
	}
	
	user_tcp_listener *listener = (user_tcp_listener*)calloc(1, sizeof(user_tcp_listener));
	if (!listener) {
		return -1;
	}

	listener->sockid = sockid;
	listener->backlog = backlog;
	listener->s = s;

	if (pthread_cond_init(&listener->accept_cond, NULL)) {
		user_trace_api("pthread_cond_init of ctx->accept_cond\n");
		free(listener);
		return -1;
	}

	if (pthread_mutex_init(&listener->accept_lock, NULL)) {
		user_trace_api("pthread_mutex_init of ctx->accept_lock\n");
		free(listener);
		return -1;
	}

	listener->acceptq = CreateStreamQueue(backlog);
	if (!listener->acceptq) {
		free(listener);
		errno = -ENOMEM;
		return -1;
	}
	listener->sockid = sockid;

	user_trace_api(" CreateStreamQueue \n");
	s->listener = listener;
	ListenerHTInsert(tcp->listeners, listener);

	user_trace_api(" ListenerHTInsert \n");
	return 0;
}

int accept(int sockid, struct sockaddr *addr, socklen_t *addrlen) {

	user_tcp_manager *tcp = user_get_tcp_manager();
	if (!tcp) return -1;

	if (sockid < 0) {
		errno = -EBADF;
		return -1;
	}
	
	if (tcp->fdtable == NULL) {
		errno = -EBADF;
		return -1;
	}
	struct _user_socket *s = tcp->fdtable->sockfds[sockid];

	if (s == NULL) {
		errno = -EBADF;
		return -1;
	}

	if (s->socktype != USER_TCP_SOCK_LISTENER) {
		errno = EINVAL;
		return -1;
	}

	user_tcp_listener *listener = s->listener;
	user_tcp_stream *accepted = StreamDequeue(listener->acceptq);
	if (!accepted) {
		if (listener->s->opts & USER_TCP_NONBLOCK) {
			errno = -EAGAIN;
			return -1;
		} else {
			user_trace_api(" Enter accept :%d, sockid:%d\n", s->id, sockid);
			pthread_mutex_lock(&listener->accept_lock);
			while (accepted == NULL && ((accepted = StreamDequeue(listener->acceptq)) == NULL)) {
				pthread_cond_wait(&listener->accept_cond, &listener->accept_lock);

				//printf("");
				if (tcp->ctx->done || tcp->ctx->exit) {
					pthread_mutex_unlock(&listener->accept_lock);
					errno = -EINTR;
					return -1;
				}
			}
			pthread_mutex_unlock(&listener->accept_lock);
		}
	}

	struct _user_socket *socket = NULL;
	if (!accepted->s) {
		socket = user_socket_allocate(USER_TCP_SOCK_STREAM);
		if (!socket) {
			user_trace_api("Failed to create new socket!\n");
			/* TODO: destroy the stream */
			errno = -ENFILE;
			return -1;
		}

		socket->stream = accepted;
		accepted->s = socket;

		socket->s_addr.sin_family = AF_INET;
		socket->s_addr.sin_port = accepted->dport;
		socket->s_addr.sin_addr.s_addr = accepted->daddr;
	}

	user_trace_api("Stream %d accepted.\n", accepted->id);

	if (addr && addrlen) {
		struct sockaddr_in *addr_in = (struct sockaddr_in *)addr;
		addr_in->sin_family = AF_INET;
		addr_in->sin_port = accepted->dport;
		addr_in->sin_addr.s_addr = accepted->daddr;
		*addrlen = sizeof(struct sockaddr_in);
	}

	return accepted->s->id;

}

ssize_t recv(int sockid, void *buf, size_t len, int flags) {
	user_tcp_manager *tcp = user_get_tcp_manager();
	if (!tcp) return -1;

	if (sockid < 0) {
		errno = EBADF;
		return -1;
	}

	if (tcp->fdtable == NULL) {
		errno = EBADF;
		return -1;
	}
	struct _user_socket *s = tcp->fdtable->sockfds[sockid];
	if (s == NULL) {
		errno = EBADF;
		return -1;
	}
	if (s->socktype == USER_TCP_SOCK_UNUSED) {
		errno = EINVAL;
		return -1;
	}
	
	if (s->socktype != USER_TCP_SOCK_STREAM) {
		errno = ENOTSOCK;
		return -1;
	}
	
	/* stream should be in ESTABLISHED, FIN_WAIT_1, FIN_WAIT_2, CLOSE_WAIT */
	user_tcp_stream *cur_stream = s->stream;
	if (!cur_stream ||
		!(cur_stream->state == USER_TCP_ESTABLISHED ||
		  cur_stream->state == USER_TCP_CLOSE_WAIT ||
		  cur_stream->state == USER_TCP_FIN_WAIT_1 ||
		  cur_stream->state == USER_TCP_FIN_WAIT_2)) {
		errno = ENOTCONN;
		return -1;
	}
	
	user_tcp_recv *rcv = cur_stream->rcv;
	if (cur_stream->state == USER_TCP_CLOSE_WAIT) {
		if (!rcv->recvbuf) return 0;
		if (rcv->recvbuf->merged_len == 0) return 0;
	}

	if (s->opts & USER_TCP_NONBLOCK) {
		if (!rcv->recvbuf || rcv->recvbuf->merged_len == 0) {
			errno = EAGAIN;
			return -1;
		}
	}
	
	//SBUF_LOCK(&rcv->read_lock);
	pthread_mutex_lock(&rcv->read_lock);
#if USER_ENABLE_BLOCKING

	if (!(s->opts & USER_TCP_NONBLOCK)) {
		
		while (!rcv->recvbuf || rcv->recvbuf->merged_len == 0) {
			if (!cur_stream || cur_stream->state != USER_TCP_ESTABLISHED) {
				pthread_mutex_unlock(&rcv->read_lock);
				
				if (rcv->recvbuf->merged_len == 0) { //disconnect
					errno = 0;
					return 0;
				} else {
					errno = -EINTR;
					return -1;
				}
			}
			
			pthread_cond_wait(&rcv->read_cond, &rcv->read_lock);
		}
	}
#endif

	int ret = 0;
	switch (flags) {
		case 0: {
			ret = user_copy_to_user(cur_stream, buf, len);
			break;
		}
		default: {
			pthread_mutex_unlock(&rcv->read_lock);
			ret = -1;
			errno = EINVAL;
			return ret;
		}		
	}
#if 0
	int event_remaining = 0;
	if (s->epoll & USER_EPOLLIN) {
		if (!(s->epoll & USER_EPOLLET) && rcv->recvbuf->merged_len > 0) {
			event_remaining = 1;
		}
	}
#endif

	if (cur_stream->state == USER_TCP_CLOSE_WAIT && 
	    rcv->recvbuf->merged_len == 0 && ret > 0) { //closed 
		//event_remaining = 1;
	}
	
	pthread_mutex_unlock(&rcv->read_lock);

	user_trace_api("Stream %d: mtcp_recv() returning %d\n", cur_stream->id, ret);
	
    return ret;
}

ssize_t send(int sockid, const void *buf, size_t len, int flags) {
	user_tcp_manager *tcp = user_get_tcp_manager();
	if (!tcp) return -1;

	if (sockid < 0) {
		errno = EBADF;
		return -1;
	}
	
	if (tcp->fdtable == NULL) {
		errno = EBADF;
		return -1;
	}
	struct _user_socket *s = tcp->fdtable->sockfds[sockid];
	if (s->socktype == USER_TCP_SOCK_UNUSED) {
		errno = EINVAL;
		return -1;
	}
	if (s->socktype != USER_TCP_SOCK_STREAM) {
		errno = ENOTSOCK;
		return -1;
	}
	/* stream should be in ESTABLISHED, FIN_WAIT_1, FIN_WAIT_2, CLOSE_WAIT */
	user_tcp_stream *cur_stream = s->stream;
	if (!cur_stream ||
		!(cur_stream->state == USER_TCP_ESTABLISHED ||
		  cur_stream->state == USER_TCP_CLOSE_WAIT)) {
		errno = ENOTCONN;
		return -1;
	}

	if (len <= 0) {
		if (s->opts & USER_TCP_NONBLOCK) {
			errno = EAGAIN;
			return -1;
		} else {
			return 0;
		}
	}

	user_tcp_send *snd = cur_stream->snd;

	pthread_mutex_lock(&snd->write_lock);

#if USER_ENABLE_BLOCKING
	if (!(s->opts & USER_TCP_NONBLOCK)) {
		while (snd->snd_wnd <= 0) {
			if (!cur_stream || cur_stream->state != USER_TCP_ESTABLISHED) {
				pthread_mutex_unlock(&snd->write_lock);
				errno = EINTR;
				return -1;
			}
			
			pthread_cond_wait(&snd->write_cond, &snd->write_lock);
		}
	}
#endif
	int ret = user_copy_from_user(cur_stream, buf, len);
	pthread_mutex_unlock(&snd->write_lock);

	user_trace_api("user_copy_from_user --> %d, %d\n", 
		snd->on_sendq, snd->on_send_list);
	if (ret > 0 && !(snd->on_sendq || snd->on_send_list)) {
		snd->on_sendq = 1;
		StreamEnqueue(tcp->sendq, cur_stream);
		tcp->wakeup_flag = 1;
	}

	if (ret == 0 && (s->opts & USER_TCP_NONBLOCK)) {
		ret = -1;
		errno = EAGAIN;
	}
#if 0
	if (snd->snd_wnd > 0) {
		if ((s->epoll & USER_EPOLLOUT) && !(s->epoll & USER_EPOLLET)) {
			user_epoll_add_event(tcp->ep, USR_SHADOW_EVENT_QUEUE, socket, USER_EPOLLOUT);
#if USER_ENABLE_BLOCKING
		} else if (!(socket->opts & USER_TCP_NONBLOCK)) {
			if (!cur_stream->on_snd_br_list) {
				cur_stream->on_snd_br_list = 1;
				TAILQ_INSERT_TAIL(&tcp->snd_br_list, cur_stream, snd->snd_br_link);
				tcp->snd_br_list_cnt ++;
			}
#endif
		}
	}
#endif
	
	user_trace_api("Stream %d: mtcp_write() returning %d\n", cur_stream->id, ret);
	return ret;
}

int close(int sockid) {
	user_tcp_manager *tcp = user_get_tcp_manager();
	if (!tcp) return -1;

	if (sockid < 0) {
		errno = EBADF;
		return -1;
	}

	//user_socket_map *socket = &tcp->smap[sockid];
	if (!tcp->fdtable) return -1;
	
	struct _user_socket *s = tcp->fdtable->sockfds[sockid];
	if (s->socktype == USER_TCP_SOCK_UNUSED) {
		errno = EINVAL;
		return -1;
	}
	user_trace_api("Socket %d, type:%d mtcp_close called.\n", sockid, s->socktype);

	int ret = -1;
	switch (s->socktype) {
		case USER_TCP_SOCK_STREAM: {
			ret = user_socket_close_stream(sockid);
			break;
		}
		case USER_TCP_SOCK_LISTENER: {
			ret = user_socket_close_listening(sockid);
			break;
		}
		case USER_TCP_SOCK_EPOLL: {
			ret = user_epoll_close_socket(sockid);
			break;
		}
		default: {
			errno = EINVAL;
			ret = -1;
			break;
		}
	}

	user_socket_free(sockid);
	return ret;
}


#endif