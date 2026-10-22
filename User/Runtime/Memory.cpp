#include <User/Runtime/Memory.hpp>

extern "C" void* memcpy(void* destination, const void* source, size_t size) noexcept {
    auto* out = static_cast<unsigned char*>(destination);
    const auto* in = static_cast<const unsigned char*>(source);
    for (size_t index = 0; index < size; ++index) out[index] = in[index];
    return destination;
}

extern "C" void* memmove(void* destination, const void* source, size_t size) noexcept {
    auto* out = static_cast<unsigned char*>(destination);
    const auto* in = static_cast<const unsigned char*>(source);

    if (out == in || size == 0) return destination;
    if (reinterpret_cast<__UINTPTR_TYPE__>(out) < reinterpret_cast<__UINTPTR_TYPE__>(in)) {
        for (size_t index = 0; index < size; ++index) out[index] = in[index];
    } else {
        while (size != 0) {
            --size;
            out[size] = in[size];
        }
    }
    return destination;
}

extern "C" void* memset(void* destination, int value, size_t size) noexcept {
    auto* out = static_cast<unsigned char*>(destination);
    for (size_t index = 0; index < size; ++index) out[index] = static_cast<unsigned char>(value);
    return destination;
}

extern "C" int memcmp(const void* left, const void* right, size_t size) noexcept {
    const auto* a = static_cast<const unsigned char*>(left);
    const auto* b = static_cast<const unsigned char*>(right);
    for (size_t index = 0; index < size; ++index) {
        if (a[index] != b[index]) return static_cast<int>(a[index]) - static_cast<int>(b[index]);
    }
    return 0;
}
