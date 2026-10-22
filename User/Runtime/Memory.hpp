#pragma once

#include <stddef.h>

extern "C" {
    void* memcpy(void* destination, const void* source, size_t size) noexcept;
    void* memmove(void* destination, const void* source, size_t size) noexcept;
    void* memset(void* destination, int value, size_t size) noexcept;
    int memcmp(const void* left, const void* right, size_t size) noexcept;
}
