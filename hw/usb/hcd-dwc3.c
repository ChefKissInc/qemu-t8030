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

#include "qemu/osdep.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "hw/usb/dwc3-regs.h"
#include "hw/usb/hcd-dwc3.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qom/object.h"
#include "trace.h"

// #define DEBUG_DWC3

#ifdef DEBUG_DWC3
#define DPRINTF(fmt, ...)                                   \
    do {                                                    \
        qemu_log_mask(LOG_GUEST_ERROR, fmt, ##__VA_ARGS__); \
    } while (0)
#else
#define DPRINTF(fmt, ...) \
    do {                  \
    } while (0)
#endif

#ifdef DEBUG_DWC3
static const char *TRBControlType_names[] = {
    [TRBCTL_RESERVED] = "TRBCTL_RESERVED",
    [TRBCTL_NORMAL] = "TRBCTL_NORMAL",
    [TRBCTL_CONTROL_SETUP] = "TRBCTL_CONTROL_SETUP",
    [TRBCTL_CONTROL_STATUS2] = "TRBCTL_CONTROL_STATUS2",
    [TRBCTL_CONTROL_STATUS3] = "TRBCTL_CONTROL_STATUS3",
    [TRBCTL_CONTROL_DATA] = "TRBCTL_CONTROL_DATA",
    [TRBCTL_ISOCHRONOUS_FIRST] = "TRBCTL_ISOCHRONOUS_FIRST",
    [TRBCTL_ISOCHRONOUS] = "TRBCTL_ISOCHRONOUS",
    [TRBCTL_LINK_TRB] = "TRBCTL_LINK_TRB",
};
#endif

static void dwc3_device_event(DWC3State *s, struct dwc3_event_devt devt);
static void dwc3_ep_event(DWC3State *s, int epid,
                          struct dwc3_event_depevt depevt);
static void dwc3_event(DWC3State *s, union dwc3_event event, int v);
static void dwc3_ep_run(DWC3State *s, DWC3Endpoint *ep);

static inline dma_addr_t dwc3_addr64(uint32_t low, uint32_t high)
{
    if (sizeof(dma_addr_t) == 4) {
        return low;
    } else {
        return low | (((dma_addr_t)high << 16) << 16);
    }
}

static int dwc3_packet_find_epid(DWC3State *s, USBPacket *p)
{
    if (p->ep->nr == 0) {
        switch (p->pid) {
        case USB_TOKEN_SETUP:
        case USB_TOKEN_OUT:
            return 0;
            break;
        case USB_TOKEN_IN:
            return 1;
            break;
        default:
            g_assert_not_reached();
            break;
        }
    }

    for (int i = 0; i < DWC3_NUM_EPS; i++) {
        if (s->eps[i].uep == p->ep) {
            return i;
        }
    }
    return -1;
}

static void dwc3_update_irq(DWC3State *s)
{
    int ip = 0;
    for (int i = 0; i < s->numintrs; i++) {
        int level = 1;
        level &= !(s->gevntsiz(i) & GEVNTSIZ_EVNTINTRPTMASK);
        level &= (s->intrs[i].count > 0);
        qemu_set_irq(s->sysbus_xhci.irq[i], level);
        ip |= level;
    }
    if (ip) {
        s->gsts |= GSTS_DEVICE_IP;
    } else {
        s->gsts &= ~GSTS_DEVICE_IP;
    }
}

static bool dwc3_host_intr_raise(XHCIState *xhci, int n, bool level)
{
    XHCISysbusState *xhci_sysbus = container_of(xhci, XHCISysbusState, xhci);
    DWC3State *s = container_of(xhci_sysbus, DWC3State, sysbus_xhci);
    bool host_ip = false;

    s->host_intr_state[n] = level;
    for (int i = 0; i < DWC3_NUM_INTRS; i++) {
        if (s->host_intr_state[i]) {
            host_ip = true;
            break;
        }
    }
    if (host_ip) {
        s->gsts |= GSTS_HOST_IP;
    } else {
        s->gsts &= ~GSTS_HOST_IP;
    }
    qemu_set_irq(xhci_sysbus->irq[n], level);

    return false;
}

#ifdef DEBUG_DWC3
static void dwc3_td_dump(DWC3Transfer *xfer)
{
    DWC3BufferDesc *desc;
    int k = 0;

    DPRINTF("Dumping td 0x%x (0x" HWADDR_FMT_plx "):\n", xfer->rsc_idx, xfer->tdaddr);
    if (QTAILQ_EMPTY(&xfer->buffers)) {
        DPRINTF("<empty>\n");
        return;
    }

    (void)TRBControlType_names;
    QTAILQ_FOREACH (desc, &xfer->buffers, queue) {
        DPRINTF("Buffer Desc %d:\n", ++k);
        for (int i = 0; i < desc->count; i++) {
            DPRINTF("\tTRB %d @ 0x" HWADDR_FMT_plx ":\n", i, desc->trbs[i].addr);
            DPRINTF("\t\tbp: 0x" HWADDR_FMT_plx "\n", desc->trbs[i].bp);
            DPRINTF("\t\tsize: 0x%x\n", desc->trbs[i].size);
            DPRINTF("\t\tcontrol: 0x%x (%s %s %s %s %s %s %s sid: %d)\n",
                    desc->trbs[i].ctrl,
                    (desc->trbs[i].ctrl & TRB_CTRL_HWO) ? "HWO" : "",
                    (desc->trbs[i].ctrl & TRB_CTRL_LST) ? "LST" : "",
                    (desc->trbs[i].ctrl & TRB_CTRL_CHN) ? "CHN" : "",
                    (desc->trbs[i].ctrl & TRB_CTRL_CSP) ? "CSP" : "",
                    TRBControlType_names[TRB_CTRL_TRBCTL(desc->trbs[i].ctrl)],
                    (desc->trbs[i].ctrl & TRB_CTRL_ISP_IMI) ? "ISP_IMI" : "",
                    (desc->trbs[i].ctrl & TRB_CTRL_IOC) ? "IOC" : "",
                    TRB_CTRL_SID_SOFN(desc->trbs[i].ctrl));
        }
    }
}
#endif

static int dwc3_bd_length(DWC3State *s, dma_addr_t tdaddr)
{
    struct dwc3_trb trb = { 0 };
    int length = 0;

    while (1) {
        if (dma_memory_read(&s->dma_as, tdaddr, &trb, sizeof(trb),
                            MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: failed to read trb\n",
                          __func__);
            return 0;
        }
        if (!(trb.ctrl & TRB_CTRL_HWO)) {
            return -length;
        }

        if (TRB_CTRL_TRBCTL(trb.ctrl) == TRBCTL_LINK_TRB) {
            return length;
        }
        length++;
        tdaddr += sizeof(trb);
        if (trb.ctrl & TRB_CTRL_LST) {
            return -length;
        }
    }
}

static void dwc3_bd_map(DWC3State *s, DWC3BufferDesc *desc, USBPacket *p)
{
    DMADirection dir = (p->pid == USB_TOKEN_IN ? DMA_DIRECTION_TO_DEVICE :
                                                 DMA_DIRECTION_FROM_DEVICE);
    void *mem;
    int i;
    bool faulted = false;

    if (desc->mapped) {
        return;
    }
    desc->dir = dir;
    for (i = 0; i < desc->sgl.nsg && !faulted; i++) {
        dma_addr_t base = desc->sgl.sg[i].base;
        dma_addr_t len = desc->sgl.sg[i].len;

        while (len) {
            dma_addr_t xlen = len;
            mem = dma_memory_map(desc->sgl.as, base, &xlen, dir,
                                 MEMTXATTRS_UNSPECIFIED);
            if (!mem) {
                // is it correct to set mapped to true even in this error case?
                qemu_log_mask(LOG_GUEST_ERROR, "%s: !mem: base: 0x%" HWADDR_PRIx " len: 0x%" HWADDR_PRIx "\n", __func__, base, len);
                faulted = true;
                s->gbuserraddrlo = base;
                s->gbuserraddrhi = base >> 32;
                break;
            }
            if (xlen > len) {
                xlen = len;
            }
            qemu_iovec_add(&desc->iov, mem, xlen);
            len -= xlen;
            base += xlen;
        }
    }
    desc->mapped = true;
    desc->actual_length = 0;
}

static void dwc3_bd_unmap(DWC3State *s, DWC3BufferDesc *desc)
{
    if (!desc->mapped) {
        return;
    }
    desc->mapped = false;
    for (int i = 0; i < desc->iov.niov; i++) {
        if (desc->iov.iov[i].iov_base) {
            dma_memory_unmap(&s->dma_as, desc->iov.iov[i].iov_base,
                             desc->iov.iov[i].iov_len, desc->dir, 0);
            desc->iov.iov[i].iov_base = 0;
        }
    }
}

static bool dwc3_bd_writeback(DWC3State *s, DWC3BufferDesc *desc, USBPacket *p,
                              bool buserr)
{
    int i = 0;
    int j = 0;
    int length = desc->actual_length;
    int unmap_length = desc->actual_length;
    struct dwc3_event_depevt event = { .endpoint_number = desc->epid };
    USBPacket *next_p = QTAILQ_NEXT(p, queue);
    bool setupPending = (next_p && next_p->pid == USB_TOKEN_SETUP);
    bool short_packet = p->pid != USB_TOKEN_IN &&
                        (usb_packet_size(p) % p->ep->max_packet_size != 0 ||
                         usb_packet_size(p) == 0);

    //desc->mapped = false;
    while (j < desc->iov.niov && unmap_length > 0) {
        int access_len = desc->iov.iov[j].iov_len;
        if (access_len > unmap_length) {
            access_len = unmap_length;
        }

        if (desc->iov.iov[j].iov_base) {
            dma_memory_unmap(&s->dma_as, desc->iov.iov[j].iov_base,
                             desc->iov.iov[j].iov_len, desc->dir, access_len);
            desc->iov.iov[j].iov_base = 0;
        }
        unmap_length -= access_len;
        j++;
    }

    while (i < desc->count && event.endpoint_event != DEPEVT_XFERCOMPLETE) {
        DWC3TRB *trb = &desc->trbs[i];
        if (trb->ctrl & TRB_CTRL_HWO) {
            if (length > trb->size) {
                trb->size = 0;
                length -= trb->size;
            } else {
                trb->size -= length;
                length = 0;
            }

            if (setupPending) {
                trb->trbsts = TRBSTS_SETUP_PENDING;
            } else {
                trb->trbsts = TRBSTS_OK;
            }

            trb->ctrl &= ~TRB_CTRL_HWO;

            dma_memory_write(&s->dma_as, trb->addr + 0x8, &trb->status,
                             sizeof(trb->status), MEMTXATTRS_UNSPECIFIED);
            dma_memory_write(&s->dma_as, trb->addr + 0xc, &trb->ctrl,
                             sizeof(trb->ctrl), MEMTXATTRS_UNSPECIFIED);
            if (length <= 0 && buserr) {
                event.status |= DEPEVT_STATUS_BUSERR;
            }
            if (p->pid == USB_TOKEN_IN) {
            /* IN token */
            trb_complete:
                if (trb->size == 0) {
                    if (trb->ctrl & TRB_CTRL_LST) {
                        event.endpoint_event = DEPEVT_XFERCOMPLETE;
                        event.status |= DEPEVT_STATUS_LST;
                        if (trb->ctrl & TRB_CTRL_IOC) {
                            event.status |= DEPEVT_STATUS_IOC;
                        }
                    } else if (trb->ctrl & TRB_CTRL_IOC) {
                        event.endpoint_event = DEPEVT_XFERINPROGRESS;
                        event.status |= DEPEVT_STATUS_IOC;
                    }
                    dwc3_ep_event(s, desc->epid, event);
                }
            } else {
                /* OUT token */
                if (length <= 0 && short_packet) {
                    event.status |= DEPEVT_STATUS_SHORT;
                    if (trb->ctrl & TRB_CTRL_CSP) {
                        bool ioc = trb->ctrl & TRB_CTRL_IOC;
                        bool isp = trb->ctrl & TRB_CTRL_ISP_IMI;
                        switch (trb->ctrl & (TRB_CTRL_CHN | TRB_CTRL_LST)) {
                        case TRB_CTRL_LST:
                            goto short_complete;
                            break;
                        case TRB_CTRL_CHN: {
                            for (j = 0; j < desc->count; j++) {
                                ioc |= (desc->trbs[j].ctrl & TRB_CTRL_IOC) != 0;
                                isp |= (desc->trbs[j].ctrl &
                                        TRB_CTRL_ISP_IMI) != 0;
                            }
                            QEMU_FALLTHROUGH;
                        }
                        case 0:
                            if (!ioc && !isp) {
                                break;
                            }
                            event.endpoint_event = DEPEVT_XFERINPROGRESS;
                            if (ioc) {
                                event.status |= DEPEVT_STATUS_IOC;
                            }
                            dwc3_ep_event(s, desc->epid, event);
                            break;
                        default:
                            g_assert_not_reached();
                            break;
                        }
                    } else {
                    /* no CSP */
                    short_complete:
                        event.endpoint_event = DEPEVT_XFERCOMPLETE;
                        if (trb->ctrl & TRB_CTRL_LST) {
                            event.status |= DEPEVT_STATUS_LST;
                        }
                        if (trb->ctrl & TRB_CTRL_IOC) {
                            event.status |= DEPEVT_STATUS_IOC;
                        }
                        dwc3_ep_event(s, desc->epid, event);
                    }
                } else if (!short_packet) {
                    goto trb_complete;
                }
            }
        }
        if (length <= 0) {
            break;
        }
        i++;
    }
    return p->actual_length == usb_packet_size(p) ||
           desc->actual_length % p->ep->max_packet_size != 0;
    // desc->trbs[i - 1].size < p->ep->max_packet_size;
}

static int dwc3_bd_copy(DWC3State *s, DWC3BufferDesc *desc, USBPacket *p)
{
    g_autofree void *buffer = NULL;
    int packet_left = usb_packet_size(p) - p->actual_length;
    int desc_left = desc->length - desc->actual_length;
    int actual_xfer = 0;
    int xfer_size;

    // assert(p->actual_length == 0);

    xfer_size = packet_left;
    if (xfer_size > desc_left) {
        xfer_size = desc_left;
    }

#if 1
    if (xfer_size == 0) {
        if (p->pid == USB_TOKEN_IN) {
    #if 1
            DPRINTF("%s NULL IN Transfer 0x%x on EP %d to 0x" HWADDR_FMT_plx "\n", __func__, xfer_size,
                    desc->epid, desc->trbs[0].bp);
            DPRINTF("%s: p: 0x%x/0x%lx\n", __func__, p->actual_length,
                    usb_packet_size(p));
    #endif
        } else {
    #if 1
            DPRINTF("%s NULL OUT Transfer 0x%x on EP %d to 0x" HWADDR_FMT_plx "\n", __func__,
                    xfer_size, desc->epid, desc->trbs[0].bp);
            DPRINTF("%s: p: 0x%x/0x%lx\n", __func__, p->actual_length,
                    usb_packet_size(p));
    #endif
        }
        usb_packet_copy(p, NULL, xfer_size);

        if (desc->length - desc->actual_length > 0 && packet_left > 0 &&
            packet_left % p->ep->max_packet_size == 0) {
            p->status = USB_RET_SUCCESS;
            DPRINTF("%s: buffer == NULL ; xfer_size 0x%x if_0: USB_RET_SUCCESS\n", __func__, xfer_size);
            return xfer_size;
        }

        desc->ended = true;
#if 0
        p->status = USB_RET_SUCCESS;
        return xfer_size;
#endif
        // dwc3_bd_writeback must be called
        if (dwc3_bd_writeback(s, desc, p, false)) {
            DPRINTF("%s: buffer == NULL ; xfer_size 0x%x if_1: USB_RET_SUCCESS\n", __func__, xfer_size);
            p->status = USB_RET_SUCCESS;
        } else {
            DPRINTF("%s: buffer == NULL ; xfer_size 0x%x if_2: USB_RET_SUCCESS\n", __func__, xfer_size);
    #if 1
            //if (desc->trbs[0].ctrl & TRB_CTRL_LST)
            if (0)
            {
                struct dwc3_event_depevt event = { .endpoint_number = desc->epid,
                                                .endpoint_event =
                                                    DEPEVT_XFERNOTREADY };
                event.status |= DEPEVT_STATUS_TRANSFER_ACTIVE;
                p->status = USB_RET_ASYNC; // fixes USBMUXD_DEFAULT_DEVICE_MODE=3
                dwc3_ep_event(s, desc->epid, event);
            } else
    #endif
            {
                p->status = USB_RET_SUCCESS; // fixes hangs for idevicesyslog and error disconnects when running "launchctl list" or "htop"/"top" using ssh, because iOS likes to send empty packets on high load (when transporting a lot of data out of iOS).
            }
        }
        return xfer_size;
    }

#endif

    dwc3_bd_map(s, desc, p);

    buffer = g_malloc0(xfer_size);
    g_assert_nonnull(buffer);
    if (p->pid == USB_TOKEN_IN) {
#if 1
        DPRINTF("%s IN Transfer 0x%x on EP %d to 0x" HWADDR_FMT_plx "\n", __func__, xfer_size,
                desc->epid, desc->trbs[0].bp);
        DPRINTF("%s: p: 0x%x/0x%lx\n", __func__, p->actual_length,
                usb_packet_size(p));
#endif
        actual_xfer = qemu_iovec_to_buf(&desc->iov, desc->actual_length, buffer,
                                        xfer_size);
        usb_packet_copy(p, buffer, xfer_size);

#if 0
#ifdef DEBUG_DWC3
        qemu_hexdump(stderr, __func__, buffer, xfer_size);
#endif
#endif
    } else {
#if 1
        DPRINTF("%s OUT Transfer 0x%x on EP %d to 0x" HWADDR_FMT_plx "\n", __func__,
                xfer_size, desc->epid, desc->trbs[0].bp);
        DPRINTF("%s: p: 0x%x/0x%lx\n", __func__, p->actual_length,
                usb_packet_size(p));
#endif
        usb_packet_copy(p, buffer, xfer_size);
        actual_xfer = qemu_iovec_from_buf(&desc->iov, desc->actual_length,
                                          buffer, xfer_size);

#if 0
#ifdef DEBUG_DWC3
        qemu_hexdump(stderr, __func__, buffer, xfer_size);
#endif
#endif
    }

    desc->actual_length += actual_xfer;
    if (desc->length - desc->actual_length > 0 && packet_left > 0 &&
        packet_left % p->ep->max_packet_size == 0) {
        DPRINTF("%s: buffer != NULL ; xfer_size 0x%x if_0: USB_RET_SUCCESS\n", __func__, xfer_size);
        p->status = USB_RET_SUCCESS;
        //dwc3_bd_unmap(s, desc); // please uncomment this only if you know what you're doing, unlike me. breaks right clicks in vnc over ssh, by leading to null-pointer crashes in glibc.
        return xfer_size;
    }

    desc->ended = true;
    if (dwc3_bd_writeback(s, desc, p, actual_xfer < xfer_size)) {
        DPRINTF("%s: buffer != NULL ; xfer_size 0x%x if_1: USB_RET_SUCCESS\n", __func__, xfer_size);
        p->status = USB_RET_SUCCESS;
    } else {
        DPRINTF("%s: buffer != NULL ; xfer_size 0x%x if_2: USB_RET_SUCCESS\n", __func__, xfer_size);
#if 1
        //if (desc->trbs[0].ctrl & TRB_CTRL_LST)
        if (1)
        {
            struct dwc3_event_depevt event = { .endpoint_number = desc->epid,
                                            .endpoint_event =
                                                DEPEVT_XFERNOTREADY };
            event.status |= DEPEVT_STATUS_TRANSFER_ACTIVE;
            p->status = USB_RET_ASYNC; // fixes USBMUXD_DEFAULT_DEVICE_MODE=3
            dwc3_ep_event(s, desc->epid, event);
        } else
#endif
        {
            p->status = USB_RET_SUCCESS; // fixes hangs for idevicesyslog and error disconnects when running "launchctl list" or "htop"/"top" using ssh, because iOS likes to send empty packets on high load (when transporting a lot of data out of iOS).
        }
    }
    //
    dwc3_bd_unmap(s, desc);
    return xfer_size;
}

static void dwc3_bd_free(DWC3State *s, DWC3BufferDesc *desc)
{
    dwc3_bd_unmap(s, desc);
    g_free(desc->trbs);
    qemu_iovec_destroy(&desc->iov);
    qemu_sglist_destroy(&desc->sgl);
    desc->trbs = NULL;
    g_free(desc);
}

static void dwc3_td_free_buffers(DWC3State *s, DWC3Transfer *xfer)
{
    while (!QTAILQ_EMPTY(&xfer->buffers)) {
        DWC3BufferDesc *desc = QTAILQ_FIRST(&xfer->buffers);
        QTAILQ_REMOVE(&xfer->buffers, desc, queue);
        xfer->count--;
        dwc3_bd_free(s, desc);
    }
}

static void dwc3_td_free(DWC3State *s, DWC3Transfer *xfer)
{
    dwc3_td_free_buffers(s, xfer);
    g_free(xfer);
}

static void dwc3_td_fetch(DWC3State *s, DWC3Transfer *xfer, dma_addr_t tdaddr)
{
    struct dwc3_trb trb = { 0 };
    int count;
    bool ended = false;

    do {
        DWC3BufferDesc *desc;

        count = dwc3_bd_length(s, tdaddr);
        if (count < 0) {
            ended = true;
            count = -count;
        }
        if (count == 0) {
            ended = true;
            break;
        }

        desc = g_new0(DWC3BufferDesc, 1);
        desc->epid = xfer->epid;
        desc->count = 0;
        desc->trbs = g_new0(DWC3TRB, count);
        desc->length = 0;
        qemu_iovec_init(&desc->iov, 1);
        qemu_sglist_init(&desc->sgl, DEVICE(s), 1, &s->dma_as);

        do {
            dma_memory_read(&s->dma_as, tdaddr, &trb, sizeof(trb),
                            MEMTXATTRS_UNSPECIFIED);

            if (!(trb.ctrl & TRB_CTRL_HWO)) {
                ended = true;
                break;
            }

            if (TRB_CTRL_TRBCTL(trb.ctrl) == TRBCTL_LINK_TRB) {
                DWC3BufferDesc *d;

                tdaddr = dwc3_addr64(trb.bpl, trb.bph);

                if (desc->trbs[0].addr <= tdaddr &&
                    tdaddr <= desc->trbs[0].addr + sizeof(trb) * (count + 1)) {
                    /* self loop */
                    ended = true;
                    break;
                }

                /* Multi Buffer Loops */
                QTAILQ_FOREACH (d, &xfer->buffers, queue) {
                    g_assert(d->count > 0 && d->trbs);
                    if (d->trbs[0].addr <= tdaddr &&
                        d->trbs[d->count - 1].addr <= tdaddr) {
                        ended = true;
                        break;
                    }
                }
                break;
            }
            if (desc->count >= count) {
                /* We don't include the link TRB in the desc count */
                ended = true;
                break;
            }
            desc->trbs[desc->count].bp = dwc3_addr64(trb.bpl, trb.bph);
            desc->trbs[desc->count].addr = tdaddr;
            desc->trbs[desc->count].status = trb.status;
            desc->trbs[desc->count].ctrl = trb.ctrl;
            qemu_sglist_add(&desc->sgl, desc->trbs[desc->count].bp,
                            desc->trbs[desc->count].size);
            desc->length += desc->trbs[desc->count].size;
            desc->count++;

            tdaddr += sizeof(trb);

            if (trb.ctrl & TRB_CTRL_LST) {
                tdaddr = -1;
                trb.ctrl &= ~TRB_CTRL_CHN;
                ended = true;
                break;
            }
        } while (!ended);
        QTAILQ_INSERT_TAIL(&xfer->buffers, desc, queue);
        xfer->count++;
    } while (!ended && xfer->count < 256);
    xfer->tdaddr = tdaddr;
#ifdef DEBUG_DWC3
#if 0
    dwc3_td_dump(xfer);
#endif
#endif
}

static DWC3Transfer *dwc3_xfer_alloc(DWC3State *s, int epid, dma_addr_t tdaddr)
{
    DWC3Transfer *xfer = g_new0(DWC3Transfer, 1);

    xfer->epid = epid;
    xfer->tdaddr = tdaddr;
    QTAILQ_INIT(&xfer->buffers);
    xfer->count = 0;
    xfer->rsc_idx = tdaddr & 0x7f;

    dwc3_td_fetch(s, xfer, tdaddr);
    return xfer;
}

static void dwc3_write_event(DWC3State *s, union dwc3_event event, int v)
{
    DWC3EventRing *intr = &s->intrs[v];
    dma_addr_t ring_base;
    dma_addr_t ev_addr;

    ring_base = dwc3_addr64(s->gevntadr_lo(v), s->gevntadr_hi(v));
    intr = &s->intrs[v];

    ev_addr =
        ring_base + qatomic_fetch_add(&intr->head, EVENT_SIZE) % intr->size;
    dma_memory_write(&s->dma_as, ev_addr, &event.raw, EVENT_SIZE,
                     MEMTXATTRS_UNSPECIFIED);
    smp_wmb();
    qatomic_add(&intr->count, EVENT_SIZE);
    smp_wmb();
}

static void dwc3_event(DWC3State *s, union dwc3_event event, int v)
{
    DWC3EventRing *intr;

    if (v >= s->numintrs) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: ring nr out of range (%d >= %d)\n",
                      __func__, v, s->numintrs);
        return;
    }
    intr = &s->intrs[v];

    if (intr->count + 1 >= intr->size) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: ring nr %d is full. "
                      "Dropping event.\n",
                      __func__, v);
        return;
    } else if (intr->count + 2 == intr->size) {
        union dwc3_event overflow = { .devt = { 1, 0, DEVT_EVNTOVERFLOW } };
        if (event.raw != overflow.raw) {
            dwc3_device_event(s, overflow.devt);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: ring nr %d is full."
                          "Sending event overflow.\n",
                          __func__, v);
        }
    } else {
        dwc3_write_event(s, event, v);
    }
    dwc3_update_irq(s);
}

static void dwc3_device_event(DWC3State *s, struct dwc3_event_devt devt)
{
    union dwc3_event event = { .devt = devt };
    int v = DCFG_INTRNUM_GET(s->dcfg);
    if (s->devten & (1 << (devt.type))) {
        dwc3_event(s, event, v);
    }
}

static void dwc3_ep_event(DWC3State *s, int epid,
                          struct dwc3_event_depevt depevt)
{
    union dwc3_event event = { .depevt = depevt };
    DWC3Endpoint *ep = &s->eps[epid];
    int v = ep->intrnum;
    DPRINTF("%s: epid: %d ev: %d raw: 0x%x\n", __func__, epid,
            depevt.endpoint_event, event.raw);
    if (depevt.endpoint_event == DEPEVT_XFERNOTREADY) {
        if (ep->not_ready) {
            return;
        }
        ep->not_ready = true;
    }
    if (ep->event_en & (1 << (depevt.endpoint_event))) {
        dwc3_event(s, event, v);
    }
}

static void dwc3_dcore_reset(DWC3State *s)
{
    USBDevice *udev = USB_DEVICE(&s->device);

    /* Clear Interrupts */
    for (int i = 0; i < s->numintrs; i++) {
        s->intrs[i].size = 0;
        s->intrs[i].head = 0;
        s->intrs[i].count = 0;
    }

    /* Clearing MMR */
    s->gsbuscfg0 = (1 << 3) | (1 << 2) | (1 << 1);
    s->gsbuscfg1 = (0xf << 8);
    s->gtxthrcfg = 0;
    s->grxthrcfg = 0;
    s->gctl = GCTL_PWRDNSCALE(0x4b0) | GCTL_PRTCAPDIR(GCTL_PRTCAP_OTG);
    s->guctl = (1 << 15) | (0x10 << 0);
    s->gbuserraddrlo = 0;
    s->gbuserraddrhi = 0;
    s->gsts &= ~GSTS_BUS_ERR_ADDR_VLD;
    s->gprtbimaplo = 0;
    s->gprtbimaphi = 0;
    s->gprtbimap_hs_lo = 0;
    s->gprtbimap_hs_hi = 0;
    s->gprtbimap_fs_lo = 0;
    s->gprtbimap_fs_hi = 0;
    s->ghwparams0 = 0x40204048 | (GHWPARAMS0_MODE_DRD);
    s->ghwparams1 = 0x222493b;
    s->ghwparams2 = 0x12345678;
    s->ghwparams3 = (0x20 << 23) | GHWPARAMS3_NUM_IN_EPS(DWC3_NUM_EPS >> 1) |
                    GHWPARAMS3_NUM_EPS(DWC3_NUM_EPS) | (0x2 << 6) | (0x3 << 2) |
                    (0x1 << 0);
    s->ghwparams4 = 0x47822004;
    s->ghwparams5 = 0x4202088;
    s->ghwparams6 = 0x7850c20;
    s->ghwparams7 = 0x0;
    s->ghwparams8 = 0x478;
    memset(s->gtxfifosiz, 0, sizeof(s->gtxfifosiz));
    memset(s->grxfifosiz, 0, sizeof(s->grxfifosiz));
    memset(s->gevntregs, 0, sizeof(s->gevntregs));
    s->dgcmdpar = 0;
    s->dgcmd = 0;
    s->dalepena = 0;
    memset(s->depcmdreg, 0, sizeof(s->depcmdreg));

    /* Terminate all USB transaction */
    for (int i = 0; i < DWC3_NUM_EPS; i++) {
        DWC3Endpoint *ep = &s->eps[i];
        USBPacket *p;

        if (ep->xfer) {
            dwc3_td_free(s, ep->xfer);
            ep->xfer = NULL;
        }

        if (ep->uep) {
            p = QTAILQ_FIRST(&ep->uep->queue);
            if (p) {
                p->status = USB_RET_IOERROR;
                usb_packet_complete(udev, p);
            }
        }
        memset(ep, 0, sizeof(*ep));
        ep->epid = i;
    }
    usb_ep_reset(udev);
}

static void dwc3_reset_enter(Object *obj, ResetType type)
{
    DWC3Class *c = DWC3_USB_GET_CLASS(obj);
    DWC3State *s = DWC3_USB(obj);

    if (c->parent_phases.enter) {
        c->parent_phases.enter(obj, type);
    }

    dwc3_dcore_reset(s);
    s->gsts = GSTS_CURMOD_DRD;
    s->gsnpsid = GSNPSID_REVISION_180A;
    s->ggpio = 0;
    s->guid = 0;
    s->gusb2phycfg = GUSB2PHYCFG_SUSPHY;
    s->gusb2phyacc = 0;
    s->gusb3pipectl = (1 << 24) | (1 << 19) | (1 << 18);
    s->dcfg = (1 << 23) | (2 << 10) | DCFG_SUPERSPEED;
    s->dsts = DSTS_COREIDLE | DSTS_USBLNKST(LINK_STATE_SS_DIS) |
              DSTS_RXFIFOEMPTY | DSTS_HIGHSPEED;
}

static void dwc3_reset_hold(Object *obj, ResetType type)
{
    DWC3Class *c = DWC3_USB_GET_CLASS(obj);
    DWC3State *s = DWC3_USB(obj);

    if (c->parent_phases.hold != NULL) {
        c->parent_phases.hold(obj, type);
    }

    dwc3_update_irq(s);
}

static void dwc3_reset_exit(Object *obj, ResetType type)
{
    DWC3Class *c = DWC3_USB_GET_CLASS(obj);
    DWC3State *s = DWC3_USB(obj);

    if (c->parent_phases.exit != NULL) {
        c->parent_phases.exit(obj, type);
    }

    USB_DEVICE(&s->device)->addr = 0;
}

static uint64_t usb_dwc3_gevntreg_read(void *ptr, hwaddr addr, int index)
{
    DWC3State *s = DWC3_USB(ptr);
    uint32_t val;
    uint32_t *mmio;
    uint32_t v = index >> 2;
    DWC3EventRing *intr = &s->intrs[v];

    if (addr >= GHWPARAMS8) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return 0;
    }

    mmio = &s->gevntregs[index];
    val = *mmio;

    switch (GEVNTADRLO(0) + (addr & 0xc)) {
    case GEVNTCOUNT(0):
        val = qatomic_read(&intr->count) & 0xffff;
        break;
    default:
        break;
    }

    return val;
}

static void usb_dwc3_gevntreg_write(void *ptr, hwaddr addr, int index,
                                    uint64_t val)
{
    DWC3State *s = DWC3_USB(ptr);
    uint32_t *mmio;
    uint32_t old;
    uint32_t v = index >> 2;
    DWC3EventRing *intr = &s->intrs[v];
    int iflg = 0;

    if (addr >= GHWPARAMS8) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return;
    }

    mmio = &s->gevntregs[index];
    old = *mmio;

    switch (GEVNTADRLO(0) + (addr & 0xc)) {
    case GEVNTSIZ(0):
        val &= (GEVNTCOUNT_EVENTSIZ_MASK | GEVNTSIZ_EVNTINTRPTMASK);
        if ((old & GEVNTCOUNT_EVENTSIZ_MASK) != 0) {
            val &= ~GEVNTCOUNT_EVENTSIZ_MASK;
            val |= (old & GEVNTCOUNT_EVENTSIZ_MASK);
        } else {
            intr->size = val & GEVNTCOUNT_EVENTSIZ_MASK;
        }
        iflg = true;
        break;
    case GEVNTCOUNT(0): {
        uint32_t dec = (val & 0xffff);
        if (dec > intr->count) {
            qatomic_set(&intr->count, 0);
        } else {
            qatomic_sub(&intr->count, dec);
        }
        iflg = true;
        break;
    }
    default:
        break;
    }

    *mmio = val;

    if (iflg) {
        dwc3_update_irq(s);
    }
}

static uint64_t usb_dwc3_glbreg_read(void *ptr, hwaddr addr, int index)
{
    DWC3State *s = DWC3_USB(ptr);
    uint32_t val;

    if (addr > GHWPARAMS8) {
#if 0
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
#endif
        return 0;
    }

    val = s->glbreg[index];

    switch (addr) {
    case GEVNTADRLO(0)... GEVNTCOUNT(15):
        val = usb_dwc3_gevntreg_read(s, addr, (addr - GEVNTADRLO(0)) >> 2);
        break;
    default:
        break;
    }
    return val;
}

static void usb_dwc3_glbreg_write(void *ptr, hwaddr addr, int index,
                                  uint64_t val)
{
    DWC3State *s = DWC3_USB(ptr);
    uint32_t *mmio;
    uint32_t old;
    int iflg = 0;

    if (addr > GHWPARAMS8) {
#if 0
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
#endif
        return;
    }

    mmio = &s->glbreg[index];
    old = *mmio;

    switch (addr) {
    case GCTL:
        if (!(old & GCTL_CORESOFTRESET) && (val & GCTL_CORESOFTRESET)) {
            device_cold_reset(DEVICE(s));
        }
        break;
    case GSTS:
        val &= (GSTS_CSR_TIMEOUT | GSTS_BUS_ERR_ADDR_VLD);
        /* clearing Write to Clear bits */
        val = old & ~val;
        break;
    case GSNPSID:
    case GGPIO:
    case GBUSERRADDR0:
    case GBUSERRADDR1:
    case GPRTBIMAP1:
    case GHWPARAMS0:
    case GHWPARAMS1:
    case GHWPARAMS2:
    case GHWPARAMS3:
    case GHWPARAMS4:
    case GHWPARAMS5:
    case GHWPARAMS6:
    case GHWPARAMS7:
    case GHWPARAMS8:
    case GPRTBIMAP_HS1:
    case GPRTBIMAP_FS1:
        val = old;
        break;
    case GPRTBIMAP0:
    case GPRTBIMAP_HS0:
    case GPRTBIMAP_FS0:
        val &= (0xf << 0);
        break;
    case GUSB2PHYCFG(0):
        val &= ~((1 << 7) | (1 << 5) | (1 << 3));
        if (!(old & GUSB2PHYCFG_PHYSOFTRST) && (val & GUSB2PHYCFG_PHYSOFTRST)) {
            /* TODO: Implement Phy Soft Reset */
            qemu_log_mask(LOG_UNIMP, "%s: Phy Soft Reset not implemented\n",
                          __func__);
            break;
        }
        if ((old & GUSB2PHYCFG_SUSPHY) != (val & GUSB2PHYCFG_SUSPHY)) {
/* TODO: Implement Phy Suspend */
#if 0
                qemu_log_mask(LOG_UNIMP, "%s: Phy (un)Suspend not implemented\n",
                              __func__);
#endif
            break;
        }
        break;
    case GUSB2PHYACC(0):
        val &= ~((1 << 26) | (1 << 24) | (1 << 23));
        break;
    case GUSB3PIPECTL(0):
        val &= ~((3 << 15));
        if (!(old & GUSB3PIPECTL_PHYSOFTRST) &&
            (val & GUSB3PIPECTL_PHYSOFTRST)) {
            /* TODO: Implement Phy Soft Reset */
            qemu_log_mask(LOG_UNIMP, "%s: Phy Soft Reset not implemented\n",
                          __func__);
            break;
        }
        if ((old & GUSB3PIPECTL_SUSPHY) != (val & GUSB3PIPECTL_SUSPHY)) {
            /* TODO: Implement Phy Suspend */
            qemu_log_mask(LOG_UNIMP, "%s: Phy (un)Suspend not implemented\n",
                          __func__);
            break;
        }
        break;
    case GEVNTADRLO(0)... GEVNTCOUNT(15):
        usb_dwc3_gevntreg_write(s, addr, (addr - GEVNTADRLO(0)) >> 2, val);
        break;
    default:
        break;
    }

    *mmio = val;

    if (iflg) {
        dwc3_update_irq(s);
    }
}

static uint64_t usb_dwc3_dreg_read(void *ptr, hwaddr addr, int index)
{
    DWC3State *s = DWC3_USB(ptr);
    uint32_t val;
    uint32_t *mmio;

    if (addr > DALEPENA) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return 0;
    }

    mmio = &s->dreg[index];
    val = *mmio;

    switch (addr) {
    case DCTL:
        /* Self-clearing bits */
        val &= ~(DCTL_CSFTRST);
        *mmio = val;
        break;
    case DGCMD:
        /* Self-clearing bits */
        val &= ~(DGCMD_CMDACT);
        *mmio = val;
        break;
    default:
        break;
    }

    return val;
}

static void usb_dwc3_dreg_write(void *ptr, hwaddr addr, int index, uint64_t val)
{
    DWC3State *s = DWC3_USB(ptr);
    USBDevice *udev = USB_DEVICE(&s->device);
    uint32_t *mmio;
    uint32_t old;
    int iflg = 0;

    if (addr > DALEPENA) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return;
    }

    mmio = &s->dreg[index];
    old = *mmio;

    switch (addr) {
    case DCFG: {
        int devaddr = DCFG_DEVADDR_GET(val);
        if (devaddr != udev->addr) {
            udev->addr = devaddr;
        }
        trace_usb_set_addr(devaddr);
        break;
    }
    case DCTL:
        if (!(old & DCTL_CSFTRST) && (val & DCTL_CSFTRST)) {
            dwc3_dcore_reset(s);
            iflg = true;
        }

        if (!(old & DCTL_RUN_STOP) && (val & DCTL_RUN_STOP)) {
            /* go on bus */
            usb_device_attach(udev, NULL);
            s->dsts &= ~DSTS_DEVCTRLHLT;
        }
        if ((old & DCTL_RUN_STOP) && !(val & DCTL_RUN_STOP)) {
            /* go off bus */
            if (udev->attached) {
                usb_device_detach(udev);
            }
            s->dsts |= DSTS_DEVCTRLHLT;
        }
        /* Self clearing bits */
        val |= old & (DCTL_CSFTRST);
        break;
    case DSTS:
        val = old;
        break;
    case DGCMD:
        val &= ~(DGCMD_CMDSTATUS);
        val |= (old & (DGCMD_CMDSTATUS | DGCMD_CMDACT));
        if (!(val & DGCMD_CMDACT)) {
            break;
        }
        /* TODO DGCMD */
        switch (DGCMD_CMDTYPE_GET(val)) {
        case DGCMD_SET_LMP:
            qemu_log_mask(LOG_UNIMP,
                          "%s: Set Link Function LPM is "
                          "not implemented\n",
                          __func__);
            break;
        case DGCMD_SET_PERIODIC_PAR:
            qemu_log_mask(LOG_UNIMP,
                          "%s: Set Periodic Parameters is "
                          "not implemented\n",
                          __func__);
            break;
        case DGCMD_XMIT_FUNCTION:
            qemu_log_mask(LOG_UNIMP,
                          "%s: Transmit Function Notification "
                          "is not implemented\n",
                          __func__);
            break;
        default:
            qemu_log_mask(LOG_UNIMP, "%s: Unsupported DGCMD\n", __func__);
            val |= (DGCMD_CMDSTATUS);
            break;
        }
        if (val & DGCMD_CMDIOC) {
            struct dwc3_event_devt ioc = { 1, 0, DEVT_CMDCMPLT };
            dwc3_device_event(s, ioc);
        }
        break;
    case DALEPENA:
        break;
    default:
        break;
    }

    *mmio = val;

    if (iflg) {
        dwc3_update_irq(s);
    }
}

static uint64_t usb_dwc3_depcmdreg_read(void *ptr, hwaddr addr, int index)
{
    DWC3State *s = DWC3_USB(ptr);
    uint32_t val;
    uint32_t *mmio;

    if (addr > DEPCMD(DWC3_NUM_EPS)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return 0;
    }
    mmio = &s->depcmdreg[index];
    val = *mmio;

    switch (DEPCMDPAR2(0) + (addr & 0xc)) {
    case DEPCMD(0):
        /* Self-clearing bits */
        val &= ~(DEPCMD_CMDACT);
        *mmio = val;
        break;
    default:
        break;
    }
    return val;
}

static const char *DEPCMD_names[] = {
    [DEPCMD_CFG] = "DEPCFG",
    [DEPCMD_XFERCFG] = "DEPXFERCFG",
    [DEPCMD_GETSEQNUMBER] = "DEPGETDSEQ",
    [DEPCMD_SETSTALL] = "DEPSETSTALL",
    [DEPCMD_CLEARSTALL] = "DEPCSTALL",
    [DEPCMD_STARTXFER] = "DEPSTRTXFER",
    [DEPCMD_UPDATEXFER] = "DEPUPDXFER",
    [DEPCMD_ENDXFER] = "DEPENDXFER",
    [DEPCMD_STARTCFG] = "DEPSTARTCFG",
};

static void usb_dwc3_depcmdreg_write(void *ptr, hwaddr addr, int index,
                                     uint64_t val)
{
    DWC3State *s = DWC3_USB(ptr);
    USBDevice *udev = USB_DEVICE(&s->device);
    uint32_t *mmio;
    uint32_t old;
    int iflg = 0;
    uint32_t epid = index >> 2;
    DWC3Endpoint *ep = &s->eps[epid];

    if (addr > DEPCMD(DWC3_NUM_EPS)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return;
    }

    mmio = &s->depcmdreg[index];
    old = *mmio;

    switch (DEPCMDPAR2(0) + (addr & 0xc)) {
    case DEPCMD(0): {
        uint32_t par0 = s->depcmdpar0(epid);
        uint32_t par1 = s->depcmdpar1(epid);
        uint32_t G_GNUC_UNUSED par2 = s->depcmdpar2(epid);
        struct dwc3_event_depevt ioc = { 0, epid, DEPEVT_EPCMDCMPLT,
                                         0, 0,    DEPCMD_CMD_GET(val) << 8 };
        val &= ~(DEPCMD_STATUS);
        val |= (old & (DEPCMD_CMDACT));
        if (!(val & DEPCMD_CMDACT)) {
            if (!(val & DEPCMD_CMDIOC) &&
                DEPCMD_CMD_GET(val) == DEPCMD_UPDATEXFER) {
                /* Special no response update? */
                dwc3_td_fetch(s, ep->xfer, ep->xfer->tdaddr);
                ep->not_ready = false;
                dwc3_ep_run(s, ep);
            }
            break;
        }
        (void)DEPCMD_names;
#ifdef DEBUG_DWC3
        qemu_log_mask(LOG_UNIMP,
                      "DEPCMD: %s epid: %d "
                      "par2: 0x%x par1: 0x%x par0: 0x%x\n",
                      DEPCMD_names[DEPCMD_CMD_GET(val)], epid, par2, par1,
                      par0);
#endif
        switch (DEPCMD_CMD_GET(val)) {
        case DEPCMD_CFG: {
            int epnum = DEPCFG_EP_NUMBER(par1);
            if (epid == 0 || epid == 1 || (epnum >> 1) == 0) {
                if (epnum != epid) {
                    val |= DEPCMD_STATUS;
                    ioc.status = 1;
                    break;
                }
            }
            ep->epnum = epnum;
            ep->intrnum = DEPCFG_INT_NUM(par1);
            ep->event_en = DEPCFG_EVENT_EN(par1);
            ep->uep = usb_ep_get(udev, epnum & 1 ? USB_TOKEN_IN : USB_TOKEN_OUT,
                                 epnum >> 1);
            ep->uep->max_packet_size = DEPCFG_MAX_PACKET_SIZE(par0);
            ep->uep->type = DEPCFG_EP_TYPE(par0);
            if (DEPCFG_ACTION(par0) == DEPCFG_ACTION_INIT) {
                ep->dseqnum = 0;
            }
            break;
        }
        case DEPCMD_XFERCFG:
            ioc.status = DEPXFERCFG_NUMXFERRES(par0) != 1;
            val |= (ioc.status ? DEPCMD_STATUS : 0);
            break;
        case DEPCMD_SETSTALL:
            ep->stalled = true;
            dwc3_ep_run(s, ep);
            break;
        case DEPCMD_CLEARSTALL:
            if (epid == 0 || epid == 1) {
                /* Automatically cleared upon SETUP */
                break;
            }
            ep->stalled = false;
            ep->not_ready = false;
            ep->dseqnum = 0;
            dwc3_ep_run(s, ep);
            break;
        case DEPCMD_GETSEQNUMBER:
            ioc.parameters = ep->dseqnum & 0xf;
            break;
        case DEPCMD_STARTXFER: {
            dma_addr_t tdaddr = dwc3_addr64(par1, par0);
            if (ep->xfer) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "DEPCMD_STARTXFER: xfer existed\n");
                val |= DEPCMD_STATUS;
                break;
            }
            if (ep->xfer) {
                dwc3_td_free(s, ep->xfer);
                ep->xfer = NULL;
            }
            ep->xfer = dwc3_xfer_alloc(s, epid, tdaddr);
            if (!ep->xfer) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "DEPCMD_STARTXFER: Cannot alloc xfer\n");
                val |= DEPCMD_STATUS;
                break;
            }
            val &= ~DEPCMD_PARAM_MASK;
            val |= DEPCFG_RSC_IDX(ep->xfer->rsc_idx);
            ioc.parameters = ep->xfer->rsc_idx & 0x7f;
            ep->not_ready = false;
            dwc3_ep_run(s, ep);
            break;
        }
        case DEPCMD_UPDATEXFER: {
            if (!ep->xfer || (ep->xfer->rsc_idx) != DEPCFG_RSC_IDX_GET(val)) {
                val |= DEPCMD_STATUS;
                qemu_log_mask(LOG_GUEST_ERROR, "UPDATEXFER: Unknown rsc_idx\n");

                break;
            }
            dwc3_td_fetch(s, ep->xfer, ep->xfer->tdaddr);
            if (ep->xfer->count == 0) {
                val |= DEPCMD_STATUS;
                qemu_log_mask(LOG_GUEST_ERROR, "UPDATEXFER: empty xfer\n");
                break;
            }
            ep->not_ready = false;
            dwc3_ep_run(s, ep);
            break;
        }
        case DEPCMD_ENDXFER:
            if (ep->xfer) {
                dwc3_td_free(s, ep->xfer);
                ep->xfer = NULL;
                if (ep->uep) {
                    USBPacket *p = QTAILQ_FIRST(&ep->uep->queue);
                    if (p) {
                        p->status = USB_RET_IOERROR;
                        usb_packet_complete(udev, p);
                    }
                }
            } else {
                val |= DEPCMD_STATUS;
            }
            break;
        default:
            break;
        }

        if (val & DEPCMD_CMDIOC) {
            if ((val & DEPCMD_STATUS) && (ioc.status == 0)) {
                ioc.status = 1;
            }
            dwc3_ep_event(s, epid, ioc);
        }
    }
    default:
        break;
    }

    *mmio = val;

    if (iflg) {
        dwc3_update_irq(s);
    }
}

static uint64_t usb_dwc3_read(void *ptr, hwaddr addr, unsigned size)
{
    uint64_t val = 0;
    switch (addr) {
    case GLOBALS_REGS_START ... GLOBALS_REGS_END:
        val = usb_dwc3_glbreg_read(ptr, addr, (addr - GLOBALS_REGS_START) >> 2);
        break;
    case DEVICE_REGS_START ... DEVICE_REGS_END:
        val = usb_dwc3_dreg_read(ptr, addr, (addr - DEVICE_REGS_START) >> 2);
        break;
    case DEPCMD_REGS_START ... DEPCMD_REGS_END:
        val =
            usb_dwc3_depcmdreg_read(ptr, addr, (addr - DEPCMD_REGS_START) >> 2);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: addr: 0x%" HWADDR_PRIx "\n", __func__, addr);
        // g_assert_not_reached();
        break;
    };
    // fprintf(stderr, "%s: addr: 0x%" HWADDR_PRIx " val: 0x%" PRIx64 "\n", __func__, addr, val);
    return val;
}

static void usb_dwc3_write(void *ptr, hwaddr addr, uint64_t val, unsigned size)
{
    // fprintf(stderr, "%s: addr: 0x%" HWADDR_PRIx " val: 0x%" PRIx64 "\n", __func__, addr, val);
    switch (addr) {
    case GLOBALS_REGS_START ... GLOBALS_REGS_END:
        usb_dwc3_glbreg_write(ptr, addr, (addr - GLOBALS_REGS_START) >> 2, val);
        break;
    case DEVICE_REGS_START ... DEVICE_REGS_END:
        usb_dwc3_dreg_write(ptr, addr, (addr - DEVICE_REGS_START) >> 2, val);
        break;
    case DEPCMD_REGS_START ... DEPCMD_REGS_END:
        usb_dwc3_depcmdreg_write(ptr, addr, (addr - DEPCMD_REGS_START) >> 2,
                                 val);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: addr: 0x%" HWADDR_PRIx " val: 0x%" PRIx64 "\n", __func__,
                      addr, val);
        // g_assert_not_reached();
        break;
    };
}

static const MemoryRegionOps usb_dwc3_ops = {
    .read = usb_dwc3_read,
    .write = usb_dwc3_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void usb_dwc3_realize(DeviceState *dev, Error **errp)
{
    DWC3State *s = DWC3_USB(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    Error *err = NULL;
    Object *obj;
    MemoryRegion *dma_mr;

    obj = object_property_get_link(OBJECT(dev), "dma-mr", &error_abort);

    dma_mr = MEMORY_REGION(obj);
    address_space_init(&s->dma_as, dma_mr, "dwc3");

    obj = object_property_get_link(OBJECT(dev), "dma-xhci", &error_abort);
    s->sysbus_xhci.xhci.dma_mr = MEMORY_REGION(obj);

    sysbus_realize(SYS_BUS_DEVICE(&s->sysbus_xhci), &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }
    s->numintrs = s->sysbus_xhci.xhci.numintrs;

    memory_region_add_subregion(
        &s->iomem, 0,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->sysbus_xhci), 0));
    sysbus_pass_irq(sbd, SYS_BUS_DEVICE(&s->sysbus_xhci));
    s->sysbus_xhci.xhci.intr_raise = dwc3_host_intr_raise;
}

static void usb_dwc3_init(Object *obj)
{
    DWC3State *s = DWC3_USB(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    object_initialize_child(obj, "dwc3-xhci", &s->sysbus_xhci,
                            TYPE_XHCI_SYSBUS);
    object_initialize_child(obj, "dwc3-usb-device", &s->device,
                            TYPE_DWC3_USB_DEVICE);
    qdev_alias_all_properties(DEVICE(&s->sysbus_xhci), obj);

    memory_region_init_io(&s->iomem, obj, &usb_dwc3_ops, s, "dwc3-io",
                          DWC3_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);

    for (int i = 0; i < DWC3_NUM_EPS; i++) {
        s->eps[i].epid = i;
    }
}

static void dwc3_process_packet(DWC3State *s, DWC3Endpoint *ep, USBPacket *p)
{
    USBDevice *udev = USB_DEVICE(&s->device);
    DWC3BufferDesc *desc = NULL;
    DWC3Transfer *xfer = NULL;

    DPRINTF("%s: pid: 0x%x ep: %d id: 0x%" PRIx64 " (%d/%" PRIx64 ")\n", __func__, p->pid,
            ep->epid, p->id, p->actual_length, usb_packet_size(p));
    assert(bql_locked());
    if (ep->stalled && p->actual_length == 0) {
        p->status = USB_RET_STALL;
        goto complete;
        return;
    }

    if (ep->xfer == NULL) {
        struct dwc3_event_depevt event = { 0, ep->epid, DEPEVT_XFERNOTREADY, 0,
                                           0 };
        dwc3_ep_event(s, ep->epid, event);
        p->status = USB_RET_ASYNC;
        return;
    }

    xfer = ep->xfer;
    desc = QTAILQ_FIRST(&xfer->buffers);
    if (desc == NULL) {
        struct dwc3_event_depevt event = { 0, ep->epid, DEPEVT_XFERNOTREADY, 0,
                                           0 };
        event.status |= DEPEVT_STATUS_TRANSFER_ACTIVE;
        p->status = USB_RET_ASYNC;
        dwc3_ep_event(s, ep->epid, event);
        return;
    }

    dwc3_bd_copy(s, desc, p);
    if (desc->ended) {
        QTAILQ_REMOVE(&xfer->buffers, desc, queue);
        xfer->count--;
        dwc3_bd_free(s, desc);
        if (xfer->count == 0 && xfer->tdaddr == -1) {
            ep->xfer = NULL;
            smp_wmb();
            dwc3_td_free(s, xfer);
        }
    }
complete:
    if (p->status != USB_RET_ASYNC) {
        if (usb_packet_is_inflight(p)) {
            usb_packet_complete(udev, p);
        }
    }
}

static void dwc3_usb_device_realize(USBDevice *dev, Error **errp)
{
    dev->speed = USB_SPEED_HIGH;
    dev->speedmask = USB_SPEED_MASK_HIGH;
    dev->flags |= (1 << USB_DEV_FLAG_IS_HOST);
    dev->auto_attach = false;
}

static void dwc3_usb_device_handle_attach(USBDevice *dev)
{
    DWC3DeviceState *udev = DWC3_USB_DEVICE(dev);
    DWC3State *s = container_of(udev, DWC3State, device);

    s->dsts = (s->dsts & ~DSTS_CONNECTSPD) | DSTS_HIGHSPEED;
    s->dsts = (s->dsts & ~DSTS_USBLNKST_MASK) | DSTS_USBLNKST(LINK_STATE_U0);

    struct dwc3_event_devt ulschng = { 1, 0, DEVT_ULSTCHNG, 0, LINK_STATE_U0 };
    struct dwc3_event_devt connect = { 1, 0, DEVT_CONNECTDONE };
    dwc3_device_event(s, ulschng);
    dwc3_device_event(s, connect);
}

static void dwc3_usb_device_handle_detach(USBDevice *dev)
{
    DWC3DeviceState *udev = DWC3_USB_DEVICE(dev);
    DWC3State *s = container_of(udev, DWC3State, device);

    s->dsts = (s->dsts & ~DSTS_CONNECTSPD) | DSTS_HIGHSPEED;
    s->dsts =
        (s->dsts & ~DSTS_USBLNKST_MASK) | DSTS_USBLNKST(LINK_STATE_SS_DIS);

    struct dwc3_event_devt ulschng = { 1, 0, DEVT_ULSTCHNG, 0,
                                       LINK_STATE_SS_DIS };
    dwc3_device_event(s, ulschng);
    struct dwc3_event_devt disconn = { 1, 0, DEVT_DISCONN };
    dwc3_device_event(s, disconn);
}

static void dwc3_usb_device_handle_reset(USBDevice *dev)
{
    DWC3DeviceState *udev = DWC3_USB_DEVICE(dev);
    DWC3State *s = container_of(udev, DWC3State, device);

    s->dcfg &= ~DCFG_DEVADDR_MASK;
    s->dsts = (s->dsts & ~DSTS_CONNECTSPD) | DSTS_HIGHSPEED;

    struct dwc3_event_devt usbrst = { 1, 0, DEVT_USBRST };
    dwc3_device_event(s, usbrst);
    struct dwc3_event_devt connect = { 1, 0, DEVT_CONNECTDONE };
    dwc3_device_event(s, connect);
}

static void dwc3_usb_device_cancel_packet(USBDevice *dev, USBPacket *p)
{
    /* TODO: complete td if packet partially complete */
    DPRINTF("%s: pid: 0x%x ep: %d id: 0x%" PRIx64 "\n", __func__, p->pid, p->ep->nr,
            p->id);
    // assert(p->actual_length == 0);
}

static void dwc3_usb_device_handle_packet(USBDevice *dev, USBPacket *p)
{
    DWC3DeviceState *udev = DWC3_USB_DEVICE(dev);
    DWC3State *s = container_of(udev, DWC3State, device);
    int epid = dwc3_packet_find_epid(s, p);
    DWC3Endpoint *ep;

    if (epid == -1) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Unable to find ep for nr: %d pid: 0x%x\n", __func__,
                      p->ep->nr, p->pid);
        p->status = USB_RET_NAK;
        return;
    }

    ep = &s->eps[epid];
    if (!ep->uep) {
        return;
    }

    if (p->pid == USB_TOKEN_SETUP && ep->uep->nr == 0) {
        s->eps[0].stalled = false;
        s->eps[0].not_ready = false;
        s->eps[1].stalled = false;
        s->eps[1].not_ready = false;
    }


    if (ep->stalled) {
        p->status = USB_RET_STALL;
        return;
    }

    if (!(s->dalepena & (1 << epid))) {
        p->status = usb_packet_is_inflight(p) ? USB_RET_IOERROR : USB_RET_NAK;
        return;
    }

    dwc3_process_packet(s, ep, p);
}

static void dwc3_ep_run(DWC3State *s, DWC3Endpoint *ep)
{
    USBPacket *p;
    if (!ep->uep) {
        return;
    }

    p = QTAILQ_FIRST(&ep->uep->queue);
    if (p) {
        dwc3_process_packet(s, ep, p);
    }
}

static int dwc3_buffer_desc_pre_save(void *opaque)
{
    DWC3BufferDesc *s = opaque;
    if (s->mapped) {
        error_report("dwc3: Cannot save when a transfer is ongoing");
        return -EINVAL;
    }
    return 0;
}

static int usb_dwc3_post_load(void *opaque, int version_id)
{
    DWC3State *s = opaque;
    USBDevice *udev = USB_DEVICE(&s->device);

    s->eps[0].uep = &udev->ep_ctl;
    s->eps[1].uep = &udev->ep_ctl;
    for (int i = 2; i < DWC3_NUM_EPS; i++) {
        if (s->eps[i].epnum) {
            s->eps[i].uep = usb_ep_get(
                udev, s->eps[i].epnum & 1 ? USB_TOKEN_IN : USB_TOKEN_OUT,
                s->eps[i].epnum >> 1);
        }
    }
    return 0;
}

static const VMStateDescription vmstate_dwc3_event_ring = {
    .name = "dwc3/event_ring",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]){ VMSTATE_UINT32(size, DWC3EventRing),
                                VMSTATE_UINT32(head, DWC3EventRing),
                                VMSTATE_UINT32(count, DWC3EventRing),
                                VMSTATE_END_OF_LIST() },
};

static const VMStateDescription vmstate_dwc3_trb = {
    .name = "dwc3/trb",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]){ VMSTATE_UINT64(bp, DWC3TRB),
                                VMSTATE_UINT64(addr, DWC3TRB),
                                VMSTATE_UINT32(ctrl, DWC3TRB),
                                VMSTATE_END_OF_LIST() },
};

static const VMStateDescription vmstate_dwc3_buffer_desc = {
    .name = "dwc3/buffer_descriptor",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = dwc3_buffer_desc_pre_save,
    .fields = (VMStateField[]){ VMSTATE_INT32(epid, DWC3BufferDesc),
                                VMSTATE_UINT32(count, DWC3BufferDesc),
                                VMSTATE_UINT32(length, DWC3BufferDesc),
                                VMSTATE_UINT32(actual_length, DWC3BufferDesc),
                                VMSTATE_UINT32(dir, DWC3BufferDesc),
                                VMSTATE_BOOL(ended, DWC3BufferDesc),
                                VMSTATE_STRUCT_VARRAY_POINTER_UINT32(
                                    trbs, DWC3BufferDesc, count,
                                    vmstate_dwc3_trb, DWC3TRB),
                                VMSTATE_END_OF_LIST() },
};

static const VMStateDescription vmstate_dwc3_transfer = {
    .name = "dwc3/transfer",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]){ VMSTATE_UINT64(tdaddr, DWC3Transfer),
                                VMSTATE_INT32(epid, DWC3Transfer),
                                VMSTATE_UINT32(count, DWC3Transfer),
                                VMSTATE_UINT32(rsc_idx, DWC3Transfer),
                                VMSTATE_QTAILQ_V(buffers, DWC3Transfer, 1,
                                                 vmstate_dwc3_buffer_desc,
                                                 DWC3BufferDesc, queue),
                                VMSTATE_END_OF_LIST() },
};

static const VMStateDescription vmstate_dwc3_endpoint = {
    .name = "dwc3/endpoint",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]){ VMSTATE_UINT32(epnum, DWC3Endpoint),
                                VMSTATE_UINT32(intrnum, DWC3Endpoint),
                                VMSTATE_UINT32(event_en, DWC3Endpoint),
                                VMSTATE_UINT32(xfer_resource_idx, DWC3Endpoint),
                                VMSTATE_UINT8(dseqnum, DWC3Endpoint),
                                VMSTATE_BOOL(stalled, DWC3Endpoint),
                                VMSTATE_BOOL(not_ready, DWC3Endpoint),
                                VMSTATE_STRUCT_POINTER(xfer, DWC3Endpoint,
                                                       vmstate_dwc3_transfer,
                                                       DWC3Transfer),
                                VMSTATE_END_OF_LIST() },
};

static const VMStateDescription vmstate_usb_dwc3 = {
    .name = "dwc3",
    .version_id = 1,
    .post_load = usb_dwc3_post_load,
    .fields =
        (VMStateField[]){
            VMSTATE_UINT32_ARRAY(glbreg, DWC3State,
                                 DWC3_GLBREG_SIZE / sizeof(uint32_t)),
            VMSTATE_UINT32_ARRAY(dreg, DWC3State,
                                 DWC3_DREG_SIZE / sizeof(uint32_t)),
            VMSTATE_UINT32_ARRAY(depcmdreg, DWC3State,
                                 DWC3_DEPCMDREG_SIZE / sizeof(uint32_t)),
            VMSTATE_BOOL_ARRAY(host_intr_state, DWC3State, DWC3_NUM_INTRS),
            VMSTATE_STRUCT_ARRAY(eps, DWC3State, DWC3_NUM_EPS, 1,
                                 vmstate_dwc3_endpoint, DWC3Endpoint),
            VMSTATE_STRUCT_ARRAY(intrs, DWC3State, DWC3_NUM_INTRS, 1,
                                 vmstate_dwc3_event_ring, DWC3EventRing),
            VMSTATE_END_OF_LIST() }
};

static Property usb_dwc3_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void dwc3_usb_device_class_initfn(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->realize = dwc3_usb_device_realize;
    uc->product_desc = "DWC3 USB Device";
    uc->unrealize = NULL;
    uc->cancel_packet = dwc3_usb_device_cancel_packet;
    uc->handle_attach = dwc3_usb_device_handle_attach;
    uc->handle_detach = dwc3_usb_device_handle_detach;
    uc->handle_reset = dwc3_usb_device_handle_reset;
    uc->handle_data = NULL;
    uc->handle_control = NULL;
    uc->handle_packet = dwc3_usb_device_handle_packet;
    uc->flush_ep_queue = NULL;
    uc->ep_stopped = NULL;
    uc->alloc_streams = NULL;
    uc->free_streams = NULL;
    uc->usb_desc = NULL;
    set_bit(DEVICE_CATEGORY_USB, dc->categories);
}

static void usb_dwc3_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    DWC3Class *c = DWC3_USB_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = usb_dwc3_realize;
    dc->vmsd = &vmstate_usb_dwc3;
    set_bit(DEVICE_CATEGORY_USB, dc->categories);
    device_class_set_props(dc, usb_dwc3_properties);
    resettable_class_set_parent_phases(rc, dwc3_reset_enter, dwc3_reset_hold,
                                       dwc3_reset_exit, &c->parent_phases);
}

static const TypeInfo dwc3_usb_device_type_info = {
    .name = TYPE_DWC3_USB_DEVICE,
    .parent = TYPE_USB_DEVICE,
    .instance_size = sizeof(DWC3DeviceState),
    .class_init = dwc3_usb_device_class_initfn,
};

static const TypeInfo usb_dwc3_info = {
    .name = TYPE_DWC3_USB,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DWC3State),
    .instance_init = usb_dwc3_init,
    .class_size = sizeof(DWC3Class),
    .class_init = usb_dwc3_class_init,
};

static void usb_dwc3_register_types(void)
{
    type_register_static(&dwc3_usb_device_type_info);
    type_register_static(&usb_dwc3_info);
}

type_init(usb_dwc3_register_types)
