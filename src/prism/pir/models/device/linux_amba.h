/* PRISM device-model harness, Linux flavour: the AMBA bus. */
#ifndef PRISM_LINUX_AMBA_H
#define PRISM_LINUX_AMBA_H

#include <linux/amba/bus.h>

int amba_request_regions(struct amba_device *dev, const char *name)
{
	(void)dev; (void)name;
	if (prism_nondet_bool())
		return -16;                                     /* -EBUSY */
	PRISM_MOD_CHECK(prism_regions == 0, pairing, "amba_request_regions twice without amba_release_regions");
	prism_regions++;
	return 0;
}

void amba_release_regions(struct amba_device *dev)
{
	(void)dev;
	PRISM_API_POINT();
	PRISM_MOD_CHECK(prism_regions > 0, pairing, "amba_release_regions without a successful amba_request_regions");
	if (prism_regions > 0)
		prism_regions--;
}

#endif
