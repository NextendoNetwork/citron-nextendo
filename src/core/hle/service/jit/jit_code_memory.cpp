// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/jit/jit_code_memory.h"

namespace Service::JIT {

Result CodeMemory::Initialize(Kernel::KProcess& process, Kernel::KCodeMemory& code_memory,
                              size_t size, Kernel::Svc::MemoryPermission perm,
                              std::mt19937_64& generate_random) {
    auto& page_table = process.GetPageTable();
    const u64 alias_code_start =
        GetInteger(page_table.GetAliasCodeRegionStart()) / Kernel::PageSize;
    const u64 alias_code_size = page_table.GetAliasCodeRegionSize() / Kernel::PageSize;

    // Only consider addresses that leave room for the whole allocation: picking from the entire
    // region lets address + size run past its end, which fails as ResultInvalidCurrentMemory and
    // reaches the game as 2010-0106 rather than being retried.
    const u64 size_pages = Common::DivideUp(size, Kernel::PageSize);
    R_UNLESS(alias_code_size > size_pages, Kernel::ResultOutOfMemory);
    const u64 candidate_pages = alias_code_size - size_pages;

    for (size_t trial = 0; trial < 4096; trial++) {
        // Generate a new trial address.
        const u64 mapped_address =
            (alias_code_start + (generate_random() % candidate_pages)) * Kernel::PageSize;

        // Try to map the address
        R_TRY_CATCH(code_memory.MapToOwner(mapped_address, size, perm)) {
            R_CATCH(Kernel::ResultInvalidMemoryRegion) {
                // If we could not map here, retry.
                continue;
            }
            R_CATCH(Kernel::ResultInvalidCurrentMemory) {
                // The range is occupied or does not fit; retry elsewhere.
                continue;
            }
        }
        R_END_TRY_CATCH;

        // Set members.
        m_code_memory = std::addressof(code_memory);
        m_size = size;
        m_address = mapped_address;
        m_perm = perm;

        // Open a new reference to the code memory.
        m_code_memory->Open();

        // We succeeded.
        R_SUCCEED();
    }

    R_THROW(Kernel::ResultOutOfMemory);
}

void CodeMemory::Finalize() {
    if (m_code_memory) {
        R_ASSERT(m_code_memory->UnmapFromOwner(m_address, m_size));
        m_code_memory->Close();
    }

    m_code_memory = nullptr;
}

} // namespace Service::JIT
