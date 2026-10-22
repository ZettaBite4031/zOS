#pragma once

#include <Kernel/Architecture/AMD64/CPU.hpp>
#include <Kernel/Memory/Memory.hpp>
#include <Kernel/UserFault.hpp>

namespace Zos::Kernel::Architecture::AMD64 {
    class DescriptorTables;

    /*
     * Saved by SystemCallEntryAssembly after SYSCALL has switched to CPL0 and
     * the kernel has moved from the untrusted user stack to the current
     * thread's kernel stack.
     *
     * RCX and R11 are architectural SYSCALL scratch registers and therefore
     * become UserRip and UserRflags rather than preserved general registers.
     */
    struct SystemCallContext final {
        Memory::Uint64 R15{};
        Memory::Uint64 R14{};
        Memory::Uint64 R13{};
        Memory::Uint64 R12{};
        Memory::Uint64 R10{};
        Memory::Uint64 R9{};
        Memory::Uint64 R8{};

        Memory::Uint64 Rsi{};
        Memory::Uint64 Rdi{};
        Memory::Uint64 Rbp{};
        Memory::Uint64 Rdx{};
        Memory::Uint64 Rbx{};
        Memory::Uint64 Rax{};

        Memory::Uint64 UserRsp{};
        Memory::Uint64 UserRip{};
        Memory::Uint64 UserRflags{};
    };

    static_assert(sizeof(SystemCallContext) == 16 * sizeof(Memory::Uint64));
    static_assert(__builtin_offsetof(SystemCallContext, Rax) == 12 * sizeof(Memory::Uint64));
    static_assert(__builtin_offsetof(SystemCallContext, UserRsp) == 13 * sizeof(Memory::Uint64));
    static_assert(__builtin_offsetof(SystemCallContext, UserRip) == 14 * sizeof(Memory::Uint64));
    static_assert(__builtin_offsetof(SystemCallContext, UserRflags) == 15 * sizeof(Memory::Uint64));

    enum class SystemCallInitializationError : Memory::Uint32 {
        Success,
        AlreadyInitialized,
        InvalidDependency,
        Unsupported,
        InvalidEntryPoint,
        EferWriteFailed,
        StarWriteFailed,
        LstarWriteFailed,
        FmaskWriteFailed,
    };

    class SystemCallEntry final {
    public:
        constexpr SystemCallEntry() noexcept = default;

        SystemCallEntry(const SystemCallEntry&) = delete;
        SystemCallEntry& operator=(const SystemCallEntry&) = delete;

        [[nodiscard]] SystemCallInitializationError Initialize(const CpuInfo& cpu, const DescriptorTables& descriptor_tables) noexcept;
        [[nodiscard]] bool BindUserFaultHandler(UserFaultHandler handler) noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept { return m_Initialized; }
        [[nodiscard]] Memory::Uint64 DispatchCount() const noexcept { return m_DispatchCount; }

        [[nodiscard]] static const char* Describe(SystemCallInitializationError error) noexcept;

        void Dispatch(SystemCallContext& context) noexcept;

    private:
        [[noreturn]] void RejectReturnState(const SystemCallContext& context, Memory::VirtualAddress address) noexcept;
        [[nodiscard]] bool ReturnStateValid(const SystemCallContext& context, Memory::VirtualAddress& invalid_address) const noexcept;

        UserFaultHandler m_UserFaultHandler{};
        Memory::Uint64 m_DispatchCount{};
        bool m_Initialized{};
    };
}
