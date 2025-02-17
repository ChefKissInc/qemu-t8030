#ifndef HW_SSI_APPLE_SPI_H
#define HW_SSI_APPLE_SPI_H

#include "hw/arm/apple-silicon/dtb.h"
#include "hw/sysbus.h"
#include "qom/object.h"

#define APPLE_SPI_MMIO_SIZE (0x4000)

#define TYPE_APPLE_SPI "apple.spi"
OBJECT_DECLARE_SIMPLE_TYPE(AppleSPIState, APPLE_SPI)

SysBusDevice *apple_spi_create(DTBNode *node);
SSIBus *apple_spi_get_bus(AppleSPIState *s);
#endif /* HW_SSI_APPLE_SPI_H */
