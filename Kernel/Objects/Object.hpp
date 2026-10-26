#pragma once

#include <Kernel/Memory/Memory.hpp>

namespace Zos::Kernel::Objects {
    using ObjectId = Memory::Uint64;

    enum class ObjectType : Memory::Uint32 {
        Invalid,
        Process,
        Thread,
    };

    class HandleTable;

    template<typename T>
    class ObjectReference;

    class Object {
    public:
        constexpr Object() noexcept = default;

        Object(const Object&) = delete;
        Object& operator=(const Object&) = delete;

        [[nodiscard]] bool IsObjectInitialized() const noexcept { return m_ObjectInitialized; }
        [[nodiscard]] ObjectType Type() const noexcept { return m_Type; }
        [[nodiscard]] ObjectId Id() const noexcept { return m_ObjectId; }
        [[nodiscard]] Memory::Uint64 ReferenceCount() const noexcept { return m_ReferenceCount; }

    protected:
        [[nodiscard]] bool InitializeObject(ObjectType type) noexcept;

    private:
        friend class HandleTable;

        template<typename T>
        friend class ObjectReference;

        [[nodiscard]] bool AddReference() noexcept;
        [[nodiscard]] bool ReleaseReference() noexcept;

        ObjectId m_ObjectId{};
        Memory::Uint64 m_ReferenceCount{};
        ObjectType m_Type{ ObjectType::Invalid };
        bool m_ObjectInitialized{};
    };

    template<typename T>
    class ObjectReference final {
    public:
        constexpr ObjectReference() noexcept = default;

        ObjectReference(const ObjectReference&) = delete;
        ObjectReference& operator=(const ObjectReference&) = delete;

        ObjectReference(ObjectReference&& other) noexcept
            : m_Object(other.m_Object) {
            other.m_Object = nullptr;
        }

        ObjectReference& operator=(ObjectReference&& other) noexcept {
            if (this == &other)
                return *this;

            Reset();
            m_Object = other.m_Object;
            other.m_Object = nullptr;
            return *this;
        }

        ~ObjectReference() {
            Reset();
        }

        [[nodiscard]] T* Get() noexcept { return m_Object; }
        [[nodiscard]] const T* Get() const noexcept { return m_Object; }

        [[nodiscard]] T* operator->() noexcept { return m_Object; }
        [[nodiscard]] const T* operator->() const noexcept { return m_Object; }

        [[nodiscard]] T& operator*() noexcept { return *m_Object; }
        [[nodiscard]] const T& operator*() const noexcept { return *m_Object; }

        [[nodiscard]] explicit operator bool() const noexcept {
            return m_Object != nullptr;
        }

        void Reset() noexcept {
            if (m_Object == nullptr)
                return;

            (void)m_Object->ReleaseReference();
            m_Object = nullptr;
        }

    private:
        friend class HandleTable;

        void Adopt(T& object) noexcept {
            m_Object = &object;
        }

        T* m_Object{};
    };
}
