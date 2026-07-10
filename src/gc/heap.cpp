#include "lang/gc/heap.hpp"

#include <array>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
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
constexpr std::size_t kStorageSlotBytes = sizeof(std::int64_t);

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

std::uint32_t generation_for_new_base(std::uint32_t generation) {
    if (generation == 0) {
        return kFirstGeneration;
    }
    return next_generation(generation);
}

std::size_t storage_slot_count(const Object& object) {
    switch (object.kind) {
    case ObjectKind::Pair:
        return 1;
    case ObjectKind::ScalarArray:
        return object.length == 0 ? 1 : object.length;
    case ObjectKind::RefArray:
        return object.length == 0 ? 1 : object.length;
    case ObjectKind::Str:
        return 1 + (static_cast<std::size_t>(object.length) +
                    kStorageSlotBytes - 1) /
                       kStorageSlotBytes;
    case ObjectKind::Closure:
        return 1 + static_cast<std::size_t>(object.length);
    }
    throw std::logic_error("unknown object kind");
}

void validate_descriptor_shape(const Object& object) {
    switch (object.kind) {
    case ObjectKind::Pair:
        if (object.length != 2 || !object.scalar_elements.empty() ||
            !object.ref_elements.empty() || !object.string_bytes.empty() ||
            !object.closure_captures.empty() ||
            !object.closure_capture_map.empty()) {
            throw std::logic_error("pair object descriptor does not match pair payload");
        }
        return;
    case ObjectKind::ScalarArray:
        if (object.scalar_elements.size() != object.length ||
            !object.ref_elements.empty() || !object.string_bytes.empty() ||
            !object.closure_captures.empty() ||
            !object.closure_capture_map.empty()) {
            throw std::logic_error("scalar array descriptor length does not match payload");
        }
        return;
    case ObjectKind::RefArray:
        if (!object.scalar_elements.empty() ||
            object.ref_elements.size() != object.length ||
            !object.string_bytes.empty() || !object.closure_captures.empty() ||
            !object.closure_capture_map.empty()) {
            throw std::logic_error("ref array descriptor length does not match payload");
        }
        return;
    case ObjectKind::Str:
        if (!object.scalar_elements.empty() || !object.ref_elements.empty() ||
            object.string_bytes.size() != object.length ||
            object.left.tag() != Value::Tag::Nil ||
            object.right.tag() != Value::Tag::Nil ||
            !object.closure_captures.empty() ||
            !object.closure_capture_map.empty()) {
            throw std::logic_error("string descriptor length does not match opaque byte payload");
        }
        return;
    case ObjectKind::Closure:
        if (!object.scalar_elements.empty() || !object.ref_elements.empty() ||
            !object.string_bytes.empty() ||
            object.closure_captures.size() != object.length ||
            object.closure_capture_map.size() != object.length ||
            object.left.tag() != Value::Tag::Nil ||
            object.right.tag() != Value::Tag::Nil) {
            throw std::logic_error(
                "closure descriptor length does not match capture-map payload");
        }
        for (std::size_t i = 0; i < object.closure_captures.size(); ++i) {
            const auto tag = object.closure_captures[i].tag();
            if (object.closure_capture_map[i]) {
                if (tag != Value::Tag::Object && tag != Value::Tag::Nil) {
                    throw std::logic_error(
                        "closure capture map marks a scalar payload as a reference");
                }
            } else if (tag == Value::Tag::Object) {
                throw std::logic_error(
                    "closure capture map marks a reference payload as scalar");
            }
        }
        return;
    }
    throw std::logic_error("unknown object kind");
}

void require_object_reference_value(Value value, const char* context) {
    if (!value.is_object()) {
        throw std::logic_error(std::string(context) + " must be an object reference");
    }
}

template <typename Fn>
void visit_reference_fields(Object& object, Fn&& fn) {
    validate_descriptor_shape(object);
    switch (object.kind) {
    case ObjectKind::Pair:
        fn(object.left);
        fn(object.right);
        return;
    case ObjectKind::ScalarArray:
        return;
    case ObjectKind::RefArray:
        for (auto& element : object.ref_elements) {
            fn(element);
        }
        return;
    case ObjectKind::Str:
        return;
    case ObjectKind::Closure:
        for (std::size_t i = 0; i < object.closure_captures.size(); ++i) {
            if (object.closure_capture_map[i]) {
                fn(object.closure_captures[i]);
            }
        }
        return;
    }
    throw std::logic_error("unknown object kind");
}

template <typename Fn>
void visit_reference_fields(const Object& object, Fn&& fn) {
    validate_descriptor_shape(object);
    switch (object.kind) {
    case ObjectKind::Pair:
        fn(object.left);
        fn(object.right);
        return;
    case ObjectKind::ScalarArray:
        return;
    case ObjectKind::RefArray:
        for (const auto& element : object.ref_elements) {
            fn(element);
        }
        return;
    case ObjectKind::Str:
        return;
    case ObjectKind::Closure:
        for (std::size_t i = 0; i < object.closure_captures.size(); ++i) {
            if (object.closure_capture_map[i]) {
                fn(object.closure_captures[i]);
            }
        }
        return;
    }
    throw std::logic_error("unknown object kind");
}

template <typename Fn>
void visit_reference_fields_for_lifo_marking(const Object& object, Fn&& fn) {
    validate_descriptor_shape(object);
    switch (object.kind) {
    case ObjectKind::Pair:
        // The mark worklist is LIFO, so right then left preserves the pre-existing
        // left-before-right trace order while keeping the scan descriptor-owned.
        fn(object.right);
        fn(object.left);
        return;
    case ObjectKind::ScalarArray:
        return;
    case ObjectKind::RefArray:
        // Push in reverse because the mark worklist is LIFO; this preserves
        // ascending element trace order while keeping scan shape descriptor-owned.
        for (std::size_t i = object.ref_elements.size(); i > 0; --i) {
            fn(object.ref_elements[i - 1]);
        }
        return;
    case ObjectKind::Str:
        return;
    case ObjectKind::Closure:
        for (std::size_t i = object.closure_captures.size(); i > 0; --i) {
            if (object.closure_capture_map[i - 1]) {
                fn(object.closure_captures[i - 1]);
            }
        }
        return;
    }
    throw std::logic_error("unknown object kind");
}

} // namespace

Object Object::pair(Value left_value, Value right_value) {
    Object object;
    object.kind = ObjectKind::Pair;
    object.length = 2;
    object.left = left_value;
    object.right = right_value;
    return object;
}

Object Object::scalar_array(std::size_t length, std::int64_t init) {
    if (length > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("scalar array length exceeds object header limit");
    }
    Object object;
    object.kind = ObjectKind::ScalarArray;
    object.length = static_cast<std::uint32_t>(length);
    object.left = Value::nil();
    object.right = Value::nil();
    object.scalar_elements.assign(length, init);
    return object;
}

Object Object::ref_array(std::size_t length, Value init) {
    if (length > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("ref array length exceeds object header limit");
    }
    require_object_reference_value(init, "ref array init value");
    Object object;
    object.kind = ObjectKind::RefArray;
    object.length = static_cast<std::uint32_t>(length);
    object.left = Value::nil();
    object.right = Value::nil();
    object.ref_elements.assign(length, init);
    return object;
}

Object Object::string(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("string length exceeds object header limit");
    }
    Object object;
    object.kind = ObjectKind::Str;
    object.length = static_cast<std::uint32_t>(bytes.size());
    object.left = Value::nil();
    object.right = Value::nil();
    object.string_bytes.assign(bytes.begin(), bytes.end());
    return object;
}

Object Object::closure(std::size_t layout_index, std::size_t function_index,
                       std::vector<Value> captures,
                       std::vector<bool> capture_map) {
    if (captures.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("closure capture count exceeds object header limit");
    }
    if (layout_index > std::numeric_limits<std::uint32_t>::max() ||
        function_index > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("closure metadata index exceeds object header limit");
    }
    Object object;
    object.kind = ObjectKind::Closure;
    object.length = static_cast<std::uint32_t>(captures.size());
    object.left = Value::nil();
    object.right = Value::nil();
    object.closure_layout_index = static_cast<std::uint32_t>(layout_index);
    object.closure_function_index = static_cast<std::uint32_t>(function_index);
    object.closure_captures = std::move(captures);
    object.closure_capture_map = std::move(capture_map);
    validate_descriptor_shape(object);
    return object;
}

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

    auto id = allocate_object(Object::pair(left, right));

    if (stress_config_.collect_after_every_allocation) {
        Value allocated = Value::object(id);
        std::array<Value*, 1> allocation_root{&allocated};
        collect_with_extra_roots(allocation_root);
        id = allocated.as_object();
    }

    return id;
}

ObjectId Heap::allocate_scalar_array(std::size_t length, std::int64_t init) {
    if (stress_config_.collect_before_every_allocation) {
        collect_with_extra_roots({});
    }

    auto id = allocate_object(Object::scalar_array(length, init));

    if (stress_config_.collect_after_every_allocation) {
        Value allocated = Value::object(id);
        std::array<Value*, 1> allocation_root{&allocated};
        collect_with_extra_roots(allocation_root);
        id = allocated.as_object();
    }

    return id;
}

ObjectId Heap::allocate_ref_array(std::size_t length, Value init) {
    require_object_reference_value(init, "ref array init value");
    if (stress_config_.collect_before_every_allocation) {
        std::array<Value*, 1> operand_roots{&init};
        collect_with_extra_roots(operand_roots);
    }

    auto id = allocate_object(Object::ref_array(length, init));

    if (stress_config_.collect_after_every_allocation) {
        Value allocated = Value::object(id);
        std::array<Value*, 1> allocation_root{&allocated};
        collect_with_extra_roots(allocation_root);
        id = allocated.as_object();
    }

    return id;
}

ObjectId Heap::allocate_string(std::span<const std::uint8_t> bytes) {
    if (stress_config_.collect_before_every_allocation) {
        collect_with_extra_roots({});
    }

    auto id = allocate_object(Object::string(bytes));

    if (stress_config_.collect_after_every_allocation) {
        Value allocated = Value::object(id);
        std::array<Value*, 1> allocation_root{&allocated};
        collect_with_extra_roots(allocation_root);
        id = allocated.as_object();
    }

    return id;
}

ObjectId Heap::allocate_string_concat(Value left, Value right) {
    require_object_reference_value(left, "string concat left operand");
    require_object_reference_value(right, "string concat right operand");
    (void)checked_string(left.as_object());
    (void)checked_string(right.as_object());

    if (stress_config_.collect_before_every_allocation) {
        std::array<Value*, 2> operand_roots{&left, &right};
        collect_with_extra_roots(operand_roots);
    }

    const auto left_bytes = string_bytes(left.as_object());
    const auto right_bytes = string_bytes(right.as_object());
    if (right_bytes.size() >
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) -
            left_bytes.size()) {
        throw std::length_error("string concatenation exceeds object header limit");
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(left_bytes.size() + right_bytes.size());
    bytes.insert(bytes.end(), left_bytes.begin(), left_bytes.end());
    bytes.insert(bytes.end(), right_bytes.begin(), right_bytes.end());

    auto id = allocate_object(Object::string(bytes));
    if (stress_config_.collect_after_every_allocation) {
        Value allocated = Value::object(id);
        std::array<Value*, 1> allocation_root{&allocated};
        collect_with_extra_roots(allocation_root);
        id = allocated.as_object();
    }
    return id;
}

ObjectId Heap::allocate_closure(std::size_t layout_index,
                                std::size_t function_index,
                                std::vector<Value> captures,
                                std::vector<bool> capture_map) {
    if (captures.size() != capture_map.size()) {
        throw std::logic_error(
            "closure capture-map length does not match capture payload");
    }
    (void)Object::closure(layout_index, function_index, captures, capture_map);

    if (stress_config_.collect_before_every_allocation) {
        std::vector<Value*> operand_roots;
        operand_roots.reserve(captures.size());
        for (auto& capture : captures) {
            operand_roots.push_back(&capture);
        }
        collect_with_extra_roots(operand_roots);
    }

    auto id = allocate_object(Object::closure(
        layout_index, function_index, std::move(captures), std::move(capture_map)));
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

ObjectId Heap::allocate_object(Object object) {
    validate_descriptor_shape(object);
    const auto required_slots = storage_slot_count(object);
    auto base = find_free_storage_run(required_slots);
    if (!base.has_value()) {
        const auto old_size = objects_.size();
        if (required_slots >
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) -
                old_size + 1) {
            throw std::length_error("heap object slot limit exceeded");
        }
        objects_.resize(old_size + required_slots);
        generations_.resize(old_size + required_slots, 0);
        base = old_size;
    }

    assert(base.has_value());
    assert(*base < objects_.size());
    const auto base_slot = *base;
    for (std::size_t offset = 0; offset < required_slots; ++offset) {
        assert(is_storage_slot_free(base_slot + offset) &&
               "allocator selected a storage run overlapping a live object");
    }

    generations_[base_slot] = generation_for_new_base(generations_[base_slot]);
    object.marked = false;
    object.generation = ObjectGeneration::Young;
    objects_[base_slot] = std::move(object);
    ++metrics_.allocations;
    if (objects_.size() > metrics_.heap_peak_slots) {
        metrics_.heap_peak_slots = objects_.size();
    }
    return make_object_id(static_cast<std::uint32_t>(base_slot), generations_[base_slot]);
}

std::optional<std::size_t> Heap::find_free_storage_run(std::size_t required_slots) const {
    assert(required_slots > 0 && "heap objects must reserve at least one storage slot");
    for (std::size_t base = 0; base + required_slots <= objects_.size(); ++base) {
        bool free = true;
        for (std::size_t offset = 0; offset < required_slots; ++offset) {
            if (!is_storage_slot_free(base + offset)) {
                free = false;
                break;
            }
        }
        if (free) {
            return base;
        }
    }
    return std::nullopt;
}

bool Heap::is_storage_slot_free(std::size_t slot) const {
    if (slot >= objects_.size()) {
        return false;
    }
    if (objects_[slot].has_value()) {
        return false;
    }
    for (std::size_t base = 0; base < objects_.size(); ++base) {
        if (!objects_[base].has_value()) {
            continue;
        }
        const auto width = storage_slot_count(*objects_[base]);
        if (base < slot && slot < base + width) {
            return false;
        }
    }
    return true;
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

const Object& Heap::checked_pair(ObjectId id) const {
    const auto& object = *objects_[checked_slot(id)];
    if (object.kind != ObjectKind::Pair) {
        throw std::logic_error("object is not a pair");
    }
    validate_descriptor_shape(object);
    return object;
}

Object& Heap::checked_pair(ObjectId id) {
    auto& object = *objects_[checked_slot(id)];
    if (object.kind != ObjectKind::Pair) {
        throw std::logic_error("object is not a pair");
    }
    validate_descriptor_shape(object);
    return object;
}

const Object& Heap::checked_scalar_array(ObjectId id) const {
    const auto& object = *objects_[checked_slot(id)];
    if (object.kind != ObjectKind::ScalarArray) {
        throw std::logic_error("object is not a scalar array");
    }
    validate_descriptor_shape(object);
    return object;
}

Object& Heap::checked_scalar_array(ObjectId id) {
    auto& object = *objects_[checked_slot(id)];
    if (object.kind != ObjectKind::ScalarArray) {
        throw std::logic_error("object is not a scalar array");
    }
    validate_descriptor_shape(object);
    return object;
}

const Object& Heap::checked_ref_array(ObjectId id) const {
    const auto& object = *objects_[checked_slot(id)];
    if (object.kind != ObjectKind::RefArray) {
        throw std::logic_error("object is not a ref array");
    }
    validate_descriptor_shape(object);
    return object;
}

Object& Heap::checked_ref_array(ObjectId id) {
    auto& object = *objects_[checked_slot(id)];
    if (object.kind != ObjectKind::RefArray) {
        throw std::logic_error("object is not a ref array");
    }
    validate_descriptor_shape(object);
    return object;
}

const Object& Heap::checked_string(ObjectId id) const {
    const auto& object = *objects_[checked_slot(id)];
    if (object.kind != ObjectKind::Str) {
        throw std::logic_error("object is not a string");
    }
    validate_descriptor_shape(object);
    return object;
}

const Object& Heap::checked_closure(ObjectId id) const {
    const auto& object = *objects_[checked_slot(id)];
    if (object.kind != ObjectKind::Closure) {
        throw std::logic_error("object is not a closure");
    }
    validate_descriptor_shape(object);
    return object;
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
    return checked_pair(id).left;
}

Value Heap::right(ObjectId id) const {
    return checked_pair(id).right;
}

void Heap::set_left(ObjectId id, Value value) {
    store_pair_field(id, PairField::Left, value);
}

void Heap::set_right(ObjectId id, Value value) {
    store_pair_field(id, PairField::Right, value);
}

std::size_t Heap::array_length(ObjectId id) const {
    return checked_scalar_array(id).length;
}

std::int64_t Heap::array_get(ObjectId id, std::size_t index) const {
    const auto& object = checked_scalar_array(id);
    if (index >= object.scalar_elements.size()) {
        throw std::out_of_range("scalar array index out of bounds");
    }
    return object.scalar_elements[index];
}

void Heap::array_set(ObjectId id, std::size_t index, std::int64_t value) {
    auto& object = checked_scalar_array(id);
    if (index >= object.scalar_elements.size()) {
        throw std::out_of_range("scalar array index out of bounds");
    }
    object.scalar_elements[index] = value;
}

std::size_t Heap::ref_array_length(ObjectId id) const {
    return checked_ref_array(id).length;
}

Value Heap::ref_array_get(ObjectId id, std::size_t index) const {
    const auto& object = checked_ref_array(id);
    if (index >= object.ref_elements.size()) {
        throw std::out_of_range("ref array index out of bounds");
    }
    return object.ref_elements[index];
}

void Heap::ref_array_set(ObjectId id, std::size_t index, Value value) {
    store_ref_array_element(id, index, value);
}

std::size_t Heap::string_length(ObjectId id) const {
    return checked_string(id).length;
}

std::span<const std::uint8_t> Heap::string_bytes(ObjectId id) const {
    return checked_string(id).string_bytes;
}

bool Heap::string_equal(ObjectId left, ObjectId right) const {
    const auto left_bytes = string_bytes(left);
    const auto right_bytes = string_bytes(right);
    return left_bytes.size() == right_bytes.size() &&
           std::equal(left_bytes.begin(), left_bytes.end(), right_bytes.begin());
}

std::uint8_t Heap::string_index(ObjectId id, std::size_t index) const {
    const auto bytes = string_bytes(id);
    if (index >= bytes.size()) {
        throw std::out_of_range("string index out of bounds");
    }
    return bytes[index];
}

std::size_t Heap::closure_layout_index(ObjectId id) const {
    return checked_closure(id).closure_layout_index;
}

std::size_t Heap::closure_function_index(ObjectId id) const {
    return checked_closure(id).closure_function_index;
}

std::size_t Heap::closure_capture_count(ObjectId id) const {
    return checked_closure(id).closure_captures.size();
}

Value Heap::closure_capture(ObjectId id, std::size_t index) const {
    const auto& object = checked_closure(id);
    if (index >= object.closure_captures.size()) {
        throw std::out_of_range("closure capture index out of bounds");
    }
    return object.closure_captures[index];
}

void Heap::store_pair_field(ObjectId id, PairField field, Value value) {
    // Barrier hook: every pair field mutation must flow through this method. The public
    // heap API intentionally exposes only const object inspection plus set_left/set_right,
    // so bytecode cannot publish a field without running this old-to-young barrier.
    auto& obj = checked_pair(id);
    const bool barrier_triggered = record_write_barrier_if_needed(id, value);

    if (field == PairField::Left) {
        obj.left = value;
    } else {
        obj.right = value;
    }

    if (barrier_triggered && stress_config_.collect_minor_after_every_write_barrier) {
        collect_minor();
    }
}

void Heap::store_ref_array_element(ObjectId id, std::size_t index, Value value) {
    require_object_reference_value(value, "ref array stored value");
    auto& obj = checked_ref_array(id);
    if (index >= obj.ref_elements.size()) {
        throw std::out_of_range("ref array index out of bounds");
    }

    const bool barrier_triggered = record_write_barrier_if_needed(id, value);
    obj.ref_elements[index] = value;

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
        throw std::logic_error("remembered-set insertion attempted for a non-old object");
    }
    if (!remembered_set_contains(id)) {
        remembered_set_.push_back(id);
    }
    if (remembered_set_.size() > metrics_.remembered_set_peak) {
        metrics_.remembered_set_peak = remembered_set_.size();
    }
}

void Heap::record_promoted_object_edges(
    std::span<const std::size_t> promoted_slots) {
    for (const auto slot : promoted_slots) {
        if (slot >= objects_.size() || !objects_[slot].has_value() ||
            !is_old_slot(slot)) {
            throw std::logic_error(
                "collector promotion edge scan did not name an installed old object");
        }
        const auto owner = make_object_id(static_cast<std::uint32_t>(slot),
                                          generations_[slot]);
        bool has_young_reference = false;
        visit_reference_fields(*objects_[slot], [&](Value value) {
            if (!value.is_object()) {
                return;
            }
            const auto target_slot = checked_slot(value.as_object());
            has_young_reference = has_young_reference || is_young_slot(target_slot);
        });
        if (has_young_reference) {
            record_remembered_object(owner);
            assert(remembered_set_contains(owner) &&
                   "collector promotion failed to record an old-to-young edge");
        }
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
        visit_reference_fields_for_lifo_marking(*objects_[slot], [&](Value value) {
            enqueue_mark_value(value, worklist, CollectionKind::Minor);
        });
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

        visit_reference_fields_for_lifo_marking(obj, [&](Value value) {
            enqueue_mark_value(value, worklist, kind);
        });
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
    validate_heap_storage_layout();
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
    record_promoted_object_edges(compacted.promoted_slots);
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
            result.promoted_slots.push_back(next_live_slot);
        }

        const auto required_slots = storage_slot_count(moved);
        if (next_live_slot + required_slots > result.objects.size()) {
            throw std::logic_error("compaction cursor exceeded heap storage capacity");
        }

        if (old_slot != next_live_slot) {
            result.generations[next_live_slot] =
                generation_for_new_base(result.generations[next_live_slot]);
            ++result.objects_moved;
        }

        const auto new_id =
            make_object_id(static_cast<std::uint32_t>(next_live_slot),
                           result.generations[next_live_slot]);
        result.forwarding[old_slot] = new_id;
        result.objects[next_live_slot] = moved;
        next_live_slot += required_slots;
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
        visit_reference_fields(*slot, [&](Value& field) {
            rewrite_value(field, forwarding);
        });
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

        bool has_young_reference = false;
        visit_reference_fields(*objects_[slot], [&](Value value) {
            if (!value.is_object()) {
                return;
            }
            const auto target_slot = checked_slot(value.as_object());
            has_young_reference = has_young_reference || is_young_slot(target_slot);
        });
        if (has_young_reference) {
            pruned.push_back(make_object_id(static_cast<std::uint32_t>(slot), generations_[slot]));
        }
    }
    remembered_set_ = std::move(pruned);
}

void Heap::validate_heap_storage_layout() const {
    std::vector<bool> covered(objects_.size(), false);
    for (std::size_t base = 0; base < objects_.size(); ++base) {
        if (!objects_[base].has_value()) {
            continue;
        }
        if (covered[base]) {
            throw std::logic_error("heap object header overlaps another object's storage run");
        }
        validate_descriptor_shape(*objects_[base]);
        const auto width = storage_slot_count(*objects_[base]);
        if (width == 0 || base + width > objects_.size()) {
            throw std::logic_error("heap object descriptor extends past heap storage");
        }
        for (std::size_t offset = 0; offset < width; ++offset) {
            const auto slot = base + offset;
            if (covered[slot]) {
                throw std::logic_error("heap object storage runs overlap");
            }
            if (offset != 0 && objects_[slot].has_value()) {
                throw std::logic_error("heap object payload slot contains an object header");
            }
            covered[slot] = true;
        }
    }
}

void Heap::validate_after_collection(RootProvider* roots, std::span<Value*> extra_roots) const {
    ++TEST_ONLY_validation_count_;

    validate_heap_storage_layout();

    ValidatingVisitor visitor(*this);
    trace_collection_roots(visitor, roots, extra_roots);

    for (const auto& slot : objects_) {
        if (!slot.has_value()) {
            continue;
        }
        assert(!slot->marked && "collector invariant violated: mark bit survived collection");
        validate_descriptor_shape(*slot);
        visit_reference_fields(*slot, [&](Value value) {
            validate_value(value);
        });
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
        visit_reference_fields(*objects_[slot], [&](Value value) {
            if (!value.is_object()) {
                return;
            }
            const auto target_slot = checked_slot(value.as_object());
            if (is_young_slot(target_slot) && !remembered_set_contains(owner)) {
                throw std::logic_error(
                    "old-to-young reference missing remembered-set entry");
            }
        });
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

bool Heap::TEST_ONLY_is_scalar_array(ObjectId id) const {
    return object(id).kind == ObjectKind::ScalarArray;
}

bool Heap::TEST_ONLY_is_ref_array(ObjectId id) const {
    return object(id).kind == ObjectKind::RefArray;
}

bool Heap::TEST_ONLY_is_string(ObjectId id) const {
    return object(id).kind == ObjectKind::Str;
}

bool Heap::TEST_ONLY_is_closure(ObjectId id) const {
    return object(id).kind == ObjectKind::Closure;
}

void Heap::TEST_ONLY_skip_next_write_barrier_for_barrier_validator() {
    TEST_ONLY_skip_next_write_barrier_ = true;
}

void Heap::TEST_ONLY_promote_object_through_collector_path(ObjectId id) {
    const auto slot = checked_slot(id);
    if (!is_young_slot(slot)) {
        throw std::logic_error("test promotion requires a young object");
    }
    objects_[slot]->generation = ObjectGeneration::Old;
    const std::array<std::size_t, 1> promoted{slot};
    record_promoted_object_edges(promoted);
    validate_remembered_set();
}

void Heap::TEST_ONLY_validate_gc_invariants() const {
    validate_after_collection(nullptr, {});
}

} // namespace lang::gc
