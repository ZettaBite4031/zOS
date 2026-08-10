#pragma once

namespace Zos::Kernel::Memory {
    using Uint8 = unsigned char;
    using Uint16 = unsigned short;
    using Uint32 = unsigned int;
    using Uint64 = unsigned long long;

    inline constexpr Uint64 PageSize{ 4096 };
    inline constexpr Uint64 Dma32AddressLimit{ 0xFFFFFFFFULL };

    class PhysicalAddress final {
    public:
        constexpr PhysicalAddress() noexcept = default;
        explicit constexpr PhysicalAddress(Uint64 value) noexcept : m_Value(value) {}

        [[nodiscard]] constexpr Uint64 Value() const noexcept { return m_Value; }
        [[nodiscard]] constexpr bool IsNull() const noexcept { return m_Value == 0; }
        [[nodiscard]] constexpr bool IsPageAligned() const noexcept { return (m_Value & (PageSize - 1)) == 0; }

        friend constexpr bool operator==(PhysicalAddress left, PhysicalAddress right) noexcept { return left.m_Value == right.m_Value; }
        friend constexpr bool operator!=(PhysicalAddress left, PhysicalAddress right) noexcept { return !(left == right); }
        friend constexpr bool operator<(PhysicalAddress left, PhysicalAddress right) noexcept { return left.m_Value < right.m_Value; }
        friend constexpr bool operator<=(PhysicalAddress left, PhysicalAddress right) noexcept { return left.m_Value <= right.m_Value; }
        friend constexpr bool operator>(PhysicalAddress left, PhysicalAddress right) noexcept { return right < left; }
        friend constexpr bool operator>=(PhysicalAddress left, PhysicalAddress right) noexcept { return right <= left; }

        
    private:
        Uint64 m_Value{};
    };

    [[nodiscard]] inline constexpr PhysicalAddress operator+(PhysicalAddress address, Uint64 offset) noexcept { return PhysicalAddress(address.Value() + offset); }
    [[nodiscard]] inline constexpr PhysicalAddress operator-(PhysicalAddress address, Uint64 offset) noexcept { return PhysicalAddress(address.Value() - offset); }

    class VirtualAddress final {
        public:
        constexpr VirtualAddress() noexcept = default;
        explicit constexpr VirtualAddress(Uint64 value) noexcept : m_Value(value) {}

        [[nodiscard]] constexpr Uint64 Value() const noexcept { return m_Value; }
        [[nodiscard]] constexpr bool IsNull() const noexcept { return m_Value == 0; }
        [[nodiscard]] constexpr bool IsPageAligned() const noexcept { return (m_Value & (PageSize - 1)) == 0; }

        friend constexpr bool operator==(VirtualAddress left, VirtualAddress right) noexcept { return left.m_Value == right.m_Value; }
        friend constexpr bool operator!=(VirtualAddress left, VirtualAddress right) noexcept { return !(left == right); }
        friend constexpr bool operator<(VirtualAddress left, VirtualAddress right) noexcept { return left.m_Value < right.m_Value; }
        friend constexpr bool operator<=(VirtualAddress left, VirtualAddress right) noexcept { return left.m_Value <= right.m_Value; }
        friend constexpr bool operator>(VirtualAddress left, VirtualAddress right) noexcept { return right < left; }
        friend constexpr bool operator>=(VirtualAddress left, VirtualAddress right) noexcept { return right <= left; }

    private:
        Uint64 m_Value{};
    };

    [[nodiscard]] inline constexpr VirtualAddress operator+(VirtualAddress address, Uint64 offset) noexcept { return VirtualAddress(address.Value() + offset); }
    [[nodiscard]] inline constexpr VirtualAddress operator-(VirtualAddress address, Uint64 offset) noexcept { return VirtualAddress(address.Value() - offset); }

    struct PhysicalSpan final {
        PhysicalAddress Base{};
        Uint64 PageCount{};

        [[nodiscard]] constexpr bool IsEmpty() const noexcept { return PageCount == 0; }
        [[nodiscard]] constexpr Uint64 SizeBytes() const noexcept { return PageCount * PageSize; };
    };

    struct VirtualSpan final {
        VirtualAddress Base{};
        Uint64 PageCount{};

        [[nodiscard]] constexpr bool IsEmpty() const noexcept { return PageCount == 0; }
        [[nodiscard]] constexpr Uint64 SizeBytes() const noexcept { return PageCount * PageSize; };
    };

    enum class PageAccess : Uint32 {
        None = 0,
        Read = 1u << 0,
        Write = 1u << 1,
        Execute = 1u << 2,
        User = 1u << 3,
        Global = 1u << 4,
    };

    [[nodiscard]] constexpr PageAccess operator|(PageAccess left, PageAccess right) noexcept {
        return static_cast<PageAccess>(static_cast<Uint32>(left) | static_cast<Uint32>(right));
    }

    [[nodiscard]] constexpr PageAccess operator&(PageAccess left, PageAccess right) noexcept {
        return static_cast<PageAccess>(static_cast<Uint32>(left) & static_cast<Uint32>(right));
    }

    constexpr PageAccess& operator|=(PageAccess& left, PageAccess right) noexcept {
        left = left | right;
        return left;
    }

    [[nodiscard]] constexpr bool HasAccess(PageAccess value, PageAccess required) noexcept {
        return (static_cast<Uint32>(value) & static_cast<Uint32>(required)) == static_cast<Uint32>(required);
    }

    enum class CachePolicy : Uint32 {
        WriteBack,
        Uncached,
    };

    struct MappingOptions final {
        PageAccess Access{ PageAccess::Read };
        CachePolicy Cache{ CachePolicy::WriteBack };
    };

    static_assert(sizeof(PhysicalAddress) == sizeof(Uint64));
    static_assert(sizeof(VirtualAddress) == sizeof(Uint64));
    static_assert(sizeof(PhysicalSpan) == 16);
    static_assert(sizeof(VirtualSpan) == 16);
}