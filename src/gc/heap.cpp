#include "lang/gc/heap.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace lang::gc {

namespace {

constexpr ObjectId kSlotMask = 0xFFFF'FFFFull;
constexpr unsigned kGenerationShift = 32;
constexpr std::uint32_t kFirstGeneration = 1;
constexpr std::uint32_t kMaxGeneration = 0x7FFF'FFFFu;

ObjectId make_object_id(std::uint32_t slot, std::uint32_t generation) {
    return (static_cast<ObjectId>(generation) << kGenerationShift) | slot;
}

std::uint32_t slot_from(ObjectId id) {
    return static_cast<std::uint32_t>(id & kSlotMask);
}

std::uint32_t generation_from(ObjectId id) {
    return static_cast<std::uint32_t>(id >> kGenerationShift);
}

std::uint32_t next_generation(std::uint32_t generation) {
    if (generation == kMaxGeneration) {
        throw std::overflow_error("object id generation exhausted");
    }
    return generation + 1;
}

} // namespace

class Heap::MarkingVisitor final : public RootVisitor {
public:
    explicit MarkingVisitor(Heap& heap) : heap_(heap) {}

    void visit(Value& root) override { heap_.mark_value(root); }

private:
    Heap& heap_;
};

ObjectId Heap::allocate_pair(Value left, Value right) {
    if (stress_config_.collect_before_every_allocation) {
        std::array<Value*, 2> operand_roots{&left, &right};
        collect_with_extra_roots(operand_roots);
    }

    auto id = allocate_slot(left, right);

    if (stress_config_.collect_after_every_allocation) {
        Value allocated = Value::object(id);
        std::array<Value*, 1> allocation_root{&allocated};
        collect_with_extra_roots(allocation_root);
        id = allocated.as_object();
    }

    return id;
}

ObjectId Heap::allocate_slot(Value left, Value right) {
    for (std::size_t i = 0; i < objects_.size(); ++i) {
        if (!objects_[i].has_value()) {
            generations_[i] = next_generation(generations_[i]);
            objects_[i] = Object{false, left, right};
            return make_object_id(static_cast<std::uint32_t>(i), generations_[i]);
        }
    }

    if (objects_.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("heap object slot limit exceeded");
    }
    objects_.push_back(Object{false, left, right});
    generations_.push_back(kFirstGeneration);
    return make_object_id(static_cast<std::uint32_t>(objects_.size() - 1), kFirstGeneration);
}

std::size_t Heap::checked_slot(ObjectId id) const {
    const auto slot = slot_from(id);
    const auto generation = generation_from(id);
    if (generation == 0 || slot >= objects_.size() || slot >= generations_.size() ||
        generations_[slot] != generation || !objects_[slot].has_value()) {
        throw std::out_of_range("invalid or stale object id");
    }
    return slot;
}

const Object& Heap::object(ObjectId id) const {
    return *objects_[checked_slot(id)];
}

Object& Heap::object(ObjectId id) {
    return *objects_[checked_slot(id)];
}

Value Heap::left(ObjectId id) const {
    return object(id).left;
}

Value Heap::right(ObjectId id) const {
    return object(id).right;
}

void Heap::set_left(ObjectId id, Value value) {
    store_pair_field(id, PairField::Left, value);
}

void Heap::set_right(ObjectId id, Value value) {
    store_pair_field(id, PairField::Right, value);
}

void Heap::store_pair_field(ObjectId id, PairField field, Value value) {
    // Barrier hook: every pair field mutation flows through this method. A future
    // generational collector should run its old-to-young write barrier here before
    // publishing the new field value.
    auto& obj = object(id);
    if (field == PairField::Left) {
        obj.left = value;
    } else {
        obj.right = value;
    }
}

void Heap::mark_value(Value value) {
    if (value.is_object()) {
        mark_object(value.as_object());
    }
}

void Heap::mark_object(ObjectId id) {
    auto& obj = *objects_[checked_slot(id)];
    if (obj.marked) {
        return;
    }
    obj.marked = true;
    mark_value(obj.left);
    mark_value(obj.right);
}

void Heap::collect() {
    collect_with_extra_roots({});
}

void Heap::collect(RootProvider& roots) {
    MarkingVisitor visitor(*this);
    if (root_provider_ != nullptr && root_provider_ != &roots) {
        root_provider_->trace_roots(visitor);
    }
    roots.trace_roots(visitor);
    sweep();
}

void Heap::collect_with_extra_roots(std::span<Value*> extra_roots) {
    MarkingVisitor visitor(*this);
    if (root_provider_ != nullptr) {
        root_provider_->trace_roots(visitor);
    }
    for (auto* root : extra_roots) {
        if (root != nullptr) {
            visitor.visit(*root);
        }
    }
    sweep();
}

void Heap::sweep() {
    for (auto& slot : objects_) {
        if (!slot.has_value()) {
            continue;
        }
        if (slot->marked) {
            slot->marked = false;
        } else {
            slot.reset();
        }
    }
}

std::size_t Heap::live_count() const {
    std::size_t n = 0;
    for (const auto& slot : objects_) {
        if (slot.has_value()) ++n;
    }
    return n;
}

} // namespace lang::gc
