/*
 * Apple A9 CPU.
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
#include "hw/arm/apple-silicon/a9.h"
#include "hw/arm/apple-silicon/dtb.h"
#include "hw/or-irq.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "arm-powerctl.h"
#include "target/arm/cpregs.h"

#define VMSTATE_A9_CPREG(name) \
    VMSTATE_UINT64(A9_CPREG_VAR_NAME(name), AppleA9State)

#define A9_CPREG_DEF(p_name, p_op0, p_op1, p_crn, p_crm, p_op2, p_access,      \
                     p_reset)                                                  \
    {                                                                          \
        .cp = CP_REG_ARM64_SYSREG_CP, .name = #p_name, .opc0 = p_op0,          \
        .crn = p_crn, .crm = p_crm, .opc1 = p_op1, .opc2 = p_op2,              \
        .access = p_access, .resetvalue = p_reset, .state = ARM_CP_STATE_AA64, \
        .type = ARM_CP_OVERRIDE,                                               \
        .fieldoffset = offsetof(AppleA9State, A9_CPREG_VAR_NAME(p_name)) -     \
                       offsetof(ARMCPU, env),                                  \
    }

inline bool apple_a9_cpu_is_sleep(AppleA9State *tcpu)
{
    return CPU(tcpu)->halted;
}

inline bool apple_a9_cpu_is_powered_off(AppleA9State *tcpu)
{
    return ARM_CPU(tcpu)->power_state == PSCI_OFF;
}

void apple_a9_cpu_start(AppleA9State *tcpu)
{
    int ret = QEMU_ARM_POWERCTL_RET_SUCCESS;

    if (ARM_CPU(tcpu)->power_state != PSCI_ON) {
        ret = arm_set_cpu_on_and_reset(tcpu->mpidr);
    }

    if (ret != QEMU_ARM_POWERCTL_RET_SUCCESS) {
        error_report("Failed to bring up CPU %d: err %d", tcpu->cpu_id, ret);
    }
}

static const ARMCPRegInfo a9_cp_reginfo_tcg[] = {
    A9_CPREG_DEF(HID11, 3, 0, 15, 13, 0, PL1_RW, 0),
    A9_CPREG_DEF(HID3, 3, 0, 15, 3, 0, PL1_RW, 0),
    A9_CPREG_DEF(HID4, 3, 0, 15, 4, 0, PL1_RW, 0),
    A9_CPREG_DEF(HID5, 3, 0, 15, 5, 0, PL1_RW, 0),
    A9_CPREG_DEF(HID7, 3, 0, 15, 7, 0, PL1_RW, 0),
    A9_CPREG_DEF(HID8, 3, 0, 15, 8, 0, PL1_RW, 0),
    A9_CPREG_DEF(PMCR0, 3, 1, 15, 0, 0, PL1_RW, 0),
    A9_CPREG_DEF(PMCR1, 3, 1, 15, 1, 0, PL1_RW, 0),
    A9_CPREG_DEF(PMCR2, 3, 1, 15, 2, 0, PL1_RW, 0),
    A9_CPREG_DEF(PMCR4, 3, 1, 15, 4, 0, PL1_RW, 0),
    A9_CPREG_DEF(PMESR0, 3, 1, 15, 5, 0, PL1_RW, 0),
    A9_CPREG_DEF(PMESR1, 3, 1, 15, 6, 0, PL1_RW, 0),
    A9_CPREG_DEF(OPMAT0, 3, 1, 15, 7, 0, PL1_RW, 0),
    A9_CPREG_DEF(OPMAT1, 3, 1, 15, 8, 0, PL1_RW, 0),
    A9_CPREG_DEF(OPMSK0, 3, 1, 15, 9, 0, PL1_RW, 0),
    A9_CPREG_DEF(OPMSK1, 3, 1, 15, 10, 0, PL1_RW, 0),
    A9_CPREG_DEF(PMSR, 3, 1, 15, 13, 0, PL1_RW, 0),
    A9_CPREG_DEF(PMC0, 3, 2, 15, 0, 0, PL1_RW, 0),
    A9_CPREG_DEF(PMC1, 3, 2, 15, 1, 0, PL1_RW, 0),
    A9_CPREG_DEF(PMTRHLD6, 3, 2, 15, 12, 0, PL1_RW, 0),
    A9_CPREG_DEF(PMTRHLD4, 3, 2, 15, 13, 0, PL1_RW, 0),
    A9_CPREG_DEF(PMTRHLD2, 3, 2, 15, 14, 0, PL1_RW, 0),
    A9_CPREG_DEF(PMMMAP, 3, 2, 15, 15, 0, PL1_RW, 0),
    A9_CPREG_DEF(SYS_LSU_ERR_STS, 3, 3, 15, 0, 0, PL1_RW, 0),
    A9_CPREG_DEF(LSU_ERR_STS, 3, 3, 15, 8, 0, PL1_RW, 0),
    A9_CPREG_DEF(LSU_ERR_ADR, 3, 3, 15, 9, 0, PL1_RW, 0),
    A9_CPREG_DEF(L2C_ERR_INF, 3, 3, 15, 10, 0, PL1_RW, 0),
    A9_CPREG_DEF(FED_ERR_STS, 3, 4, 15, 0, 0, PL1_RW, 0),
    A9_CPREG_DEF(CYC_CFG, 3, 5, 15, 4, 0, PL1_RW, 0),
    A9_CPREG_DEF(RMR_EL3, 3, 6, 12, 0, 2, PL1_RW, 0),
    A9_CPREG_DEF(MMU_ERR_STS, 3, 6, 15, 0, 0, PL1_RW, 0),
};

static void a9_add_cpregs(AppleA9State *tcpu)
{
    ARMCPU *cpu = ARM_CPU(tcpu);
    define_arm_cp_regs(cpu, a9_cp_reginfo_tcg);
}

static void apple_a9_realize(DeviceState *dev, Error **errp)
{
    AppleA9State *tcpu = APPLE_A9(dev);
    AppleA9Class *tclass = APPLE_A9_GET_CLASS(dev);
    DeviceState *fiq_or;
    Object *obj = OBJECT(dev);

    object_property_set_link(OBJECT(tcpu), "memory", OBJECT(&tcpu->memory),
                             errp);
    if (*errp) {
        return;
    }
    a9_add_cpregs(tcpu);
    tclass->parent_realize(dev, errp);
    if (*errp) {
        return;
    }
    fiq_or = qdev_new(TYPE_OR_IRQ);
    object_property_add_child(obj, "fiq-or", OBJECT(fiq_or));
    qdev_prop_set_uint16(fiq_or, "num-lines", 16);
    qdev_realize_and_unref(fiq_or, NULL, errp);
    if (*errp) {
        return;
    }
    qdev_connect_gpio_out(fiq_or, 0, qdev_get_gpio_in(dev, ARM_CPU_FIQ));

    qdev_connect_gpio_out(dev, GTIMER_VIRT, qdev_get_gpio_in(fiq_or, 0));
}

// static void apple_a9_reset(DeviceState *dev)
// {
//     AppleA9Class *tclass = APPLE_A9_GET_CLASS(dev);
//     tclass->parent_reset(dev);
// }

static void apple_a9_instance_init(Object *obj)
{
    object_property_set_uint(obj, "cntfrq", 24000000, &error_fatal);
}

AppleA9State *apple_a9_create(DTBNode *node, char *name, uint32_t cpu_id,
                              uint32_t phys_id)
{
    DeviceState *dev;
    AppleA9State *tcpu;
    ARMCPU *cpu;
    Object *obj;
    DTBProp *prop;
    uint64_t *reg;

    obj = object_new(TYPE_APPLE_A9);
    dev = DEVICE(obj);
    tcpu = APPLE_A9(dev);
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
    } else {
        dev->id = g_strdup(name);
        tcpu->cpu_id = cpu_id;
        tcpu->phys_id = phys_id;
    }

    tcpu->mpidr = tcpu->phys_id | (1LL << 31) | (1 << ARM_AFF2_SHIFT);

    cpu->midr = FIELD_DP64(0, MIDR_EL1, IMPLEMENTER, 0x61);
    cpu->midr = FIELD_DP64(cpu->midr, MIDR_EL1, ARCHITECTURE, 0xf);
    cpu->midr = FIELD_DP64(cpu->midr, MIDR_EL1, PARTNUM, 0x4); /* Maui */
    cpu->midr = FIELD_DP64(cpu->midr, MIDR_EL1, VARIANT, 0x1); /* B1 */
    cpu->midr = FIELD_DP64(cpu->midr, MIDR_EL1, REVISION, 0x1);

    cpu->isar.id_aa64mmfr1 =
        FIELD_DP64(cpu->isar.id_aa64mmfr1, ID_AA64MMFR1, PAN, 0);

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

    // Need to set the CPU frequencies instead of iBoot
    if (node) {
        dtb_set_prop_u64(node, "timebase-frequency", 24000000);
        dtb_set_prop_u64(node, "fixed-frequency", 24000000);
        dtb_set_prop_u64(node, "peripheral-frequency", 24000000);
        dtb_set_prop_u64(node, "memory-frequency", 24000000);
        dtb_set_prop_u64(node, "bus-frequency", 24000000);
        dtb_set_prop_u64(node, "clock-frequency", 24000000);
    }

    object_property_set_bool(obj, "has_el3", true, NULL);
    object_property_set_bool(obj, "has_el2", true, NULL);

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
                                              TYPE_APPLE_A9 ".impl-reg", reg[1],
                                              g_malloc0(reg[1]));
            memory_region_add_subregion(get_system_memory(), reg[0],
                                        &tcpu->impl_reg);
        }

        prop = dtb_find_prop(node, "coresight-reg");
        if (prop) {
            g_assert_cmpuint(prop->length, ==, 16);

            reg = (uint64_t *)prop->data;

            memory_region_init_ram_device_ptr(&tcpu->coresight_reg, obj,
                                              TYPE_APPLE_A9 ".coresight-reg",
                                              reg[1], g_malloc0(reg[1]));
            memory_region_add_subregion(get_system_memory(), reg[0],
                                        &tcpu->coresight_reg);
        }
    }

    return tcpu;
}

static Property apple_a9_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription vmstate_apple_a9 = {
    .name = "apple_a9",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields =
        (VMStateField[]){
            VMSTATE_A9_CPREG(HID11),       VMSTATE_A9_CPREG(HID3),
            VMSTATE_A9_CPREG(HID4),        VMSTATE_A9_CPREG(HID5),
            VMSTATE_A9_CPREG(HID7),        VMSTATE_A9_CPREG(HID8),
            VMSTATE_A9_CPREG(PMCR0),       VMSTATE_A9_CPREG(PMCR1),
            VMSTATE_A9_CPREG(PMCR2),       VMSTATE_A9_CPREG(PMCR4),
            VMSTATE_A9_CPREG(PMESR0),      VMSTATE_A9_CPREG(PMESR1),
            VMSTATE_A9_CPREG(OPMAT0),      VMSTATE_A9_CPREG(OPMAT1),
            VMSTATE_A9_CPREG(OPMSK0),      VMSTATE_A9_CPREG(OPMSK1),
            VMSTATE_A9_CPREG(PMSR),        VMSTATE_A9_CPREG(PMC0),
            VMSTATE_A9_CPREG(PMC1),        VMSTATE_A9_CPREG(PMTRHLD6),
            VMSTATE_A9_CPREG(PMTRHLD4),    VMSTATE_A9_CPREG(PMTRHLD2),
            VMSTATE_A9_CPREG(PMMMAP),      VMSTATE_A9_CPREG(SYS_LSU_ERR_STS),
            VMSTATE_A9_CPREG(LSU_ERR_STS), VMSTATE_A9_CPREG(LSU_ERR_ADR),
            VMSTATE_A9_CPREG(L2C_ERR_INF), VMSTATE_A9_CPREG(FED_ERR_STS),
            VMSTATE_A9_CPREG(CYC_CFG),     VMSTATE_A9_CPREG(RMR_EL3),
            VMSTATE_A9_CPREG(MMU_ERR_STS), VMSTATE_END_OF_LIST(),
        }
};

static void apple_a9_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    AppleA9Class *tc = APPLE_A9_CLASS(klass);

    device_class_set_parent_realize(dc, apple_a9_realize, &tc->parent_realize);
    // device_class_set_parent_reset(dc, apple_a9_reset, &tc->parent_reset);
    dc->desc = "Apple A9 CPU";
    dc->vmsd = &vmstate_apple_a9;
    set_bit(DEVICE_CATEGORY_CPU, dc->categories);
    device_class_set_props(dc, apple_a9_properties);
}

static const TypeInfo apple_a9_info = {
    .name = TYPE_APPLE_A9,
    .parent = ARM_CPU_TYPE_NAME("max"),
    .instance_size = sizeof(AppleA9State),
    .instance_init = apple_a9_instance_init,
    .class_size = sizeof(AppleA9Class),
    .class_init = apple_a9_class_init,
};

static void apple_a9_register_types(void)
{
    type_register_static(&apple_a9_info);
}

type_init(apple_a9_register_types);
