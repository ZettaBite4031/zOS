#include <Kernel/Architecture/AMD64/CPU.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>

namespace Zos::Kernel::Architecture::AMD64 {
    namespace {
        inline constexpr Uint32 ExtendedLeafBase{ 0x80000000u };
        inline constexpr Uint32 AmdTopologyLeaf{ 0x8000001Eu };

        [[nodiscard]] constexpr Uint32 Bit(Uint32 index) noexcept {
            return Uint32{ 1 } << index; 
        }

        void CopyRegisterBytes(Uint32 value, char* destination) noexcept {
            destination[0] = static_cast<char>(value & 0xFFu);
            destination[1] = static_cast<char>((value >> 8) & 0xFFu);
            destination[2] = static_cast<char>((value >> 16) & 0xFFu);
            destination[3] = static_cast<char>((value >> 24) & 0xFFu);
        }

        [[nodiscard]] bool StringEquals(const char* left, const char* right) noexcept {
            if (left == nullptr || right == nullptr) return false;

            while (*left != '\0' && *right != '\0') {
                if (*left != *right) return false;
                left++;
                right++;
            }

            return *left == *right;
        }

        void WriteFeature(const char* name, bool supported) noexcept {
            Diagnostics::Write("[zOS/CPU] ");
            Diagnostics::Write(name);
            Diagnostics::Write(": ");
            Diagnostics::Write(supported ? "supported\n" : "not supported\n");
        }
    }

    CpuidResult Cpuid(Uint32 leaf, Uint32 subleaf) noexcept {
        Uint32 eax = leaf;
        Uint32 ebx{};
        Uint32 ecx = subleaf;
        Uint32 edx{};

        __asm__ volatile(
            "cpuid"
            : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx)
            :
            : "memory"
        );

        return CpuidResult{
            .Eax = eax,
            .Ebx = ebx,
            .Ecx = ecx,
            .Edx = edx,
        };
    }

    Uint64 ReadMsr(Uint32 index) noexcept {
        Uint32 low{};
        Uint32 high{};

        __asm__ volatile(
            "rdmsr"
            : "=a"(low), "=d"(high)
            : "c"(index)
            : "memory"
        );

        return static_cast<Uint64>(low) | (static_cast<Uint64>(high) << 32);
    }

    void WriteMsr(Uint32 index, Uint64 value) noexcept {
        const Uint32 low = static_cast<Uint32>(value);
        const Uint32 high = static_cast<Uint32>(value >> 32);

        __asm__ volatile(
            "wrmsr"
            :
            : "c"(index), "a"(low), "d"(high)
            : "memory"
        );
    }

    Uint64 ReadTsc() noexcept {
        Uint32 low{};
        Uint32 high{};

        __asm__ volatile(
            "rdtsc"
            : "=a"(low), "=d"(high)
            :
            : "memory"
        );

        return static_cast<Uint64>(low) | (static_cast<Uint64>(high) << 32);
    }

    CpuInitializationError CpuInfo::Initialize() noexcept {
        if (m_Initialized) return CpuInitializationError::AlreadyInitialized;

        const CpuidResult basic_information = Cpuid(0);
        if (basic_information.Eax < 1)
            return CpuInitializationError::BasicFeatureLeafUnavailable;

        m_MaximumBasicLeaf = basic_information.Eax;

        CopyRegisterBytes(basic_information.Ebx, &m_Vendor[0]);
        CopyRegisterBytes(basic_information.Edx, &m_Vendor[4]);
        CopyRegisterBytes(basic_information.Ecx, &m_Vendor[8]);
        m_Vendor[12] = '\0';

        const CpuidResult extended_information = Cpuid(ExtendedLeafBase);
        if (extended_information.Eax >= ExtendedLeafBase)
            m_MaximumExtendedLeaf = extended_information.Eax;

        const CpuidResult basic_features = Cpuid(1);

        m_Features.Tsc = (basic_features.Edx & Bit(4)) != 0;
        m_Features.Apic = (basic_features.Edx & Bit(9)) != 0;
        m_Features.Pcid = (basic_features.Ecx & Bit(17)) != 0;
        m_Features.X2Apic = (basic_features.Ecx & Bit(21)) != 0;

        if (m_MaximumBasicLeaf >= 7) {
            const CpuidResult structured_features = Cpuid(7, 0);
            m_Features.Smep = (structured_features.Ebx & Bit(7)) != 0;
            m_Features.Smap = (structured_features.Ebx & Bit(20)) != 0;
        }

        if (m_MaximumExtendedLeaf >= 0x80000001u) {
            const CpuidResult extended_features = Cpuid(0x80000001u);
            m_Features.Syscall = (extended_features.Edx & Bit(11)) != 0;
            m_Features.Nx = (extended_features.Edx & Bit(20)) != 0;
        }

        if (m_MaximumExtendedLeaf >= 0x80000007u) {
            const CpuidResult invariant_tsc = Cpuid(0x80000007u);
            m_Features.InvariantTsc = m_Features.Tsc && (invariant_tsc.Edx & Bit(8)) != 0;
        }

        /*
         * Prefer the architectural V2 topology leaf, then the older
         * extended-topology leaf. Both expose a full 32-bit x2APIC ID.
         */
        if (m_MaximumBasicLeaf >= 0x1Fu) {
            const CpuidResult topology = Cpuid(0x1Fu, 0);

            if ((topology.Ebx & 0xFFFFu) != 0) {
                m_BootstrapApicId = topology.Edx;
                m_BootstrapApicIdValid = true;
                m_ExtendedTopologyApicId = true;
            }
        }

        if (!m_BootstrapApicIdValid && m_MaximumBasicLeaf >= 0x0Bu) {
            const CpuidResult topology = Cpuid(0x0Bu, 0);

            if ((topology.Ebx & 0xFFFFu) != 0) {
                m_BootstrapApicId = topology.Edx;
                m_BootstrapApicIdValid = true;
                m_ExtendedTopologyApicId = true;
            }
        }

        /*
         * AMD additionally exposes the processor APIC ID through its
         * extended topology leaf.
         */
        if (!m_BootstrapApicIdValid &&
            StringEquals(m_Vendor, "AuthenticAMD") &&
            m_MaximumExtendedLeaf >= AmdTopologyLeaf) {
            const CpuidResult topology = Cpuid(AmdTopologyLeaf);
            m_BootstrapApicId = topology.Eax;
            m_BootstrapApicIdValid = true;
            m_ExtendedTopologyApicId = true;
        }

        /*
         * CPUID.01H only contains the historical 8-bit initial APIC ID.
         * Keep it as a last-resort fallback rather than allowing it to
         * truncate a topology ID when a modern enumeration leaf exists.
         */
        if (!m_BootstrapApicIdValid && m_Features.Apic) {
            m_BootstrapApicId = basic_features.Ebx >> 24;
            m_BootstrapApicIdValid = true;
        }

        m_Initialized = true;
        Print();
        return CpuInitializationError::Success;
    }

    void CpuInfo::Print() const noexcept {
        Diagnostics::Write("[zOS/CPU] Vendor: ");
        Diagnostics::Write(m_Vendor);
        Diagnostics::Write("\n");

        WriteFeature("APIC", m_Features.Apic);
        WriteFeature("x2APIC", m_Features.X2Apic);

        Diagnostics::Write("[zOS/CPU] BSP APIC ID: ");
        if (m_BootstrapApicIdValid) {
            Diagnostics::WriteDecimal(m_BootstrapApicId);

            if (m_ExtendedTopologyApicId)
                Diagnostics::Write(" (extended topology)");
        } else {
            Diagnostics::Write("unavailable");
        }
        Diagnostics::Write("\n");

        WriteFeature("TSC", m_Features.Tsc);
        WriteFeature("Invariant TSC", m_Features.InvariantTsc);
        WriteFeature("NX", m_Features.Nx);
        WriteFeature("SYSCALL/SYSRET", m_Features.Syscall);
        WriteFeature("SMEP", m_Features.Smep);
        WriteFeature("SMAP", m_Features.Smap);
        WriteFeature("PCID", m_Features.Pcid);
    }

    const char* CpuInfo::Describe(CpuInitializationError error) noexcept {
        switch (error) {
        case CpuInitializationError::Success: return "success";
        case CpuInitializationError::AlreadyInitialized: return "CPU information is already initialized";
        case CpuInitializationError::BasicFeatureLeafUnavailable: return "CPUID does not provide the basic feature leaf";
        default: return "unknown CPU initialization error";
        }
    }
}
