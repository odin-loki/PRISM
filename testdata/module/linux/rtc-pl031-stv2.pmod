# Module description: Linux drivers/rtc/rtc-pl031.c, the ST v2 variant (vendor data stv2_pl031).
# Used for the hostile-device and missing-precondition triage of docs/DEVICE_MODEL.md section 9.3:
#   prism-module run ... --device none                 the hardware is arbitrary
#   prism-module run ... --define RAW_TM               the RTC core hands over any struct rtc_time
module rtc_pl031_stv2;
kernel linux;
driver "drivers/rtc/rtc-pl031.c";
device builtin:pl031_stv2;
stubs amba rtc;
steps 2;
irq_points api steps;
irq_spurious auto;
teardown_ops yes;
assume "the driver is the ST v2 variant: probe is called with pl031_ids[2] (id 0x00280031, vendor data stv2_pl031)";
assume "the AMBA device has one 4 KiB resource at the device window and irq[0] is either 0 or a nonzero line";
assume "the RTC core calls ops only while the rtc is registered; unless RAW_TM is defined it passes a calendar-valid rtc_time (rtc_valid_tm)";

env {
	static struct amba_device adev;
	static struct rtc_time tm;
	static struct rtc_wkalrm alrm;

	static void prism_fill_tm(struct rtc_time *t)
	{
#ifdef RAW_TM
		t->tm_sec = prism_nondet_int();
		t->tm_min = prism_nondet_int();
		t->tm_hour = prism_nondet_int();
		t->tm_mday = prism_nondet_int();
		t->tm_mon = prism_nondet_int();
		t->tm_year = prism_nondet_int();
		t->tm_wday = prism_nondet_int();
		t->tm_yday = prism_nondet_int();
#else
		t->tm_sec = (int)prism_nondet_choice(60);
		t->tm_min = (int)prism_nondet_choice(60);
		t->tm_hour = (int)prism_nondet_choice(24);
		t->tm_mday = 1 + (int)prism_nondet_choice(31);
		t->tm_mon = (int)prism_nondet_choice(12);
		t->tm_year = (int)prism_nondet_choice(200);
		t->tm_wday = (int)prism_nondet_choice(7);
		t->tm_yday = (int)prism_nondet_choice(366);
#endif
		t->tm_isdst = 0;
	}
#define PRISM_OP_REGISTERED(member, fn) PRISM_MOD_CHECK(prism_rtc_dev.ops->member == (fn), api, "registered ops member is not the function this operation exercises")
}

setup {
	adev.res.start = 0;
	adev.res.end = 0xfff;
	adev.irq[0] = prism_nondet_bool() ? 33 : 0;
}

probe { return pl031_probe(&adev, &pl031_ids[2]); }
remove { pl031_remove(&adev); }

op read_time {
	if (!prism_rtc_registered) return;
	PRISM_COVER_POINT(PRISM_OP_COVER);
	PRISM_OP_REGISTERED(read_time, pl031_stv2_read_time);
	(void)pl031_stv2_read_time(&adev.dev, &tm);
}
op set_time {
	if (!prism_rtc_registered) return;
	PRISM_COVER_POINT(PRISM_OP_COVER);
	PRISM_OP_REGISTERED(set_time, pl031_stv2_set_time);
	prism_fill_tm(&tm);
	(void)pl031_stv2_set_time(&adev.dev, &tm);
}
op read_alarm {
	if (!prism_rtc_registered) return;
	PRISM_COVER_POINT(PRISM_OP_COVER);
	PRISM_OP_REGISTERED(read_alarm, pl031_stv2_read_alarm);
	(void)pl031_stv2_read_alarm(&adev.dev, &alrm);
}
op set_alarm {
	if (!prism_rtc_registered) return;
	PRISM_COVER_POINT(PRISM_OP_COVER);
	PRISM_OP_REGISTERED(set_alarm, pl031_stv2_set_alarm);
	prism_fill_tm(&alrm.time);
	alrm.enabled = (unsigned char)prism_nondet_choice(2);
	alrm.pending = 0;
	(void)pl031_stv2_set_alarm(&adev.dev, &alrm);
}
