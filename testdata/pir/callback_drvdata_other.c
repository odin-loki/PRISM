// --pir-callback-models drvdata: a driver whose probe stores something other than one allocation in driver_data (a
// static object) gets no model; its remove is checked exactly as without the option. Rows: callback_models.tsv.
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

static void dev_set_drvdata(struct device* dev, void* data) { dev->driver_data = data; }
static void* dev_get_drvdata(struct device* dev) { return dev->driver_data; }
static void platform_set_drvdata(struct platform_device* p, void* d) { dev_set_drvdata(&p->dev, d); }
static void* platform_get_drvdata(struct platform_device* p) { return dev_get_drvdata(&p->dev); }

struct priv {
    int regs[8];
};
static struct priv the_state;

static int other_probe(struct platform_device* pdev) {
    platform_set_drvdata(pdev, &the_state);
    return 0;
}
static int other_remove(struct platform_device* pdev) {
    struct priv* p = platform_get_drvdata(pdev);
    memset(p, 0, sizeof(struct priv));
    return 0;
}

struct platform_driver other_driver = {other_probe, other_remove, 0};
