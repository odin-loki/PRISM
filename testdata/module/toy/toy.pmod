# Module description for the toy driver and the toy device model.
module toy;
kernel toy;
driver "toy_driver.c";
device "toy.pdm";
steps 2;
irq_points api steps;
irq_spurious auto;
teardown_ops no;
assume "the toy bus calls start/chan/data only between a successful probe and remove";

env {
	static struct device tdev_dev;
}
setup { }
probe { return toy_probe(&tdev_dev); }
remove { toy_remove(&tdev_dev); }
op start { (void)toy_start(&tdev_dev); }
op chan { (void)toy_chan(&tdev_dev); }
op data { toy_data(&tdev_dev); }
