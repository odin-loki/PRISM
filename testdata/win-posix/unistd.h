/* Windows-only astlint fixture stand-in (see README.md). */
#ifndef PRISM_WIN_POSIX_UNISTD_H
#define PRISM_WIN_POSIX_UNISTD_H
#include <stddef.h>
typedef int pid_t;
typedef unsigned uid_t;
typedef unsigned gid_t;
typedef long long ssize_t;
typedef long long off_t;
#ifndef R_OK
#  define F_OK 0
#  define X_OK 1
#  define W_OK 2
#  define R_OK 4
#endif
int access(const char *path, int mode);
int chdir(const char *path);
int chroot(const char *path);
int close(int fd);
int dup(int fd);
int dup2(int fd, int fd2);
int execv(const char *path, char *const argv[]);
int execvp(const char *file, char *const argv[]);
pid_t fork(void);
pid_t getpid(void);
uid_t getuid(void);
uid_t geteuid(void);
int open(const char *path, int flags, ...);
int pipe(int fds[2]);
ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
off_t lseek(int fd, off_t off, int whence);
unsigned sleep(unsigned s);
int unlink(const char *path);
int rmdir(const char *path);
int symlink(const char *from, const char *to);
int setuid(uid_t uid);
int seteuid(uid_t uid);
int setgid(gid_t gid);
int mkstemp(char *tmpl);
int mkstemps(char *tmpl, int suffixlen);
char *mktemp(char *tmpl);
#endif
