# Module description: Linux drivers/rtc/rtc-pl031.c (ARM PrimeCell PL031 RTC, ARM variant)
# against the PL031 device model. Entry points come from the driver's registration tables
# (struct amba_driver pl031_driver: .probe .remove; struct pl031_vendor_data arm_pl031: .ops).
module rtc_pl031;
kernel linux;
driver "drivers/rtc/rtc-pl031.c";
device builtin:pl031;
stubs amba rtc;
steps 2;
irq_points api steps;
irq_spurious auto;
teardown_ops yes;
assume "the driver is the ARM variant: probe is called with pl031_ids[0] (id 0x00041031, vendor data arm_pl031); the ST variants stv1/stv2 are outside this device model";
assume "the AMBA device has one 4 KiB resource at the device window and irq[0] is either 0 (no interrupt) or a nonzero line";
assume "the RTC core calls ops only while the rtc is registered, with a valid struct device and a calendar-valid rtc_time (rtc_valid_tm) / rtc_wkalrm";

env {
	static struct amba_device adev;
	static struct rtc_time tm;
	static struct rtc_wkalrm alrm;

	/* the RTC core calls what the driver registered: check the registered ops member is the function this operation exercises
	 * (the operation then calls it directly, which avoids inlining every function of the same signature at an indirect call) */
#define PRISM_OP_REGISTERED(member, fn) PRISM_MOD_CHECK(prism_rtc_dev.ops->member == (fn), api, "registered ops member is not the function this operation exercises")

	/* what rtc_valid_tm accepts: the core rejects anything else before set_time / set_alarm */
	static void prism_fill_valid_tm(struct rtc_time *t)
	{
		t->tm_sec = (int)prism_nondet_choice(60);
		t->tm_min = (int)prism_nondet_choice(60);
		t->tm_hour = (int)prism_nondet_choice(24);
		t->tm_mday = 1 + (int)prism_nondet_choice(31);
		t->tm_mon = (int)prism_nondet_choice(12);
		t->tm_year = (int)prism_nondet_choice(200);
		t->tm_wday = (int)prism_nondet_choice(7);
		t->tm_yday = (int)prism_nondet_choice(366);
		t->tm_isdst = 0;
	}
}

setup {
	adev.res.start = 0;
	adev.res.end = 0xfff;
	adev.irq[0] = prism_nondet_bool() ? 33 : 0;
}

probe { return pl031_probe(&adev, &pl031_ids[0]); }
remove { pl031_remove(&adev); }

op read_time {
	if (!prism_rtc_registered) return;
	PRISM_OP_REGISTERED(read_time, pl031_read_time);
	(void)pl031_read_time(&adev.dev, &tm);
}
op set_time {
	if (!prism_rtc_registered) return;
	PRISM_OP_REGISTERED(set_time, pl031_set_time);
	prism_fill_valid_tm(&tm);
	(void)pl031_set_time(&adev.dev, &tm);
}
op read_alarm {
	if (!prism_rtc_registered) return;
	PRISM_OP_REGISTERED(read_alarm, pl031_read_alarm);
	(void)pl031_read_alarm(&adev.dev, &alrm);
}
op set_alarm {
	if (!prism_rtc_registered) return;
	PRISM_OP_REGISTERED(set_alarm, pl031_set_alarm);
	prism_fill_valid_tm(&alrm.time);
	alrm.enabled = (unsigned char)prism_nondet_choice(2);
	alrm.pending = 0;
	(void)pl031_set_alarm(&adev.dev, &alrm);
}
op alarm_irq_enable {
	if (!prism_rtc_registered) return;
	PRISM_OP_REGISTERED(alarm_irq_enable, pl031_alarm_irq_enable);
	(void)pl031_alarm_irq_enable(&adev.dev, prism_nondet_choice(2));
}
