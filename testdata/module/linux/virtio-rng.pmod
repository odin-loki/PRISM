# Module description: Linux drivers/char/hw_random/virtio-rng.c with an UNTRUSTED DEVICE (a hostile or
# buggy host) behind its one virtqueue. Second pilot of docs/DEVICE_MODEL.md section 10.
#   prism-module run virtio-rng.pmod ...                      hostile host: any used length, any bytes
#   prism-module run virtio-rng.pmod --define PRISM_VIRTIO_CONFORMANT ...   conforming host: used length <= buffer
module virtio_rng;
kernel linux;
driver "drivers/char/hw_random/virtio-rng.c";
stubs virtio;
steps 2;
irq_points api steps;
irq_spurious yes;
teardown_ops no;
assume "one virtqueue, room for one buffer; the host may complete a posted buffer at any time after the kick, fill it with arbitrary bytes and report any used length (a hostile host), unless PRISM_VIRTIO_CONFORMANT bounds the length";
assume "the virtqueue callback may run at any time while the queue exists, spuriously included";
assume "the virtio core calls scan after a successful probe, freeze/restore on suspend and resume, and remove when the driver is unbound, also after a failed restore (drivers/virtio/virtio.c virtio_dev_remove: read, not run)";
# every indirect call is inlined once per address-taken function of its signature, and void(ptr) includes
# virtrng_remove itself: remove_common -> del_vqs -> virtrng_remove -> remove_common ... grows without bound.
# The transport's del_vqs and the hwrng cleanup are therefore called directly (adaptation below).
define PRISM_HWRNG_CLEANUP(r) virtio_cleanup(r);
assume "the hwrng core calls read with a buffer of 64 bytes and at most that size, only while the hwrng is registered";

env {
	static struct virtio_device vdev;
	static unsigned char rbuf[64];
	static void prism_do_read(int wait, int cov)
	{
		unsigned size;
		int ret;
		if (!prism_hwrng_registered) return;
		PRISM_COVER_POINT(cov);
		PRISM_MOD_CHECK(prism_hwrng->read == virtio_read, api, "the registered hwrng read is not virtio_read");
		size = prism_nondet_choice(sizeof(rbuf) + 1);
		ret = virtio_read(prism_hwrng, rbuf, size, wait);
		PRISM_MOD_CHECK(ret < 0 || (unsigned)ret <= size, api, "hwrng read returned more than was asked for");
	}
}

setup { vdev.config = &prism_config_ops; }

probe {
	int r = virtrng_probe(&vdev);
	if (r == 0)
		virtrng_scan(&vdev);
	return r;
}
remove { virtrng_remove(&vdev); }

op read_nowait { prism_do_read(0, PRISM_OP_COVER); }
op read_wait { prism_do_read(1, PRISM_OP_COVER); }
op suspend_resume {
	if (prism_vqs_live == 0) return;                 /* only a bound device with its queue is suspended */
	PRISM_COVER_POINT(PRISM_OP_COVER);
	(void)virtrng_freeze(&vdev);
	(void)virtrng_restore(&vdev);
}

# ADAPTATION (not a mutant): the hwrng core keeps the driver's pointer in `unsigned long priv` and the driver
# casts it back. pir does not model address values (UNENCODED: ptrtoint / inttoptr), so the round trip goes
# through a variable instead. Semantics-preserving as long as rng->priv is the pointer set in probe_common
# (true in this source) and there is one hwrng instance (true in the module).
patch { static DEFINE_IDA(rng_index_ida); }
      { static DEFINE_IDA(rng_index_ida);
        struct virtrng_info;
        static struct virtrng_info *prism_vi; };
patch { .priv = (unsigned long)vi, } { .priv = 0, };
patch { vdev->priv = vi; } { vdev->priv = vi; prism_vi = vi; };
patch { (struct virtrng_info *)rng->priv } { prism_vi };
patch { vdev->config->del_vqs(vdev); } { prism_del_vqs(vdev); };
