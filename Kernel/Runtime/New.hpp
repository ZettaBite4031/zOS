#pragma once

namespace Zos::Kernel::Memory {
    class KernelHeap;
}

namespace std {
    /*
     * ABI-compatible freestanding definitions used by the compiler for
     * over-aligned and nothrow allocation expressions.
     * 
     * zOS does not link a hosted C++ standard library, so these types are
     * provided by the kernel runtime itself.
     */
    enum class align_val_t : __SIZE_TYPE__ {};

    struct nothrow_t final {
        explicit constexpr nothrow_t() noexcept = default;
    };

    extern const nothrow_t nothrow;
}

/*
 * Replaceable scalar/array allocation functions.
 */
[[nodiscard]] void* operator new(__SIZE_TYPE__ size);
[[nodiscard]] void* operator new[](__SIZE_TYPE__ size);

/*
 * Scalar/array nothrow allocation.
 */
[[nodiscard]] void* operator new(__SIZE_TYPE__ size, const std::nothrow_t&) noexcept;
[[nodiscard]] void* operator new[](__SIZE_TYPE__ size, const std::nothrow_t&) noexcept;

/*
 * Overaligned allocation.
 */
[[nodiscard]] void* operator new(__SIZE_TYPE__, std::align_val_t);
[[nodiscard]] void* operator new[](__SIZE_TYPE__, std::align_val_t);

/*
 * Over-aligned nothrow allocation.
 */
[[nodiscard]] void* operator new(__SIZE_TYPE__ size, std::align_val_t alignment, const std::nothrow_t&) noexcept;
[[nodiscard]] void* operator new[](__SIZE_TYPE__ size, std::align_val_t alignment, const std::nothrow_t&) noexcept;

/*
 * Ordinary deallocation.
 */
void operator delete(void* allocation) noexcept;
void operator delete[](void* allocation) noexcept;

/*
 * Sized deallocation.
 *
 * The heap already records the authoritative allocation size, so the
 * compiler supplied size is intentionally not trusted for ownership.
 */
void operator delete(void* allocation, __SIZE_TYPE__ size) noexcept;
void operator delete[](void* allocation, __SIZE_TYPE__ size) noexcept;

/*
 * Over-aligned deallocation.
 */
void operator delete(void* allocation, std::align_val_t alignment) noexcept;
void operator delete[](void* allocation, std::align_val_t alignment) noexcept;

/*
 * Sized + over-aligned deallocation.
 */
void operator delete(void* allocation, __SIZE_TYPE__ size, std::align_val_t alignment) noexcept;
void operator delete[](void* allocation, __SIZE_TYPE__ size, std::align_val_t alignment) noexcept;

/*
 * Nothrow matching deallocation functions.
 */
void operator delete(void* allocation, const std::nothrow_t&) noexcept;
void operator delete[](void* allocation, const std::nothrow_t&) noexcept;

void operator delete(void* allocation, std::align_val_t alignment, const std::nothrow_t&) noexcept;
void operator delete[](void* allocation, std::align_val_t alignment, const std::nothrow_t&) noexcept;

/*
 * Placement allocation is not heap allocation.
 *
 * KernelHeap already uses placement new internally; centralizing these
 * definitions here removes its current one-off declaration.
 */
[[nodiscard]] inline void* operator new(__SIZE_TYPE__, void* address) noexcept { return address; }
[[nodiscard]] inline void* operator new[](__SIZE_TYPE__, void* address) noexcept { return address; }

inline void operator delete(void*, void*) noexcept {}
inline void operator delete[](void*, void*) noexcept {}

namespace Zos::Kernel::Runtime {
    /*
     * One-way transition.
     *
     * Global C++ allocation is intentionally unavailable until startup has
     * proven the permanent heap and explicitly binds it here.
     */
    [[nodiscard]] bool BindKernelHeap(Memory::KernelHeap& heap) noexcept;
    [[nodiscard]] bool IsKernelHeapBound() noexcept;
}