// --pir-callback-models drvdata (docs/DRIVER_ENV_MODELS.md): the probe-first ordering. The driver's probe stores a
// devm_kzalloc'd struct priv in driver_data; its other callbacks read it back. Rows: callback_models.tsv.
// Not a kernel unit: the struct and helper names mimic the kernel's (the -O0 IR of the real helpers has the same shape).
#include <string.h>

struct device {
    const char* name;
    void* driver_data;
};
struct platform_device {
    int id;
    struct device dev;
};
struct platform_driver {
    int (*probe)(struct platform_device*);
    int (*remove)(struct platform_device*);
    int (*shutdown)(struct platform_device*);
};
struct dev_pm_ops {
    int (*suspend)(struct device*);
};

void* devm_kzalloc(struct device* dev, unsigned long size, unsigned gfp);
static void dev_set_drvdata(struct device* dev, void* data) { dev->driver_data = data; }
static void* dev_get_drvdata(struct device* dev) { return dev->driver_data; }
static void platform_set_drvdata(struct platform_device* p, void* d) { dev_set_drvdata(&p->dev, d); }
static void* platform_get_drvdata(struct platform_device* p) { return dev_get_drvdata(&p->dev); }

struct priv {
    int regs[8];
};
struct priv_big {  // a larger view of the same object: a bug in the driver
    int regs[8];
    int extra;
};

static int my_probe(struct platform_device* pdev) {
    struct priv* p = devm_kzalloc(&pdev->dev, sizeof(*p), 0);
    if (!p) return -12;
    p->regs[0] = 1;
    platform_set_drvdata(pdev, p);
    return 0;
}
// model-dependent: clears the whole private object; without the model driver_data is an arbitrary pointer
static int my_remove(struct platform_device* pdev) {
    struct priv* p = platform_get_drvdata(pdev);
    memset(p, 0, sizeof(struct priv));
    return 0;
}
// a real violation the model does not remove: regs[0] may be 0 (the object's contents are arbitrary)
static int my_shutdown(struct platform_device* pdev) {
    struct priv* p = platform_get_drvdata(pdev);
    return 100 / p->regs[0];
}
// a real violation the model reveals: the probe allocated a struct priv (32 bytes), this reads byte 32..35
static int my_suspend(struct device* dev) {
    struct priv_big* q = dev_get_drvdata(dev);
    return q->extra;
}

struct platform_driver my_driver = {my_probe, my_remove, my_shutdown};
struct dev_pm_ops my_pm = {my_suspend};
