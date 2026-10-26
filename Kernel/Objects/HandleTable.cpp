#include <Kernel/Objects/HandleTable.hpp>

namespace Zos::Kernel::Objects {
    HandleError HandleTable::Initialize() noexcept {
        if (m_Initialized) return HandleError::AlreadyInitialized;

        for (auto& entry : m_Entries) {
            entry = {};
            entry.Generation = 1;
        }

        m_ActiveCount = 0;
        m_Initialized = true;
        return HandleError::Success;
    }

    HandleError HandleTable::Open(Object& object, AccessMask rights, Handle& output) noexcept {
        if (!m_Initialized) return HandleError::NotInitialized;
        if (output.IsValid()) return HandleError::OutputBusy;
        if (!object.IsObjectInitialized()) return HandleError::InvalidObject;
        if (rights == NoAccess) return HandleError::InvalidRights;

        for (Memory::Uint32 slot = 0; slot < Capacity; ++slot) {
            Entry& entry = m_Entries[slot];
            if (entry.Occupied) continue;
            if (!object.AddReference())
                return HandleError::ReferenceOverflow;

            entry.ObjectPointer = &object;
            entry.Rights = rights;
            entry.Occupied = true;

            output = Handle::Encode(slot, entry.Generation);
            ++m_ActiveCount;
            return HandleError::Success;
        }

        return HandleError::TableFull;
    }

    HandleError HandleTable::Close(Handle handle) noexcept {
        if (!m_Initialized) return HandleError::NotInitialized;

        Entry* entry{};
        const HandleError resolve_error = Resolve(handle, entry);
        if (resolve_error != HandleError::Success)
            return resolve_error;

        if (m_ActiveCount == 0 || entry->ObjectPointer == nullptr)
            return HandleError::InvalidHandle;

        Object* object = entry->ObjectPointer;

        /*
         * Release first so a failed reference transition leaves the handle
         * table unchanged rather than half-closing the entry.
         */
        if (!object->ReleaseReference())
            return HandleError::InvalidObject;

        entry->ObjectPointer = nullptr;
        entry->Rights = NoAccess;
        entry->Occupied = false;
        AdvanceGeneration(*entry);

        --m_ActiveCount;
        return HandleError::Success;
    }

    HandleError HandleTable::CloseAll() noexcept {
        if (!m_Initialized) return HandleError::NotInitialized;
        if (!Validate()) return HandleError::InvalidObject;

        for (auto& entry : m_Entries) {
            if (!entry.Occupied) continue;

            Object* object = entry.ObjectPointer;
            if (object == nullptr || !object->ReleaseReference())
                return HandleError::InvalidObject;

            entry.ObjectPointer = nullptr;
            entry.Rights = NoAccess;
            entry.Occupied = false;
            AdvanceGeneration(entry);
        }

        m_ActiveCount = 0;
        return HandleError::Success;
    }

    HandleError HandleTable::Resolve(Handle handle, Entry*& output) noexcept {
        const Entry* resolved{};
        const HandleError error = static_cast<const HandleTable*>(this)->Resolve(handle, resolved);
        output = const_cast<Entry*>(resolved);
        return error;
    }

    HandleError HandleTable::Resolve(Handle handle, const Entry*& output) const noexcept {
        output = nullptr;

        if (!m_Initialized) return HandleError::NotInitialized;
        if (!handle.IsValid() || handle.Slot() >= Capacity)
            return HandleError::InvalidHandle;

        const Entry& entry = m_Entries[handle.Slot()];
        if (!entry.Occupied || entry.ObjectPointer == nullptr ||
            entry.Generation != handle.Generation())
            return HandleError::InvalidHandle;

        output = &entry;
        return HandleError::Success;
    }

    void HandleTable::AdvanceGeneration(Entry& entry) noexcept {
        ++entry.Generation;
        if (entry.Generation == 0) entry.Generation = 1;
    }

    bool HandleTable::Validate() const noexcept {
        if (!m_Initialized) return false;

        Memory::Uint32 occupied{};

        for (const auto& entry : m_Entries) {
            if (entry.Generation == 0) return false;

            if (!entry.Occupied) {
                if (entry.ObjectPointer != nullptr || entry.Rights != NoAccess)
                    return false;

                continue;
            }

            ++occupied;
            if (entry.ObjectPointer == nullptr || !entry.ObjectPointer->IsObjectInitialized() ||
                entry.ObjectPointer->ReferenceCount() <= 1 || entry.Rights == NoAccess)
                return false;
        }

        return occupied == m_ActiveCount;
    }

    const char* HandleTable::Describe(HandleError error) noexcept {
        switch (error) {
        case HandleError::Success: return "success";
        case HandleError::AlreadyInitialized: return "handle table is already initialized";
        case HandleError::NotInitialized: return "handle table is not initialized";
        case HandleError::InvalidHandle: return "handle is invalid or stale";
        case HandleError::InvalidObject: return "kernel object is invalid";
        case HandleError::InvalidRights: return "handle rights are invalid";
        case HandleError::TableFull: return "handle table is full";
        case HandleError::WrongType: return "handle refers to the wrong object type";
        case HandleError::AccessDenied: return "handle does not grant the required rights";
        case HandleError::OutputBusy: return "output already owns a handle or object reference";
        case HandleError::ReferenceOverflow: return "kernel object reference count overflowed";
        default: return "unknown handle error";
        }
    }
}
