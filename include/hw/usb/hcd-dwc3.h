/*
 * QEMU model of the USB DWC3 dual-role controller emulation.
 *
 * Copyright (c) 2022 Nguyen Hoang Trung (TrungNguyen1909)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#ifndef HCD_DWC3_H
#define HCD_DWC3_H

#include "qemu/osdep.h"
#include "hw/register.h"
#include "hw/usb/hcd-xhci-sysbus.h"
#include "qemu/queue.h"
#include "sysemu/dma.h"

#define DWC3_SIZE 0x10000
#define DWC3_MMIO_SIZE 0x10000
#define DWC3_NUM_INTRS (16)
#define DWC3_NUM_EPS (16)

typedef struct DWC3EventRing {
    uint32_t size;
    uint32_t head;
    uint32_t count;
} DWC3EventRing;

typedef struct DWC3TRB {
    dma_addr_t bp;
    dma_addr_t addr;
    union {
        uint32_t status;
        struct QEMU_PACKED {
            uint32_t size : 24;
            uint32_t pcm : 2;
            uint32_t reserved27_26 : 2;
            uint32_t trbsts : 4;
        };
    };
    uint32_t ctrl;
} DWC3TRB;

typedef struct DWC3BufferDesc {
    DWC3TRB *trbs;
    QTAILQ_ENTRY(DWC3BufferDesc) queue;
    QEMUSGList sgl;
    QEMUIOVector iov;
    int epid;
    uint32_t count;
    uint32_t length;
    uint32_t actual_length;
    DMADirection dir;
    bool mapped;
    bool ended;
} DWC3BufferDesc;

typedef struct DWC3Transfer {
    dma_addr_t tdaddr;
    QTAILQ_HEAD(, DWC3BufferDesc) buffers;
    int epid;
    uint32_t count;
    uint32_t rsc_idx;
} DWC3Transfer;

typedef struct DWC3Endpoint {
    USBEndpoint *uep;
    DWC3Transfer *xfer;
    int epid;
    uint32_t epnum;
    uint32_t intrnum;
    uint32_t event_en;
    uint32_t xfer_resource_idx;
    uint8_t dseqnum;
    bool stalled;
    bool not_ready;
} DWC3Endpoint;

typedef struct DWC3DeviceState {
    USBDevice parent_obj;
} DWC3DeviceState;

typedef struct DWC3State {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    AddressSpace dma_as;
    XHCISysbusState sysbus_xhci;
    struct DWC3DeviceState device;
    qemu_irq irq;

    DWC3EventRing intrs[DWC3_NUM_INTRS];
    uint32_t numintrs;
    DWC3Endpoint eps[DWC3_NUM_EPS];
    bool host_intr_state[DWC3_NUM_INTRS];

    union {
#define DWC3_GLBREG_SIZE 0x504
        uint32_t glbreg[DWC3_GLBREG_SIZE / sizeof(uint32_t)];
        struct {
            uint32_t gsbuscfg0; /* c100 */
            uint32_t gsbuscfg1; /* c104 */
            uint32_t gtxthrcfg; /* c108 */
            uint32_t grxthrcfg; /* c10c */
            uint32_t gctl; /* c110 */
            uint32_t __padc104; /* c114 */
            uint32_t gsts; /* c118 */
            uint32_t __padc11c; /* c11c */
            uint32_t gsnpsid; /* c120 */
            uint32_t ggpio; /* c124 */
            uint32_t guid; /* c128 */
            uint32_t guctl; /* c12c */
            uint32_t gbuserraddrlo; /* c130 */
            uint32_t gbuserraddrhi; /* c134 */
            uint32_t gprtbimaplo; /* c138 */
            uint32_t gprtbimaphi; /* c13c */
            uint32_t ghwparams0; /* c140 */
            uint32_t ghwparams1; /* c144 */
            uint32_t ghwparams2; /* c148 */
            uint32_t ghwparams3; /* c14c */
            uint32_t ghwparams4; /* c150 */
            uint32_t ghwparams5; /* c154 */
            uint32_t ghwparams6; /* c158 */
            uint32_t ghwparams7; /* c15c */
            uint8_t __padc160[0x20]; /* c160 */
            uint32_t gprtbimap_hs_lo; /* c180 */
            uint32_t gprtbimap_hs_hi; /* c184 */
            uint32_t gprtbimap_fs_lo; /* c188 */
            uint32_t gprtbimap_fs_hi; /* c18c */
            uint8_t __pad190[0x70]; /* c190 */
            uint32_t gusb2phycfg; /* c200 */
            uint8_t __padc204[0x7c]; /* c204 */
            uint32_t gusb2phyacc; /* c280 */
            uint8_t __padc284[0x3c]; /* c284 */
            uint32_t gusb3pipectl; /* c2c0 */
            uint8_t __padc2c4[0x3c]; /* c2c4 */
            uint32_t gtxfifosiz[0x20]; /* c300 */
            uint32_t grxfifosiz[0x20]; /* c380 */
            uint32_t gevntregs[0x80]; /* c400 */
#define gevntadr_lo(_ch) gevntregs[((_ch) << 2) + 0] /* c400, c410, ... */
#define gevntadr_hi(_ch) gevntregs[((_ch) << 2) + 1] /* c404, c414, ... */
#define gevntsiz(_ch) gevntregs[((_ch) << 2) + 2] /* c408, c418, ... */
#define gevntcount(_ch) gevntregs[((_ch) << 2) + 3] /* c40c, c41c, ... */
            uint32_t ghwparams8; /* c600 */
        };
    };

    union {
#define DWC3_DREG_SIZE 0x24
        uint32_t dreg[DWC3_DREG_SIZE / sizeof(uint32_t)];
        struct {
            uint32_t dcfg; /* c700 */
            uint32_t dctl; /* c704 */
            uint32_t devten; /* c708 */
            uint32_t dsts; /* c70c */
            uint32_t dgcmdpar; /* c710 */
            uint32_t dgcmd; /* c714 */
            uint32_t __padc718[2]; /* c718 */
            uint32_t dalepena; /* c720 */
        };
    };
    union {
#define DWC3_DEPCMDREG_SIZE 0x400
        uint32_t depcmdreg[DWC3_DEPCMDREG_SIZE / sizeof(uint32_t)];
        struct {
#define depcmdpar2(_ch) depcmdreg[((_ch) << 2) + 0] /* c800, c810, ... */
#define depcmdpar1(_ch) depcmdreg[((_ch) << 2) + 1] /* c804, c814, ... */
#define depcmdpar0(_ch) depcmdreg[((_ch) << 2) + 2] /* c808, c818, ... */
#define depcmd(_ch) depcmdreg[((_ch) << 2) + 3] /* c80c, c81c, ... */
        };
    };
} DWC3State;


struct DWC3Class {
    /*< private >*/
    SysBusDeviceClass parent_class;
    ResettablePhases parent_phases;

    /*< public >*/
};

#define TYPE_DWC3_USB_DEVICE "dwc3-usb-device"
OBJECT_DECLARE_TYPE(DWC3DeviceState, USBDeviceClass, DWC3_USB_DEVICE)

#define TYPE_DWC3_USB "dwc3-usb"
OBJECT_DECLARE_TYPE(DWC3State, DWC3Class, DWC3_USB)

#endif
