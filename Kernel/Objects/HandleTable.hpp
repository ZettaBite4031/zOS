#pragma once

#include <Kernel/Objects/Handle.hpp>
#include <Kernel/Objects/Object.hpp>

namespace Zos::Kernel::Objects {
    enum class HandleError : Memory::Uint32 {
        Success,
        AlreadyInitialized,
        NotInitialized,
        InvalidHandle,
        InvalidObject,
        InvalidRights,
        TableFull,
        WrongType,
        AccessDenied,
        OutputBusy,
        ReferenceOverflow,
    };

    class HandleTable final {
    public:
        inline static constexpr Memory::Uint32 Capacity{ 64 };

        constexpr HandleTable() noexcept = default;

        HandleTable(const HandleTable&) = delete;
        HandleTable& operator=(const HandleTable&) = delete;

        [[nodiscard]] HandleError Initialize() noexcept;

        [[nodiscard]] HandleError Open(Object& object, AccessMask rights, Handle& output) noexcept;

        [[nodiscard]] HandleError Close(Handle handle) noexcept;

        [[nodiscard]] HandleError CloseAll() noexcept;

        template<typename T>
        [[nodiscard]] HandleError Acquire(Handle handle, AccessMask required_rights, ObjectReference<T>& output) noexcept {
            if (!m_Initialized) return HandleError::NotInitialized;
            if (output) return HandleError::OutputBusy;

            Entry* entry{};
            const HandleError resolve_error = Resolve(handle, entry);
            if (resolve_error != HandleError::Success)
                return resolve_error;

            if (entry->ObjectPointer->Type() != T::ObjectKind)
                return HandleError::WrongType;

            if (!GrantsAccess(entry->Rights, required_rights))
                return HandleError::AccessDenied;

            if (!entry->ObjectPointer->AddReference())
                return HandleError::ReferenceOverflow;

            output.Adopt(*static_cast<T*>(entry->ObjectPointer));
            return HandleError::Success;
        }

        [[nodiscard]] bool IsInitialized() const noexcept { return m_Initialized; }
        [[nodiscard]] Memory::Uint32 ActiveCount() const noexcept { return m_ActiveCount; }

        [[nodiscard]] bool Validate() const noexcept;

        [[nodiscard]] static const char* Describe(HandleError error) noexcept;

    private:
        struct Entry final {
            Object* ObjectPointer{};
            AccessMask Rights{};
            Memory::Uint32 Generation{ 1 };
            bool Occupied{};
        };

        [[nodiscard]] HandleError Resolve(Handle handle, Entry*& output) noexcept;
        [[nodiscard]] HandleError Resolve(Handle handle, const Entry*& output) const noexcept;

        static void AdvanceGeneration(Entry& entry) noexcept;

        Entry m_Entries[Capacity]{};
        Memory::Uint32 m_ActiveCount{};
        bool m_Initialized{};
    };
}
