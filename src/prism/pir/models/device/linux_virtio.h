/* PRISM device-model harness, Linux flavour: one virtqueue whose DEVICE side is untrusted (hostile or
 * buggy host), the virtio config ops of the transport, and the hwrng core. See docs/DEVICE_MODEL.md
 * section 10.
 *
 * The device may, within the virtio ring protocol: return a posted buffer at any time after it was
 * kicked, fill it with arbitrary bytes, and report an arbitrary used length (PRISM_VIRTIO_CONFORMANT
 * restricts the length to the buffer size: a conforming host); invoke the virtqueue callback at any time,
 * spuriously included ("shared IRQs + virtio_pci"); fail find_vqs.
 *
 * What is NOT modelled, and the report says so: DMA ownership (a CPU access to a buffer the device owns,
 * between kick and get_buf, is not detected), several virtqueues, indirect descriptors, the
 * negotiated features, a queue with room for more than one buffer.
 */
#ifndef PRISM_LINUX_VIRTIO_H
#define PRISM_LINUX_VIRTIO_H

#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/hw_random.h>

void __prism_havoc_bytes(void *p, unsigned long n);

static struct virtqueue prism_vq;
static int prism_vqs_live;              /* find_vqs without del_vqs */
static int prism_vq_posted;             /* a buffer was added and not yet returned */
static int prism_vq_kicked;
static void *prism_vq_token;
static void *prism_vq_buf;
static unsigned long prism_vq_buflen;
static u8 prism_vdev_status;
static struct hwrng *prism_hwrng;
static int prism_hwrng_registered;

int virtqueue_add_inbuf(struct virtqueue *vq, struct scatterlist sg[], unsigned int num, void *data, gfp_t gfp)
{
	(void)gfp;
	PRISM_MOD_CHECK(vq == &prism_vq, api, "virtqueue_add_inbuf on a queue find_vqs did not return");
	PRISM_MOD_CHECK(prism_vqs_live > 0, use_after_remove, "virtqueue_add_inbuf after del_vqs");
	PRISM_MOD_CHECK(num == 1, api, "the model has single-segment buffers");
	if (prism_vq_posted || prism_nondet_bool())
		return -28;                                     /* -ENOSPC: the queue holds one buffer */
	prism_vq_posted = 1;
	prism_vq_token = data;
	prism_vq_buf = prism_sg_buf;
	prism_vq_buflen = sg[0].length;
	return 0;
}

bool virtqueue_kick(struct virtqueue *vq)
{
	PRISM_MOD_CHECK(vq == &prism_vq, api, "virtqueue_kick on a queue find_vqs did not return");
	PRISM_MOD_CHECK(prism_vqs_live > 0, use_after_remove, "virtqueue_kick after del_vqs");
	if (prism_vq_posted)
		prism_vq_kicked = 1;
	return true;
}

void *virtqueue_get_buf(struct virtqueue *vq, unsigned int *len)
{
	void *token;
	unsigned int l;
	PRISM_MOD_CHECK(vq == &prism_vq, api, "virtqueue_get_buf on a queue find_vqs did not return");
	PRISM_MOD_CHECK(prism_vqs_live > 0, use_after_remove, "virtqueue_get_buf after del_vqs");
	if (!(prism_vq_posted && prism_vq_kicked) || !prism_nondet_bool())
		return (void *)0;
	token = prism_vq_token;
	prism_vq_posted = 0;
	prism_vq_kicked = 0;
#ifdef PRISM_VIRTIO_BUFLEN
	/* a havoc of symbolic length costs the encoder gigabytes: the module description states the buffer length,
	 * and the stub checks the driver's buffer has it */
	PRISM_MOD_CHECK(prism_vq_buflen == PRISM_VIRTIO_BUFLEN, api, "the posted buffer length differs from PRISM_VIRTIO_BUFLEN in the module description");
	__prism_havoc_bytes(prism_vq_buf, PRISM_VIRTIO_BUFLEN);   /* the device wrote arbitrary bytes */
#else
	__prism_havoc_bytes(prism_vq_buf, prism_vq_buflen);   /* the device wrote arbitrary bytes (symbolic length: expensive) */
#endif
	l = (unsigned int)prism_nondet_u64();                  /* ... and reports an arbitrary used length */
#ifdef PRISM_VIRTIO_CONFORMANT
	__VERIFIER_assume(l <= prism_vq_buflen);
#endif
	*len = l;
	return token;
}

/* the virtqueue callback is the interrupt of the ghost layer: it may run spuriously whenever the queue exists */
static int prism_vq_irq(int irq, void *cookie)
{
	struct virtqueue *vq = cookie;
	(void)irq;
#ifdef PRISM_VQ_CALLBACK
	PRISM_VQ_CALLBACK(vq);                             /* named by the module description: a direct call */
#else
	if (vq->callback)
		vq->callback(vq);                               /* indirect: every void(ptr) function is a candidate */
#endif
	return 1;
}

static int prism_find_vqs(struct virtio_device *vdev, unsigned int nvqs, struct virtqueue *vqs[],
			  struct virtqueue_info vqs_info[], struct irq_affinity *desc)
{
	(void)desc;
	if (prism_nondet_bool())
		return -12;                                     /* -ENOMEM */
	PRISM_MOD_CHECK(nvqs == 1, api, "the model has one virtqueue");
	prism_vq.vdev = vdev;
	prism_vq.callback = vqs_info[0].callback;
	vqs[0] = &prism_vq;
	prism_vqs_live++;
	prism_irq_on = 1;
	prism_irq_no = 0;
	prism_irq_fn = prism_vq_irq;
	prism_irq_cookie = &prism_vq;
	prism_irq_spurious = 1;
	return 0;
}

static void prism_del_vqs(struct virtio_device *vdev)
{
	(void)vdev;
	PRISM_MOD_CHECK(prism_vqs_live > 0, pairing, "del_vqs without find_vqs (double del_vqs)");
	if (prism_vqs_live > 0)
		prism_vqs_live--;
	prism_irq_on = 0;                                   /* del_vqs stops the callbacks */
	prism_vq_posted = 0;
	prism_vq_kicked = 0;
}

static u8 prism_get_status(struct virtio_device *vdev) { (void)vdev; return prism_vdev_status; }
static void prism_set_status(struct virtio_device *vdev, u8 status) { (void)vdev; prism_vdev_status = status; }
static void prism_reset(struct virtio_device *vdev) { (void)vdev; prism_vdev_status = 0; prism_vq_kicked = 0; }

/* the transport's ops, as the virtio core calls them; exactly the members the driver path uses */
static const struct virtio_config_ops prism_config_ops = {
	.get_status = prism_get_status,
	.set_status = prism_set_status,
	.reset = prism_reset,
	.find_vqs = prism_find_vqs,
	.del_vqs = prism_del_vqs,
};

/* drivers/virtio/virtio.c */
void virtio_reset_device(struct virtio_device *dev)
{
	prism_reset(dev);                                   /* the transport's reset, called directly */
}

/* drivers/char/hw_random/core.c, as far as a driver sees it */
int hwrng_register(struct hwrng *rng)
{
	PRISM_MOD_CHECK(rng != (struct hwrng *)0 && rng->read != (void *)0 && rng->name != (const char *)0, api, "hwrng_register of an hwrng with no name or no read");
	PRISM_MOD_CHECK(!prism_hwrng_registered, pairing, "hwrng_register while an hwrng is registered");
	if (prism_nondet_bool())
		return -17;                                     /* -EEXIST */
	prism_hwrng = rng;
	prism_hwrng_registered = 1;
	return 0;
}

void hwrng_unregister(struct hwrng *rng)
{
	PRISM_MOD_CHECK(prism_hwrng_registered && rng == prism_hwrng, pairing, "hwrng_unregister of an hwrng that is not registered");
	prism_hwrng_registered = 0;
#ifdef PRISM_HWRNG_CLEANUP
	PRISM_HWRNG_CLEANUP(rng);                           /* the module description names the driver's cleanup: a direct call */
#else
	if (rng->cleanup)
		rng->cleanup(rng);                              /* the core cleans the current rng up (indirect: see section 4.2) */
#endif
}

#endif
