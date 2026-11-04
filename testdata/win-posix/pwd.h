/* Windows-only astlint fixture stand-in (see README.md). */
#ifndef PRISM_WIN_POSIX_PWD_H
#define PRISM_WIN_POSIX_PWD_H
typedef unsigned int uid_t;
typedef unsigned int gid_t;
struct passwd {
    char *pw_name;
    char *pw_passwd;
    uid_t pw_uid;
    gid_t pw_gid;
    char *pw_gecos;
    char *pw_dir;
    char *pw_shell;
};
struct passwd *getpwuid(uid_t uid);
struct passwd *getpwnam(const char *name);
#endif
