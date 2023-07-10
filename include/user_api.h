#ifndef __USER_API_H__
#define __USER_API_H__

#include <sys/types.h>
#include <sys/socket.h>

int user_socket(int domain, int type, int protocol);
int user_bind(int sockid, const struct sockaddr *addr, socklen_t addrlen);
int user_listen(int sockid, int backlog);
int user_accept(int sockid, struct sockaddr *addr, socklen_t *addrlen);
ssize_t user_recv(int sockid, char *buf, size_t len, int flags);
ssize_t user_send(int sockid, const char *buf, size_t len);
int user_close(int sockid);
void user_tcp_setup(void);

int socket(int domain, int type, int protocol);
int bind(int sockid, const struct sockaddr *addr, socklen_t addrlen);
int listen(int sockid, int backlog);
int accept(int sockid, struct sockaddr *addr, socklen_t *addrlen);
ssize_t recv(int sockid, void *buf, size_t len, int flags);
ssize_t send(int sockid, const void *buf, size_t len, int flags);

#endif