void fs_remove_bad(void) {
    path p;
    filesystem::remove(p);
    filesystem::exists(p);
}

void fs_remove_ok(void) {
    path p;
    if (filesystem::exists(p))
        filesystem::file_size(p);
}
