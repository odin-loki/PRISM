/* PRISM device-model harness, Linux flavour: the RTC class core, as far as a
 * driver sees it. A registered device may have its ops called by user space
 * until devres unregisters it (that is when prism_rtc_registered drops).
 *
 * Models taken from drivers/rtc/class.c, lib/rtc.c (Linux 6.18); each is a
 * restriction of what the real function does, stated per function. */
#ifndef PRISM_LINUX_RTC_H
#define PRISM_LINUX_RTC_H

#include <linux/rtc.h>
#include <linux/bcd.h>

static struct rtc_device prism_rtc_dev;
static int prism_rtc_allocated;
static int prism_rtc_registered;

struct rtc_device *devm_rtc_allocate_device(struct device *dev)
{
	(void)dev;
	if (prism_nondet_bool())
		return ERR_PTR(-12);                            /* -ENOMEM */
	prism_rtc_allocated = 1;
	return &prism_rtc_dev;
}

int __devm_rtc_register_device(struct module *owner, struct rtc_device *rtc)
{
	(void)owner;
	PRISM_MOD_CHECK(rtc == &prism_rtc_dev && prism_rtc_allocated, api, "devm_rtc_register_device of a device that devm_rtc_allocate_device did not return");
	PRISM_MOD_CHECK(rtc->ops != (const struct rtc_class_ops *)0, api, "devm_rtc_register_device with no ops set (rtc_class registers it with ops == NULL)");
	if (prism_nondet_bool())
		return -19;                                     /* -ENODEV */
	prism_rtc_registered = 1;
	prism_dr_add(PRISM_DR_CLEAR, &prism_rtc_registered);   /* devres unregisters the device */
	return 0;
}

void rtc_update_irq(struct rtc_device *rtc, unsigned long num, unsigned long events)
{
	(void)num; (void)events;
	PRISM_MOD_CHECK(rtc == &prism_rtc_dev, api, "rtc_update_irq on a device that is not the allocated rtc_device");
}

/* lib/rtc.c rtc_time64_to_tm: the result is a valid broken-down time; here
 * any calendar-valid field combination (the real function computes it from
 * the argument; this over-approximates the set of results). */
void rtc_time64_to_tm(time64_t time, struct rtc_time *tm)
{
	(void)time;
	tm->tm_sec = (int)prism_nondet_choice(60);
	tm->tm_min = (int)prism_nondet_choice(60);
	tm->tm_hour = (int)prism_nondet_choice(24);
	tm->tm_mday = 1 + (int)prism_nondet_choice(31);
	tm->tm_mon = (int)prism_nondet_choice(12);
	tm->tm_year = (int)prism_nondet_choice(200);
	tm->tm_wday = (int)prism_nondet_choice(7);
	tm->tm_yday = (int)prism_nondet_choice(366);
	tm->tm_isdst = 0;
}

/* lib/rtc.c rtc_tm_to_time64: mktime64 of the fields; any result (reads every field) */
time64_t rtc_tm_to_time64(struct rtc_time *tm)
{
	int sink = tm->tm_sec + tm->tm_min + tm->tm_hour + tm->tm_mday + tm->tm_mon + tm->tm_year;
	(void)sink;
	return (time64_t)prism_nondet_u64();
}

/* lib/rtc.c rtc_year_days, copied: indexes rtc_ydays[leap][month] */
static const unsigned short prism_rtc_ydays[2][13] = {
	{0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365},
	{0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366}
};
int rtc_year_days(unsigned int day, unsigned int month, unsigned int year)
{
	int leap = (!(year % 4) && (year % 100)) || !(year % 400);
	return prism_rtc_ydays[leap][month] + day - 1;
}


/* lib/bcd.c, copied (Linux 6.18) */
unsigned _bcd2bin(unsigned char val)
{
	return (val & 0x0f) + (val >> 4) * 10;
}

unsigned char _bin2bcd(unsigned val)
{
	return ((val / 10) << 4) + val % 10;
}
#endif
