// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#pragma once

#include <mxtl/canary.h>
#include <mxtl/macros.h>
#include <sys/types.h>
#include <kernel/vm.h>

// flags
const uint ARCH_MMU_FLAG_CACHED =          (0u << 0);
const uint ARCH_MMU_FLAG_UNCACHED =        (1u << 0);
const uint ARCH_MMU_FLAG_UNCACHED_DEVICE = (2u << 0); // only exists on some arches, otherwise UNCACHED
const uint ARCH_MMU_FLAG_WRITE_COMBINING = (3u << 0); // only exists on some arches, otherwise UNCACHED
const uint ARCH_MMU_FLAG_CACHE_MASK =      (3u << 0);
const uint ARCH_MMU_FLAG_PERM_USER =       (1u << 2);
const uint ARCH_MMU_FLAG_PERM_READ =       (1u << 3);
const uint ARCH_MMU_FLAG_PERM_WRITE =      (1u << 4);
const uint ARCH_MMU_FLAG_PERM_EXECUTE =    (1u << 5);
const uint ARCH_MMU_FLAG_NS =              (1u << 6); // NON-SECURE
const uint ARCH_MMU_FLAG_INVALID =         (1u << 7); // indicates that flags are not specified

const uint ARCH_ASPACE_FLAG_KERNEL =       (1u << 0);

// per arch base class api to encapsulate the mmu routines on an aspace
class ArchVmAspaceBase {
public:
    ArchVmAspaceBase() {}
    virtual ~ArchVmAspaceBase() {}

    DISALLOW_COPY_ASSIGN_AND_MOVE(ArchVmAspaceBase);

    virtual status_t Init(vaddr_t base, size_t size, uint mmu_flags) = 0;
    virtual status_t Destroy() = 0;

    // main methods
    virtual status_t Map(vaddr_t vaddr, paddr_t paddr, size_t count, uint mmu_flags, size_t* mapped) = 0;
    virtual status_t Unmap(vaddr_t vaddr, size_t count, size_t* unmapped) = 0;
    virtual status_t Protect(vaddr_t vaddr, size_t count, uint mmu_flags) = 0;
    virtual status_t Query(vaddr_t vaddr, paddr_t* paddr, uint* mmu_flags) = 0;

    virtual vaddr_t PickSpot(vaddr_t base, uint prev_region_mmu_flags,
                     vaddr_t end, uint next_region_mmu_flags,
                     vaddr_t align, size_t size, uint mmu_flags) {
        return PAGE_ALIGN(base);
    }

    static void ContextSwitch(ArchVmAspaceBase *from, ArchVmAspaceBase *to);

private:
    mxtl::Canary<mxtl::magic("VAAS")> canary_;
};

#include <arch/aspace.h>

