int setxattr(const char *path, const char *name, const void *value,
             unsigned long size, int flags);
int getxattr(const char *path, const char *name, void *value, unsigned long size);
int listxattr(const char *path, char *list, unsigned long size);

void setxattr_bad(void) {
    setxattr("x","u",0,0,0);
}

void setxattr_ok(void) {
    if (setxattr("x","u",0,0,0)!=0)
        return;
}
