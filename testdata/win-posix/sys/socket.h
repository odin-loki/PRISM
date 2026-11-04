/* Windows-only astlint fixture stand-in (see ../README.md). */
#ifndef PRISM_WIN_POSIX_SYS_SOCKET_H
#define PRISM_WIN_POSIX_SYS_SOCKET_H
#include <stddef.h>
typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;
struct sockaddr {
    sa_family_t sa_family;
    char sa_data[14];
};
#define AF_UNIX 1
#define AF_INET 2
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr *addr, socklen_t len);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *addr, socklen_t *len);
int connect(int fd, const struct sockaddr *addr, socklen_t len);
long send(int fd, const void *buf, size_t n, int flags);
long recv(int fd, void *buf, size_t n, int flags);
#endif
