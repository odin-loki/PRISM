struct CopyPtr {
    int *p;
};

void copy_assign_bad(CopyPtr &self, const CopyPtr &o) {
    self.p = o.p;
}

void copy_assign_ok(CopyPtr &self, const CopyPtr &o) {
    self.p = new int(*o.p);
}
