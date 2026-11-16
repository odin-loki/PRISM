/* Linux kernel driver-data and socket-lock specs (docs/KERNEL_MODELS.md), W1 group g4.
 *
 * Reference semantics (include/linux/i2c.h, include/linux/pci.h, include/net/sock.h,
 * net/core/sock.c; Linux 6.18):
 *   i2c_set_clientdata(client, data), pci_set_drvdata(pdev, data): store data as
 *     the driver data of the embedded device (dev_set_drvdata); no failure path.
 *   lock_sock(sk): acquires the socket lock (blocking); release_sock(sk): releases it.
 *     No failure path.
 * requires: client, pdev and sk point to valid kernel objects of their types
 *   (the real functions dereference them; the model does not check it, it is
 *   named in the assumptions).
 * Assumptions introduced (named in the verdict):
 *   - the store into the embedded device is not modelled (no memory effect); no
 *     spec reads the driver data back
 *   - lock_sock and release_sock: blocking, the lock state and the may-sleep
 *     (atomic context) rules are not modelled, so a missing release_sock is not
 *     reported
 * Not specified (see NEEDS REVIEW in docs/KERNEL_MODELS.md): spi_controller_get_devdata
 *   (returns the stored pointer, which the model cannot represent soundly yet).
 */
#include "kernel_model.h"

extern "C" {

/* include/linux/i2c.h: static inline void i2c_set_clientdata(struct i2c_client *client, void *data) */
void i2c_set_clientdata(void *client, void *data) {
    ASSUME("linux-g4-i2c_set_clientdata: stores data as the driver data of client->dev (dev_set_drvdata, Linux 6.18); the store is not modelled (no memory effect) and no spec reads it back; client must be a valid struct i2c_client");
    (void)client;
    (void)data;
}

/* include/linux/pci.h: static inline void pci_set_drvdata(struct pci_dev *pdev, void *data) */
void pci_set_drvdata(void *pdev, void *data) {
    ASSUME("linux-g4-pci_set_drvdata: stores data as the driver data of pdev->dev (dev_set_drvdata, Linux 6.18); the store is not modelled (no memory effect) and no spec reads it back; pdev must be a valid struct pci_dev");
    (void)pdev;
    (void)data;
}

/* include/net/sock.h: static inline void lock_sock(struct sock *sk) { lock_sock_nested(sk, 0); } */
void lock_sock(void *sk) {
    ASSUME("linux-g4-lock_sock: acquires the socket lock (net/core/sock.c lock_sock_nested, Linux 6.18); blocking, the lock state and the may-sleep rules are not modelled, so lock_sock has no effect here and a missing release_sock is not reported; sk must be a valid struct sock");
    (void)sk;
}

/* net/core/sock.c: void release_sock(struct sock *sk): releases the lock taken by lock_sock */
void release_sock(void *sk) {
    ASSUME("linux-g4-release_sock: releases the socket lock taken by lock_sock (net/core/sock.c, Linux 6.18); no effect here (the lock state is not modelled); sk must be a valid struct sock");
    (void)sk;
}

} /* extern "C" */
