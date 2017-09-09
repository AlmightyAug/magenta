// Copyright 2016 The Fuchsia Authors
// Copyright (c) 2012-2015 Travis Geiselbrecht
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#if WITH_DEV_PCIE
#include <dev/pcie_bus_driver.h>
#include <dev/pcie_platform.h>
#include <dev/pcie_root.h>
#include <dev/interrupt/arm_gicv2m_msi.h>
#include <inttypes.h>
#include <lk/init.h>
#include <fbl/alloc_checker.h>
#include <fbl/ref_ptr.h>
#include <trace.h>
#include <mdi/mdi.h>
#include <mdi/mdi-defs.h>
#include <pdev/driver.h>
#include <pdev/interrupt.h>

class ArmGicV2PciePlatformSupport : public PciePlatformInterface {
public:
    ArmGicV2PciePlatformSupport(bool has_msi_gic)
        : PciePlatformInterface(has_msi_gic ? MsiSupportLevel::MSI_WITH_MASKING
                                            : MsiSupportLevel::NONE) { }

    status_t AllocMsiBlock(uint requested_irqs,
                           bool can_target_64bit,
                           bool is_msix,
                           pcie_msi_block_t* out_block) override {
        return arm_gicv2m_alloc_msi_block(requested_irqs, can_target_64bit, is_msix, out_block);
    }

    void FreeMsiBlock(pcie_msi_block_t* block) override {
        arm_gicv2m_free_msi_block(block);
    }

    void RegisterMsiHandler(const pcie_msi_block_t* block,
                            uint                    msi_id,
                            int_handler             handler,
                            void*                   ctx) override {
        arm_gicv2m_register_msi_handler(block, msi_id, handler, ctx);
    }

    void MaskUnmaskMsi(const pcie_msi_block_t* block,
                       uint                    msi_id,
                       bool                    mask) override {
        arm_gicv2m_mask_unmask_msi(block, msi_id, mask);
    }
};

static void arm_gicv2_pcie_init(mdi_node_ref_t* node, uint level) {
    /* Initialize the MSI allocator */
    status_t res = arm_gicv2m_msi_init();
    if (res != MX_OK)
        TRACEF("Failed to initialize MSI allocator (res = %d).  PCI will be "
               "restricted to legacy IRQ mode.\n", res);

    /* Initialize the PCI platform supported based on whether or not we support MSI */
    static ArmGicV2PciePlatformSupport platform_pcie_support(res == MX_OK);

    res = PcieBusDriver::InitializeDriver(platform_pcie_support);
    if (res != MX_OK) {
        TRACEF("Failed to initialize PCI bus driver (res %d).  "
               "PCI will be non-functional.\n", res);
    }
}

LK_PDEV_INIT(arm_gicv2_pcie_init, MDI_ARM_GIC_V2_PCIE, arm_gicv2_pcie_init, LK_INIT_LEVEL_PLATFORM);

#endif  // if WITH_DEV_PCIE
