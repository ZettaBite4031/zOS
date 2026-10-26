#pragma once

#include <ABI/Handles.hpp>

namespace Zos::User::Runtime {
    class Handle final {
    public:
        constexpr Handle() noexcept = default;

        constexpr explicit Handle(ABI::HandleValue value) noexcept
            : m_Value(value) {}

        [[nodiscard]] constexpr ABI::HandleValue Value() const noexcept {
            return m_Value;
        }

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return m_Value != ABI::InvalidHandleValue;
        }

        [[nodiscard]] friend constexpr bool operator==(Handle left, Handle right) noexcept {
            return left.m_Value == right.m_Value;
        }

        [[nodiscard]] friend constexpr bool operator!=(Handle left, Handle right) noexcept {
            return !(left == right);
        }

    private:
        ABI::HandleValue m_Value{};
    };

    [[nodiscard]] bool CloseHandle(Handle handle) noexcept;
}
