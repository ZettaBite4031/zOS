#pragma once

namespace Zos::Kernel::Memory {
    using Uint8 = unsigned char;
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

    static_assert(sizeof(PhysicalAddress) == sizeof(Uint64));
    static_assert(sizeof(VirtualAddress) == sizeof(Uint64));
    static_assert(sizeof(PhysicalSpan) == 16);
    static_assert(sizeof(VirtualSpan) == 16);
}