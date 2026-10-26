#include <Kernel/Objects/SelfTest.hpp>

#include <Kernel/Diagnostics/Diagnostics.hpp>
#include <Kernel/Objects/HandleTable.hpp>

namespace Zos::Kernel::Objects::SelfTest {
    namespace {
        inline constexpr AccessMask QueryRight{ 1ULL << 0 };
        inline constexpr AccessMask ModifyRight{ 1ULL << 1 };

        template<ObjectType TypeValue>
        class TestObject final : public Object {
        public:
            inline static constexpr ObjectType ObjectKind{ TypeValue };

            [[nodiscard]] bool Initialize() noexcept {
                return InitializeObject(ObjectKind);
            }
        };
    }

    void VerifyFoundation() noexcept {
        TestObject<ObjectType::Process> process{};
        TestObject<ObjectType::Thread> thread{};
        HandleTable table{};

        if (!process.Initialize() || !thread.Initialize() ||
            process.Id() == 0 || thread.Id() == 0 ||
            process.Id() == thread.Id() ||
            process.ReferenceCount() != 1 ||
            thread.ReferenceCount() != 1)
            Diagnostics::Fatal("Object", "typed object identity initialization failed");

        if (table.Initialize() != HandleError::Success || !table.Validate())
            Diagnostics::Fatal("Handle", "handle table initialization failed");

        Handle first{};
        if (table.Open(process, QueryRight, first) != HandleError::Success ||
            !first.IsValid() || process.ReferenceCount() != 2 ||
            table.ActiveCount() != 1 || !table.Validate())
            Diagnostics::Fatal("Handle", "handle open/reference accounting failed");

        ObjectReference<TestObject<ObjectType::Process>> process_reference{};
        if (table.Acquire(first, QueryRight, process_reference) != HandleError::Success ||
            !process_reference || process_reference->Id() != process.Id() ||
            process.ReferenceCount() != 3)
            Diagnostics::Fatal("Handle", "typed handle acquisition failed");

        ObjectReference<TestObject<ObjectType::Thread>> wrong_type{};
        if (table.Acquire(first, QueryRight, wrong_type) != HandleError::WrongType)
            Diagnostics::Fatal("Handle", "wrong-type handle acquisition was not rejected");

        ObjectReference<TestObject<ObjectType::Process>> denied{};
        if (table.Acquire(first, ModifyRight, denied) != HandleError::AccessDenied)
            Diagnostics::Fatal("Handle", "handle rights were not enforced");

        process_reference.Reset();

        Handle duplicate{};
        if (table.Open(process, QueryRight, duplicate) != HandleError::Success ||
            duplicate == first || process.ReferenceCount() != 3)
            Diagnostics::Fatal("Handle", "multiple handles did not preserve object identity");

        const ObjectId shared_id = process.Id();
        if (table.Close(first) != HandleError::Success || process.ReferenceCount() != 2)
            Diagnostics::Fatal("Handle", "handle close/reference release failed");

        ObjectReference<TestObject<ObjectType::Process>> stale{};
        if (table.Acquire(first, QueryRight, stale) != HandleError::InvalidHandle)
            Diagnostics::Fatal("Handle", "closed handle remained valid");

        if (table.Close(duplicate) != HandleError::Success || process.ReferenceCount() != 1)
            Diagnostics::Fatal("Handle", "duplicate handle close failed");

        Handle reused{};
        if (table.Open(process, QueryRight, reused) != HandleError::Success || reused == first ||
            reused.Slot() != first.Slot() || reused.Generation() == first.Generation() || process.Id() != shared_id)
            Diagnostics::Fatal("Handle", "stale-handle generation protection failed");

        Handle thread_handle{};
        if (table.Open(thread, QueryRight, thread_handle) != HandleError::Success)
            Diagnostics::Fatal("Handle", "second object handle open failed");

        if (table.CloseAll() != HandleError::Success || table.ActiveCount() != 0 ||
            process.ReferenceCount() != 1 || thread.ReferenceCount() != 1 || !table.Validate())
            Diagnostics::Fatal("Handle", "handle-table close-all accounting failed");

        Diagnostics::Write("[zOS/Object] Typed object identity verified.\n");
        Diagnostics::Write("[zOS/Handle] Rights and type validation verified.\n");
        Diagnostics::Write("[zOS/Handle] Stale generation rejection verified.\n");
        Diagnostics::Write("[zOS/Handle] Reference lifetime verified.\n");
        Diagnostics::Write("[zOS/Object] Kernel object foundation verified.\n");
    }
}
