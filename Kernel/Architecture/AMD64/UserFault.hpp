#pragma once

#include <Kernel/UserFault.hpp>

namespace Zos::Kernel::Architecture::AMD64 {
    struct InterruptContext;

    /*
     * Converts the synchronous AMD64 exceptions currently treated as fatal
     * userspace faults into the architecture-neutral kernel fault contract.
     */
    [[nodiscard]] bool TranslateUserFault(const InterruptContext& context, UserFault& fault) noexcept;
}
