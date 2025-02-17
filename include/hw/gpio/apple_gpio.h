#ifndef APPLE_GPIO_H
#define APPLE_GPIO_H

#include "hw/arm/apple-silicon/dtb.h"
#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_APPLE_GPIO "apple.gpio"
OBJECT_DECLARE_SIMPLE_TYPE(AppleGPIOState, APPLE_GPIO)

typedef struct {
    uint32_t value;
    bool interrupted;
} AppleGPIOPinState;

struct AppleGPIOState {
    SysBusDevice parent_obj;
    MemoryRegion *iomem;
    uint64_t pin_count;
    uint64_t irq_group_count;
    qemu_irq *irqs;
    qemu_irq *out;
    uint32_t *gpio_cfg;
    uint32_t **int_config;
    uint32_t *in;
    uint32_t *in_old;
    uint32_t npl;
};

DeviceState *apple_custom_gpio_create(char *name, uint64_t mmio_size,
                                      uint32_t gpio_pins,
                                      uint32_t gpio_int_groups);
DeviceState *apple_gpio_create(DTBNode *node);
#endif
