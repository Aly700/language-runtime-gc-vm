#include "lang/gc/heap.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

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
    MarkingVisitor(Heap& heap, std::vector<ObjectId>& worklist)
        : heap_(heap), worklist_(worklist) {}

    void visit(Value& root) override { heap_.enqueue_mark_value(root, worklist_); }

private:
    Heap& heap_;
    std::vector<ObjectId>& worklist_;
};

class Heap::ForwardingVisitor final : public RootVisitor {
public:
    ForwardingVisitor(const Heap& heap, const ForwardingTable& forwarding)
        : heap_(heap), forwarding_(forwarding) {}

    void visit(Value& root) override { heap_.rewrite_value(root, forwarding_); }

private:
    const Heap& heap_;
    const ForwardingTable& forwarding_;
};

class Heap::ValidatingVisitor final : public RootVisitor {
public:
    explicit ValidatingVisitor(const Heap& heap) : heap_(heap) {}

    void visit(Value& root) override { heap_.validate_value(root); }

private:
    const Heap& heap_;
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

void Heap::enqueue_mark_value(Value value, std::vector<ObjectId>& worklist) {
    if (value.is_object()) {
        const auto slot = checked_slot(value.as_object());
        if (!objects_[slot]->marked) {
            worklist.push_back(value.as_object());
        }
    }
}

void Heap::drain_mark_worklist(std::vector<ObjectId>& worklist) {
    while (!worklist.empty()) {
        const auto id = worklist.back();
        worklist.pop_back();

        auto& obj = *objects_[checked_slot(id)];
        if (obj.marked) {
            continue;
        }
        obj.marked = true;

        // Push right first so the LIFO worklist processes left before right.
        enqueue_mark_value(obj.right, worklist);
        enqueue_mark_value(obj.left, worklist);
    }
}

void Heap::collect() {
    collect_impl(nullptr, {});
}

void Heap::collect(RootProvider& roots) {
    collect_impl(&roots, {});
}

void Heap::collect_with_extra_roots(std::span<Value*> extra_roots) {
    collect_impl(nullptr, extra_roots);
}

void Heap::collect_impl(RootProvider* roots, std::span<Value*> extra_roots) {
    std::vector<ObjectId> worklist;
    MarkingVisitor marker(*this, worklist);
    trace_collection_roots(marker, roots, extra_roots);
    drain_mark_worklist(worklist);

    auto compacted = compact_marked_objects();
    rewrite_references(compacted.forwarding, compacted.objects, roots, extra_roots);

    objects_ = std::move(compacted.objects);
    generations_ = std::move(compacted.generations);
    validate_after_collection(roots, extra_roots);
}

void Heap::trace_collection_roots(RootVisitor& visitor, RootProvider* roots,
                                  std::span<Value*> extra_roots) const {
    if (root_provider_ != nullptr && root_provider_ != roots) {
        root_provider_->trace_roots(visitor);
    }
    if (roots != nullptr) {
        roots->trace_roots(visitor);
    }
    for (auto* root : extra_roots) {
        if (root != nullptr) {
            visitor.visit(*root);
        }
    }
}

Heap::CompactionResult Heap::compact_marked_objects() const {
    CompactionResult result;
    result.forwarding.resize(objects_.size());
    result.objects.resize(objects_.size());
    result.generations = generations_;

    std::size_t next_live_slot = 0;
    for (std::size_t old_slot = 0; old_slot < objects_.size(); ++old_slot) {
        const auto& slot = objects_[old_slot];
        if (!slot.has_value() || !slot->marked) {
            continue;
        }

        auto moved = *slot;
        moved.marked = false;
        if (old_slot != next_live_slot) {
            result.generations[next_live_slot] = next_generation(result.generations[next_live_slot]);
        }

        const auto new_id =
            make_object_id(static_cast<std::uint32_t>(next_live_slot),
                           result.generations[next_live_slot]);
        result.forwarding[old_slot] = new_id;
        result.objects[next_live_slot] = moved;
        ++next_live_slot;
    }

    return result;
}

void Heap::rewrite_references(const ForwardingTable& forwarding,
                              std::vector<std::optional<Object>>& compacted_objects,
                              RootProvider* roots, std::span<Value*> extra_roots) const {
    ForwardingVisitor visitor(*this, forwarding);
    trace_collection_roots(visitor, roots, extra_roots);

    for (auto& slot : compacted_objects) {
        if (!slot.has_value()) {
            continue;
        }
        rewrite_value(slot->left, forwarding);
        rewrite_value(slot->right, forwarding);
    }
}

void Heap::rewrite_value(Value& value, const ForwardingTable& forwarding) const {
    if (!value.is_object()) {
        return;
    }

    const auto old_slot = checked_slot(value.as_object());
    if (old_slot >= forwarding.size() || !forwarding[old_slot].has_value()) {
        throw std::logic_error("live object reference missing compaction forwarding entry");
    }
    value = Value::object(*forwarding[old_slot]);
}

void Heap::validate_after_collection(RootProvider* roots, std::span<Value*> extra_roots) const {
    ValidatingVisitor visitor(*this);
    trace_collection_roots(visitor, roots, extra_roots);

    for (const auto& slot : objects_) {
        if (!slot.has_value()) {
            continue;
        }
        assert(!slot->marked && "collector invariant violated: mark bit survived collection");
        validate_value(slot->left);
        validate_value(slot->right);
    }
}

void Heap::validate_value(Value value) const {
    if (!value.is_object()) {
        return;
    }
    const auto slot = checked_slot(value.as_object());
    assert(objects_[slot].has_value() &&
           "collector invariant violated: reference points outside current heap objects");
}

std::size_t Heap::live_count() const {
    std::size_t n = 0;
    for (const auto& slot : objects_) {
        if (slot.has_value()) ++n;
    }
    return n;
}

} // namespace lang::gc
