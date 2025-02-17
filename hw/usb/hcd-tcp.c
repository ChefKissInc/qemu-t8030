#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "hw/usb.h"
#include "hw/usb/hcd-tcp.h"
#include "io/channel-util.h"
#include "io/channel.h"
#include "migration/blocker.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/lockable.h"
#include "qemu/main-loop.h"
#include "qom/object.h"
#include "sysemu/iothread.h"
#include "tcp-usb.h"

// #define DEBUG_HCD_TCP

#ifdef DEBUG_HCD_TCP
#define DPRINTF(fmt, ...)                                \
    do {                                                 \
        fprintf(stderr, "hcd-tcp: " fmt, ##__VA_ARGS__); \
    } while (0)
#else
#define DPRINTF(fmt, ...) \
    do {                  \
    } while (0)
#endif

static void usb_tcp_host_closed(USBTCPHostState *s)
{
    DPRINTF("%s\n", __func__);
    if (s->ioc) {
        qio_channel_shutdown(s->ioc, QIO_CHANNEL_SHUTDOWN_BOTH, NULL);
        qio_channel_close(s->ioc, NULL);
        object_unref(OBJECT(s->ioc));
        s->ioc = NULL;
    }
    s->closed = true;
    migrate_del_blocker(&s->migration_blocker);
}

static ssize_t tcp_usb_read(QIOChannel *ioc, void *buf, size_t len)
{
    struct iovec iov = { .iov_base = buf, .iov_len = len };
    bool iolock = bql_locked();
    bool iothread = qemu_in_iothread();
    ssize_t ret = -1;
    Error *err = NULL;

    /*
     * Dont use in IOThread out of co-routine context as
     * it will block IOThread.
     */
    assert(qemu_in_coroutine() || !iothread);

    if (iolock && !iothread && !qemu_in_coroutine()) {
        bql_unlock();
    }

    ret = qio_channel_readv_full_all_eof(ioc, &iov, 1, NULL, 0, &err);

    if (iolock && !iothread && !qemu_in_coroutine()) {
        bql_lock();
    }

    if (err) {
        error_report_err(err);
    }
    return (ret <= 0) ? ret : iov.iov_len;
}

static bool tcp_usb_write(QIOChannel *ioc, void *buf, ssize_t len)
{
    struct iovec iov = { .iov_base = buf, .iov_len = len };
    bool iolock = bql_locked();
    bool iothread = qemu_in_iothread();
    bool ret = false;
    Error *err = NULL;

    /*
     * Dont use in IOThread out of co-routine context as
     * it will block IOThread.
     */
    assert(qemu_in_coroutine() || !iothread);

    if (iolock && !iothread && !qemu_in_coroutine()) {
        bql_unlock();
    }

    if (!qio_channel_writev_full_all(ioc, &iov, 1, NULL, 0, 0, &err)) {
        ret = true;
    }

    if (iolock && !iothread && !qemu_in_coroutine()) {
        bql_lock();
    }

    if (err) {
        error_report_err(err);
    }
    return ret;
}

static USBPort *usb_tcp_host_find_active_port(USBTCPHostState *s)
{
    for (int i = 0; i < G_N_ELEMENTS(s->uports) - 1; i++) {
        if (s->uports[i].dev->attached) {
            return &s->uports[i];
        }
    }
    return &s->uports[0];
}

static void coroutine_fn usb_tcp_host_respond_packet_co(void *opaque)
{
    USBTCPPacket *pkt = (USBTCPPacket *)opaque;
    USBTCPHostState *s = USB_TCP_HOST(pkt->s);
    USBPacket *p = &pkt->p;
    tcp_usb_header_t hdr = { 0 };
    tcp_usb_response_header resp = { 0 };
    g_autofree void *buffer = NULL;
    USBPort *uport = usb_tcp_host_find_active_port(s);

    WITH_QEMU_LOCK_GUARD(&s->write_mutex)
    {
        if (!s->closed) {
            hdr.type = TCP_USB_RESPONSE;
            resp.addr = uport->dev->addr;
            resp.pid = p->pid;
            resp.ep = p->ep->nr;
            resp.id = p->id;
            resp.status = p->status;
            resp.length = p->iov.size;

            if (resp.length > p->actual_length) {
                resp.length = p->actual_length;
            }

            if (p->pid == USB_TOKEN_IN && p->status != USB_RET_ASYNC) {
                buffer = g_malloc(resp.length);
                iov_to_buf(p->iov.iov, p->iov.niov, 0, buffer, resp.length);
            }

            if (!tcp_usb_write(s->ioc, &hdr, sizeof(hdr))) {
                usb_tcp_host_closed(s);
                return;
            }

            if (!tcp_usb_write(s->ioc, &resp, sizeof(resp))) {
                usb_tcp_host_closed(s);
                return;
            }

            if (buffer) {
                if (!tcp_usb_write(s->ioc, buffer, resp.length)) {
                    usb_tcp_host_closed(s);
                    return;
                }
            }
        }
    }

    if (!usb_packet_is_inflight(p)) {
        if (pkt->buffer) {
            g_free(pkt->buffer);
        }
        usb_packet_cleanup(p);
        g_free(pkt);
    }
}

static void usb_tcp_host_respond_packet(USBTCPHostState *s, USBTCPPacket *pkt)
{
    Coroutine *co = NULL;
    co = qemu_coroutine_create(usb_tcp_host_respond_packet_co, pkt);
    qemu_coroutine_enter(co);
}

static void coroutine_fn usb_tcp_host_msg_loop_co(void *opaque)
{
    USBTCPHostState *s = USB_TCP_HOST(opaque);
    USBPort *uport = usb_tcp_host_find_active_port(s);
    QIOChannel *ioc = s->ioc;

    for (;;) {
        tcp_usb_header_t hdr = { 0 };

        if (unlikely((tcp_usb_read(ioc, &hdr, sizeof(hdr)) != sizeof(hdr)))) {
            usb_tcp_host_closed(s);
            return;
        }

        switch (hdr.type) {
        case TCP_USB_REQUEST: {
            /* fprintf(stderr, "%s: TCP_USB_REQUEST\n", __func__); */
            tcp_usb_request_header pkt_hdr = { 0 };
            g_autofree void *buffer = NULL;
            g_autofree USBTCPPacket *pkt =
                (USBTCPPacket *)g_malloc0(sizeof(USBTCPPacket));
            USBEndpoint *ep = NULL;

            if (unlikely(tcp_usb_read(ioc, &pkt_hdr, sizeof(pkt_hdr)) !=
                         sizeof(pkt_hdr))) {
                usb_tcp_host_closed(s);
                return;
            }

#if 0
                DPRINTF("%s: TCP_USB_REQUEST pid: 0x%x ep: %d id: 0x%lx\n", __func__, pkt_hdr.pid, pkt_hdr.ep, pkt_hdr.id);
#endif
            ep = usb_ep_get(uport->dev, pkt_hdr.pid, pkt_hdr.ep);
            if (ep == NULL) {
                DPRINTF("%s: TCP_USB_REQUEST unknown EP\n", __func__);
                usb_tcp_host_closed(s);
                return;
            }

            usb_packet_init(&pkt->p);
            usb_packet_setup(&pkt->p, pkt_hdr.pid, ep, pkt_hdr.stream,
                             pkt_hdr.id, pkt_hdr.short_not_ok, pkt_hdr.int_req);

            if (pkt_hdr.length > 0) {
                buffer = g_malloc0(pkt_hdr.length);

                if (pkt_hdr.pid != USB_TOKEN_IN) {
                    if (unlikely(tcp_usb_read(s->ioc, buffer, pkt_hdr.length) !=
                                 pkt_hdr.length)) {
                        usb_tcp_host_closed(s);
                        usb_packet_cleanup(&pkt->p);
                        return;
                    }
                    /* qemu_hexdump(stderr, __func__, buffer, pkt_hdr.length);
                     */
                }

                usb_packet_addbuf(&pkt->p, buffer, pkt_hdr.length);
                pkt->buffer = buffer;
                g_steal_pointer(&buffer);
            }

            if (pkt_hdr.addr != uport->dev->addr) {
                /*
                 * fprintf(stderr,
                 *         "%s: USB_RET_NODEV: pkt_hdr.addr != uport->dev->addr:
                 * %d != %d\n",
                 *         __func__, pkt_hdr.addr, uport->dev->addr);
                 */
                /* Can't enforce this check because dwc2 address transition time
                 * is slow */
            }
            pkt->dev = ep->dev;
            pkt->s = s;
            pkt->addr = pkt_hdr.addr;
            assert(bql_locked());

            usb_handle_packet(pkt->dev, &pkt->p);
            usb_tcp_host_respond_packet(s, pkt);
            g_steal_pointer(&pkt);
            break;
        }
        case TCP_USB_RESPONSE:
            fprintf(stderr, "%s: unexpected TCP_USB_RESPONSE\n", __func__);
            usb_tcp_host_closed(s);
            return;
        case TCP_USB_CANCEL: {
            /* DPRINTF("%s: TCP_USB_CANCEL\n", __func__); */
            tcp_usb_cancel_header pkt_hdr = { 0 };
            USBTCPPacket *pkt = NULL;
            USBPacket *p = NULL;

            if (unlikely(tcp_usb_read(ioc, &pkt_hdr, sizeof(pkt_hdr)) !=
                         sizeof(pkt_hdr))) {
                usb_tcp_host_closed(s);
                return;
            }

#if 1
            DPRINTF("%s: TCP_USB_CANCEL pid: 0x%x ep: %d\n", __func__,
                    pkt_hdr.pid, pkt_hdr.ep);
#endif

            if (pkt_hdr.addr != uport->dev->addr) {
                /*
                 * fprintf(stderr,
                 *         "%s: USB_RET_NODEV: pkt_hdr.addr != uport->dev->addr:
                 * %d != %d\n",
                 *         __func__, pkt_hdr.addr, uport->dev->addr);
                 */
                /* Can't enforce this check because dwc2 address transition time
                 * is slow */
            }
            assert(bql_locked());
            p = usb_ep_find_packet_by_id(uport->dev, pkt_hdr.pid, pkt_hdr.ep,
                                         pkt_hdr.id);
            if (p) {
                pkt = container_of(p, USBTCPPacket, p);
                usb_cancel_packet(&pkt->p);
                DPRINTF("%s: TCP_USB_CANCEL: packet"
                        " pid: 0x%x ep: %d id: 0x%lx len: 0x%x\n",
                        __func__, pkt_hdr.pid, pkt_hdr.ep, pkt_hdr.id,
                        p->actual_length);
                usb_tcp_host_respond_packet(s, pkt);
            } else {
                warn_report("%s: TCP_USB_CANCEL: packet"
                            " pid: 0x%x ep: %d id: 0x%" PRIx64 " not found",
                            __func__, pkt_hdr.pid, pkt_hdr.ep, pkt_hdr.id);
            }
            break;
        }
        case TCP_USB_RESET:
            /* fprintf(stderr, "%s: TCP_USB_RESET\n", __func__); */
            DPRINTF("%s: TCP_USB_RESET\n", __func__);
            assert(bql_locked());
            usb_device_reset(uport->dev);
            break;
            ;
        default:
            g_assert_not_reached();
            break;
        }
    }

    return;
}

static void usb_tcp_host_attach(USBPort *uport)
{
    struct sockaddr_un server_addr;
    USBTCPHostState *s = USB_TCP_HOST(uport->opaque);
    int sock = -1;
    Coroutine *co = NULL;
    QIOChannel *ioc = NULL;
    int ret;
    Error *err = NULL;

    if (uport->index >= G_N_ELEMENTS(s->uports) - 1) {
        error_report("%s: attached to unused port\n", __func__);
        return;
    }

    if (usb_tcp_host_find_active_port(s) != uport) {
        error_report("%s: Attaching to 2 proxy ports at the same time. "
                     "This port might not be able to receive packets.\n",
                     __func__);
        return;
    }

    if (!uport->dev || !uport->dev->attached) {
        return;
    }

    sock = socket(AF_UNIX, SOCK_STREAM, 0);

    if (socket < 0) {
        error_report("%s: cannot open socket", __func__);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sun_family = AF_UNIX;
    strncpy(server_addr.sun_path, socket_path, sizeof(server_addr.sun_path));
    server_addr.sun_path[sizeof(server_addr.sun_path) - 1] = '\0';

    ret = connect(sock, (const struct sockaddr *)&server_addr,
                  sizeof(server_addr));
    if (ret < 0) {
        error_report("%s: failed to connect to server: %d\n", __func__, ret);
        close(sock);
        return;
    }

    ioc = qio_channel_new_fd(sock, &err);
    if (!ioc) {
        error_report_err(err);
        close(sock);
        return;
    }
    object_ref(ioc);
    qio_channel_set_blocking(ioc, false, NULL);
    s->closed = false;
    s->ioc = ioc;

    migrate_add_blocker(&s->migration_blocker, NULL);
    co = qemu_coroutine_create(usb_tcp_host_msg_loop_co, s);
    qemu_coroutine_enter(co);
}

static void usb_tcp_host_detach(USBPort *uport)
{
    USBTCPHostState *s = USB_TCP_HOST(uport->opaque);

    usb_tcp_host_closed(s);
}

static void usb_tcp_host_async_packet_complete(USBPort *port, USBPacket *p)
{
    USBTCPHostState *s = USB_TCP_HOST(port->opaque);
    usb_tcp_host_respond_packet(s, container_of(p, USBTCPPacket, p));
}

static USBBusOps usb_tcp_bus_ops = {};

static USBPortOps usb_tcp_host_port_ops = {
    .attach = usb_tcp_host_attach,
    .detach = usb_tcp_host_detach,
    .child_detach = NULL,
    .wakeup = NULL,
    .complete = usb_tcp_host_async_packet_complete,
};

static void usb_tcp_host_realize(DeviceState *dev, Error **errp)
{
    USBTCPHostState *s = USB_TCP_HOST(dev);

    usb_bus_new(&s->bus, sizeof(s->bus), &usb_tcp_bus_ops, dev);
    for (int i = 0; i < G_N_ELEMENTS(s->uports); i++) {
        usb_register_port(&s->bus, &s->uports[i], s, i, &usb_tcp_host_port_ops,
                          USB_SPEED_MASK_LOW | USB_SPEED_MASK_FULL |
                              USB_SPEED_MASK_HIGH);
    }

    s->closed = 1;
    qemu_co_mutex_init(&s->write_mutex);
}

static void usb_tcp_host_unrealize(DeviceState *dev)
{
    USBTCPHostState *s = USB_TCP_HOST(dev);

    if (s->ioc) {
        qio_channel_shutdown(s->ioc, QIO_CHANNEL_SHUTDOWN_BOTH, NULL);
        qio_channel_close(s->ioc, NULL);
        s->ioc = NULL;
    }

    s->closed = 1;
    s->stopped = 1;
}

static void usb_tcp_host_init(Object *obj)
{
    USBTCPHostState *s = USB_TCP_HOST(obj);
    s->closed = 1;
    error_setg(&s->migration_blocker,
               "%s does not support migration "
               "while connected",
               TYPE_USB_TCP_HOST);
}

static Property usb_tcp_host_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void usb_tcp_host_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = usb_tcp_host_realize;
    dc->unrealize = usb_tcp_host_unrealize;
    dc->desc = "QEMU USB Passthrough Host Controller";
    device_class_set_props(dc, usb_tcp_host_properties);
    set_bit(DEVICE_CATEGORY_USB, dc->categories);
}

static const TypeInfo usb_tcp_host_type_info = {
    .name = TYPE_USB_TCP_HOST,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(USBTCPHostState),
    .class_init = usb_tcp_host_class_init,
    .instance_init = usb_tcp_host_init,
};

static void usb_tcp_host_register_types(void)
{
    type_register_static(&usb_tcp_host_type_info);
}

type_init(usb_tcp_host_register_types)
