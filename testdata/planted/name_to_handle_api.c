int name_to_handle_at(int dirfd, const char *path, void *handle,
                      int *mount_id, int flags);

void nth_bad(void) {
    name_to_handle_at(0,0,0,0,0);
}

void nth_ok(void) {
    if (name_to_handle_at(0,0,0,0,0)!=0)
        return;
}
