#include "lang/gc/heap.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace lang::gc {

struct HeapLifetime {
    bool alive{true};
};

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

void Handle::ensure_usable() const {
    if (heap_ == nullptr) {
        throw std::logic_error("GC handle used after move");
    }
    if (!lifetime_ || !lifetime_->alive) {
        assert(false && "GC handle used after its heap was destroyed");
        throw std::logic_error("GC handle used after its heap was destroyed");
    }
}

Handle::Handle(Heap& heap, Value value)
    : heap_(&heap), lifetime_(heap.lifetime_), slot_(value) {
    heap.register_handle_root(&slot_);
}

Handle::~Handle() {
    release();
}

Handle::Handle(Handle&& other) noexcept
    : heap_(other.heap_), lifetime_(std::move(other.lifetime_)), slot_(other.slot_) {
    if (heap_ != nullptr) {
        if (!lifetime_ || !lifetime_->alive) {
            assert(false && "moving a GC handle after its heap was destroyed");
            std::abort();
        }
        heap_->move_handle_root(&other.slot_, &slot_);
    }
    other.heap_ = nullptr;
    other.slot_ = Value::nil();
}

Handle& Handle::operator=(Handle&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    release();

    heap_ = other.heap_;
    lifetime_ = std::move(other.lifetime_);
    slot_ = other.slot_;
    if (heap_ != nullptr) {
        if (!lifetime_ || !lifetime_->alive) {
            assert(false && "move-assigning a GC handle after its heap was destroyed");
            std::abort();
        }
        heap_->move_handle_root(&other.slot_, &slot_);
    }
    other.heap_ = nullptr;
    other.slot_ = Value::nil();
    return *this;
}

Value Handle::value() const {
    ensure_usable();
    return slot_;
}

ObjectId Handle::object() const {
    return value().as_object();
}

void Handle::release() noexcept {
    if (heap_ == nullptr) {
        return;
    }

    if (!lifetime_ || !lifetime_->alive) {
        assert(false && "destroying a GC handle after its heap was destroyed");
        std::abort();
    }

    heap_->deregister_handle_root(&slot_);
    heap_ = nullptr;
    lifetime_.reset();
    slot_ = Value::nil();
}

class Heap::MarkingVisitor final : public RootVisitor {
public:
    MarkingVisitor(Heap& heap, std::vector<ObjectId>& worklist, CollectionKind kind)
        : heap_(heap), worklist_(worklist), kind_(kind) {}

    void visit(Value& root) override { heap_.enqueue_mark_value(root, worklist_, kind_); }

private:
    Heap& heap_;
    std::vector<ObjectId>& worklist_;
    CollectionKind kind_;
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

Heap::Heap() : lifetime_(std::make_shared<HeapLifetime>()) {}

Heap::~Heap() noexcept {
    if (lifetime_) {
        lifetime_->alive = false;
    }
    if (!handle_roots_.empty()) {
        assert(false && "Heap destroyed while GC handles are still live");
        std::abort();
    }
}

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

Handle Heap::make_handle(Value value) {
    if (value.is_object()) {
        (void)checked_slot(value.as_object());
    }
    return Handle(*this, value);
}

Handle Heap::make_handle(ObjectId id) {
    (void)checked_slot(id);
    return Handle(*this, Value::object(id));
}

ObjectId Heap::allocate_slot(Value left, Value right) {
    for (std::size_t i = 0; i < objects_.size(); ++i) {
        if (!objects_[i].has_value()) {
            generations_[i] = next_generation(generations_[i]);
            objects_[i] = Object{false, ObjectGeneration::Young, left, right};
            ++metrics_.allocations;
            if (objects_.size() > metrics_.heap_peak_slots) {
                metrics_.heap_peak_slots = objects_.size();
            }
            return make_object_id(static_cast<std::uint32_t>(i), generations_[i]);
        }
    }

    if (objects_.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("heap object slot limit exceeded");
    }
    objects_.push_back(Object{false, ObjectGeneration::Young, left, right});
    generations_.push_back(kFirstGeneration);
    ++metrics_.allocations;
    if (objects_.size() > metrics_.heap_peak_slots) {
        metrics_.heap_peak_slots = objects_.size();
    }
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

Object& Heap::mutable_object(ObjectId id) {
    return *objects_[checked_slot(id)];
}

void Heap::register_handle_root(Value* slot) {
    assert(slot != nullptr && "cannot register a null GC handle root slot");
    handle_roots_.push_back(slot);
}

void Heap::deregister_handle_root(Value* slot) noexcept {
    for (auto it = handle_roots_.begin(); it != handle_roots_.end(); ++it) {
        if (*it == slot) {
            handle_roots_.erase(it);
            return;
        }
    }
    assert(false && "GC handle root slot was not registered");
    std::abort();
}

void Heap::move_handle_root(Value* from, Value* to) noexcept {
    assert(to != nullptr && "cannot move a GC handle root to a null slot");
    for (auto& root : handle_roots_) {
        if (root == from) {
            root = to;
            return;
        }
    }
    assert(false && "moved GC handle root slot was not registered");
    std::abort();
}

void Heap::trace_handle_roots(RootVisitor& visitor) const {
    for (auto* root : handle_roots_) {
        assert(root != nullptr && "registered GC handle root slot is null");
        visitor.visit(*root);
    }
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
    // Barrier hook: every pair field mutation must flow through this method. The public
    // heap API intentionally exposes only const object inspection plus set_left/set_right,
    // so bytecode cannot publish a field without running this old-to-young barrier.
    const bool barrier_triggered = record_write_barrier_if_needed(id, value);

    auto& obj = mutable_object(id);
    if (field == PairField::Left) {
        obj.left = value;
    } else {
        obj.right = value;
    }

    if (barrier_triggered && stress_config_.collect_minor_after_every_write_barrier) {
        collect_minor();
    }
}

bool Heap::record_write_barrier_if_needed(ObjectId owner, Value value) {
    if (!value.is_object()) {
        return false;
    }

    const auto owner_slot = checked_slot(owner);
    const auto target_slot = checked_slot(value.as_object());
    const bool must_remember = is_old_slot(owner_slot) && is_young_slot(target_slot);
    if (!must_remember) {
        return false;
    }

    if (TEST_ONLY_skip_next_write_barrier_) {
        TEST_ONLY_skip_next_write_barrier_ = false;
        return false;
    }

    ++metrics_.write_barrier_hits;
    record_remembered_object(owner);
    return true;
}

void Heap::record_remembered_object(ObjectId id) {
    const auto slot = checked_slot(id);
    if (!is_old_slot(slot)) {
        throw std::logic_error("write barrier attempted to remember a non-old object");
    }
    if (!remembered_set_contains(id)) {
        remembered_set_.push_back(id);
    }
    if (remembered_set_.size() > metrics_.remembered_set_peak) {
        metrics_.remembered_set_peak = remembered_set_.size();
    }
}

bool Heap::remembered_set_contains(ObjectId id) const {
    for (const auto remembered : remembered_set_) {
        if (remembered == id) {
            return true;
        }
    }
    return false;
}

bool Heap::is_young_slot(std::size_t slot) const {
    assert(slot < objects_.size());
    return objects_[slot].has_value() &&
           objects_[slot]->generation == ObjectGeneration::Young;
}

bool Heap::is_old_slot(std::size_t slot) const {
    assert(slot < objects_.size());
    return objects_[slot].has_value() &&
           objects_[slot]->generation == ObjectGeneration::Old;
}

void Heap::enqueue_mark_value(Value value, std::vector<ObjectId>& worklist,
                              CollectionKind kind) {
    if (!value.is_object()) {
        return;
    }

    const auto slot = checked_slot(value.as_object());
    if (kind == CollectionKind::Minor && !is_young_slot(slot)) {
        return;
    }
    if (!objects_[slot]->marked) {
        worklist.push_back(value.as_object());
    }
}

void Heap::enqueue_young_references_from_remembered_set(std::vector<ObjectId>& worklist) {
    for (const auto remembered : remembered_set_) {
        const auto slot = checked_slot(remembered);
        if (!is_old_slot(slot)) {
            throw std::logic_error("remembered-set entry does not name an old object");
        }
        const auto& obj = *objects_[slot];
        // Push right first so the LIFO worklist processes left before right.
        enqueue_mark_value(obj.right, worklist, CollectionKind::Minor);
        enqueue_mark_value(obj.left, worklist, CollectionKind::Minor);
    }
}

void Heap::drain_mark_worklist(std::vector<ObjectId>& worklist, CollectionKind kind) {
    while (!worklist.empty()) {
        const auto id = worklist.back();
        worklist.pop_back();

        const auto slot = checked_slot(id);
        if (kind == CollectionKind::Minor && !is_young_slot(slot)) {
            continue;
        }

        auto& obj = *objects_[slot];
        if (obj.marked) {
            continue;
        }
        obj.marked = true;

        // Push right first so the LIFO worklist processes left before right.
        enqueue_mark_value(obj.right, worklist, kind);
        enqueue_mark_value(obj.left, worklist, kind);
    }
}

void Heap::collect() {
    collect_impl(CollectionKind::Major, nullptr, {});
}

void Heap::collect(RootProvider& roots) {
    collect_impl(CollectionKind::Major, &roots, {});
}

void Heap::collect_minor() {
    collect_impl(CollectionKind::Minor, nullptr, {});
}

void Heap::collect_minor(RootProvider& roots) {
    collect_impl(CollectionKind::Minor, &roots, {});
}

void Heap::collect_with_extra_roots(std::span<Value*> extra_roots) {
    collect_impl(CollectionKind::Major, nullptr, extra_roots);
}

void Heap::collect_impl(CollectionKind kind, RootProvider* roots, std::span<Value*> extra_roots) {
    validate_remembered_set();
    if (kind == CollectionKind::Major) {
        ++metrics_.major_collections;
    } else {
        ++metrics_.minor_collections;
    }

    std::vector<ObjectId> worklist;
    MarkingVisitor marker(*this, worklist, kind);
    trace_collection_roots(marker, roots, extra_roots);
    if (kind == CollectionKind::Minor) {
        enqueue_young_references_from_remembered_set(worklist);
    }
    drain_mark_worklist(worklist, kind);

    auto compacted = compact_live_objects(kind);
    metrics_.objects_moved += compacted.objects_moved;
    rewrite_references(compacted.forwarding, compacted.objects, roots, extra_roots);
    auto rewritten_remembered_set = rewrite_remembered_set(compacted.forwarding);

    objects_ = std::move(compacted.objects);
    generations_ = std::move(compacted.generations);
    remembered_set_ = std::move(rewritten_remembered_set);
    prune_remembered_set();

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
    trace_handle_roots(visitor);
    for (auto* root : extra_roots) {
        if (root != nullptr) {
            visitor.visit(*root);
        }
    }
}

Heap::CompactionResult Heap::compact_live_objects(CollectionKind kind) const {
    CompactionResult result;
    result.forwarding.resize(objects_.size());
    result.objects.resize(objects_.size());
    result.generations = generations_;

    std::size_t next_live_slot = 0;
    for (std::size_t old_slot = 0; old_slot < objects_.size(); ++old_slot) {
        const auto& slot = objects_[old_slot];
        if (!slot.has_value()) {
            continue;
        }

        const bool live = kind == CollectionKind::Major
                              ? slot->marked
                              : (slot->generation == ObjectGeneration::Old || slot->marked);
        if (!live) {
            continue;
        }

        auto moved = *slot;
        moved.marked = false;
        if (moved.generation == ObjectGeneration::Young) {
            moved.generation = ObjectGeneration::Old;
        }

        if (old_slot != next_live_slot) {
            result.generations[next_live_slot] = next_generation(result.generations[next_live_slot]);
            ++result.objects_moved;
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

std::vector<ObjectId> Heap::rewrite_remembered_set(const ForwardingTable& forwarding) const {
    std::vector<ObjectId> rewritten;
    rewritten.reserve(remembered_set_.size());

    for (const auto remembered : remembered_set_) {
        const auto old_slot = checked_slot(remembered);
        if (old_slot >= forwarding.size() || !forwarding[old_slot].has_value()) {
            continue;
        }
        const auto rewritten_id = *forwarding[old_slot];
        bool already_present = false;
        for (const auto existing : rewritten) {
            if (existing == rewritten_id) {
                already_present = true;
                break;
            }
        }
        if (!already_present) {
            rewritten.push_back(rewritten_id);
        }
    }

    return rewritten;
}

void Heap::prune_remembered_set() {
    std::vector<ObjectId> pruned;
    for (std::size_t slot = 0; slot < objects_.size(); ++slot) {
        if (!objects_[slot].has_value() || !is_old_slot(slot)) {
            continue;
        }

        const auto& obj = *objects_[slot];
        bool has_young_reference = false;
        for (const auto value : {obj.left, obj.right}) {
            if (!value.is_object()) {
                continue;
            }
            const auto target_slot = checked_slot(value.as_object());
            has_young_reference = has_young_reference || is_young_slot(target_slot);
        }
        if (has_young_reference) {
            pruned.push_back(make_object_id(static_cast<std::uint32_t>(slot), generations_[slot]));
        }
    }
    remembered_set_ = std::move(pruned);
}

void Heap::validate_after_collection(RootProvider* roots, std::span<Value*> extra_roots) const {
    ++TEST_ONLY_validation_count_;

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

    validate_remembered_set();
}

void Heap::validate_remembered_set() const {
    for (const auto remembered : remembered_set_) {
        const auto slot = checked_slot(remembered);
        if (!is_old_slot(slot)) {
            throw std::logic_error("remembered-set entry does not name an old object");
        }
    }

    for (std::size_t slot = 0; slot < objects_.size(); ++slot) {
        if (!objects_[slot].has_value() || !is_old_slot(slot)) {
            continue;
        }

        const auto owner = make_object_id(static_cast<std::uint32_t>(slot), generations_[slot]);
        const auto& obj = *objects_[slot];
        for (const auto value : {obj.left, obj.right}) {
            if (!value.is_object()) {
                continue;
            }
            const auto target_slot = checked_slot(value.as_object());
            if (is_young_slot(target_slot) && !remembered_set_contains(owner)) {
                throw std::logic_error(
                    "old-to-young reference missing remembered-set entry");
            }
        }
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

bool Heap::TEST_ONLY_is_young_object(ObjectId id) const {
    return is_young_slot(checked_slot(id));
}

bool Heap::TEST_ONLY_is_old_object(ObjectId id) const {
    return is_old_slot(checked_slot(id));
}

void Heap::TEST_ONLY_skip_next_write_barrier_for_barrier_validator() {
    TEST_ONLY_skip_next_write_barrier_ = true;
}

void Heap::TEST_ONLY_validate_gc_invariants() const {
    validate_after_collection(nullptr, {});
}

} // namespace lang::gc
