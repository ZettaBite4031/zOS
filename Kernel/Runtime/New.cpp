#include <Kernel/Runtime/New.hpp>

#include <Kernel/Memory/KernelHeap.hpp>
#include <Kernel/Diagnostics/Diagnostics.hpp>

namespace std {
    const nothrow_t nothrow{};
}

namespace {
    using Size = __SIZE_TYPE__;

    using Zos::Kernel::Memory::KernelHeap;
    using Zos::Kernel::Memory::KernelHeapError;
    using Zos::Kernel::Memory::Uint64;

    KernelHeap* g_KernelHeap{};

    static_assert(sizeof(Size) == sizeof(Uint64));

    /*
     * Ordinary new must satisfy the implementation's default alignment.
     */
    static_assert(KernelHeap::DefaultAlignment >= __STDCPP_DEFAULT_NEW_ALIGNMENT__);

    [[noreturn]] void FatalAllocationError(KernelHeapError error) noexcept {
        Zos::Kernel::Diagnostics::Fatal("C++ Runtime", KernelHeap::Describe(error));
    }

    [[nodiscard]] KernelHeap& BoundHeap() noexcept {
        if (g_KernelHeap == nullptr) 
            Zos::Kernel::Diagnostics::Fatal("C++ Runtime", "dynamic allocation requestedbefore kernel heap binding");
        return *g_KernelHeap;
    }

    [[nodiscard]] constexpr Size NormalizeSize(Size size) noexcept {
        /*
         * The C++ allocation contract permits zero-sized new expressions,
         * but a successful allocation still needs a distinct non-null
         * result while it remains live.
         */
        return size == 0 ? Size{ 1 } : size;
    }

    [[nodiscard]] bool IsRecoverableAllocationFailure(KernelHeapError error) noexcept {
        switch (error) {
        case KernelHeapError::InvalidRequest:
        case KernelHeapError::VirtualAllocationFailed:
        case KernelHeapError::PhysicalAllocationFailed:
        case KernelHeapError::MappingFailed:
        case KernelHeapError::ArenaExhausted:
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] void* Allocate(Size size, Size alignment, bool nothrow) noexcept {
        void* allocation = nullptr;
        const KernelHeapError error = BoundHeap().Allocate(static_cast<Uint64>(NormalizeSize(size)), allocation, static_cast<Uint64>(alignment));
        if (error == KernelHeapError::Success) {
            if (allocation == nullptr) 
                Zos::Kernel::Diagnostics::Fatal("C++ Runtime", "kernel heap reported allocation success with a null pointer");
            return allocation;
        }

        if (nothrow && IsRecoverableAllocationFailure(error)) return nullptr;
        FatalAllocationError(error);
    }

    void Release(void* allocation) noexcept {
        /*
         * All global delete forms must accept nullptr;
         */
        if (allocation == nullptr) return;

        const KernelHeapError error = BoundHeap().Free(allocation);
        if (error != KernelHeapError::Success) FatalAllocationError(error);
    }

    [[nodiscard]] Size AlignmentValue(std::align_val_t alignment) noexcept {
        return static_cast<Size>(alignment);
    }
}

namespace Zos::Kernel::Runtime {
    bool BindKernelHeap(Memory::KernelHeap& heap) noexcept {
        if (g_KernelHeap != nullptr) return false;
        if (!heap.IsInitialized() || !heap.Validate()) return false;
        g_KernelHeap = &heap;
        return true;
    }

    bool IsKernelHeapBound() noexcept {
        return g_KernelHeap != nullptr;
    }
}

/*
 * Throwing scalar/array new.
 *
 * Exceptions are disabled in zOS. A failure of the ordinary allocation
 * functions is therefore fatal rather than returning nullptr and violating
 * compiler assumptions about ordinary new.
 */
void* operator new(Size size) {
    return Allocate(size, static_cast<Size>(KernelHeap::DefaultAlignment), false);
}

void* operator new[](Size size) {
    return Allocate(size, static_cast<Size>(KernelHeap::DefaultAlignment), false);
}

/*
 * Over-aligned new.
 */
void* operator new(Size size, std::align_val_t alignment) {
    return Allocate(size, AlignmentValue(alignment), false);
}

void* operator new[](Size size, std::align_val_t alignment) {
    return Allocate(size, AlignmentValue(alignment), false);
}

/*
 * Nothrow new.
 */
void* operator new(Size size, const std::nothrow_t&) noexcept {
    return Allocate(size, static_cast<Size>(KernelHeap::DefaultAlignment), true);
}

void* operator new[](Size size, const std::nothrow_t&) noexcept {
    return Allocate(size, static_cast<Size>(KernelHeap::DefaultAlignment), true);
}

/*
 * Over-aligned nothrow new.
 */
void* operator new(Size size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return Allocate(size, AlignmentValue(alignment), true);
}

void* operator new[](Size size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return Allocate(size, AlignmentValue(alignment), true);
}

/*
 * Unsized delete.
 */
void operator delete(void* allocation) noexcept {
    Release(allocation);
}

void operator delete[](void* allocation) noexcept {
    Release(allocation);
}

/*
 * Sized delete.
 */
void operator delete(void* allocation, Size) noexcept {
    Release(allocation);
}

void operator delete[](void* allocation, Size) noexcept {
    Release(allocation);
}

/*
 * Aligned delete.
 *
 * KernelHeap stores the authoritative alignment inside its allocation
 * metadata, so no caller-supplied alignment is needed to release it.
 */
void operator delete(void* allocation, std::align_val_t) noexcept {
    Release(allocation);
}

void operator delete[](void* allocation, std::align_val_t) noexcept {
    Release(allocation);
}

/*
 * Sized + aligned delete.
 */
void operator delete(void* allocation, Size, std::align_val_t) noexcept {
    Release(allocation);
}

void operator delete[](void* allocation, Size, std::align_val_t) noexcept {
    Release(allocation);
}

/*
 * Matching nothrow deallocation.
 */
void operator delete(void* allocation, const std::nothrow_t&) noexcept {
    Release(allocation);
}

void operator delete[](void* allocation, const std::nothrow_t&) noexcept {
    Release(allocation);
}

void operator delete(void* allocation, std::align_val_t, const std::nothrow_t&) noexcept {
    Release(allocation);
}

void operator delete[](void* allocation, std::align_val_t, const std::nothrow_t&) noexcept {
    Release(allocation);
}