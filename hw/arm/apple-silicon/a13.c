/*
 * Apple A13 CPU.
 *
 * Copyright (c) 2023-2025 Visual Ehrmanntraut (VisualEhrmanntraut).
 * Copyright (c) 2023-2025 Christian Inci (chris-pcguy).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "exec/address-spaces.h"
#include "hw/arm/apple-silicon/a13.h"
#include "hw/arm/apple-silicon/a13_gxf.h"
#include "hw/arm/apple-silicon/dtb.h"
#include "hw/irq.h"
#include "hw/or-irq.h"
#include "hw/qdev-properties.h"
#include "hw/resettable.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/queue.h"
#include "qemu/timer.h"
#include "sysemu/reset.h"
#include "arm-powerctl.h"
#include "target/arm/cpregs.h"

#define VMSTATE_A13_CPREG(name) \
    VMSTATE_UINT64(A13_CPREG_VAR_NAME(name), AppleA13State)

#define VMSTATE_A13_CLUSTER_CPREG(name) \
    VMSTATE_UINT64(A13_CPREG_VAR_NAME(name), AppleA13Cluster)

#define A13_CPREG_DEF(p_name, p_op0, p_op1, p_crn, p_crm, p_op2, p_access,     \
                      p_reset)                                                 \
    {                                                                          \
        .cp = CP_REG_ARM64_SYSREG_CP, .name = #p_name, .opc0 = p_op0,          \
        .crn = p_crn, .crm = p_crm, .opc1 = p_op1, .opc2 = p_op2,              \
        .access = p_access, .resetvalue = p_reset, .state = ARM_CP_STATE_AA64, \
        .type = ARM_CP_OVERRIDE,                                               \
        .fieldoffset = offsetof(AppleA13State, A13_CPREG_VAR_NAME(p_name)) -   \
                       offsetof(ARMCPU, env)                                   \
    }

#define A13_CLUSTER_CPREG_DEF(p_name, p_op0, p_op1, p_crn, p_crm, p_op2,     \
                              p_access)                                      \
    {                                                                        \
        .cp = CP_REG_ARM64_SYSREG_CP, .name = #p_name, .opc0 = p_op0,        \
        .crn = p_crn, .crm = p_crm, .opc1 = p_op1, .opc2 = p_op2,            \
        .access = p_access, .type = ARM_CP_IO, .state = ARM_CP_STATE_AA64,   \
        .readfn = apple_a13_cluster_cpreg_read,                              \
        .writefn = apple_a13_cluster_cpreg_write,                            \
        .fieldoffset = offsetof(AppleA13Cluster, A13_CPREG_VAR_NAME(p_name)) \
    }

#define IPI_SR_SRC_CPU_SHIFT 8
#define IPI_SR_SRC_CPU_WIDTH 8
#define IPI_SR_SRC_CPU_MASK \
    (((1 << IPI_SR_SRC_CPU_WIDTH) - 1) << IPI_SR_SRC_CPU_SHIFT)
#define IPI_SR_SRC_CPU(ipi_sr_val) \
    (((ipi_sr_val) & IPI_SR_SRC_CPU_MASK) >> IPI_SR_SRC_CPU_SHIFT)

#define IPI_RR_TARGET_CLUSTER_SHIFT 16

#define IPI_RR_TYPE_IMMEDIATE (0 << 28)
#define IPI_RR_TYPE_RETRACT (1 << 28)
#define IPI_RR_TYPE_DEFERRED (2 << 28)
#define IPI_RR_TYPE_NOWAKE (3 << 28)
#define IPI_RR_TYPE_MASK (3 << 28)
#define NSEC_PER_USEC 1000ull /* nanoseconds per microsecond */
#define USEC_PER_SEC 1000000ull /* microseconds per second */
#define NSEC_PER_SEC 1000000000ull /* nanoseconds per second */
#define NSEC_PER_MSEC 1000000ull /* nanoseconds per millisecond */
#define RTCLOCK_SEC_DIVISOR 24000000ull

static void absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *result)
{
    uint64_t t64;

    *result = (t64 = abstime / RTCLOCK_SEC_DIVISOR) * NSEC_PER_SEC;
    abstime -= (t64 * RTCLOCK_SEC_DIVISOR);
    *result += (abstime * NSEC_PER_SEC) / RTCLOCK_SEC_DIVISOR;
}

static void nanoseconds_to_absolutetime(uint64_t nanosecs, uint64_t *result)
{
    uint64_t t64;

    *result = (t64 = nanosecs / NSEC_PER_SEC) * RTCLOCK_SEC_DIVISOR;
    nanosecs -= (t64 * NSEC_PER_SEC);
    *result += (nanosecs * RTCLOCK_SEC_DIVISOR) / NSEC_PER_SEC;
}


static QTAILQ_HEAD(, AppleA13Cluster) clusters =
    QTAILQ_HEAD_INITIALIZER(clusters);

static uint64_t ipi_cr = kDeferredIPITimerDefault;
static QEMUTimer *ipicr_timer = NULL;

inline bool apple_a13_cpu_is_sleep(AppleA13State *tcpu)
{
    return CPU(tcpu)->halted;
}

inline bool apple_a13_cpu_is_powered_off(AppleA13State *tcpu)
{
    return ARM_CPU(tcpu)->power_state == PSCI_OFF;
}

void apple_a13_cpu_start(AppleA13State *tcpu)
{
    int ret = QEMU_ARM_POWERCTL_RET_SUCCESS;

    if (ARM_CPU(tcpu)->power_state != PSCI_ON) {
        ret = arm_set_cpu_on_and_reset(tcpu->mpidr);
    }

    if (ret != QEMU_ARM_POWERCTL_RET_SUCCESS) {
        error_report("Failed to bring up CPU %d: err %d", tcpu->cpu_id, ret);
    }
}

void apple_a13_cpu_reset(AppleA13State *tcpu)
{
    int ret = QEMU_ARM_POWERCTL_RET_SUCCESS;

    if (ARM_CPU(tcpu)->power_state != PSCI_OFF) {
        ret = arm_reset_cpu(tcpu->mpidr);
    }

    if (ret != QEMU_ARM_POWERCTL_RET_SUCCESS) {
        error_report("%s: failed to reset CPU %d: err %d", __func__,
                     tcpu->cpu_id, ret);
    }
}

void apple_a13_cpu_off(AppleA13State *tcpu)
{
    int ret = QEMU_ARM_POWERCTL_RET_SUCCESS;

    if (ARM_CPU(tcpu)->power_state != PSCI_OFF) {
        ret = arm_set_cpu_off(tcpu->mpidr);
    }

    if (ret != QEMU_ARM_POWERCTL_RET_SUCCESS) {
        error_report("%s: failed to turn off CPU %d: err %d", __func__,
                     tcpu->cpu_id, ret);
    }
}

static AppleA13Cluster *apple_a13_find_cluster(int cluster_id)
{
    AppleA13Cluster *cluster = NULL;
    QTAILQ_FOREACH (cluster, &clusters, next) {
        if (CPU_CLUSTER(cluster)->cluster_id == cluster_id)
            return cluster;
    }
    return NULL;
}

static uint64_t apple_a13_cluster_cpreg_read(CPUARMState *env,
                                             const ARMCPRegInfo *ri)
{
    AppleA13State *tcpu = APPLE_A13(env_archcpu(env));
    AppleA13Cluster *c = apple_a13_find_cluster(tcpu->cluster_id);

    if (unlikely(!c)) {
        return 0;
    }

    return *(uint64_t *)((char *)(c) + (ri)->fieldoffset);
}

static void apple_a13_cluster_cpreg_write(CPUARMState *env,
                                          const ARMCPRegInfo *ri,
                                          uint64_t value)
{
    AppleA13State *tcpu = APPLE_A13(env_archcpu(env));
    AppleA13Cluster *c = apple_a13_find_cluster(tcpu->cluster_id);

    if (unlikely(!c)) {
        return;
    }
    *(uint64_t *)((char *)(c) + (ri)->fieldoffset) = value;
}

/* Deliver IPI */
static void apple_a13_cluster_deliver_ipi(AppleA13Cluster *c, uint64_t cpu_id,
                                          uint64_t src_cpu, uint64_t flag)
{
    if (c->cpus[cpu_id]->ipi_sr)
        return;

    c->cpus[cpu_id]->ipi_sr = 1LL | (src_cpu << IPI_SR_SRC_CPU_SHIFT) | flag;
    qemu_irq_raise(c->cpus[cpu_id]->fast_ipi);
}

static int apple_a13_cluster_pre_save(void *opaque)
{
    AppleA13Cluster *cluster = APPLE_A13_CLUSTER(opaque);
    cluster->ipi_cr = ipi_cr;
    return 0;
}

static int apple_a13_cluster_post_load(void *opaque, int version_id)
{
    AppleA13Cluster *cluster = APPLE_A13_CLUSTER(opaque);
    ipi_cr = cluster->ipi_cr;
    return 0;
}

static void apple_a13_cluster_reset(DeviceState *dev)
{
    AppleA13Cluster *cluster = APPLE_A13_CLUSTER(dev);
    memset(cluster->deferredIPI, 0, sizeof(cluster->deferredIPI));
    memset(cluster->noWakeIPI, 0, sizeof(cluster->noWakeIPI));
}

static int add_cpu_to_cluster(Object *obj, void *opaque)
{
    AppleA13Cluster *cluster = APPLE_A13_CLUSTER(opaque);
    CPUState *cpu = (CPUState *)object_dynamic_cast(obj, TYPE_CPU);
    AppleA13State *tcpu =
        (AppleA13State *)object_dynamic_cast(obj, TYPE_APPLE_A13);

    if (!cpu) {
        return 0;
    }
    cpu->cluster_index = CPU_CLUSTER(cluster)->cluster_id;
    if (!tcpu) {
        return 0;
    }
    cluster->base = tcpu->cluster_reg[0];
    cluster->size = tcpu->cluster_reg[1];
    cluster->cpus[tcpu->cpu_id] = tcpu;
    return 0;
}

static void apple_a13_cluster_realize(DeviceState *dev, Error **errp)
{
    AppleA13Cluster *cluster = APPLE_A13_CLUSTER(dev);
    object_child_foreach_recursive(OBJECT(cluster), add_cpu_to_cluster, dev);

    if (cluster->size) {
        memory_region_init_ram_device_ptr(
            &cluster->mr, OBJECT(cluster),
            TYPE_APPLE_A13_CLUSTER ".cpm-impl-reg", cluster->size,
            g_malloc0(cluster->size));
    }
}

static void apple_a13_cluster_tick(AppleA13Cluster *c)
{
    int i, j;

    for (i = 0; i < A13_MAX_CPU; i++) { /* source */
        for (j = 0; j < A13_MAX_CPU; j++) { /* target */
            if (c->cpus[j] && c->deferredIPI[i][j] &&
                !apple_a13_cpu_is_powered_off(c->cpus[j])) {
                apple_a13_cluster_deliver_ipi(c, j, i, IPI_RR_TYPE_DEFERRED);
                break;
            }
        }
    }

    for (i = 0; i < A13_MAX_CPU; i++) { /* source */
        for (j = 0; j < A13_MAX_CPU; j++) { /* target */
            if (c->cpus[j] && c->noWakeIPI[i][j] &&
                !apple_a13_cpu_is_sleep(c->cpus[j]) &&
                !apple_a13_cpu_is_powered_off(c->cpus[j])) {
                apple_a13_cluster_deliver_ipi(c, j, i, IPI_RR_TYPE_NOWAKE);
                break;
            }
        }
    }
}

static void apple_a13_cluster_ipicr_tick(void *opaque)
{
    AppleA13Cluster *cluster;
    QTAILQ_FOREACH (cluster, &clusters, next) {
        apple_a13_cluster_tick(cluster);
    }

    timer_mod_ns(ipicr_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ipi_cr);
}


static void apple_a13_cluster_reset_handler(void *opaque)
{
    if (ipicr_timer) {
        timer_del(ipicr_timer);
        ipicr_timer = NULL;
    }
    ipicr_timer =
        timer_new_ns(QEMU_CLOCK_VIRTUAL, apple_a13_cluster_ipicr_tick, NULL);
    timer_mod_ns(ipicr_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                  kDeferredIPITimerDefault);
}

static void apple_a13_cluster_instance_init(Object *obj)
{
    AppleA13Cluster *cluster = APPLE_A13_CLUSTER(obj);
    QTAILQ_INSERT_TAIL(&clusters, cluster, next);

    if (ipicr_timer == NULL) {
        qemu_register_reset(apple_a13_cluster_reset_handler, NULL);
    }
}

/* Deliver local IPI */
static void apple_a13_ipi_rr_local(CPUARMState *env, const ARMCPRegInfo *ri,
                                   uint64_t value)
{
    AppleA13State *tcpu = APPLE_A13(env_archcpu(env));

    uint32_t phys_id = (value & 0xff) | (tcpu->cluster_id << 8);
    AppleA13Cluster *c = apple_a13_find_cluster(tcpu->cluster_id);
    uint32_t cpu_id = -1;
    int i;

    for (i = 0; i < A13_MAX_CPU; i++) {
        if (c->cpus[i]) {
            if (c->cpus[i]->phys_id == phys_id) {
                cpu_id = i;
                break;
            }
        }
    }

    if (cpu_id == -1 || c->cpus[cpu_id] == NULL) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "CPU %x failed to send fast IPI to local CPU %x: value: "
                      "0x" HWADDR_FMT_plx "\n",
                      tcpu->phys_id, phys_id, value);
        return;
    }

    switch (value & IPI_RR_TYPE_MASK) {
    case IPI_RR_TYPE_NOWAKE:
        if (apple_a13_cpu_is_sleep(c->cpus[cpu_id])) {
            c->noWakeIPI[tcpu->cpu_id][cpu_id] = 1;
        } else {
            apple_a13_cluster_deliver_ipi(c, cpu_id, tcpu->cpu_id,
                                          IPI_RR_TYPE_IMMEDIATE);
        }
        break;
    case IPI_RR_TYPE_DEFERRED:
        c->deferredIPI[tcpu->cpu_id][cpu_id] = 1;
        break;
    case IPI_RR_TYPE_RETRACT:
        c->deferredIPI[tcpu->cpu_id][cpu_id] = 0;
        c->noWakeIPI[tcpu->cpu_id][cpu_id] = 0;
        break;
    case IPI_RR_TYPE_IMMEDIATE:
        apple_a13_cluster_deliver_ipi(c, cpu_id, tcpu->cpu_id,
                                      IPI_RR_TYPE_IMMEDIATE);
        break;
    default:
        g_assert_not_reached();
        break;
    }
}

/* Deliver global IPI */
static void apple_a13_ipi_rr_global(CPUARMState *env, const ARMCPRegInfo *ri,
                                    uint64_t value)
{
    AppleA13State *tcpu = APPLE_A13(env_archcpu(env));
    uint32_t cluster_id = (value >> IPI_RR_TARGET_CLUSTER_SHIFT) & 0xff;
    AppleA13Cluster *c = apple_a13_find_cluster(cluster_id);

    if (!c) {
        return;
    }

    uint32_t phys_id = (value & 0xff) | (cluster_id << 8);
    uint32_t cpu_id = -1;
    int i;

    for (i = 0; i < A13_MAX_CPU; i++) {
        if (c->cpus[i] == NULL) {
            continue;
        }
        if (c->cpus[i]->phys_id == phys_id) {
            cpu_id = i;
            break;
        }
    }

    if (cpu_id == -1 || c->cpus[cpu_id] == NULL) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "CPU %x failed to send fast IPI to global CPU %x: value: "
                      "0x" HWADDR_FMT_plx "\n",
                      tcpu->phys_id, phys_id, value);
        return;
    }

    switch (value & IPI_RR_TYPE_MASK) {
    case IPI_RR_TYPE_NOWAKE:
        if (apple_a13_cpu_is_sleep(c->cpus[cpu_id])) {
            c->noWakeIPI[tcpu->cpu_id][cpu_id] = 1;
        } else {
            apple_a13_cluster_deliver_ipi(c, cpu_id, tcpu->cpu_id,
                                          IPI_RR_TYPE_IMMEDIATE);
        }
        break;
    case IPI_RR_TYPE_DEFERRED:
        c->deferredIPI[tcpu->cpu_id][cpu_id] = 1;
        break;
    case IPI_RR_TYPE_RETRACT:
        c->deferredIPI[tcpu->cpu_id][cpu_id] = 0;
        c->noWakeIPI[tcpu->cpu_id][cpu_id] = 0;
        break;
    case IPI_RR_TYPE_IMMEDIATE:
        apple_a13_cluster_deliver_ipi(c, cpu_id, tcpu->cpu_id,
                                      IPI_RR_TYPE_IMMEDIATE);
        break;
    default:
        g_assert_not_reached();
        break;
    }
}

/* Receiving IPI */
static uint64_t apple_a13_ipi_read_sr(CPUARMState *env, const ARMCPRegInfo *ri)
{
    AppleA13State *tcpu = APPLE_A13(env_archcpu(env));

    g_assert_cmphex(env_archcpu(env)->mp_affinity, ==, tcpu->mpidr);
    return tcpu->ipi_sr;
}

/* Acknowledge received IPI */
static void apple_a13_ipi_write_sr(CPUARMState *env, const ARMCPRegInfo *ri,
                                   uint64_t value)
{
    AppleA13State *tcpu = APPLE_A13(env_archcpu(env));
    AppleA13Cluster *c = apple_a13_find_cluster(tcpu->cluster_id);
    uint64_t src_cpu = IPI_SR_SRC_CPU(value);

    tcpu->ipi_sr = 0;
    qemu_irq_lower(tcpu->fast_ipi);

    switch (value & IPI_RR_TYPE_MASK) {
    case IPI_RR_TYPE_NOWAKE:
        c->noWakeIPI[src_cpu][tcpu->cpu_id] = 0;
        break;
    case IPI_RR_TYPE_DEFERRED:
        c->deferredIPI[src_cpu][tcpu->cpu_id] = 0;
        break;
    default:
        break;
    }
}

/* Read deferred interrupt timeout (global) */
static uint64_t apple_a13_ipi_read_cr(CPUARMState *env, const ARMCPRegInfo *ri)
{
    uint64_t abstime;

    nanoseconds_to_absolutetime(ipi_cr, &abstime);
    return abstime;
}

/* Set deferred interrupt timeout (global) */
static void apple_a13_ipi_write_cr(CPUARMState *env, const ARMCPRegInfo *ri,
                                   uint64_t value)
{
    uint64_t nanosec = 0;

    absolutetime_to_nanoseconds(value, &nanosec);

    uint64_t ct;

    if (value == 0)
        value = kDeferredIPITimerDefault;

    ct = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_mod_ns(ipicr_timer, (ct / ipi_cr) * ipi_cr + nanosec);
    ipi_cr = nanosec;
}

static const ARMCPRegInfo apple_a13_cp_reginfo_tcg[] = {
    A13_CPREG_DEF(ARM64_REG_EHID3, 3, 0, 15, 3, 1, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_EHID4, 3, 0, 15, 4, 1, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_EHID10, 3, 0, 15, 10, 1, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_HID0, 3, 0, 15, 0, 0, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_HID1, 3, 0, 15, 1, 0, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_HID3, 3, 0, 15, 3, 0, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_HID4, 3, 0, 15, 4, 0, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_HID5, 3, 0, 15, 5, 0, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_HID7, 3, 0, 15, 7, 0, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_HID8, 3, 0, 15, 8, 0, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_HID9, 3, 0, 15, 9, 0, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_HID11, 3, 0, 15, 11, 0, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_HID13, 3, 0, 15, 14, 0, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_HID14, 3, 0, 15, 15, 0, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_HID16, 3, 0, 15, 15, 2, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_LSU_ERR_STS, 3, 3, 15, 0, 0, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_LSU_ERR_STS_, 3, 3, 15, 2, 0, PL1_RW, 0), // this one is supposed to be ARM64_REG_LSU_ERR_STS according to a gist file.
    A13_CPREG_DEF(ARM64_REG_FED_ERR_STS, 3, 4, 15, 0, 2, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_LLC_ERR_STS, 3, 3, 15, 8, 0, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_LLC_ERR_INF, 3, 3, 15, 10, 0, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_LLC_ERR_ADR, 3, 3, 15, 9, 0, PL1_RW, 0),
    A13_CPREG_DEF(IMP_BARRIER_LBSY_BST_SYNC_W0_EL0, 3, 3, 15, 15, 0, PL1_RW, 0),
    A13_CPREG_DEF(IMP_BARRIER_LBSY_BST_SYNC_W1_EL0, 3, 3, 15, 15, 1, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_3_3_15_7, 3, 3, 15, 7, 0, PL1_RW,
                  0x8000000000332211ULL),
    A13_CPREG_DEF(PMC0, 3, 2, 15, 0, 0, PL1_RW, 0),
    A13_CPREG_DEF(PMC1, 3, 2, 15, 1, 0, PL1_RW, 0),
    A13_CPREG_DEF(PMCR0, 3, 1, 15, 0, 0, PL1_RW, 0),
    A13_CPREG_DEF(PMCR1, 3, 1, 15, 1, 0, PL1_RW, 0),
    A13_CPREG_DEF(PMSR, 3, 1, 15, 13, 0, PL1_RW, 0),
    A13_CPREG_DEF(S3_4_c15_c0_5, 3, 4, 15, 0, 5, PL1_RW, 0),
    A13_CPREG_DEF(AMX_STATUS_EL1, 3, 4, 15, 1, 3, PL1_R, 0),
    A13_CPREG_DEF(AMX_CTL_EL1, 3, 4, 15, 1, 4, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_CYC_OVRD, 3, 5, 15, 5, 0, PL1_RW, 0),
    A13_CPREG_DEF(ARM64_REG_ACC_CFG, 3, 5, 15, 4, 0, PL1_RW, 0),
    A13_CPREG_DEF(S3_5_c15_c10_1, 3, 5, 15, 10, 1, PL0_RW, 0),
    A13_CPREG_DEF(SYS_ACC_PWR_DN_SAVE, 3, 7, 15, 2, 0, PL1_RW, 0),
    A13_CPREG_DEF(UPMPCM, 3, 7, 15, 5, 4, PL1_RW, 0),
    A13_CPREG_DEF(UPMCR0, 3, 7, 15, 0, 4, PL1_RW, 0),
    A13_CPREG_DEF(UPMSR, 3, 7, 15, 6, 4, PL1_RW, 0),
    A13_CLUSTER_CPREG_DEF(CTRR_A_LWR_EL1, 3, 4, 15, 2, 3, PL1_RW),
    A13_CLUSTER_CPREG_DEF(CTRR_A_UPR_EL1, 3, 4, 15, 2, 4, PL1_RW),
    A13_CLUSTER_CPREG_DEF(CTRR_B_LWR_EL1, 3, 4, 15, 1, 7, PL1_RW),
    A13_CLUSTER_CPREG_DEF(CTRR_B_UPR_EL1, 3, 4, 15, 1, 6, PL1_RW),
    A13_CLUSTER_CPREG_DEF(CTRR_CTL_EL1, 3, 4, 15, 2, 5, PL1_RW),
    A13_CLUSTER_CPREG_DEF(CTRR_LOCK_EL1, 3, 4, 15, 2, 2, PL1_RW),
    {
        .cp = CP_REG_ARM64_SYSREG_CP,
        .name = "ARM64_REG_IPI_RR_LOCAL",
        .opc0 = 3,
        .opc1 = 5,
        .crn = 15,
        .crm = 0,
        .opc2 = 0,
        .access = PL1_W,
        .type = ARM_CP_IO | ARM_CP_NO_RAW,
        .state = ARM_CP_STATE_AA64,
        .readfn = arm_cp_read_zero,
        .writefn = apple_a13_ipi_rr_local,
    },
    {
        .cp = CP_REG_ARM64_SYSREG_CP,
        .name = "ARM64_REG_IPI_RR_GLOBAL",
        .opc0 = 3,
        .opc1 = 5,
        .crn = 15,
        .crm = 0,
        .opc2 = 1,
        .access = PL1_W,
        .type = ARM_CP_IO | ARM_CP_NO_RAW,
        .state = ARM_CP_STATE_AA64,
        .readfn = arm_cp_read_zero,
        .writefn = apple_a13_ipi_rr_global,
    },
    {
        .cp = CP_REG_ARM64_SYSREG_CP,
        .name = "ARM64_REG_IPI_SR",
        .opc0 = 3,
        .opc1 = 5,
        .crn = 15,
        .crm = 1,
        .opc2 = 1,
        .access = PL1_RW,
        .type = ARM_CP_IO | ARM_CP_NO_RAW,
        .state = ARM_CP_STATE_AA64,
        .readfn = apple_a13_ipi_read_sr,
        .writefn = apple_a13_ipi_write_sr,
    },
    {
        .cp = CP_REG_ARM64_SYSREG_CP,
        .name = "ARM64_REG_IPI_CR",
        .opc0 = 3,
        .opc1 = 5,
        .crn = 15,
        .crm = 3,
        .opc2 = 1,
        .access = PL1_RW,
        .type = ARM_CP_IO,
        .state = ARM_CP_STATE_AA64,
        .readfn = apple_a13_ipi_read_cr,
        .writefn = apple_a13_ipi_write_cr,
    },
};

static void apple_a13_add_cpregs(AppleA13State *tcpu)
{
    ARMCPU *cpu = ARM_CPU(tcpu);
    define_arm_cp_regs(cpu, apple_a13_cp_reginfo_tcg);
    apple_a13_init_gxf(tcpu);
}

static void apple_a13_realize(DeviceState *dev, Error **errp)
{
    AppleA13State *tcpu = APPLE_A13(dev);
    AppleA13Class *tclass = APPLE_A13_GET_CLASS(dev);
    DeviceState *fiq_or;
    Object *obj = OBJECT(dev);

    object_property_set_link(OBJECT(tcpu), "memory", OBJECT(&tcpu->memory),
                             errp);
    if (*errp) {
        return;
    }
    apple_a13_add_cpregs(tcpu);
    tclass->parent_realize(dev, errp);
    if (*errp) {
        return;
    }
    apple_a13_init_gxf_override(tcpu);
    fiq_or = qdev_new(TYPE_OR_IRQ);
    object_property_add_child(obj, "fiq-or", OBJECT(fiq_or));
    qdev_prop_set_uint16(fiq_or, "num-lines", 16);
    qdev_realize_and_unref(fiq_or, NULL, errp);
    if (*errp) {
        return;
    }
    qdev_connect_gpio_out(fiq_or, 0, qdev_get_gpio_in(dev, ARM_CPU_FIQ));

    qdev_connect_gpio_out(dev, GTIMER_VIRT, qdev_get_gpio_in(fiq_or, 0));
    tcpu->fast_ipi = qdev_get_gpio_in(fiq_or, 1);
}

static void apple_a13_reset_hold(Object *obj, ResetType type)
{
    AppleA13Class *tclass = APPLE_A13_GET_CLASS(obj);
    if (tclass->parent_phases.hold != NULL) {
        tclass->parent_phases.hold(obj, type);
    }
}

static void apple_a13_instance_init(Object *obj)
{
    ARMCPU *cpu = ARM_CPU(obj);
    object_property_set_uint(obj, "cntfrq", 24000000, &error_fatal);
    object_property_add_uint64_ptr(obj, "pauth-mlo", &cpu->m_key_lo,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint64_ptr(obj, "pauth-mhi", &cpu->m_key_hi,
                                   OBJ_PROP_FLAG_READWRITE);
}

AppleA13State *apple_a13_cpu_create(DTBNode *node, char *name, uint32_t cpu_id,
                                    uint32_t phys_id, uint32_t cluster_id,
                                    uint8_t cluster_type)
{
    DeviceState *dev;
    AppleA13State *tcpu;
    ARMCPU *cpu;
    Object *obj;
    DTBProp *prop;
    uint64_t *reg;

    obj = object_new(TYPE_APPLE_A13);
    dev = DEVICE(obj);
    tcpu = APPLE_A13(dev);
    cpu = ARM_CPU(tcpu);

    if (node) {
        prop = dtb_find_prop(node, "name");
        dev->id = g_strdup((char *)prop->data);

        prop = dtb_find_prop(node, "cpu-id");
        g_assert_cmpuint(prop->length, ==, 4);
        tcpu->cpu_id = *(unsigned int *)prop->data;

        prop = dtb_find_prop(node, "reg");
        g_assert_cmpuint(prop->length, ==, 4);
        tcpu->phys_id = *(unsigned int *)prop->data;

        prop = dtb_find_prop(node, "cluster-id");
        g_assert_cmpuint(prop->length, ==, 4);
        tcpu->cluster_id = *(unsigned int *)prop->data;
    } else {
        dev->id = g_strdup(name);
        tcpu->cpu_id = cpu_id;
        tcpu->phys_id = phys_id;
        tcpu->cluster_id = cluster_id;
    }

    tcpu->mpidr = tcpu->phys_id | (1LL << 31);

    cpu->midr = FIELD_DP64(0, MIDR_EL1, IMPLEMENTER, 0x61); /* Apple */
    /* chip-revision = (variant << 4) | (revision) */
    cpu->midr = FIELD_DP64(cpu->midr, MIDR_EL1, VARIANT, 0x1);
    cpu->midr = FIELD_DP64(cpu->midr, MIDR_EL1, REVISION, 0x1);

    if (node) {
        prop = dtb_find_prop(node, "cluster-type");
        cluster_type = *prop->data;
    }
    switch (cluster_type) {
    case 'P': // Lightning
        tcpu->mpidr |= (1 << ARM_AFF2_SHIFT);
        cpu->midr = FIELD_DP64(cpu->midr, MIDR_EL1, PARTNUM, 0x12);
        break;
    case 'E': // Thunder
        cpu->midr = FIELD_DP64(cpu->midr, MIDR_EL1, PARTNUM, 0x13);
        break;
    default:
        break;
    }

    object_property_set_uint(obj, "mp-affinity", tcpu->mpidr, &error_fatal);

    if (node != NULL) {
        dtb_remove_prop_named(node, "reg-private");
        dtb_remove_prop_named(node, "cpu-uttdbg-reg");
    }

    if (tcpu->cpu_id == 0 || node == NULL) {
        if (node != NULL) {
            dtb_set_prop(node, "state", 8, "running");
        }
        object_property_set_bool(obj, "start-powered-off", false, NULL);
    } else {
        object_property_set_bool(obj, "start-powered-off", true, NULL);
    }

    // QARMA is too slow
    object_property_set_bool(obj, "pauth-impdef", true, NULL);

    // Need to set the CPU frequencies instead of iBoot
    if (node) {
        dtb_set_prop_u64(node, "timebase-frequency", 24000000);
        dtb_set_prop_u64(node, "fixed-frequency", 24000000);
        dtb_set_prop_u64(node, "peripheral-frequency", 24000000);
        dtb_set_prop_u64(node, "memory-frequency", 24000000);
        dtb_set_prop_u64(node, "bus-frequency", 24000000);
        dtb_set_prop_u64(node, "clock-frequency", 24000000);
    }

    object_property_set_bool(obj, "has_el3", false, NULL);
    object_property_set_bool(obj, "has_el2", false, NULL);

    memory_region_init(&tcpu->memory, obj, "cpu-memory", UINT64_MAX);
    memory_region_init_alias(&tcpu->sysmem, obj, "sysmem", get_system_memory(),
                             0, UINT64_MAX);
    memory_region_add_subregion_overlap(&tcpu->memory, 0, &tcpu->sysmem, -2);

    if (node) {
        prop = dtb_find_prop(node, "cpu-impl-reg");
        if (prop) {
            g_assert_cmpuint(prop->length, ==, 16);

            reg = (uint64_t *)prop->data;

            memory_region_init_ram_device_ptr(&tcpu->impl_reg, obj,
                                              TYPE_APPLE_A13 ".impl-reg",
                                              reg[1], g_malloc0(reg[1]));
            memory_region_add_subregion(get_system_memory(), reg[0],
                                        &tcpu->impl_reg);
        }

        prop = dtb_find_prop(node, "coresight-reg");
        if (prop) {
            g_assert_cmpuint(prop->length, ==, 16);

            reg = (uint64_t *)prop->data;

            memory_region_init_ram_device_ptr(&tcpu->coresight_reg, obj,
                                              TYPE_APPLE_A13 ".coresight-reg",
                                              reg[1], g_malloc0(reg[1]));
            memory_region_add_subregion(get_system_memory(), reg[0],
                                        &tcpu->coresight_reg);
        }

        prop = dtb_find_prop(node, "cpm-impl-reg");
        if (prop) {
            g_assert_cmpuint(prop->length, ==, 16);
            memcpy(tcpu->cluster_reg, prop->data, prop->length);
        }
    }
    return tcpu;
}

static Property apple_a13_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static Property apple_a13_cluster_properties[] = {
    DEFINE_PROP_UINT32("cluster-type", AppleA13Cluster, cluster_type, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription vmstate_apple_a13 = {
    .name = "apple_a13",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields =
        (VMStateField[]){
            VMSTATE_A13_CPREG(ARM64_REG_EHID3),
            VMSTATE_A13_CPREG(ARM64_REG_EHID4),
            VMSTATE_A13_CPREG(ARM64_REG_EHID10),
            VMSTATE_A13_CPREG(ARM64_REG_HID0),
            VMSTATE_A13_CPREG(ARM64_REG_HID1),
            VMSTATE_A13_CPREG(ARM64_REG_HID3),
            VMSTATE_A13_CPREG(ARM64_REG_HID4),
            VMSTATE_A13_CPREG(ARM64_REG_HID5),
            VMSTATE_A13_CPREG(ARM64_REG_HID7),
            VMSTATE_A13_CPREG(ARM64_REG_HID8),
            VMSTATE_A13_CPREG(ARM64_REG_HID9),
            VMSTATE_A13_CPREG(ARM64_REG_HID11),
            VMSTATE_A13_CPREG(ARM64_REG_HID13),
            VMSTATE_A13_CPREG(ARM64_REG_HID14),
            VMSTATE_A13_CPREG(ARM64_REG_HID16),
            VMSTATE_A13_CPREG(ARM64_REG_LSU_ERR_STS),
            VMSTATE_A13_CPREG(ARM64_REG_LSU_ERR_STS_),
            VMSTATE_A13_CPREG(ARM64_REG_FED_ERR_STS),
            VMSTATE_A13_CPREG(ARM64_REG_LLC_ERR_STS),
            VMSTATE_A13_CPREG(ARM64_REG_LLC_ERR_INF),
            VMSTATE_A13_CPREG(ARM64_REG_LLC_ERR_ADR),
            VMSTATE_A13_CPREG(PMC0),
            VMSTATE_A13_CPREG(PMC1),
            VMSTATE_A13_CPREG(PMCR0),
            VMSTATE_A13_CPREG(PMCR1),
            VMSTATE_A13_CPREG(PMSR),
            VMSTATE_A13_CPREG(S3_4_c15_c0_5),
            VMSTATE_A13_CPREG(AMX_STATUS_EL1),
            VMSTATE_A13_CPREG(AMX_CTL_EL1),
            VMSTATE_A13_CPREG(ARM64_REG_CYC_OVRD),
            VMSTATE_A13_CPREG(ARM64_REG_ACC_CFG),
            VMSTATE_A13_CPREG(S3_5_c15_c10_1),
            VMSTATE_A13_CPREG(SYS_ACC_PWR_DN_SAVE),
            VMSTATE_A13_CPREG(UPMPCM),
            VMSTATE_A13_CPREG(UPMCR0),
            VMSTATE_A13_CPREG(UPMSR),
            VMSTATE_UINT64(env.keys.m.lo, ARMCPU),
            VMSTATE_UINT64(env.keys.m.hi, ARMCPU),
            VMSTATE_END_OF_LIST(),
        }
};

static const VMStateDescription vmstate_apple_a13_cluster = {
    .name = "apple_a13_cluster",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = apple_a13_cluster_pre_save,
    .post_load = apple_a13_cluster_post_load,
    .fields =
        (VMStateField[]){
            VMSTATE_UINT32_2DARRAY(deferredIPI, AppleA13Cluster, A13_MAX_CPU,
                                   A13_MAX_CPU),
            VMSTATE_UINT32_2DARRAY(noWakeIPI, AppleA13Cluster, A13_MAX_CPU,
                                   A13_MAX_CPU),
            VMSTATE_UINT64(tick, AppleA13Cluster),
            VMSTATE_UINT64(ipi_cr, AppleA13Cluster),
            VMSTATE_A13_CLUSTER_CPREG(CTRR_A_LWR_EL1),
            VMSTATE_A13_CLUSTER_CPREG(CTRR_A_UPR_EL1),
            VMSTATE_A13_CLUSTER_CPREG(CTRR_B_LWR_EL1),
            VMSTATE_A13_CLUSTER_CPREG(CTRR_B_UPR_EL1),
            VMSTATE_A13_CLUSTER_CPREG(CTRR_CTL_EL1),
            VMSTATE_A13_CLUSTER_CPREG(CTRR_LOCK_EL1),
            VMSTATE_END_OF_LIST(),
        }
};

static void apple_a13_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);
    AppleA13Class *tc = APPLE_A13_CLASS(klass);

    device_class_set_parent_realize(dc, apple_a13_realize, &tc->parent_realize);
    resettable_class_set_parent_phases(rc, NULL, apple_a13_reset_hold, NULL,
                                       &tc->parent_phases);
    dc->desc = "Apple A13 CPU";
    dc->vmsd = &vmstate_apple_a13;
    set_bit(DEVICE_CATEGORY_CPU, dc->categories);
    device_class_set_props(dc, apple_a13_properties);
}

static void apple_a13_cluster_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = apple_a13_cluster_realize;
    device_class_set_legacy_reset(dc, apple_a13_cluster_reset);
    dc->desc = "Apple A13 CPU Cluster";
    dc->user_creatable = false;
    dc->vmsd = &vmstate_apple_a13_cluster;
    device_class_set_props(dc, apple_a13_cluster_properties);
}

static const TypeInfo apple_a13_info = {
    .name = TYPE_APPLE_A13,
    .parent = ARM_CPU_TYPE_NAME("max"),
    .instance_size = sizeof(AppleA13State),
    .instance_init = apple_a13_instance_init,
    .class_size = sizeof(AppleA13Class),
    .class_init = apple_a13_class_init,
};

static const TypeInfo apple_a13_cluster_info = {
    .name = TYPE_APPLE_A13_CLUSTER,
    .parent = TYPE_CPU_CLUSTER,
    .instance_size = sizeof(AppleA13Cluster),
    .instance_init = apple_a13_cluster_instance_init,
    .class_init = apple_a13_cluster_class_init,
};

static void apple_a13_register_types(void)
{
    type_register_static(&apple_a13_info);
    type_register_static(&apple_a13_cluster_info);
}

type_init(apple_a13_register_types);
