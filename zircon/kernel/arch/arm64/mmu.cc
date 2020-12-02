// Copyright 2016 The Fuchsia Authors
// Copyright (c) 2014 Google Inc. All rights reserved
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT
#include "arch/arm64/mmu.h"

#include <align.h>
#include <assert.h>
#include <bits.h>
#include <debug.h>
#include <inttypes.h>
#include <lib/heap.h>
#include <lib/ktrace.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <trace.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include <arch/arm64/hypervisor/el2_state.h>
#include <arch/aspace.h>
#include <fbl/auto_call.h>
#include <fbl/auto_lock.h>
#include <kernel/mutex.h>
#include <ktl/algorithm.h>
#include <vm/arch_vm_aspace.h>
#include <vm/physmap.h>
#include <vm/pmm.h>
#include <vm/vm.h>

#include "asid_allocator.h"

#define LOCAL_TRACE 0
#define TRACE_CONTEXT_SWITCH 0

/* ktraces just local to this file */
#define LOCAL_KTRACE_ENABLE 0

#define LOCAL_KTRACE(string, args...)                                                         \
  ktrace_probe(LocalTrace<LOCAL_KTRACE_ENABLE>, TraceContext::Cpu, KTRACE_STRING_REF(string), \
               ##args)

static_assert(((long)KERNEL_BASE >> MMU_KERNEL_SIZE_SHIFT) == -1, "");
static_assert(((long)KERNEL_ASPACE_BASE >> MMU_KERNEL_SIZE_SHIFT) == -1, "");
static_assert(MMU_KERNEL_SIZE_SHIFT <= 48, "");
static_assert(MMU_KERNEL_SIZE_SHIFT >= 25, "");

// Static relocated base to prepare for KASLR. Used at early boot and by gdb
// script to know the target relocated address.
// TODO(fxbug.dev/24762): Choose it randomly.
#if DISABLE_KASLR
uint64_t kernel_relocated_base = KERNEL_BASE;
#else
uint64_t kernel_relocated_base = 0xffffffff10000000;
#endif

// The main translation table for the kernel. Globally declared because it's reached
// from assembly.
pte_t arm64_kernel_translation_table[MMU_KERNEL_PAGE_TABLE_ENTRIES_TOP] __ALIGNED(
    MMU_KERNEL_PAGE_TABLE_ENTRIES_TOP * 8);

// Global accessor for the kernel page table
pte_t* arm64_get_kernel_ptable() { return arm64_kernel_translation_table; }

namespace {

AsidAllocator asid;

// Convert user level mmu flags to flags that go in L1 descriptors.
pte_t mmu_flags_to_s1_pte_attr(uint flags) {
  pte_t attr = MMU_PTE_ATTR_AF;

  switch (flags & ARCH_MMU_FLAG_CACHE_MASK) {
    case ARCH_MMU_FLAG_CACHED:
      attr |= MMU_PTE_ATTR_NORMAL_MEMORY | MMU_PTE_ATTR_SH_INNER_SHAREABLE;
      break;
    case ARCH_MMU_FLAG_WRITE_COMBINING:
      attr |= MMU_PTE_ATTR_NORMAL_UNCACHED | MMU_PTE_ATTR_SH_INNER_SHAREABLE;
      break;
    case ARCH_MMU_FLAG_UNCACHED:
      attr |= MMU_PTE_ATTR_STRONGLY_ORDERED;
      break;
    case ARCH_MMU_FLAG_UNCACHED_DEVICE:
      attr |= MMU_PTE_ATTR_DEVICE;
      break;
    default:
      PANIC_UNIMPLEMENTED;
  }

  switch (flags & (ARCH_MMU_FLAG_PERM_USER | ARCH_MMU_FLAG_PERM_WRITE)) {
    case 0:
      attr |= MMU_PTE_ATTR_AP_P_RO_U_NA;
      break;
    case ARCH_MMU_FLAG_PERM_WRITE:
      attr |= MMU_PTE_ATTR_AP_P_RW_U_NA;
      break;
    case ARCH_MMU_FLAG_PERM_USER:
      attr |= MMU_PTE_ATTR_AP_P_RO_U_RO;
      break;
    case ARCH_MMU_FLAG_PERM_USER | ARCH_MMU_FLAG_PERM_WRITE:
      attr |= MMU_PTE_ATTR_AP_P_RW_U_RW;
      break;
  }

  if (!(flags & ARCH_MMU_FLAG_PERM_EXECUTE)) {
    attr |= MMU_PTE_ATTR_UXN | MMU_PTE_ATTR_PXN;
  }
  if (flags & ARCH_MMU_FLAG_NS) {
    attr |= MMU_PTE_ATTR_NON_SECURE;
  }

  return attr;
}

void s1_pte_attr_to_mmu_flags(pte_t pte, uint* mmu_flags) {
  switch (pte & MMU_PTE_ATTR_ATTR_INDEX_MASK) {
    case MMU_PTE_ATTR_STRONGLY_ORDERED:
      *mmu_flags |= ARCH_MMU_FLAG_UNCACHED;
      break;
    case MMU_PTE_ATTR_DEVICE:
      *mmu_flags |= ARCH_MMU_FLAG_UNCACHED_DEVICE;
      break;
    case MMU_PTE_ATTR_NORMAL_UNCACHED:
      *mmu_flags |= ARCH_MMU_FLAG_WRITE_COMBINING;
      break;
    case MMU_PTE_ATTR_NORMAL_MEMORY:
      *mmu_flags |= ARCH_MMU_FLAG_CACHED;
      break;
    default:
      PANIC_UNIMPLEMENTED;
  }

  *mmu_flags |= ARCH_MMU_FLAG_PERM_READ;
  switch (pte & MMU_PTE_ATTR_AP_MASK) {
    case MMU_PTE_ATTR_AP_P_RW_U_NA:
      *mmu_flags |= ARCH_MMU_FLAG_PERM_WRITE;
      break;
    case MMU_PTE_ATTR_AP_P_RW_U_RW:
      *mmu_flags |= ARCH_MMU_FLAG_PERM_USER | ARCH_MMU_FLAG_PERM_WRITE;
      break;
    case MMU_PTE_ATTR_AP_P_RO_U_NA:
      break;
    case MMU_PTE_ATTR_AP_P_RO_U_RO:
      *mmu_flags |= ARCH_MMU_FLAG_PERM_USER;
      break;
  }

  if (!((pte & MMU_PTE_ATTR_UXN) && (pte & MMU_PTE_ATTR_PXN))) {
    *mmu_flags |= ARCH_MMU_FLAG_PERM_EXECUTE;
  }
  if (pte & MMU_PTE_ATTR_NON_SECURE) {
    *mmu_flags |= ARCH_MMU_FLAG_NS;
  }
}

pte_t mmu_flags_to_s2_pte_attr(uint flags) {
  pte_t attr = MMU_PTE_ATTR_AF;

  switch (flags & ARCH_MMU_FLAG_CACHE_MASK) {
    case ARCH_MMU_FLAG_CACHED:
      attr |= MMU_S2_PTE_ATTR_NORMAL_MEMORY | MMU_PTE_ATTR_SH_INNER_SHAREABLE;
      break;
    case ARCH_MMU_FLAG_WRITE_COMBINING:
      attr |= MMU_S2_PTE_ATTR_NORMAL_UNCACHED | MMU_PTE_ATTR_SH_INNER_SHAREABLE;
      break;
    case ARCH_MMU_FLAG_UNCACHED:
      attr |= MMU_S2_PTE_ATTR_STRONGLY_ORDERED;
      break;
    case ARCH_MMU_FLAG_UNCACHED_DEVICE:
      attr |= MMU_S2_PTE_ATTR_DEVICE;
      break;
    default:
      PANIC_UNIMPLEMENTED;
  }

  if (flags & ARCH_MMU_FLAG_PERM_WRITE) {
    attr |= MMU_S2_PTE_ATTR_S2AP_RW;
  } else {
    attr |= MMU_S2_PTE_ATTR_S2AP_RO;
  }
  if (!(flags & ARCH_MMU_FLAG_PERM_EXECUTE)) {
    attr |= MMU_S2_PTE_ATTR_XN;
  }

  return attr;
}

void s2_pte_attr_to_mmu_flags(pte_t pte, uint* mmu_flags) {
  switch (pte & MMU_S2_PTE_ATTR_ATTR_INDEX_MASK) {
    case MMU_S2_PTE_ATTR_STRONGLY_ORDERED:
      *mmu_flags |= ARCH_MMU_FLAG_UNCACHED;
      break;
    case MMU_S2_PTE_ATTR_DEVICE:
      *mmu_flags |= ARCH_MMU_FLAG_UNCACHED_DEVICE;
      break;
    case MMU_S2_PTE_ATTR_NORMAL_UNCACHED:
      *mmu_flags |= ARCH_MMU_FLAG_WRITE_COMBINING;
      break;
    case MMU_S2_PTE_ATTR_NORMAL_MEMORY:
      *mmu_flags |= ARCH_MMU_FLAG_CACHED;
      break;
    default:
      PANIC_UNIMPLEMENTED;
  }

  *mmu_flags |= ARCH_MMU_FLAG_PERM_READ;
  switch (pte & MMU_PTE_ATTR_AP_MASK) {
    case MMU_S2_PTE_ATTR_S2AP_RO:
      break;
    case MMU_S2_PTE_ATTR_S2AP_RW:
      *mmu_flags |= ARCH_MMU_FLAG_PERM_WRITE;
      break;
    default:
      PANIC_UNIMPLEMENTED;
  }

  if (pte & MMU_S2_PTE_ATTR_XN) {
    *mmu_flags |= ARCH_MMU_FLAG_PERM_EXECUTE;
  }
}

bool is_pte_valid(pte_t pte) {
  return (pte & MMU_PTE_DESCRIPTOR_MASK) != MMU_PTE_DESCRIPTOR_INVALID;
}

void update_pte(volatile pte_t* pte, pte_t newval) { *pte = newval; }

bool page_table_is_clear(const volatile pte_t* page_table, uint page_size_shift) {
  const int count = 1U << (page_size_shift - 3);

  for (int i = 0; i < count; i++) {
    pte_t pte = page_table[i];
    if (pte != MMU_PTE_DESCRIPTOR_INVALID) {
      LTRACEF("page_table at %p still in use, index %d is %#" PRIx64 "\n", page_table, i, pte);
      return false;
    }
  }

  LTRACEF("page table at %p is clear\n", page_table);
  return true;
}

}  // namespace

uint ArmArchVmAspace::MmuFlagsFromPte(pte_t pte) {
  uint mmu_flags = 0;
  if (flags_ & ARCH_ASPACE_FLAG_GUEST) {
    s2_pte_attr_to_mmu_flags(pte, &mmu_flags);
  } else {
    s1_pte_attr_to_mmu_flags(pte, &mmu_flags);
  }
  return mmu_flags;
}

zx_status_t ArmArchVmAspace::Query(vaddr_t vaddr, paddr_t* paddr, uint* mmu_flags) {
  Guard<Mutex> al{&lock_};
  return QueryLocked(vaddr, paddr, mmu_flags);
}

zx_status_t ArmArchVmAspace::QueryLocked(vaddr_t vaddr, paddr_t* paddr, uint* mmu_flags) {
  uint index_shift;
  uint page_size_shift;
  vaddr_t vaddr_rem;

  canary_.Assert();
  LTRACEF("aspace %p, vaddr 0x%lx\n", this, vaddr);

  DEBUG_ASSERT(tt_virt_);

  DEBUG_ASSERT(IsValidVaddr(vaddr));
  if (!IsValidVaddr(vaddr)) {
    return ZX_ERR_OUT_OF_RANGE;
  }

  // Compute shift values based on if this address space is for kernel or user space.
  if (flags_ & ARCH_ASPACE_FLAG_KERNEL) {
    index_shift = MMU_KERNEL_TOP_SHIFT;
    page_size_shift = MMU_KERNEL_PAGE_SIZE_SHIFT;

    vaddr_t kernel_base = ~0UL << MMU_KERNEL_SIZE_SHIFT;
    vaddr_rem = vaddr - kernel_base;

    ulong __UNUSED index = vaddr_rem >> index_shift;
    ASSERT(index < MMU_KERNEL_PAGE_TABLE_ENTRIES_TOP);
  } else if (flags_ & ARCH_ASPACE_FLAG_GUEST) {
    index_shift = MMU_GUEST_TOP_SHIFT;
    page_size_shift = MMU_GUEST_PAGE_SIZE_SHIFT;

    vaddr_rem = vaddr;
    ulong __UNUSED index = vaddr_rem >> index_shift;
    ASSERT(index < MMU_GUEST_PAGE_TABLE_ENTRIES_TOP);
  } else {
    index_shift = MMU_USER_TOP_SHIFT;
    page_size_shift = MMU_USER_PAGE_SIZE_SHIFT;

    vaddr_rem = vaddr;
    ulong __UNUSED index = vaddr_rem >> index_shift;
    ASSERT(index < MMU_USER_PAGE_TABLE_ENTRIES_TOP);
  }

  const volatile pte_t* page_table = tt_virt_;

  while (true) {
    const ulong index = vaddr_rem >> index_shift;
    vaddr_rem -= (vaddr_t)index << index_shift;
    const pte_t pte = page_table[index];
    const uint descriptor_type = pte & MMU_PTE_DESCRIPTOR_MASK;
    const paddr_t pte_addr = pte & MMU_PTE_OUTPUT_ADDR_MASK;

    LTRACEF("va %#" PRIxPTR ", index %lu, index_shift %u, rem %#" PRIxPTR ", pte %#" PRIx64 "\n",
            vaddr, index, index_shift, vaddr_rem, pte);

    if (descriptor_type == MMU_PTE_DESCRIPTOR_INVALID) {
      return ZX_ERR_NOT_FOUND;
    }

    if (descriptor_type == ((index_shift > page_size_shift) ? MMU_PTE_L012_DESCRIPTOR_BLOCK
                                                            : MMU_PTE_L3_DESCRIPTOR_PAGE)) {
      if (paddr) {
        *paddr = pte_addr + vaddr_rem;
      }
      if (mmu_flags) {
        *mmu_flags = MmuFlagsFromPte(pte);
      }
      LTRACEF("va 0x%lx, paddr 0x%lx, flags 0x%x\n", vaddr, paddr ? *paddr : ~0UL,
              mmu_flags ? *mmu_flags : ~0U);
      return ZX_OK;
    }

    if (index_shift <= page_size_shift || descriptor_type != MMU_PTE_L012_DESCRIPTOR_TABLE) {
      PANIC_UNIMPLEMENTED;
    }

    page_table = static_cast<const volatile pte_t*>(paddr_to_physmap(pte_addr));
    index_shift -= page_size_shift - 3;
  }
}

zx_status_t ArmArchVmAspace::AllocPageTable(paddr_t* paddrp, uint page_size_shift) {
  LTRACEF("page_size_shift %u\n", page_size_shift);

  // currently we only support allocating a single page
  DEBUG_ASSERT(page_size_shift == PAGE_SIZE_SHIFT);

  // Allocate a page from the pmm via function pointer passed to us in Init().
  // The default is pmm_alloc_page so test and explicitly call it to avoid any unnecessary
  // virtual functions.
  vm_page_t* page;
  zx_status_t status;
  if (likely(!test_page_alloc_func_)) {
    status = pmm_alloc_page(0, &page, paddrp);
  } else {
    status = test_page_alloc_func_(0, &page, paddrp);
  }
  if (status != ZX_OK) {
    return status;
  }

  page->set_state(VM_PAGE_STATE_MMU);
  pt_pages_++;

  LOCAL_KTRACE("page table alloc");

  LTRACEF("allocated 0x%lx\n", *paddrp);
  return ZX_OK;
}

void ArmArchVmAspace::FreePageTable(void* vaddr, paddr_t paddr, uint page_size_shift) {
  LTRACEF("vaddr %p paddr 0x%lx page_size_shift %u\n", vaddr, paddr, page_size_shift);

  // currently we only support freeing a single page
  DEBUG_ASSERT(page_size_shift == PAGE_SIZE_SHIFT);

  LOCAL_KTRACE("page table free");

  vm_page_t* page = paddr_to_vm_page(paddr);
  if (!page) {
    panic("bad page table paddr 0x%lx\n", paddr);
  }
  pmm_free_page(page);

  pt_pages_--;
}

zx_status_t ArmArchVmAspace::SplitLargePage(vaddr_t vaddr, uint index_shift, uint page_size_shift,
                                            vaddr_t pt_index, volatile pte_t* page_table) {
  DEBUG_ASSERT(index_shift > page_size_shift);

  const pte_t pte = page_table[pt_index];
  DEBUG_ASSERT((pte & MMU_PTE_DESCRIPTOR_MASK) == MMU_PTE_L012_DESCRIPTOR_BLOCK);

  paddr_t paddr;
  zx_status_t ret = AllocPageTable(&paddr, page_size_shift);
  if (ret) {
    TRACEF("failed to allocate page table\n");
    return ret;
  }

  const uint next_shift = (index_shift - (page_size_shift - 3));

  const auto new_page_table = static_cast<volatile pte_t*>(paddr_to_physmap(paddr));
  const auto new_desc_type =
      (next_shift == page_size_shift) ? MMU_PTE_L3_DESCRIPTOR_PAGE : MMU_PTE_L012_DESCRIPTOR_BLOCK;
  const auto attrs = (pte & ~(MMU_PTE_OUTPUT_ADDR_MASK | MMU_PTE_DESCRIPTOR_MASK)) | new_desc_type;

  const uint next_size = 1U << next_shift;
  for (uint64_t i = 0, mapped_paddr = pte & MMU_PTE_OUTPUT_ADDR_MASK;
       i < MMU_KERNEL_PAGE_TABLE_ENTRIES; i++, mapped_paddr += next_size) {
    // directly write to the pte, no need to update since this is
    // a completely new table
    new_page_table[i] = mapped_paddr | attrs;
  }

  // Ensure all zeroing becomes visible prior to page table installation.
  __dmb(ARM_MB_ISHST);

  update_pte(&page_table[pt_index], paddr | MMU_PTE_L012_DESCRIPTOR_TABLE);
  LTRACEF("pte %p[%#" PRIxPTR "] = %#" PRIx64 "\n", page_table, pt_index, page_table[pt_index]);

  // no need to update the page table count here since we're replacing a block entry with a table
  // entry.

  // ensure that the update is observable from hardware page table walkers before TLB
  // operations can occur.
  __dsb(ARM_MB_ISHST);

  FlushTLBEntry(vaddr, false);

  return ZX_OK;
}

// use the appropriate TLB flush instruction to globally flush the modified entry
// terminal is set when flushing at the final level of the page table.
void ArmArchVmAspace::FlushTLBEntry(vaddr_t vaddr, bool terminal) {
  if (flags_ & ARCH_ASPACE_FLAG_GUEST) {
    paddr_t vttbr = arm64_vttbr(asid_, tt_phys_);
    __UNUSED zx_status_t status = arm64_el2_tlbi_ipa(vttbr, vaddr, terminal);
    DEBUG_ASSERT(status == ZX_OK);
  } else if (asid_ == MMU_ARM64_GLOBAL_ASID) {
    // flush this address on all ASIDs
    if (terminal) {
      ARM64_TLBI(vaale1is, vaddr >> 12);
    } else {
      ARM64_TLBI(vaae1is, vaddr >> 12);
    }
  } else {
    // flush this address for the specific asid
    if (terminal) {
      ARM64_TLBI(vale1is, vaddr >> 12 | (vaddr_t)asid_ << 48);
    } else {
      ARM64_TLBI(vae1is, vaddr >> 12 | (vaddr_t)asid_ << 48);
    }
  }
}

void ArmArchVmAspace::FlushAsid() {
  if (unlikely(flags_ & ARCH_ASPACE_FLAG_GUEST)) {
    paddr_t vttbr = arm64_vttbr(asid_, tt_phys_);
    __UNUSED zx_status_t status = arm64_el2_tlbi_vmid(vttbr);
    DEBUG_ASSERT(status == ZX_OK);
  } else if (unlikely(asid_ == MMU_ARM64_GLOBAL_ASID)) {
    ARM64_TLBI_NOADDR(alle1is);
  } else {
    // flush this address for the specific asid
    ARM64_TLBI_ASID(aside1is, asid_);
  }
}

// NOTE: caller must DSB afterwards to ensure TLB entries are flushed
ssize_t ArmArchVmAspace::UnmapPageTable(vaddr_t vaddr, vaddr_t vaddr_rel, size_t size,
                                        uint index_shift, uint page_size_shift,
                                        volatile pte_t* page_table) {
  const vaddr_t block_size = 1UL << index_shift;
  const vaddr_t block_mask = block_size - 1;

  LTRACEF(
      "vaddr 0x%lx, vaddr_rel 0x%lx, size 0x%lx, index shift %u, page_size_shift %u, page_table "
      "%p\n",
      vaddr, vaddr_rel, size, index_shift, page_size_shift, page_table);

  size_t unmap_size = 0;
  while (size) {
    const vaddr_t vaddr_rem = vaddr_rel & block_mask;
    const size_t chunk_size = ktl::min(size, block_size - vaddr_rem);
    const vaddr_t index = vaddr_rel >> index_shift;

    const pte_t pte = page_table[index];

    if (index_shift > page_size_shift &&
        (pte & MMU_PTE_DESCRIPTOR_MASK) == MMU_PTE_L012_DESCRIPTOR_TABLE) {
      const paddr_t page_table_paddr = pte & MMU_PTE_OUTPUT_ADDR_MASK;
      volatile pte_t* next_page_table =
          static_cast<volatile pte_t*>(paddr_to_physmap(page_table_paddr));

      // Recurse a level.
      UnmapPageTable(vaddr, vaddr_rem, chunk_size, index_shift - (page_size_shift - 3),
                     page_size_shift, next_page_table);

      // if we unmapped an entire page table leaf and/or the unmap made the level below us empty,
      // free the page table
      if (chunk_size == block_size || page_table_is_clear(next_page_table, page_size_shift)) {
        LTRACEF("pte %p[0x%lx] = 0 (was page table)\n", page_table, index);
        update_pte(&page_table[index], MMU_PTE_DESCRIPTOR_INVALID);

        // ensure that the update is observable from hardware page table walkers before TLB
        // operations can occur.
        __dsb(ARM_MB_ISHST);

        // flush the non terminal TLB entry
        FlushTLBEntry(vaddr, false);

        FreePageTable(const_cast<pte_t*>(next_page_table), page_table_paddr, page_size_shift);
      }
    } else if (is_pte_valid(pte)) {
      LTRACEF("pte %p[0x%lx] = 0\n", page_table, index);
      update_pte(&page_table[index], MMU_PTE_DESCRIPTOR_INVALID);

      // ensure that the update is observable from hardware page table walkers before TLB
      // operations can occur.
      __dsb(ARM_MB_ISHST);

      // flush the terminal TLB entry
      FlushTLBEntry(vaddr, true);
    } else {
      LTRACEF("pte %p[0x%lx] already clear\n", page_table, index);
    }
    vaddr += chunk_size;
    vaddr_rel += chunk_size;
    size -= chunk_size;
    unmap_size += chunk_size;
  }

  return unmap_size;
}

// NOTE: caller must DSB afterwards to ensure TLB entries are flushed
ssize_t ArmArchVmAspace::MapPageTable(vaddr_t vaddr_in, vaddr_t vaddr_rel_in, paddr_t paddr_in,
                                      size_t size_in, pte_t attrs, uint index_shift,
                                      uint page_size_shift, volatile pte_t* page_table) {
  vaddr_t vaddr = vaddr_in;
  vaddr_t vaddr_rel = vaddr_rel_in;
  paddr_t paddr = paddr_in;
  size_t size = size_in;

  const vaddr_t block_size = 1UL << index_shift;
  const vaddr_t block_mask = block_size - 1;
  LTRACEF("vaddr %#" PRIxPTR ", vaddr_rel %#" PRIxPTR ", paddr %#" PRIxPTR
          ", size %#zx, attrs %#" PRIx64 ", index shift %u, page_size_shift %u, page_table %p\n",
          vaddr, vaddr_rel, paddr, size, attrs, index_shift, page_size_shift, page_table);

  if ((vaddr_rel | paddr | size) & ((1UL << page_size_shift) - 1)) {
    TRACEF("not page aligned\n");
    return ZX_ERR_INVALID_ARGS;
  }

  auto cleanup = fbl::AutoCall([&]() {
    AssertHeld(lock_);
    UnmapPageTable(vaddr_in, vaddr_rel_in, size_in - size, index_shift, page_size_shift,
                   page_table);
  });

  size_t mapped_size = 0;
  while (size) {
    const vaddr_t vaddr_rem = vaddr_rel & block_mask;
    const size_t chunk_size = ktl::min(size, block_size - vaddr_rem);
    const vaddr_t index = vaddr_rel >> index_shift;
    pte_t pte = page_table[index];

    // if we're at an unaligned address, not trying to map a block, and not at the terminal level,
    // recurse one more level of the page table tree
    if (((vaddr_rel | paddr) & block_mask) || (chunk_size != block_size) ||
        (index_shift > MMU_PTE_DESCRIPTOR_BLOCK_MAX_SHIFT)) {
      // Lookup the next level page table, allocating if required.
      bool allocated_page_table = false;
      paddr_t page_table_paddr = 0;
      volatile pte_t* next_page_table = nullptr;

      DEBUG_ASSERT(page_size_shift <= MMU_MAX_PAGE_SIZE_SHIFT);

      switch (pte & MMU_PTE_DESCRIPTOR_MASK) {
        case MMU_PTE_DESCRIPTOR_INVALID: {
          zx_status_t ret = AllocPageTable(&page_table_paddr, page_size_shift);
          if (ret) {
            TRACEF("failed to allocate page table\n");
            return ret;
          }
          allocated_page_table = true;
          void* pt_vaddr = paddr_to_physmap(page_table_paddr);

          LTRACEF("allocated page table, vaddr %p, paddr 0x%lx\n", pt_vaddr, page_table_paddr);
          arch_zero_page(pt_vaddr);

          // ensure that the zeroing is observable from hardware page table walkers
          __dmb(ARM_MB_ISHST);

          pte = page_table_paddr | MMU_PTE_L012_DESCRIPTOR_TABLE;
          update_pte(&page_table[index], pte);
          LTRACEF("pte %p[%#" PRIxPTR "] = %#" PRIx64 "\n", page_table, index, pte);
          next_page_table = static_cast<volatile pte_t*>(pt_vaddr);
          break;
        }
        case MMU_PTE_L012_DESCRIPTOR_TABLE:
          page_table_paddr = pte & MMU_PTE_OUTPUT_ADDR_MASK;
          LTRACEF("found page table %#" PRIxPTR "\n", page_table_paddr);
          next_page_table = static_cast<volatile pte_t*>(paddr_to_physmap(page_table_paddr));
          break;
        case MMU_PTE_L012_DESCRIPTOR_BLOCK:
          return ZX_ERR_ALREADY_EXISTS;

        default:
          PANIC_UNIMPLEMENTED;
      }
      DEBUG_ASSERT(next_page_table);

      ssize_t ret =
          MapPageTable(vaddr, vaddr_rem, paddr, chunk_size, attrs,
                       index_shift - (page_size_shift - 3), page_size_shift, next_page_table);
      if (ret < 0) {
        if (allocated_page_table) {
          // We just allocated this page table. The unmap in err will not clean it up as the size
          // we pass in will not cause us to look at this page table. This is reasonable as if we
          // didn't allocate the page table then we shouldn't look into and potentially unmap
          // anything from that page table.
          // Since we just allocated it there should be nothing in it, otherwise the MapPageTable
          // call would not have failed.
          DEBUG_ASSERT(page_table_is_clear(next_page_table, page_size_shift));
          update_pte(&page_table[index], MMU_PTE_DESCRIPTOR_INVALID);

          // ensure that the update is observable from hardware page table walkers before TLB
          // operations can occur.
          __dsb(ARM_MB_ISHST);

          // flush the non terminal TLB entry
          FlushTLBEntry(vaddr, false);

          FreePageTable(const_cast<pte_t*>(next_page_table), page_table_paddr, page_size_shift);
        }
        return ret;
      }
      DEBUG_ASSERT(static_cast<size_t>(ret) == chunk_size);
    } else {
      if (is_pte_valid(pte)) {
        LTRACEF("page table entry already in use, index %#" PRIxPTR ", %#" PRIx64 "\n", index, pte);
        return ZX_ERR_ALREADY_EXISTS;
      }

      pte = paddr | attrs;
      if (index_shift > page_size_shift) {
        pte |= MMU_PTE_L012_DESCRIPTOR_BLOCK;
      } else {
        pte |= MMU_PTE_L3_DESCRIPTOR_PAGE;
      }
      if (!(flags_ & ARCH_ASPACE_FLAG_GUEST)) {
        pte |= MMU_PTE_ATTR_NON_GLOBAL;
      }
      LTRACEF("pte %p[%#" PRIxPTR "] = %#" PRIx64 "\n", page_table, index, pte);
      update_pte(&page_table[index], pte);
    }
    vaddr += chunk_size;
    vaddr_rel += chunk_size;
    paddr += chunk_size;
    size -= chunk_size;
    mapped_size += chunk_size;
  }

  cleanup.cancel();
  return mapped_size;
}

// NOTE: caller must DSB afterwards to ensure TLB entries are flushed
zx_status_t ArmArchVmAspace::ProtectPageTable(vaddr_t vaddr_in, vaddr_t vaddr_rel_in,
                                              size_t size_in, pte_t attrs, uint index_shift,
                                              uint page_size_shift, volatile pte_t* page_table) {
  vaddr_t vaddr = vaddr_in;
  vaddr_t vaddr_rel = vaddr_rel_in;
  size_t size = size_in;

  const vaddr_t block_size = 1UL << index_shift;
  const vaddr_t block_mask = block_size - 1;

  LTRACEF("vaddr %#" PRIxPTR ", vaddr_rel %#" PRIxPTR ", size %#" PRIxPTR ", attrs %#" PRIx64
          ", index shift %u, page_size_shift %u, page_table %p\n",
          vaddr, vaddr_rel, size, attrs, index_shift, page_size_shift, page_table);

  // vaddr_rel and size must be page aligned
  DEBUG_ASSERT(((vaddr_rel | size) & ((1UL << page_size_shift) - 1)) == 0);

  while (size) {
    const vaddr_t vaddr_rem = vaddr_rel & block_mask;
    const size_t chunk_size = ktl::min(size, block_size - vaddr_rem);
    const vaddr_t index = vaddr_rel >> index_shift;

    pte_t pte = page_table[index];

    if (index_shift > page_size_shift &&
        (pte & MMU_PTE_DESCRIPTOR_MASK) == MMU_PTE_L012_DESCRIPTOR_BLOCK &&
        chunk_size != block_size) {
      zx_status_t s = SplitLargePage(vaddr, index_shift, page_size_shift, index, page_table);
      if (likely(s == ZX_OK)) {
        pte = page_table[index];
      } else {
        // If split fails, just unmap the whole block and let a
        // subsequent page fault clean it up.
        UnmapPageTable(vaddr - vaddr_rel, 0, block_size, index_shift, page_size_shift, page_table);
        pte = 0;
      }
    }

    if (index_shift > page_size_shift &&
        (pte & MMU_PTE_DESCRIPTOR_MASK) == MMU_PTE_L012_DESCRIPTOR_TABLE) {
      const paddr_t page_table_paddr = pte & MMU_PTE_OUTPUT_ADDR_MASK;
      volatile pte_t* next_page_table =
          static_cast<volatile pte_t*>(paddr_to_physmap(page_table_paddr));

      // Recurse a level.
      ProtectPageTable(vaddr, vaddr_rem, chunk_size, attrs, index_shift - (page_size_shift - 3),
                       page_size_shift, next_page_table);
    } else if (is_pte_valid(pte)) {
      pte = (pte & ~MMU_PTE_PERMISSION_MASK) | attrs;
      LTRACEF("pte %p[%#" PRIxPTR "] = %#" PRIx64 "\n", page_table, index, pte);
      update_pte(&page_table[index], pte);

      // ensure that the update is observable from hardware page table walkers before TLB
      // operations can occur.
      __dsb(ARM_MB_ISHST);

      // flush the terminal TLB entry
      FlushTLBEntry(vaddr, true);
    } else {
      LTRACEF("page table entry does not exist, index %#" PRIxPTR ", %#" PRIx64 "\n", index, pte);
    }
    vaddr += chunk_size;
    vaddr_rel += chunk_size;
    size -= chunk_size;
  }

  return ZX_OK;
}

// NOTE: if this returns true, caller must DSB afterwards to ensure TLB entries are flushed
bool ArmArchVmAspace::HarvestAccessedPageTable(vaddr_t vaddr, vaddr_t vaddr_rel_in, size_t size,
                                               const uint index_shift, const uint page_size_shift,
                                               volatile pte_t* page_table,
                                               const HarvestCallback& accessed_callback) {
  const vaddr_t block_size = 1UL << index_shift;
  const vaddr_t block_mask = block_size - 1;

  vaddr_t vaddr_rel = vaddr_rel_in;

  // vaddr_rel and size must be page aligned
  DEBUG_ASSERT(((vaddr_rel | size) & ((1UL << page_size_shift) - 1)) == 0);

  bool flushed_tlb = false;

  while (size) {
    const vaddr_t vaddr_rem = vaddr_rel & block_mask;
    const size_t chunk_size = ktl::min(size, block_size - vaddr_rem);
    const vaddr_t index = vaddr_rel >> index_shift;

    pte_t pte = page_table[index];

    if (index_shift > page_size_shift &&
        (pte & MMU_PTE_DESCRIPTOR_MASK) == MMU_PTE_L012_DESCRIPTOR_BLOCK &&
        chunk_size != block_size) {
      // Ignore large pages, we do not support harvesting accessed bits from them. Having this empty
      // if block simplifies the overall logic.
    } else if (index_shift > page_size_shift &&
               (pte & MMU_PTE_DESCRIPTOR_MASK) == MMU_PTE_L012_DESCRIPTOR_TABLE) {
      const paddr_t page_table_paddr = pte & MMU_PTE_OUTPUT_ADDR_MASK;
      volatile pte_t* next_page_table =
          static_cast<volatile pte_t*>(paddr_to_physmap(page_table_paddr));
      if (HarvestAccessedPageTable(vaddr, vaddr_rem, chunk_size,
                                   index_shift - (page_size_shift - 3), page_size_shift,
                                   next_page_table, accessed_callback)) {
        flushed_tlb = true;
      }
    } else if (is_pte_valid(pte)) {
      if (pte & MMU_PTE_ATTR_AF) {
        const paddr_t pte_addr = pte & MMU_PTE_OUTPUT_ADDR_MASK;
        const paddr_t paddr = pte_addr + vaddr_rem;
        const uint mmu_flags = MmuFlagsFromPte(pte);

        // Invoke the callback to see if the accessed flag should be removed.
        if (accessed_callback(paddr, vaddr, mmu_flags)) {
          // Modifying the access flag does not require break-before-make for correctness and as we
          // do not support hardware access flag setting at the moment we do not have to deal with
          // potential concurrent modifications.
          pte = (pte & ~MMU_PTE_ATTR_AF);
          LTRACEF("pte %p[%#" PRIxPTR "] = %#" PRIx64 "\n", page_table, index, pte);
          update_pte(&page_table[index], pte);

          // ensure that the update is observable from hardware page table walkers before TLB
          // operations can occur.
          __dsb(ARM_MB_ISHST);

          // flush the terminal TLB entry
          FlushTLBEntry(vaddr, true);

          // propagate back up to the caller that a tlb flush happened so they can synchronize.
          flushed_tlb = true;
        }
      }
    }
    vaddr += chunk_size;
    vaddr_rel += chunk_size;
    size -= chunk_size;
  }
  return flushed_tlb;
}

void ArmArchVmAspace::MarkAccessedPageTable(vaddr_t vaddr, vaddr_t vaddr_rel_in, size_t size,
                                            uint index_shift, uint page_size_shift,
                                            volatile pte_t* page_table) {
  const vaddr_t block_size = 1UL << index_shift;
  const vaddr_t block_mask = block_size - 1;

  vaddr_t vaddr_rel = vaddr_rel_in;

  // vaddr_rel and size must be page aligned
  DEBUG_ASSERT(((vaddr_rel | size) & ((1UL << page_size_shift) - 1)) == 0);

  while (size) {
    const vaddr_t vaddr_rem = vaddr_rel & block_mask;
    const size_t chunk_size = ktl::min(size, block_size - vaddr_rem);
    const vaddr_t index = vaddr_rel >> index_shift;

    pte_t pte = page_table[index];

    if (index_shift > page_size_shift &&
        (pte & MMU_PTE_DESCRIPTOR_MASK) == MMU_PTE_L012_DESCRIPTOR_BLOCK &&
        chunk_size != block_size) {
      // Ignore large pages as we don't support modifying their access flags. Having this empty if
      // block simplifies the overall logic.
    } else if (index_shift > page_size_shift &&
               (pte & MMU_PTE_DESCRIPTOR_MASK) == MMU_PTE_L012_DESCRIPTOR_TABLE) {
      const paddr_t page_table_paddr = pte & MMU_PTE_OUTPUT_ADDR_MASK;
      volatile pte_t* next_page_table =
          static_cast<volatile pte_t*>(paddr_to_physmap(page_table_paddr));
      MarkAccessedPageTable(vaddr, vaddr_rem, chunk_size, index_shift - (page_size_shift - 3),
                            page_size_shift, next_page_table);
    } else if (is_pte_valid(pte)) {
      pte |= MMU_PTE_ATTR_AF;
      update_pte(&page_table[index], pte);

      // If the access bit wasn't set then we know this entry isn't cached in any TLBs and so we do
      // not need to do any TLB maintenance and can just issue a dmb to ensure the hardware walker
      // sees the new entry. If the access bit was already set then this operation is a no-op and
      // we can leave any TLB entries alone.
      __dmb(ARM_MB_ISHST);
    }
    vaddr += chunk_size;
    vaddr_rel += chunk_size;
    size -= chunk_size;
  }
}

// internal routine to map a run of pages. dsb must be called after MapPages is called.
ssize_t ArmArchVmAspace::MapPages(vaddr_t vaddr, paddr_t paddr, size_t size, pte_t attrs,
                                  vaddr_t vaddr_base, uint top_size_shift, uint top_index_shift,
                                  uint page_size_shift) {
  vaddr_t vaddr_rel = vaddr - vaddr_base;
  vaddr_t vaddr_rel_max = 1UL << top_size_shift;

  LTRACEF("vaddr %#" PRIxPTR ", paddr %#" PRIxPTR ", size %#" PRIxPTR ", attrs %#" PRIx64
          ", asid %#x\n",
          vaddr, paddr, size, attrs, asid_);

  if (vaddr_rel > vaddr_rel_max - size || size > vaddr_rel_max) {
    TRACEF("vaddr %#" PRIxPTR ", size %#" PRIxPTR " out of range vaddr %#" PRIxPTR
           ", size %#" PRIxPTR "\n",
           vaddr, size, vaddr_base, vaddr_rel_max);
    return ZX_ERR_INVALID_ARGS;
  }

  LOCAL_KTRACE("mmu map", (vaddr & ~PAGE_MASK) | ((size >> PAGE_SIZE_SHIFT) & PAGE_MASK));
  ssize_t ret = MapPageTable(vaddr, vaddr_rel, paddr, size, attrs, top_index_shift, page_size_shift,
                             tt_virt_);
  return ret;
}

ssize_t ArmArchVmAspace::UnmapPages(vaddr_t vaddr, size_t size, vaddr_t vaddr_base,
                                    uint top_size_shift, uint top_index_shift,
                                    uint page_size_shift) {
  vaddr_t vaddr_rel = vaddr - vaddr_base;
  vaddr_t vaddr_rel_max = 1UL << top_size_shift;

  LTRACEF("vaddr 0x%lx, size 0x%lx, asid 0x%x\n", vaddr, size, asid_);

  if (vaddr_rel > vaddr_rel_max - size || size > vaddr_rel_max) {
    TRACEF("vaddr 0x%lx, size 0x%lx out of range vaddr 0x%lx, size 0x%lx\n", vaddr, size,
           vaddr_base, vaddr_rel_max);
    return ZX_ERR_INVALID_ARGS;
  }

  LOCAL_KTRACE("mmu unmap", (vaddr & ~PAGE_MASK) | ((size >> PAGE_SIZE_SHIFT) & PAGE_MASK));

  ssize_t ret = UnmapPageTable(vaddr, vaddr_rel, size, top_index_shift, page_size_shift, tt_virt_);
  __dsb(ARM_MB_SY);
  return ret;
}

zx_status_t ArmArchVmAspace::ProtectPages(vaddr_t vaddr, size_t size, pte_t attrs,
                                          vaddr_t vaddr_base, uint top_size_shift,
                                          uint top_index_shift, uint page_size_shift) {
  vaddr_t vaddr_rel = vaddr - vaddr_base;
  vaddr_t vaddr_rel_max = 1UL << top_size_shift;

  LTRACEF("vaddr %#" PRIxPTR ", size %#" PRIxPTR ", attrs %#" PRIx64 ", asid %#x\n", vaddr, size,
          attrs, asid_);

  if (vaddr_rel > vaddr_rel_max - size || size > vaddr_rel_max) {
    TRACEF("vaddr %#" PRIxPTR ", size %#" PRIxPTR " out of range vaddr %#" PRIxPTR
           ", size %#" PRIxPTR "\n",
           vaddr, size, vaddr_base, vaddr_rel_max);
    return ZX_ERR_INVALID_ARGS;
  }

  LOCAL_KTRACE("mmu protect", (vaddr & ~PAGE_MASK) | ((size >> PAGE_SIZE_SHIFT) & PAGE_MASK));

  zx_status_t ret =
      ProtectPageTable(vaddr, vaddr_rel, size, attrs, top_index_shift, page_size_shift, tt_virt_);
  __dsb(ARM_MB_SY);
  return ret;
}

void ArmArchVmAspace::MmuParamsFromFlags(uint mmu_flags, pte_t* attrs, vaddr_t* vaddr_base,
                                         uint* top_size_shift, uint* top_index_shift,
                                         uint* page_size_shift) {
  if (flags_ & ARCH_ASPACE_FLAG_KERNEL) {
    if (attrs) {
      *attrs = mmu_flags_to_s1_pte_attr(mmu_flags);
    }
    *vaddr_base = ~0UL << MMU_KERNEL_SIZE_SHIFT;
    *top_size_shift = MMU_KERNEL_SIZE_SHIFT;
    *top_index_shift = MMU_KERNEL_TOP_SHIFT;
    *page_size_shift = MMU_KERNEL_PAGE_SIZE_SHIFT;
  } else if (flags_ & ARCH_ASPACE_FLAG_GUEST) {
    if (attrs) {
      *attrs = mmu_flags_to_s2_pte_attr(mmu_flags);
    }
    *vaddr_base = 0;
    *top_size_shift = MMU_GUEST_SIZE_SHIFT;
    *top_index_shift = MMU_GUEST_TOP_SHIFT;
    *page_size_shift = MMU_GUEST_PAGE_SIZE_SHIFT;
  } else {
    if (attrs) {
      *attrs = mmu_flags_to_s1_pte_attr(mmu_flags);
    }
    *vaddr_base = 0;
    *top_size_shift = MMU_USER_SIZE_SHIFT;
    *top_index_shift = MMU_USER_TOP_SHIFT;
    *page_size_shift = MMU_USER_PAGE_SIZE_SHIFT;
  }
}

zx_status_t ArmArchVmAspace::MapContiguous(vaddr_t vaddr, paddr_t paddr, size_t count,
                                           uint mmu_flags, size_t* mapped) {
  canary_.Assert();
  LTRACEF("vaddr %#" PRIxPTR " paddr %#" PRIxPTR " count %zu flags %#x\n", vaddr, paddr, count,
          mmu_flags);

  DEBUG_ASSERT(tt_virt_);

  DEBUG_ASSERT(IsValidVaddr(vaddr));
  if (!IsValidVaddr(vaddr)) {
    return ZX_ERR_OUT_OF_RANGE;
  }

  if (!(mmu_flags & ARCH_MMU_FLAG_PERM_READ)) {
    return ZX_ERR_INVALID_ARGS;
  }

  // paddr and vaddr must be aligned.
  DEBUG_ASSERT(IS_PAGE_ALIGNED(vaddr));
  DEBUG_ASSERT(IS_PAGE_ALIGNED(paddr));
  if (!IS_PAGE_ALIGNED(vaddr) || !IS_PAGE_ALIGNED(paddr)) {
    return ZX_ERR_INVALID_ARGS;
  }

  if (count == 0) {
    return ZX_OK;
  }

  ssize_t ret;
  {
    Guard<Mutex> a{&lock_};
    pte_t attrs;
    vaddr_t vaddr_base;
    uint top_size_shift, top_index_shift, page_size_shift;
    MmuParamsFromFlags(mmu_flags, &attrs, &vaddr_base, &top_size_shift, &top_index_shift,
                       &page_size_shift);
    ret = MapPages(vaddr, paddr, count * PAGE_SIZE, attrs, vaddr_base, top_size_shift,
                   top_index_shift, page_size_shift);
    __dsb(ARM_MB_SY);
  }

  if (mapped) {
    *mapped = (ret > 0) ? (ret / PAGE_SIZE) : 0u;
    DEBUG_ASSERT(*mapped <= count);
  }

  return (ret < 0) ? (zx_status_t)ret : ZX_OK;
}

zx_status_t ArmArchVmAspace::Map(vaddr_t vaddr, paddr_t* phys, size_t count, uint mmu_flags,
                                 size_t* mapped) {
  canary_.Assert();
  LTRACEF("vaddr %#" PRIxPTR " count %zu flags %#x\n", vaddr, count, mmu_flags);

  DEBUG_ASSERT(tt_virt_);

  DEBUG_ASSERT(IsValidVaddr(vaddr));
  if (!IsValidVaddr(vaddr)) {
    return ZX_ERR_OUT_OF_RANGE;
  }
  for (size_t i = 0; i < count; ++i) {
    DEBUG_ASSERT(IS_PAGE_ALIGNED(phys[i]));
    if (!IS_PAGE_ALIGNED(phys[i])) {
      return ZX_ERR_INVALID_ARGS;
    }
  }

  if (!(mmu_flags & ARCH_MMU_FLAG_PERM_READ)) {
    return ZX_ERR_INVALID_ARGS;
  }

  // vaddr must be aligned.
  DEBUG_ASSERT(IS_PAGE_ALIGNED(vaddr));
  if (!IS_PAGE_ALIGNED(vaddr)) {
    return ZX_ERR_INVALID_ARGS;
  }

  if (count == 0) {
    return ZX_OK;
  }

  size_t total_mapped = 0;
  {
    Guard<Mutex> a{&lock_};
    pte_t attrs;
    vaddr_t vaddr_base;
    uint top_size_shift, top_index_shift, page_size_shift;
    MmuParamsFromFlags(mmu_flags, &attrs, &vaddr_base, &top_size_shift, &top_index_shift,
                       &page_size_shift);

    ssize_t ret;
    size_t idx = 0;
    auto undo = fbl::MakeAutoCall([&]() TA_NO_THREAD_SAFETY_ANALYSIS {
      if (idx > 0) {
        UnmapPages(vaddr, idx * PAGE_SIZE, vaddr_base, top_size_shift, top_index_shift,
                   page_size_shift);
      }
    });

    vaddr_t v = vaddr;
    for (; idx < count; ++idx) {
      paddr_t paddr = phys[idx];
      DEBUG_ASSERT(IS_PAGE_ALIGNED(paddr));
      ret = MapPages(v, paddr, PAGE_SIZE, attrs, vaddr_base, top_size_shift, top_index_shift,
                     page_size_shift);
      if (ret < 0) {
        return static_cast<zx_status_t>(ret);
      }

      v += PAGE_SIZE;
      total_mapped += ret / PAGE_SIZE;
    }
    __dsb(ARM_MB_SY);
    undo.cancel();
  }
  DEBUG_ASSERT(total_mapped <= count);

  if (mapped) {
    *mapped = total_mapped;
  }

  return ZX_OK;
}

zx_status_t ArmArchVmAspace::Unmap(vaddr_t vaddr, size_t count, size_t* unmapped) {
  canary_.Assert();
  LTRACEF("vaddr %#" PRIxPTR " count %zu\n", vaddr, count);

  DEBUG_ASSERT(tt_virt_);

  DEBUG_ASSERT(IsValidVaddr(vaddr));

  if (!IsValidVaddr(vaddr)) {
    return ZX_ERR_OUT_OF_RANGE;
  }

  DEBUG_ASSERT(IS_PAGE_ALIGNED(vaddr));
  if (!IS_PAGE_ALIGNED(vaddr)) {
    return ZX_ERR_INVALID_ARGS;
  }

  Guard<Mutex> a{&lock_};

  ssize_t ret;
  {
    vaddr_t vaddr_base;
    uint top_size_shift, top_index_shift, page_size_shift;
    MmuParamsFromFlags(0, nullptr, &vaddr_base, &top_size_shift, &top_index_shift,
                       &page_size_shift);

    ret = UnmapPages(vaddr, count * PAGE_SIZE, vaddr_base, top_size_shift, top_index_shift,
                     page_size_shift);
  }

  if (unmapped) {
    *unmapped = (ret > 0) ? (ret / PAGE_SIZE) : 0u;
    DEBUG_ASSERT(*unmapped <= count);
  }

  return (ret < 0) ? (zx_status_t)ret : 0;
}

zx_status_t ArmArchVmAspace::Protect(vaddr_t vaddr, size_t count, uint mmu_flags) {
  canary_.Assert();

  if (!IsValidVaddr(vaddr)) {
    return ZX_ERR_INVALID_ARGS;
  }

  if (!IS_PAGE_ALIGNED(vaddr)) {
    return ZX_ERR_INVALID_ARGS;
  }

  if (!(mmu_flags & ARCH_MMU_FLAG_PERM_READ)) {
    return ZX_ERR_INVALID_ARGS;
  }

  Guard<Mutex> a{&lock_};

  int ret;
  {
    pte_t attrs;
    vaddr_t vaddr_base;
    uint top_size_shift, top_index_shift, page_size_shift;
    MmuParamsFromFlags(mmu_flags, &attrs, &vaddr_base, &top_size_shift, &top_index_shift,
                       &page_size_shift);

    ret = ProtectPages(vaddr, count * PAGE_SIZE, attrs, vaddr_base, top_size_shift, top_index_shift,
                       page_size_shift);
  }

  return ret;
}

zx_status_t ArmArchVmAspace::HarvestAccessed(vaddr_t vaddr, size_t count,
                                             const HarvestCallback& accessed_callback) {
  canary_.Assert();

  if (!IS_PAGE_ALIGNED(vaddr) || !IsValidVaddr(vaddr)) {
    return ZX_ERR_INVALID_ARGS;
  }

  Guard<Mutex> a{&lock_};
  vaddr_t vaddr_base;
  uint top_size_shift, top_index_shift, page_size_shift;
  MmuParamsFromFlags(0, nullptr, &vaddr_base, &top_size_shift, &top_index_shift, &page_size_shift);

  const vaddr_t vaddr_rel = vaddr - vaddr_base;
  const vaddr_t vaddr_rel_max = 1UL << top_size_shift;
  const size_t size = count * PAGE_SIZE;

  if (vaddr_rel > vaddr_rel_max - size || size > vaddr_rel_max) {
    TRACEF("vaddr %#" PRIxPTR ", size %#" PRIxPTR " out of range vaddr %#" PRIxPTR
           ", size %#" PRIxPTR "\n",
           vaddr, size, vaddr_base, vaddr_rel_max);
    return ZX_ERR_INVALID_ARGS;
  }

  LOCAL_KTRACE("mmu harvest accessed",
               (vaddr & ~PAGE_MASK) | ((size >> PAGE_SIZE_SHIFT) & PAGE_MASK));

  // It's fairly reasonable for there to be nothing to harvest, and dsb is expensive, so
  // HarvestAccessedPageTable will return true if it performed a TLB invalidation that we need to
  // synchronize with.
  if (HarvestAccessedPageTable(vaddr, vaddr_rel, size, top_index_shift, page_size_shift, tt_virt_,
                               accessed_callback)) {
    __dsb(ARM_MB_SY);
  }

  return ZX_OK;
}

zx_status_t ArmArchVmAspace::MarkAccessed(vaddr_t vaddr, size_t count) {
  canary_.Assert();

  if (!IS_PAGE_ALIGNED(vaddr) || !IsValidVaddr(vaddr)) {
    return ZX_ERR_OUT_OF_RANGE;
  }

  Guard<Mutex> a{&lock_};
  vaddr_t vaddr_base;
  uint top_size_shift, top_index_shift, page_size_shift;
  MmuParamsFromFlags(0, nullptr, &vaddr_base, &top_size_shift, &top_index_shift, &page_size_shift);

  const vaddr_t vaddr_rel = vaddr - vaddr_base;
  const vaddr_t vaddr_rel_max = 1UL << top_size_shift;
  const size_t size = count * PAGE_SIZE;

  if (vaddr_rel > vaddr_rel_max - size || size > vaddr_rel_max) {
    TRACEF("vaddr %#" PRIxPTR ", size %#" PRIxPTR " out of range vaddr %#" PRIxPTR
           ", size %#" PRIxPTR "\n",
           vaddr, size, vaddr_base, vaddr_rel_max);
    return ZX_ERR_OUT_OF_RANGE;
  }

  LOCAL_KTRACE("mmu mark accessed", (vaddr & ~PAGE_MASK) | ((size >> PAGE_SIZE_SHIFT) & PAGE_MASK));

  MarkAccessedPageTable(vaddr, vaddr_rel, size, top_index_shift, page_size_shift, tt_virt_);
  // MarkAccessedPageTable does not perform any TLB operations, so unlike most other top level mmu
  // functions we do not need to perform a dsb to synchronize.
  return ZX_OK;
}

zx_status_t ArmArchVmAspace::Init() {
  canary_.Assert();
  LTRACEF("aspace %p, base %#" PRIxPTR ", size 0x%zx, flags 0x%x\n", this, base_, size_, flags_);

  Guard<Mutex> a{&lock_};

  // Validate that the base + size is sane and doesn't wrap.
  DEBUG_ASSERT(size_ > PAGE_SIZE);
  DEBUG_ASSERT(base_ + size_ - 1 > base_);

  if (flags_ & ARCH_ASPACE_FLAG_KERNEL) {
    // At the moment we can only deal with address spaces as globally defined.
    DEBUG_ASSERT(base_ == ~0UL << MMU_KERNEL_SIZE_SHIFT);
    DEBUG_ASSERT(size_ == 1UL << MMU_KERNEL_SIZE_SHIFT);

    tt_virt_ = arm64_kernel_translation_table;
    tt_phys_ = vaddr_to_paddr(const_cast<pte_t*>(tt_virt_));
    asid_ = (uint16_t)MMU_ARM64_GLOBAL_ASID;
  } else {
    uint page_size_shift;
    if (flags_ & ARCH_ASPACE_FLAG_GUEST) {
      DEBUG_ASSERT(base_ + size_ <= 1UL << MMU_GUEST_SIZE_SHIFT);
      page_size_shift = MMU_GUEST_PAGE_SIZE_SHIFT;
    } else {
      DEBUG_ASSERT(base_ + size_ <= 1UL << MMU_USER_SIZE_SHIFT);
      page_size_shift = MMU_USER_PAGE_SIZE_SHIFT;
      if (asid.Alloc(&asid_) != ZX_OK) {
        return ZX_ERR_NO_MEMORY;
      }
    }

    paddr_t pa;

    // allocate a top level page table to serve as the translation table
    zx_status_t status = AllocPageTable(&pa, page_size_shift);
    if (status != ZX_OK) {
      return status;
    }

    volatile pte_t* va = static_cast<volatile pte_t*>(paddr_to_physmap(pa));

    tt_virt_ = va;
    tt_phys_ = pa;

    // zero the top level translation table.
    arch_zero_page(const_cast<pte_t*>(tt_virt_));
  }
  pt_pages_ = 1;

  LTRACEF("tt_phys %#" PRIxPTR " tt_virt %p\n", tt_phys_, tt_virt_);

  return ZX_OK;
}

zx_status_t ArmArchVmAspace::Destroy() {
  canary_.Assert();
  LTRACEF("aspace %p\n", this);

  Guard<Mutex> a{&lock_};

  // Not okay to destroy the kernel address space
  DEBUG_ASSERT((flags_ & ARCH_ASPACE_FLAG_KERNEL) == 0);

  // Check to see if the top level page table is empty. If not the user didn't
  // properly unmap everything before destroying the aspace
  vaddr_t vaddr_base;
  uint top_size_shift, top_index_shift, page_size_shift;
  MmuParamsFromFlags(0, nullptr, &vaddr_base, &top_size_shift, &top_index_shift, &page_size_shift);
  if (page_table_is_clear(tt_virt_, page_size_shift) == false) {
    panic("top level page table still in use! aspace %p tt_virt %p\n", this, tt_virt_);
  }

  if (pt_pages_ != 1) {
    panic("allocated page table count is wrong, aspace %p count %zu (should be 1)\n", this,
          pt_pages_);
  }

  // Flush the ASID or VMID associated with this aspace
  FlushAsid();

  // Free any ASID.
  if (!(flags_ & ARCH_ASPACE_FLAG_GUEST)) {
    asid.Free(asid_);
    asid_ = MMU_ARM64_UNUSED_ASID;
  }

  // Free the top level page table.
  vm_page_t* page = paddr_to_vm_page(tt_phys_);
  DEBUG_ASSERT(page);
  pmm_free_page(page);
  pt_pages_--;

  tt_phys_ = 0;
  tt_virt_ = nullptr;

  return ZX_OK;
}

void ArmArchVmAspace::ContextSwitch(ArmArchVmAspace* old_aspace, ArmArchVmAspace* aspace) {
  if (TRACE_CONTEXT_SWITCH) {
    TRACEF("aspace %p\n", aspace);
  }

  uint64_t tcr;
  uint64_t ttbr;
  if (aspace) {
    aspace->canary_.Assert();
    DEBUG_ASSERT((aspace->flags_ & (ARCH_ASPACE_FLAG_KERNEL | ARCH_ASPACE_FLAG_GUEST)) == 0);

    tcr = MMU_TCR_FLAGS_USER;
    ttbr = ((uint64_t)aspace->asid_ << 48) | aspace->tt_phys_;
    __arm_wsr64("ttbr0_el1", ttbr);
    __isb(ARM_MB_SY);

    if (TRACE_CONTEXT_SWITCH) {
      TRACEF("ttbr %#" PRIx64 ", tcr %#" PRIx64 "\n", ttbr, tcr);
    }

  } else {
    tcr = MMU_TCR_FLAGS_KERNEL;

    if (TRACE_CONTEXT_SWITCH) {
      TRACEF("tcr %#" PRIx64 "\n", tcr);
    }
  }

  __arm_wsr64("tcr_el1", tcr);
  __isb(ARM_MB_SY);
}

void arch_zero_page(void* _ptr) {
  uintptr_t ptr = (uintptr_t)_ptr;

  uint32_t zva_size = arm64_zva_size;
  uintptr_t end_ptr = ptr + PAGE_SIZE;
  do {
    __asm volatile("dc zva, %0" ::"r"(ptr));
    ptr += zva_size;
  } while (ptr != end_ptr);
}

zx_status_t arm64_mmu_translate(vaddr_t va, paddr_t* pa, bool user, bool write) {
  // disable interrupts around this operation to make the at/par instruction combination atomic
  uint64_t par;
  {
    InterruptDisableGuard irqd;

    if (user) {
      if (write) {
        __asm__ volatile("at s1e0w, %0" ::"r"(va) : "memory");
      } else {
        __asm__ volatile("at s1e0r, %0" ::"r"(va) : "memory");
      }
    } else {
      if (write) {
        __asm__ volatile("at s1e1w, %0" ::"r"(va) : "memory");
      } else {
        __asm__ volatile("at s1e1r, %0" ::"r"(va) : "memory");
      }
    }

    par = __arm_rsr64("par_el1");
  }

  // if bit 0 is clear, the translation succeeded
  if (BIT(par, 0)) {
    return ZX_ERR_NO_MEMORY;
  }

  // physical address is stored in bits [51..12], naturally aligned
  *pa = BITS(par, 51, 12) | (va & (PAGE_SIZE - 1));

  return ZX_OK;
}

ArmArchVmAspace::ArmArchVmAspace(vaddr_t base, size_t size, uint mmu_flags, page_alloc_fn_t paf)
    : test_page_alloc_func_(paf), flags_(mmu_flags), base_(base), size_(size) {}

ArmArchVmAspace::~ArmArchVmAspace() {
  // Destroy() will have freed the final page table if it ran correctly, and further validated that
  // everything else was freed.
  DEBUG_ASSERT(pt_pages_ == 0);
}

vaddr_t ArmArchVmAspace::PickSpot(vaddr_t base, uint prev_region_mmu_flags, vaddr_t end,
                                  uint next_region_mmu_flags, vaddr_t align, size_t size,
                                  uint mmu_flags) {
  canary_.Assert();
  return PAGE_ALIGN(base);
}
