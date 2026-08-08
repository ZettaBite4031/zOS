namespace {
    using Size = unsigned long long;
    using Byte = unsigned char;
    using Address = unsigned long long;
}

// Keep these primitives opaque to Clang's loop-idiom recognizer. Otherwise an
// optimized implementation of memcpy/memset can be rewritten into a call to
// itself before zSO has a libc/runtime beneath it.

#define ZOS_MEMORY_PRIMITIVE __attribute__((noinline, optnone))

extern "C" ZOS_MEMORY_PRIMITIVE void* memset(void* dst, int v, Size n) noexcept {
    auto* output = static_cast<Byte*>(dst);
    const Byte byte = static_cast<Byte>(v);
    for (Size i = 0; i < n; i++) output[i] = byte;
    return dst;
}

extern "C" ZOS_MEMORY_PRIMITIVE void* memcpy(void* dst, const void* src, Size n) noexcept {
    auto* output = static_cast<Byte*>(dst);
    const auto* input = static_cast<const Byte*>(src);
    for (Size i = 0; i < n; i++) output[i] = input[i];
    return dst;
} 

extern "C" ZOS_MEMORY_PRIMITIVE void* memmove(void* dst, const void* src, Size n) noexcept {
    auto* output = static_cast<Byte*>(dst);
    const auto* input = static_cast<const Byte*>(src);
    if (output == input || n == 0) return dst;

    if (reinterpret_cast<Address>(output) < reinterpret_cast<Address>(input)) 
        for (Size i = 0; i < n; i++) output[i] = input[i];
    else 
        for (Size i = n; i != 0; i--) output[i - 1] = input[i - 1];

    return dst;
}

extern "C" ZOS_MEMORY_PRIMITIVE int memcmp(const void* left, const void* right, Size n) noexcept {
    const auto* left_bytes = static_cast<const Byte*>(left);
    const auto* right_bytes = static_cast<const Byte*>(right);
    for (Size i = 0; i < n; i++)
        if (left_bytes[i] != right_bytes[i]) 
            return left_bytes[i] < right_bytes[i] ? -1 : 1;
    return 0;
}

#undef ZOS_MEMORY_PRIMITIVE