int process_vm_readv(int pid, void *local_iov, unsigned long liovcnt,
                     void *remote_iov, unsigned long riovcnt,
                     unsigned long flags);

void pvm_bad(void) {
    process_vm_readv(0,0,0,0,0,0);
}

void pvm_ok(void) {
    if (process_vm_readv(0,0,0,0,0,0)<0)
        return;
}
