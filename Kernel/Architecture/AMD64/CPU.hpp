#pragma once

#include <Kernel/Memory/Memory.hpp>

namespace Zos::Kernel::Architecture::AMD64 {
    using Memory::Uint32;
    using Memory::Uint64;

    struct CpuidResult final {
        Uint32 Eax{};
        Uint32 Ebx{};
        Uint32 Ecx{};
        Uint32 Edx{};
    };

    [[nodiscard]] CpuidResult Cpuid(Uint32 leaf, Uint32 subleaf = 0) noexcept;

    [[nodiscard]] Uint64 ReadMsr(Uint32 index) noexcept;
    void WriteMsr(Uint32 index, Uint64 value) noexcept;

    [[nodiscard]] Uint64 ReadTsc() noexcept;

    struct CpuFeatures final {
        bool Apic{};
        bool X2Apic{};
        bool Tsc{};
        bool InvariantTsc{};
        bool Nx{};
        bool Smep{};
        bool Smap{};
        bool Pcid{};
    };

    enum class CpuInitializationError : Uint32 {
        Success,
        AlreadyInitialized,
        BasicFeatureLeafUnavailable,
    };

    class CpuInfo final {
    public:
        constexpr CpuInfo() noexcept = default;

        CpuInfo(const CpuInfo&) = delete;
        CpuInfo& operator=(const CpuInfo&) = delete;

        [[nodiscard]] CpuInitializationError Initialize() noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept { return m_Initialized; }

        [[nodiscard]] const char* Vendor() const noexcept { return m_Vendor; }
        [[nodiscard]] Uint32 MaximumBasicLeaf() const noexcept { return m_MaximumBasicLeaf; }
        [[nodiscard]] Uint32 MaximumExtendedLeaf() const noexcept { return m_MaximumExtendedLeaf; }

        [[nodiscard]] const CpuFeatures& Features() const noexcept { return m_Features; }

        [[nodiscard]] bool HasBootstrapApicId() const noexcept { return m_BootstrapApicIdValid; }
        [[nodiscard]] Uint32 BootstrapApicId() const noexcept { return m_BootstrapApicId; }
        [[nodiscard]] bool UsesExtendedTopologyApicId() const noexcept { return m_ExtendedTopologyApicId; }

        [[nodiscard]] static const char* Describe(CpuInitializationError error) noexcept;

    private:
        void Print() const noexcept;

        char m_Vendor[13]{};

        Uint32 m_MaximumBasicLeaf{};
        Uint32 m_MaximumExtendedLeaf{};
        Uint32 m_BootstrapApicId{};

        CpuFeatures m_Features{};

        bool m_BootstrapApicIdValid{};
        bool m_ExtendedTopologyApicId{};
        bool m_Initialized{};
    };
}
