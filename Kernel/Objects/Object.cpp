#include <Kernel/Objects/Object.hpp>

namespace Zos::Kernel::Objects {
    namespace {
        ObjectId g_NextObjectId{ 1 };
    }

    bool Object::InitializeObject(ObjectType type) noexcept {
        if (m_ObjectInitialized || type == ObjectType::Invalid)
            return false;

        if (g_NextObjectId == 0)
            return false;

        const ObjectId id = g_NextObjectId++;

        m_ObjectId = id;
        m_ReferenceCount = 1;
        m_Type = type;
        m_ObjectInitialized = true;
        return true;
    }

    bool Object::AddReference() noexcept {
        if (!m_ObjectInitialized || m_ReferenceCount == 0 ||
            m_ReferenceCount == ~Memory::Uint64{ 0 })
            return false;

        ++m_ReferenceCount;
        return true;
    }

    bool Object::ReleaseReference() noexcept {
        /*
         * Step 21 keeps one permanent owner reference on every object.
         * Reaping and final object destruction arrive later, so handle and
         * temporary references may only return the count to that owner.
         */
        if (!m_ObjectInitialized || m_ReferenceCount <= 1)
            return false;

        --m_ReferenceCount;
        return true;
    }
}
