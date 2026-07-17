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
    case ObjectKind::Map:
        return 1 + 2 * static_cast<std::size_t>(object.length);
    case ObjectKind::WeakRef:
        return 1;
    case ObjectKind::Record:
        return 1 + static_cast<std::size_t>(object.length);
    case ObjectKind::Variant:
        return 2 + static_cast<std::size_t>(object.length);
    case ObjectKind::Ephemeron:
        return 1;
    }
    throw std::logic_error("unknown object kind");
}

void validate_descriptor_shape(const Object& object, HeapMetrics* metrics = nullptr) {
    if (object.kind != ObjectKind::Ephemeron &&
        (object.ephemeron_key().tag() != Value::Tag::Nil ||
         object.ephemeron_value().tag() != Value::Tag::Nil)) {
        throw std::logic_error("non-ephemeron descriptor contains ephemeron payload");
    }
    if (object.kind != ObjectKind::Record &&
        (!object.record_fields.empty() || !object.record_ref_map.empty())) {
        throw std::logic_error(
            "non-record descriptor contains collector-visible record payload");
    }
    if (object.kind != ObjectKind::Variant &&
        (!object.variant_fields.empty() ||
         !object.variant_case_ref_maps.empty())) {
        throw std::logic_error(
            "non-variant descriptor contains collector-visible variant payload");
    }
    if (metrics != nullptr) {
        if (object.kind == ObjectKind::Closure) {
            metrics->closure_capture_slots_scanned += object.closure_captures.size();
        } else if (object.kind == ObjectKind::Map) {
            metrics->map_descriptor_entries_scanned += object.map_entries.size();
        }
    }
    switch (object.kind) {
    case ObjectKind::Pair:
        if (object.length != 2 || !object.scalar_elements.empty() ||
            !object.ref_elements.empty() || !object.string_bytes.empty() ||
            !object.closure_captures.empty() ||
            !object.closure_capture_map.empty() ||
            !object.map_entries.empty() ||
            object.weak_target().tag() != Value::Tag::Nil) {
            throw std::logic_error("pair object descriptor does not match pair payload");
        }
        return;
    case ObjectKind::ScalarArray:
        if (object.scalar_elements.size() != object.length ||
            !object.ref_elements.empty() || !object.string_bytes.empty() ||
            !object.closure_captures.empty() ||
            !object.closure_capture_map.empty() ||
            !object.map_entries.empty() ||
            object.weak_target().tag() != Value::Tag::Nil) {
            throw std::logic_error("scalar array descriptor length does not match payload");
        }
        return;
    case ObjectKind::RefArray:
        if (!object.scalar_elements.empty() ||
            object.ref_elements.size() != object.length ||
            !object.string_bytes.empty() || !object.closure_captures.empty() ||
            !object.closure_capture_map.empty() ||
            !object.map_entries.empty() ||
            object.weak_target().tag() != Value::Tag::Nil) {
            throw std::logic_error("ref array descriptor length does not match payload");
        }
        return;
    case ObjectKind::Str:
        if (!object.scalar_elements.empty() || !object.ref_elements.empty() ||
            object.string_bytes.size() != object.length ||
            object.left.tag() != Value::Tag::Nil ||
            object.right.tag() != Value::Tag::Nil ||
            !object.closure_captures.empty() ||
            !object.closure_capture_map.empty() ||
            !object.map_entries.empty() ||
            object.weak_target().tag() != Value::Tag::Nil) {
            throw std::logic_error("string descriptor length does not match opaque byte payload");
        }
        return;
    case ObjectKind::Closure:
        if (!object.scalar_elements.empty() || !object.ref_elements.empty() ||
            !object.string_bytes.empty() ||
            object.closure_captures.size() != object.length ||
            object.closure_capture_map.size() != object.length ||
            object.left.tag() != Value::Tag::Nil ||
            object.right.tag() != Value::Tag::Nil ||
            !object.map_entries.empty() ||
            object.weak_target().tag() != Value::Tag::Nil) {
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
    case ObjectKind::Map:
        if (!object.scalar_elements.empty() || !object.ref_elements.empty() ||
            !object.string_bytes.empty() || !object.closure_captures.empty() ||
            !object.closure_capture_map.empty() ||
            object.map_entries.size() != object.length ||
            object.left.tag() != Value::Tag::Nil ||
            object.right.tag() != Value::Tag::Nil ||
            object.weak_target().tag() != Value::Tag::Nil) {
            throw std::logic_error(
                "map descriptor entry count does not match ordered payload");
        }
        for (const auto& entry : object.map_entries) {
            const auto key_tag = entry.key.tag();
            if (object.map_key_is_ref) {
                if (key_tag != Value::Tag::Object) {
                    throw std::logic_error(
                        "map descriptor marks a scalar key slot as a reference");
                }
            } else if (key_tag != Value::Tag::Int64 &&
                       key_tag != Value::Tag::Bool) {
                throw std::logic_error(
                    "map descriptor marks a reference key slot as scalar");
            }

            const auto value_tag = entry.value.tag();
            if (object.map_value_is_ref) {
                if (value_tag != Value::Tag::Object &&
                    value_tag != Value::Tag::Nil) {
                    throw std::logic_error(
                        "map descriptor marks a scalar value slot as a reference");
                }
            } else if (value_tag != Value::Tag::Int64 &&
                       value_tag != Value::Tag::Bool) {
                throw std::logic_error(
                    "map descriptor marks a reference value slot as scalar");
            }
        }
        return;
    case ObjectKind::WeakRef:
        if (object.length != 1 || !object.scalar_elements.empty() ||
            !object.ref_elements.empty() || !object.string_bytes.empty() ||
            !object.closure_captures.empty() ||
            !object.closure_capture_map.empty() ||
            !object.map_entries.empty() ||
            object.left.tag() != Value::Tag::Nil ||
            object.right.tag() != Value::Tag::Nil ||
            (object.weak_target().tag() != Value::Tag::Object &&
             object.weak_target().tag() != Value::Tag::Nil)) {
            throw std::logic_error(
                "weak reference descriptor does not match collector-owned target slot");
        }
        return;
    case ObjectKind::Record:
        if (!object.scalar_elements.empty() || !object.ref_elements.empty() ||
            !object.string_bytes.empty() || !object.closure_captures.empty() ||
            !object.closure_capture_map.empty() || !object.map_entries.empty() ||
            object.record_fields.size() != object.length ||
            object.record_ref_map.size() != object.length ||
            object.left.tag() != Value::Tag::Nil ||
            object.right.tag() != Value::Tag::Nil ||
            object.weak_target().tag() != Value::Tag::Nil) {
            throw std::logic_error(
                "record descriptor length does not match layout payload");
        }
        for (std::size_t i = 0; i < object.record_fields.size(); ++i) {
            const auto tag = object.record_fields[i].tag();
            if (object.record_ref_map[i]) {
                if (tag != Value::Tag::Object && tag != Value::Tag::Nil) {
                    throw std::logic_error(
                        "record reference bitmap marks a scalar payload as a reference");
                }
            } else if (tag != Value::Tag::Int64 && tag != Value::Tag::Bool) {
                throw std::logic_error(
                    "record reference bitmap marks a reference payload as scalar");
            }
        }
        return;
    case ObjectKind::Variant: {
        if (static_cast<std::size_t>(object.variant_case_index) >=
            object.variant_case_ref_maps.size()) {
            throw std::logic_error("variant case tag out of range");
        }
        const auto& selected_ref_map =
            object.variant_case_ref_maps[object.variant_case_index];
        if (selected_ref_map.size() != object.variant_fields.size() ||
            object.variant_fields.size() != object.length) {
            throw std::logic_error(
                "variant payload width does not match selected case");
        }
        if (!object.scalar_elements.empty() || !object.ref_elements.empty() ||
            !object.string_bytes.empty() || !object.closure_captures.empty() ||
            !object.closure_capture_map.empty() || !object.map_entries.empty() ||
            object.left.tag() != Value::Tag::Nil ||
            object.right.tag() != Value::Tag::Nil ||
            object.weak_target().tag() != Value::Tag::Nil) {
            throw std::logic_error(
                "variant descriptor length does not match selected case payload");
        }
        for (std::size_t i = 0; i < object.variant_fields.size(); ++i) {
            const auto tag = object.variant_fields[i].tag();
            if (selected_ref_map[i]) {
                if (tag != Value::Tag::Object && tag != Value::Tag::Nil) {
                    throw std::logic_error(
                        "variant reference bitmap marks a scalar payload as a reference");
                }
            } else if (tag != Value::Tag::Int64 && tag != Value::Tag::Bool) {
                throw std::logic_error(
                    "variant reference bitmap marks a reference payload as scalar");
            }
        }
        return;
    }
    case ObjectKind::Ephemeron: {
        const auto key_tag = object.ephemeron_key().tag();
        const auto value_tag = object.ephemeron_value().tag();
        if (object.length != 2 || !object.scalar_elements.empty() ||
            !object.ref_elements.empty() || !object.string_bytes.empty() ||
            !object.closure_captures.empty() ||
            !object.closure_capture_map.empty() || !object.map_entries.empty() ||
            object.left.tag() != Value::Tag::Nil ||
            object.right.tag() != Value::Tag::Nil ||
            object.weak_target().tag() != Value::Tag::Nil ||
            (key_tag != Value::Tag::Object && key_tag != Value::Tag::Nil)) {
            throw std::logic_error("ephemeron descriptor does not match registry payload");
        }
        if (key_tag == Value::Tag::Nil && value_tag != Value::Tag::Nil) {
            throw std::logic_error("cleared ephemeron retains a value");
        }
        if (object.ephemeron_value_is_ref()) {
            if (value_tag != Value::Tag::Object && value_tag != Value::Tag::Nil) {
                throw std::logic_error("ephemeron reference value has scalar tag");
            }
        } else if (key_tag != Value::Tag::Nil && value_tag != Value::Tag::Int64 &&
                   value_tag != Value::Tag::Bool) {
            throw std::logic_error("ephemeron scalar value has reference tag");
        }
        return;
    }
    }
    throw std::logic_error("unknown object kind");
}

void require_object_reference_value(Value value, const char* context) {
    if (!value.is_object()) {
        throw std::logic_error(std::string(context) + " must be an object reference");
    }
}

template <typename Fn>
void visit_reference_fields(Object& object, Fn&& fn,
                            HeapMetrics* metrics = nullptr) {
    validate_descriptor_shape(object, metrics);
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
        if (metrics != nullptr) {
            metrics->closure_capture_slots_scanned += object.closure_captures.size();
        }
        for (std::size_t i = 0; i < object.closure_captures.size(); ++i) {
            if (object.closure_capture_map[i]) {
                fn(object.closure_captures[i]);
            }
        }
        return;
    case ObjectKind::Map:
        if (metrics != nullptr) {
            metrics->map_descriptor_entries_scanned += object.map_entries.size();
        }
        for (auto& entry : object.map_entries) {
            if (object.map_key_is_ref) {
                fn(entry.key);
            }
            if (object.map_value_is_ref) {
                fn(entry.value);
            }
        }
        return;
    case ObjectKind::WeakRef:
        return;
    case ObjectKind::Record:
        for (std::size_t i = 0; i < object.record_fields.size(); ++i) {
            if (object.record_ref_map[i]) {
                fn(object.record_fields[i]);
            }
        }
        return;
    case ObjectKind::Variant: {
        const auto& selected_ref_map =
            object.variant_case_ref_maps[object.variant_case_index];
        for (std::size_t i = 0; i < object.variant_fields.size(); ++i) {
            if (selected_ref_map[i]) {
                fn(object.variant_fields[i]);
            }
        }
        return;
    }
    case ObjectKind::Ephemeron:
        return;
    }
    throw std::logic_error("unknown object kind");
}

template <typename Fn>
void visit_reference_fields(const Object& object, Fn&& fn,
                            HeapMetrics* metrics = nullptr) {
    validate_descriptor_shape(object, metrics);
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
        if (metrics != nullptr) {
            metrics->closure_capture_slots_scanned += object.closure_captures.size();
        }
        for (std::size_t i = 0; i < object.closure_captures.size(); ++i) {
            if (object.closure_capture_map[i]) {
                fn(object.closure_captures[i]);
            }
        }
        return;
    case ObjectKind::Map:
        if (metrics != nullptr) {
            metrics->map_descriptor_entries_scanned += object.map_entries.size();
        }
        for (const auto& entry : object.map_entries) {
            if (object.map_key_is_ref) {
                fn(entry.key);
            }
            if (object.map_value_is_ref) {
                fn(entry.value);
            }
        }
        return;
    case ObjectKind::WeakRef:
        return;
    case ObjectKind::Record:
        for (std::size_t i = 0; i < object.record_fields.size(); ++i) {
            if (object.record_ref_map[i]) {
                fn(object.record_fields[i]);
            }
        }
        return;
    case ObjectKind::Variant: {
        const auto& selected_ref_map =
            object.variant_case_ref_maps[object.variant_case_index];
        for (std::size_t i = 0; i < object.variant_fields.size(); ++i) {
            if (selected_ref_map[i]) {
                fn(object.variant_fields[i]);
            }
        }
        return;
    }
    case ObjectKind::Ephemeron:
        return;
    }
    throw std::logic_error("unknown object kind");
}

template <typename Fn>
void visit_reference_fields_for_lifo_marking(const Object& object, Fn&& fn,
                                             HeapMetrics* metrics = nullptr) {
    validate_descriptor_shape(object, metrics);
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
        if (metrics != nullptr) {
            metrics->closure_capture_slots_scanned += object.closure_captures.size();
        }
        for (std::size_t i = object.closure_captures.size(); i > 0; --i) {
            if (object.closure_capture_map[i - 1]) {
                fn(object.closure_captures[i - 1]);
            }
        }
        return;
    case ObjectKind::Map:
        if (metrics != nullptr) {
            metrics->map_descriptor_entries_scanned += object.map_entries.size();
        }
        for (std::size_t i = object.map_entries.size(); i > 0; --i) {
            const auto& entry = object.map_entries[i - 1];
            if (object.map_value_is_ref) {
                fn(entry.value);
            }
            if (object.map_key_is_ref) {
                fn(entry.key);
            }
        }
        return;
    case ObjectKind::WeakRef:
        return;
    case ObjectKind::Record:
        for (std::size_t i = object.record_fields.size(); i > 0; --i) {
            if (object.record_ref_map[i - 1]) {
                fn(object.record_fields[i - 1]);
            }
        }
        return;
    case ObjectKind::Variant: {
        const auto& selected_ref_map =
            object.variant_case_ref_maps[object.variant_case_index];
        for (std::size_t i = object.variant_fields.size(); i > 0; --i) {
            if (selected_ref_map[i - 1]) {
                fn(object.variant_fields[i - 1]);
            }
        }
        return;
    }
    case ObjectKind::Ephemeron:
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

Object Object::map(std::size_t layout_index, bool key_is_ref,
                   bool value_is_ref) {
    if (layout_index > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("map layout index exceeds object header limit");
    }
    Object object;
    object.kind = ObjectKind::Map;
    object.length = 0;
    object.left = Value::nil();
    object.right = Value::nil();
    object.map_layout_index = static_cast<std::uint32_t>(layout_index);
    object.map_key_is_ref = key_is_ref;
    object.map_value_is_ref = value_is_ref;
    validate_descriptor_shape(object);
    return object;
}

Object Object::weak_ref(Value target) {
    require_object_reference_value(target, "weak reference target");
    Object object;
    object.kind = ObjectKind::WeakRef;
    object.length = 1;
    object.left = Value::nil();
    object.right = Value::nil();
    object.weak_target_ = target;
    validate_descriptor_shape(object);
    return object;
}

Object Object::ephemeron(Value key, Value value, bool value_is_ref) {
    require_object_reference_value(key, "ephemeron key");
    if (value_is_ref) {
        if (value.tag() != Value::Tag::Object && value.tag() != Value::Tag::Nil) {
            throw std::logic_error("ephemeron reference value must be object or nil");
        }
    } else if (value.tag() != Value::Tag::Int64 && value.tag() != Value::Tag::Bool) {
        throw std::logic_error("ephemeron scalar value must be i64 or bool");
    }
    Object object;
    object.kind = ObjectKind::Ephemeron;
    object.length = 2;
    object.ephemeron_key_ = key;
    object.ephemeron_value_ = value;
    object.ephemeron_value_is_ref_ = value_is_ref;
    validate_descriptor_shape(object);
    return object;
}

Object Object::record(std::size_t layout_index, std::vector<Value> fields,
                      std::vector<bool> ref_map) {
    if (fields.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("record field count exceeds object header limit");
    }
    if (layout_index > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("record layout index exceeds object header limit");
    }
    Object object;
    object.kind = ObjectKind::Record;
    object.length = static_cast<std::uint32_t>(fields.size());
    object.left = Value::nil();
    object.right = Value::nil();
    object.record_layout_index = static_cast<std::uint32_t>(layout_index);
    object.record_fields = std::move(fields);
    object.record_ref_map = std::move(ref_map);
    validate_descriptor_shape(object);
    return object;
}

Object Object::variant(
    std::size_t layout_index, std::size_t case_index,
    std::vector<Value> fields,
    std::vector<std::vector<bool>> case_ref_maps) {
    if (case_index >= case_ref_maps.size()) {
        throw std::logic_error("variant case tag out of range");
    }
    if (case_ref_maps[case_index].size() != fields.size()) {
        throw std::logic_error(
            "variant payload width does not match selected case");
    }
    if (fields.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("variant field count exceeds object header limit");
    }
    if (layout_index > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("variant layout index exceeds object header limit");
    }
    if (case_index > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("variant case tag exceeds object header limit");
    }

    Object object;
    object.kind = ObjectKind::Variant;
    object.length = static_cast<std::uint32_t>(fields.size());
    object.left = Value::nil();
    object.right = Value::nil();
    object.variant_layout_index = static_cast<std::uint32_t>(layout_index);
    object.variant_case_index = static_cast<std::uint32_t>(case_index);
    object.variant_fields = std::move(fields);
    object.variant_case_ref_maps = std::move(case_ref_maps);
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

ObjectId Heap::allocate_string_substring(Value source, std::size_t lo,
                                         std::size_t hi) {
    require_object_reference_value(source, "string substring source operand");
    const auto initial_length = checked_string(source.as_object()).string_bytes.size();
    assert(lo <= hi && hi <= initial_length &&
           "StrSub VM boundary checks must precede allocation");

    if (stress_config_.collect_before_every_allocation) {
        std::array<Value*, 1> operand_roots{&source};
        collect_with_extra_roots(operand_roots);
    }

    const auto source_bytes = string_bytes(source.as_object());
    assert(hi <= source_bytes.size() &&
           "rooted StrSub source length changed across collection");
    auto id = allocate_object(Object::string(source_bytes.subspan(lo, hi - lo)));
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

ObjectId Heap::allocate_map(std::size_t layout_index, bool key_is_ref,
                            bool value_is_ref) {
    if (stress_config_.collect_before_every_allocation) {
        collect_with_extra_roots({});
    }

    auto id = allocate_object(Object::map(layout_index, key_is_ref,
                                          value_is_ref));
    if (stress_config_.collect_after_every_allocation) {
        Value allocated = Value::object(id);
        std::array<Value*, 1> allocation_root{&allocated};
        collect_with_extra_roots(allocation_root);
        id = allocated.as_object();
    }
    return id;
}

ObjectId Heap::allocate_weak(Value target) {
    require_object_reference_value(target, "weak reference target");
    (void)checked_slot(target.as_object());
    if (stress_config_.collect_before_every_allocation) {
        std::array<Value*, 1> operand_roots{&target};
        collect_with_extra_roots(operand_roots);
    }

    auto id = allocate_object(Object::weak_ref(target));
    if (stress_config_.collect_after_every_allocation) {
        Value allocated = Value::object(id);
        std::array<Value*, 1> allocation_root{&allocated};
        collect_with_extra_roots(allocation_root);
        id = allocated.as_object();
    }
    return id;
}

ObjectId Heap::allocate_ephemeron(Value key, Value value, bool value_is_ref) {
    (void)checked_slot(key.as_object());
    if (value_is_ref && value.is_object()) (void)checked_slot(value.as_object());
    if (stress_config_.collect_before_every_allocation) {
        std::array<Value*, 2> roots{&key, &value};
        collect_with_extra_roots(roots);
    }
    auto id = allocate_object(Object::ephemeron(key, value, value_is_ref));
    if (stress_config_.collect_after_every_allocation) {
        Value allocated = Value::object(id);
        std::array<Value*, 1> root{&allocated};
        collect_with_extra_roots(root);
        id = allocated.as_object();
    }
    return id;
}

ObjectId Heap::allocate_record(std::size_t layout_index,
                               std::vector<Value> fields,
                               std::vector<bool> ref_map) {
    if (fields.size() != ref_map.size()) {
        throw std::logic_error(
            "record reference-bitmap length does not match field payload");
    }
    (void)Object::record(layout_index, fields, ref_map);

    if (stress_config_.collect_before_every_allocation) {
        std::vector<Value*> operand_roots;
        operand_roots.reserve(fields.size());
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (ref_map[i]) {
                operand_roots.push_back(&fields[i]);
            }
        }
        collect_with_extra_roots(operand_roots);
    }

    auto id = allocate_object(Object::record(
        layout_index, std::move(fields), std::move(ref_map)));
    if (stress_config_.collect_after_every_allocation) {
        Value allocated = Value::object(id);
        std::array<Value*, 1> allocation_root{&allocated};
        collect_with_extra_roots(allocation_root);
        id = allocated.as_object();
    }
    return id;
}

ObjectId Heap::allocate_variant(
    std::size_t layout_index, std::size_t case_index,
    std::vector<Value> fields,
    std::vector<std::vector<bool>> case_ref_maps) {
    (void)Object::variant(layout_index, case_index, fields, case_ref_maps);

    if (stress_config_.collect_before_every_allocation) {
        std::vector<Value*> initializer_roots;
        initializer_roots.reserve(fields.size());
        for (auto& field : fields) {
            initializer_roots.push_back(&field);
        }
        collect_with_extra_roots(initializer_roots);
    }

    auto id = allocate_object(Object::variant(
        layout_index, case_index, std::move(fields),
        std::move(case_ref_maps)));
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
    validate_descriptor_shape(object, &metrics_);
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
    const auto id = make_object_id(static_cast<std::uint32_t>(base_slot),
                                   generations_[base_slot]);
    if (objects_[base_slot]->kind == ObjectKind::WeakRef) {
        const auto position = std::lower_bound(
            weak_refs_.begin(), weak_refs_.end(), base_slot,
            [](ObjectId existing, std::size_t slot) {
                return slot_from(existing) < slot;
            });
        weak_refs_.insert(position, id);
    }
    if (objects_[base_slot]->kind == ObjectKind::Ephemeron) {
        const auto position = std::lower_bound(
            ephemerons_.begin(), ephemerons_.end(), base_slot,
            [](ObjectId existing, std::size_t slot) {
                return slot_from(existing) < slot;
            });
        ephemerons_.insert(position, id);
    }
    if (incremental_marking_active_) {
        incremental_mark_worklist_.push_back(id);
    }
    ++metrics_.allocations;
    if (objects_.size() > metrics_.heap_peak_slots) {
        metrics_.heap_peak_slots = objects_.size();
    }
    return id;
}

std::optional<std::size_t> Heap::find_free_storage_run(std::size_t required_slots) const {
    assert(required_slots > 0 && "heap objects must reserve at least one storage slot");

    // Walk the same occupied intervals once. Descriptor widths let the search skip
    // payload slots without repeatedly asking is_storage_slot_free to rescan every
    // object header. The post-selection overlap assertions in allocate_object still
    // call is_storage_slot_free independently.
    std::size_t free_run_start = 0;
    std::size_t free_run_length = 0;
    for (std::size_t base = 0; base < objects_.size();) {
        ++metrics_.allocation_candidate_slots_examined;
        ++metrics_.storage_occupancy_headers_examined;
        if (objects_[base].has_value()) {
            free_run_length = 0;
            base += storage_slot_count(*objects_[base]);
            continue;
        }

        ++metrics_.allocation_storage_slots_checked;
        if (free_run_length == 0) {
            free_run_start = base;
        }
        ++free_run_length;
        if (free_run_length == required_slots) {
            return free_run_start;
        }
        ++base;
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
        ++metrics_.storage_occupancy_headers_examined;
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
    validate_descriptor_shape(object, &metrics_);
    return object;
}

Object& Heap::checked_pair(ObjectId id) {
    auto& object = *objects_[checked_slot(id)];
    if (object.kind != ObjectKind::Pair) {
        throw std::logic_error("object is not a pair");
    }
    validate_descriptor_shape(object, &metrics_);
    return object;
}

const Object& Heap::checked_scalar_array(ObjectId id) const {
    const auto& object = *objects_[checked_slot(id)];
    if (object.kind != ObjectKind::ScalarArray) {
        throw std::logic_error("object is not a scalar array");
    }
    validate_descriptor_shape(object, &metrics_);
    return object;
}

Object& Heap::checked_scalar_array(ObjectId id) {
    auto& object = *objects_[checked_slot(id)];
    if (object.kind != ObjectKind::ScalarArray) {
        throw std::logic_error("object is not a scalar array");
    }
    validate_descriptor_shape(object, &metrics_);
    return object;
}

const Object& Heap::checked_ref_array(ObjectId id) const {
    const auto& object = *objects_[checked_slot(id)];
    if (object.kind != ObjectKind::RefArray) {
        throw std::logic_error("object is not a ref array");
    }
    validate_descriptor_shape(object, &metrics_);
    return object;
}

Object& Heap::checked_ref_array(ObjectId id) {
    auto& object = *objects_[checked_slot(id)];
    if (object.kind != ObjectKind::RefArray) {
        throw std::logic_error("object is not a ref array");
    }
    validate_descriptor_shape(object, &metrics_);
    return object;
}

const Object& Heap::checked_string(ObjectId id) const {
    const auto& object = *objects_[checked_slot(id)];
    if (object.kind != ObjectKind::Str) {
        throw std::logic_error("object is not a string");
    }
    validate_descriptor_shape(object, &metrics_);
    return object;
}

const Object& Heap::checked_closure(ObjectId id) const {
    const auto& object = *objects_[checked_slot(id)];
    if (object.kind != ObjectKind::Closure) {
        throw std::logic_error("object is not a closure");
    }
    validate_descriptor_shape(object, &metrics_);
    return object;
}

const Object& Heap::checked_map(ObjectId id) const {
    const auto& object = *objects_[checked_slot(id)];
    if (object.kind != ObjectKind::Map) {
        throw std::logic_error("object is not a map");
    }
    validate_descriptor_shape(object, &metrics_);
    return object;
}

Object& Heap::checked_map(ObjectId id) {
    auto& object = *objects_[checked_slot(id)];
    if (object.kind != ObjectKind::Map) {
        throw std::logic_error("object is not a map");
    }
    validate_descriptor_shape(object, &metrics_);
    return object;
}

const Object& Heap::checked_weak_ref(ObjectId id) const {
    const auto& object = *objects_[checked_slot(id)];
    if (object.kind != ObjectKind::WeakRef) {
        throw std::logic_error("object is not a weak reference");
    }
    validate_descriptor_shape(object, &metrics_);
    return object;
}

const Object& Heap::checked_record(ObjectId id) const {
    const auto& object = *objects_[checked_slot(id)];
    if (object.kind != ObjectKind::Record) {
        throw std::logic_error("object is not a record");
    }
    validate_descriptor_shape(object, &metrics_);
    return object;
}

Object& Heap::checked_record(ObjectId id) {
    auto& object = *objects_[checked_slot(id)];
    if (object.kind != ObjectKind::Record) {
        throw std::logic_error("object is not a record");
    }
    validate_descriptor_shape(object, &metrics_);
    return object;
}

const Object& Heap::checked_variant(ObjectId id) const {
    const auto& object = *objects_[checked_slot(id)];
    if (object.kind != ObjectKind::Variant) {
        throw std::logic_error("object is not a variant");
    }
    validate_descriptor_shape(object, &metrics_);
    return object;
}

const Object& Heap::checked_ephemeron(ObjectId id) const {
    const auto& object = *objects_[checked_slot(id)];
    if (object.kind != ObjectKind::Ephemeron) {
        throw std::logic_error("object is not an ephemeron");
    }
    validate_descriptor_shape(object, &metrics_);
    return object;
}

Object& Heap::checked_ephemeron(ObjectId id) {
    auto& object = *objects_[checked_slot(id)];
    if (object.kind != ObjectKind::Ephemeron) {
        throw std::logic_error("object is not an ephemeron");
    }
    validate_descriptor_shape(object, &metrics_);
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

std::size_t Heap::map_layout_index(ObjectId id) const {
    return checked_map(id).map_layout_index;
}

std::size_t Heap::map_length(ObjectId id) const {
    return checked_map(id).map_entries.size();
}

void Heap::validate_map_key(const Object& map, Value key) const {
    if (map.map_key_is_ref) {
        if (!key.is_object()) {
            throw std::logic_error("map string key must be an object reference");
        }
        (void)checked_string(key.as_object());
        return;
    }
    if (key.tag() != Value::Tag::Int64 && key.tag() != Value::Tag::Bool) {
        throw std::logic_error("map scalar key must be i64 or bool");
    }
}

void Heap::validate_map_value(const Object& map, Value value) const {
    if (map.map_value_is_ref) {
        if (value.tag() != Value::Tag::Object &&
            value.tag() != Value::Tag::Nil) {
            throw std::logic_error(
                "map reference value must carry Object or Nil tag");
        }
        if (value.is_object()) {
            (void)checked_slot(value.as_object());
        }
        return;
    }
    if (value.tag() != Value::Tag::Int64 && value.tag() != Value::Tag::Bool) {
        throw std::logic_error("map scalar value must be i64 or bool");
    }
}

std::optional<std::size_t> Heap::find_map_entry(ObjectId id, Value key) const {
    const auto& map = checked_map(id);
    validate_map_key(map, key);
    for (std::size_t i = 0; i < map.map_entries.size(); ++i) {
        ++metrics_.map_lookup_entries_examined;
        const auto stored = map.map_entries[i].key;
        bool equal = false;
        if (map.map_key_is_ref) {
            equal = string_equal(stored.as_object(), key.as_object());
        } else if (stored.tag() == key.tag() &&
                   stored.tag() == Value::Tag::Int64) {
            equal = stored.as_i64() == key.as_i64();
        } else if (stored.tag() == key.tag() &&
                   stored.tag() == Value::Tag::Bool) {
            equal = stored.as_bool() == key.as_bool();
        }
        if (equal) {
            return i;
        }
    }
    return std::nullopt;
}

bool Heap::map_has(ObjectId id, Value key) const {
    return find_map_entry(id, key).has_value();
}

Value Heap::map_get(ObjectId id, Value key) const {
    const auto entry = find_map_entry(id, key);
    if (!entry.has_value()) {
        throw std::out_of_range("map key not found");
    }
    return checked_map(id).map_entries[*entry].value;
}

Value Heap::map_key_at(ObjectId id, std::size_t index) const {
    const auto& map = checked_map(id);
    if (index >= map.map_entries.size()) {
        throw std::out_of_range("map entry index out of bounds");
    }
    return map.map_entries[index].key;
}

Value Heap::map_value_at(ObjectId id, std::size_t index) const {
    const auto& map = checked_map(id);
    if (index >= map.map_entries.size()) {
        throw std::out_of_range("map entry index out of bounds");
    }
    return map.map_entries[index].value;
}

Value Heap::weak_get(ObjectId id) const {
    return checked_weak_ref(id).weak_target();
}

Value Heap::ephemeron_key(ObjectId id) const {
    return checked_ephemeron(id).ephemeron_key();
}

Value Heap::ephemeron_value(ObjectId id) const {
    return checked_ephemeron(id).ephemeron_value();
}

void Heap::ephemeron_set_value(ObjectId id, Value value) {
    store_ephemeron_value(id, value);
}

std::size_t Heap::record_layout_index(ObjectId id) const {
    return checked_record(id).record_layout_index;
}

std::size_t Heap::record_field_count(ObjectId id) const {
    return checked_record(id).record_fields.size();
}

Value Heap::record_get(ObjectId id, std::size_t index) const {
    const auto& record = checked_record(id);
    if (index >= record.record_fields.size()) {
        throw std::out_of_range("record field index out of bounds");
    }
    return record.record_fields[index];
}

void Heap::record_set(ObjectId id, std::size_t index, Value value) {
    store_record_field(id, index, value);
}

std::size_t Heap::variant_layout_index(ObjectId id) const {
    return checked_variant(id).variant_layout_index;
}

std::size_t Heap::variant_tag(ObjectId id) const {
    return checked_variant(id).variant_case_index;
}

std::size_t Heap::variant_field_count(ObjectId id) const {
    return checked_variant(id).variant_fields.size();
}

Value Heap::variant_get(ObjectId id, std::size_t index) const {
    const auto& variant = checked_variant(id);
    if (index >= variant.variant_fields.size()) {
        throw std::out_of_range("variant field index out of bounds");
    }
    return variant.variant_fields[index];
}

void Heap::map_set(ObjectId id, Value key, Value value) {
    store_map_entry(id, key, value);
}

void Heap::ensure_map_growth_storage(Value& owner, Value& key, Value& value,
                                     std::size_t required_width) {
    const auto owner_slot = checked_slot(owner.as_object());
    const auto current_width = storage_slot_count(checked_map(owner.as_object()));
    assert(required_width == current_width + 2 &&
           "map insert must grow by exactly one key/value entry");

    bool adjacent_run_is_free = true;
    const auto existing_limit = std::min(objects_.size(), owner_slot + required_width);
    for (std::size_t slot = owner_slot + current_width;
         slot < existing_limit; ++slot) {
        if (!is_storage_slot_free(slot)) {
            adjacent_run_is_free = false;
            break;
        }
    }
    if (!adjacent_run_is_free) {
        relocate_map_for_growth(owner, key, value, required_width);
        return;
    }

    if (owner_slot + required_width > objects_.size()) {
        objects_.resize(owner_slot + required_width);
        generations_.resize(owner_slot + required_width, 0);
        if (objects_.size() > metrics_.heap_peak_slots) {
            metrics_.heap_peak_slots = objects_.size();
        }
    }
}

void Heap::relocate_map_for_growth(Value& owner, Value& key, Value& value,
                                   std::size_t required_width) {
    validate_weak_targets();
    const auto old_id = owner.as_object();
    const auto old_slot = checked_slot(old_id);
    const auto old_size = objects_.size();
    if (old_size > std::numeric_limits<std::uint32_t>::max() ||
        required_width >
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) -
                old_size + 1) {
        throw std::length_error("map growth exceeds heap object slot limit");
    }

    ForwardingTable forwarding(old_size);
    for (std::size_t slot = 0; slot < old_size; ++slot) {
        if (!objects_[slot].has_value()) {
            continue;
        }
        forwarding[slot] = make_object_id(
            static_cast<std::uint32_t>(slot), generations_[slot]);
    }

    auto relocated_objects = objects_;
    auto relocated_generations = generations_;
    relocated_objects.resize(old_size + required_width);
    relocated_generations.resize(old_size + required_width, 0);
    const auto new_slot = old_size;
    relocated_generations[new_slot] =
        generation_for_new_base(relocated_generations[new_slot]);
    const auto new_id = make_object_id(static_cast<std::uint32_t>(new_slot),
                                       relocated_generations[new_slot]);
    forwarding[old_slot] = new_id;

    auto moved = *objects_[old_slot];
    relocated_objects[old_slot].reset();
    relocated_objects[new_slot] = std::move(moved);

    const auto rewritten_remembered = rewrite_remembered_set(forwarding);
    std::array<Value*, 3> growth_roots{&owner, &key, &value};
    rewrite_references(forwarding, relocated_objects, nullptr, growth_roots);
    auto rewritten_weak_refs =
        process_weak_targets(forwarding, relocated_objects, std::nullopt);
    auto rewritten_ephemerons = process_ephemerons(forwarding, relocated_objects);
    if (incremental_marking_active_) {
        for (auto& grey : incremental_mark_worklist_) {
            const auto grey_slot = checked_slot(grey);
            if (!forwarding[grey_slot].has_value()) {
                throw std::logic_error(
                    "incremental grey object missing map-growth forwarding entry");
            }
            grey = *forwarding[grey_slot];
        }
    }

    objects_ = std::move(relocated_objects);
    generations_ = std::move(relocated_generations);
    remembered_set_ = rewritten_remembered;
    weak_refs_ = std::move(rewritten_weak_refs);
    ephemerons_ = std::move(rewritten_ephemerons);
    ++metrics_.objects_moved;
    if (objects_.size() > metrics_.heap_peak_slots) {
        metrics_.heap_peak_slots = objects_.size();
    }
    validate_heap_storage_layout();
    validate_remembered_set();
    validate_weak_targets();
    validate_ephemerons();
}

void Heap::store_map_entry(ObjectId id, Value key, Value value) {
    Value owner = Value::object(id);
    const auto existing = find_map_entry(id, key);
    validate_map_value(checked_map(id), value);

    if (!existing.has_value()) {
        const auto& before = checked_map(owner.as_object());
        if (before.map_entries.size() >=
            std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("map entry count exceeds object header limit");
        }
        const auto required_width = storage_slot_count(before) + 2;
        ensure_map_growth_storage(owner, key, value, required_width);
    }

    incremental_write_barrier_before_publish(owner.as_object(), key);
    incremental_write_barrier_before_publish(owner.as_object(), value);
    const bool barrier_triggered = record_map_write_barrier_if_needed(
        owner.as_object(), existing.has_value() ? std::nullopt
                                                : std::optional<Value>(key),
        value);
    auto& map = checked_map(owner.as_object());
    if (existing.has_value()) {
        map.map_entries[*existing].value = value;
    } else {
        map.map_entries.push_back(MapEntry{key, value});
        map.length = static_cast<std::uint32_t>(map.map_entries.size());
    }
    validate_descriptor_shape(map, &metrics_);

    if (barrier_triggered &&
        stress_config_.collect_minor_after_every_write_barrier) {
        std::array<Value*, 3> mutation_roots{&owner, &key, &value};
        collect_impl(CollectionKind::Minor, nullptr, mutation_roots);
    } else {
        validate_heap_storage_layout();
        validate_remembered_set();
    }
}

void Heap::store_pair_field(ObjectId id, PairField field, Value value) {
    // Barrier hook: every pair field mutation must flow through this method. The public
    // heap API intentionally exposes only const object inspection plus set_left/set_right,
    // so bytecode cannot publish a field without running this old-to-young barrier.
    auto& obj = checked_pair(id);
    incremental_write_barrier_before_publish(id, value);
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

    incremental_write_barrier_before_publish(id, value);
    const bool barrier_triggered = record_write_barrier_if_needed(id, value);
    obj.ref_elements[index] = value;

    if (barrier_triggered && stress_config_.collect_minor_after_every_write_barrier) {
        collect_minor();
    }
}

void Heap::store_record_field(ObjectId id, std::size_t index, Value value) {
    Value owner = Value::object(id);
    auto& record = checked_record(id);
    if (index >= record.record_fields.size()) {
        throw std::out_of_range("record field index out of bounds");
    }
    const bool is_reference = record.record_ref_map[index];
    if (is_reference) {
        if (value.tag() != Value::Tag::Object && value.tag() != Value::Tag::Nil) {
            throw std::logic_error(
                "record reference field must carry Object or Nil tag");
        }
        if (value.is_object()) {
            (void)checked_slot(value.as_object());
        }
    } else if (value.tag() != Value::Tag::Int64 &&
               value.tag() != Value::Tag::Bool) {
        throw std::logic_error("record scalar field must carry i64 or bool tag");
    }

    if (is_reference) incremental_write_barrier_before_publish(id, value);
    const bool barrier_triggered =
        is_reference && record_write_barrier_if_needed(id, value);
    record.record_fields[index] = value;
    validate_descriptor_shape(record, &metrics_);

    if (barrier_triggered &&
        stress_config_.collect_minor_after_every_write_barrier) {
        std::array<Value*, 2> mutation_roots{&owner, &value};
        collect_impl(CollectionKind::Minor, nullptr, mutation_roots);
    }
}

void Heap::store_ephemeron_value(ObjectId id, Value value) {
    Value owner = Value::object(id);
    auto& ephemeron = checked_ephemeron(id);
    if (ephemeron.ephemeron_key().tag() == Value::Tag::Nil) {
        throw std::logic_error("cannot mutate a cleared ephemeron");
    }
    if (ephemeron.ephemeron_value_is_ref()) {
        if (value.tag() != Value::Tag::Object && value.tag() != Value::Tag::Nil) {
            throw std::logic_error("ephemeron reference value must be object or nil");
        }
        if (value.is_object()) (void)checked_slot(value.as_object());
    } else if (value.tag() != Value::Tag::Int64 && value.tag() != Value::Tag::Bool) {
        throw std::logic_error("ephemeron scalar value must be i64 or bool");
    }
    if (ephemeron.ephemeron_value_is_ref()) {
        incremental_write_barrier_before_publish(id, value);
    }
    const bool barrier = ephemeron.ephemeron_value_is_ref() &&
                         record_write_barrier_if_needed(id, value);
    ephemeron.ephemeron_value_ = value;
    if (barrier && stress_config_.collect_minor_after_every_write_barrier) {
        std::array<Value*, 2> roots{&owner, &value};
        collect_impl(CollectionKind::Minor, nullptr, roots);
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

bool Heap::record_map_write_barrier_if_needed(
    ObjectId owner, std::optional<Value> inserted_key, Value value) {
    const auto owner_slot = checked_slot(owner);
    if (!is_old_slot(owner_slot)) {
        return false;
    }

    bool has_young_target = false;
    const auto consider = [&](Value candidate) {
        if (!candidate.is_object()) {
            return;
        }
        const auto target_slot = checked_slot(candidate.as_object());
        has_young_target = has_young_target || is_young_slot(target_slot);
    };
    if (inserted_key.has_value()) {
        consider(*inserted_key);
    }
    consider(value);
    if (!has_young_target) {
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
        }, &metrics_);
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
        }, &metrics_);
    }
}

void Heap::drain_mark_worklist(std::vector<ObjectId>& worklist, CollectionKind kind) {
    while (scan_next_mark_object(worklist, kind)) {}
}

bool Heap::scan_next_mark_object(std::vector<ObjectId>& worklist,
                                 CollectionKind kind) {
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
        }, &metrics_);
        return true;
    }
    return false;
}

void Heap::incremental_write_barrier_before_publish(ObjectId owner, Value value) {
    if (!incremental_marking_active_ || !value.is_object()) return;
    if (TEST_ONLY_skip_next_incremental_write_barrier_) {
        TEST_ONLY_skip_next_incremental_write_barrier_ = false;
        return;
    }
    const auto owner_slot = checked_slot(owner);
    const auto target_slot = checked_slot(value.as_object());
    if (objects_[owner_slot]->marked && !objects_[target_slot]->marked) {
        incremental_mark_worklist_.push_back(value.as_object());
    }
}

void Heap::process_ephemeron_fixpoint(std::vector<ObjectId>& worklist,
                                      CollectionKind kind) {
    for (;;) {
        ++metrics_.ephemeron_fixpoint_passes;
        bool progressed = false;
        for (const auto owner_id : ephemerons_) {
            const auto owner_slot = checked_slot(owner_id);
            const auto& owner = *objects_[owner_slot];
            const bool owner_live = kind == CollectionKind::Minor
                                        ? is_old_slot(owner_slot) || owner.marked
                                        : owner.marked;
            if (!owner_live || owner.ephemeron_key().tag() == Value::Tag::Nil) continue;
            const auto key_slot = checked_slot(owner.ephemeron_key().as_object());
            const bool key_live = kind == CollectionKind::Minor
                                      ? is_old_slot(key_slot) || objects_[key_slot]->marked
                                      : objects_[key_slot]->marked;
            if (!key_live || !owner.ephemeron_value_is_ref() ||
                !owner.ephemeron_value().is_object()) continue;
            const auto value_slot = checked_slot(owner.ephemeron_value().as_object());
            const bool already_live = kind == CollectionKind::Minor
                                          ? is_old_slot(value_slot) || objects_[value_slot]->marked
                                          : objects_[value_slot]->marked;
            if (!already_live) {
                enqueue_mark_value(owner.ephemeron_value(), worklist, kind);
                ++metrics_.ephemeron_activations;
                progressed = true;
            }
        }
        drain_mark_worklist(worklist, kind);
        if (!progressed) break;
    }
}

void Heap::collect() {
    collect_impl(CollectionKind::Major, nullptr, {});
}

void Heap::start_incremental_marking() {
    if (incremental_marking_active_) {
        throw std::logic_error("incremental marking cycle already active");
    }
    validate_heap_storage_layout();
    validate_remembered_set();
    validate_weak_targets();
    validate_ephemerons();
    incremental_mark_worklist_.clear();
    MarkingVisitor marker(*this, incremental_mark_worklist_, CollectionKind::Major);
    trace_collection_roots(marker, nullptr, {});
    incremental_marking_active_ = true;
    ++metrics_.major_collections;
    ++metrics_.incremental_cycles_started;
}

std::size_t Heap::incremental_mark_step(std::size_t budget) {
    if (!incremental_marking_active_) {
        throw std::logic_error("incremental marking step without active cycle");
    }
    ++metrics_.incremental_steps;
    metrics_.incremental_budget_requested += budget;
    std::size_t consumed = 0;
    while (consumed < budget &&
           scan_next_mark_object(incremental_mark_worklist_, CollectionKind::Major)) {
        ++consumed;
    }
    metrics_.incremental_objects_scanned += consumed;
    TEST_ONLY_validate_incremental_marking();
    return consumed;
}

void Heap::TEST_ONLY_validate_incremental_marking() const {
    if (!incremental_marking_active_) return;
    const auto is_grey = [&](ObjectId id) {
        return std::find(incremental_mark_worklist_.begin(),
                         incremental_mark_worklist_.end(), id) !=
               incremental_mark_worklist_.end();
    };
    for (std::size_t slot = 0; slot < objects_.size(); ++slot) {
        if (!objects_[slot].has_value() || !objects_[slot]->marked) continue;
        visit_reference_fields(*objects_[slot], [&](Value value) {
            if (!value.is_object()) return;
            const auto target_slot = checked_slot(value.as_object());
            if (!objects_[target_slot]->marked && !is_grey(value.as_object())) {
                throw std::logic_error(
                    "incremental tri-colour invariant violated: black-to-white edge");
            }
        });
    }
}

void Heap::finish_incremental_marking() {
    finish_incremental_marking_impl(nullptr, {});
}

void Heap::finish_incremental_marking_impl(RootProvider* roots,
                                           std::span<Value*> extra_roots) {
    if (!incremental_marking_active_) {
        throw std::logic_error("incremental marking finish without active cycle");
    }
    TEST_ONLY_validate_incremental_marking();

    // The final pause is also the differential liveness oracle. Recompute from the
    // current root graph so floating garbage created by edge deletion cannot make an
    // incremental schedule differ from an atomic major collection at this boundary.
    // The bounded phase remains independently checked by the tri-colour validator above.
    for (auto& slot : objects_) {
        if (slot.has_value()) slot->marked = false;
    }
    incremental_mark_worklist_.clear();
    MarkingVisitor marker(*this, incremental_mark_worklist_, CollectionKind::Major);
    trace_collection_roots(marker, roots, extra_roots);
    drain_mark_worklist(incremental_mark_worklist_, CollectionKind::Major);
    process_ephemeron_fixpoint(incremental_mark_worklist_, CollectionKind::Major);
    validate_incremental_result_against_atomic(roots, extra_roots);

    auto compacted = compact_live_objects(CollectionKind::Major);
    metrics_.objects_moved += compacted.objects_moved;
    rewrite_references(compacted.forwarding, compacted.objects, roots, extra_roots);
    auto rewritten_remembered_set = rewrite_remembered_set(compacted.forwarding);
    auto rewritten_weak_refs = process_weak_targets(
        compacted.forwarding, compacted.objects, CollectionKind::Major);
    auto rewritten_ephemerons =
        process_ephemerons(compacted.forwarding, compacted.objects);

    objects_ = std::move(compacted.objects);
    generations_ = std::move(compacted.generations);
    remembered_set_ = std::move(rewritten_remembered_set);
    weak_refs_ = std::move(rewritten_weak_refs);
    ephemerons_ = std::move(rewritten_ephemerons);
    incremental_mark_worklist_.clear();
    incremental_marking_active_ = false;
    ++metrics_.incremental_final_pauses;
    record_promoted_object_edges(compacted.promoted_slots);
    prune_remembered_set();
    validate_after_collection(roots, extra_roots);
}

void Heap::validate_incremental_result_against_atomic(
    RootProvider* roots, std::span<Value*> extra_roots) const {
    std::vector<bool> live(objects_.size(), false);
    std::vector<ObjectId> worklist;
    class ShadowRootVisitor final : public RootVisitor {
    public:
        explicit ShadowRootVisitor(std::vector<ObjectId>& worklist)
            : worklist_(worklist) {}
        void visit(Value& value) override {
            if (value.is_object()) worklist_.push_back(value.as_object());
        }
    private:
        std::vector<ObjectId>& worklist_;
    } visitor(worklist);
    trace_collection_roots(visitor, roots, extra_roots);

    const auto drain = [&]() {
        while (!worklist.empty()) {
            const auto id = worklist.back();
            worklist.pop_back();
            const auto slot = checked_slot(id);
            if (live[slot]) continue;
            live[slot] = true;
            visit_reference_fields_for_lifo_marking(
                *objects_[slot], [&](Value value) {
                    if (value.is_object()) worklist.push_back(value.as_object());
                });
        }
    };
    drain();
    for (;;) {
        bool progressed = false;
        for (const auto owner_id : ephemerons_) {
            const auto owner_slot = checked_slot(owner_id);
            if (!live[owner_slot]) continue;
            const auto& owner = *objects_[owner_slot];
            if (!owner.ephemeron_key().is_object()) continue;
            const auto key_slot = checked_slot(owner.ephemeron_key().as_object());
            if (!live[key_slot] || !owner.ephemeron_value_is_ref() ||
                !owner.ephemeron_value().is_object()) continue;
            const auto value_slot = checked_slot(owner.ephemeron_value().as_object());
            if (!live[value_slot]) {
                worklist.push_back(owner.ephemeron_value().as_object());
                progressed = true;
            }
        }
        drain();
        if (!progressed) break;
    }

    for (std::size_t slot = 0; slot < objects_.size(); ++slot) {
        if (!objects_[slot].has_value()) continue;
        if (objects_[slot]->marked != live[slot]) {
            throw std::logic_error(
                "incremental/STW differential liveness mismatch at heap slot " +
                std::to_string(slot));
        }
    }
    ++metrics_.incremental_differential_validations;
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
    if (incremental_marking_active_) {
        finish_incremental_marking_impl(roots, extra_roots);
    }
    validate_heap_storage_layout();
    validate_remembered_set();
    validate_weak_targets();
    validate_ephemerons();
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
    process_ephemeron_fixpoint(worklist, kind);

    auto compacted = compact_live_objects(kind);
    metrics_.objects_moved += compacted.objects_moved;
    rewrite_references(compacted.forwarding, compacted.objects, roots, extra_roots);
    auto rewritten_remembered_set = rewrite_remembered_set(compacted.forwarding);
    auto rewritten_weak_refs =
        process_weak_targets(compacted.forwarding, compacted.objects, kind);
    auto rewritten_ephemerons =
        process_ephemerons(compacted.forwarding, compacted.objects);

    objects_ = std::move(compacted.objects);
    generations_ = std::move(compacted.generations);
    remembered_set_ = std::move(rewritten_remembered_set);
    weak_refs_ = std::move(rewritten_weak_refs);
    ephemerons_ = std::move(rewritten_ephemerons);
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

        const auto width_at_collection_start = storage_slot_count(*slot);
        auto moved = *slot;
        moved.marked = false;
        if (moved.generation == ObjectGeneration::Young) {
            moved.generation = ObjectGeneration::Old;
            result.promoted_slots.push_back(next_live_slot);
        }

        const auto required_slots = storage_slot_count(moved);
        assert(required_slots == width_at_collection_start &&
               "object storage width changed within one collection");
        ++metrics_.compaction_objects_copied;
        const auto copied_bytes = required_slots * kStorageSlotBytes;
        switch (moved.kind) {
        case ObjectKind::Pair:
            metrics_.compaction_pair_bytes += copied_bytes;
            break;
        case ObjectKind::ScalarArray:
            metrics_.compaction_scalar_array_bytes += copied_bytes;
            break;
        case ObjectKind::RefArray:
            metrics_.compaction_ref_array_bytes += copied_bytes;
            break;
        case ObjectKind::Str:
            metrics_.compaction_string_bytes += copied_bytes;
            break;
        case ObjectKind::Closure:
            metrics_.compaction_closure_bytes += copied_bytes;
            break;
        case ObjectKind::Map:
            metrics_.compaction_map_bytes += copied_bytes;
            break;
        case ObjectKind::WeakRef:
            metrics_.compaction_weak_ref_bytes += copied_bytes;
            break;
        case ObjectKind::Record:
            // Record accounting is intentionally not appended to the public benchmark
            // counter stream; legacy workloads must remain byte-identical.
            break;
        case ObjectKind::Variant:
            // Variant accounting is likewise omitted from the legacy public counter
            // stream while its exact descriptor width still advances the cursor.
            break;
        case ObjectKind::Ephemeron:
            break;
        }
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
        }, &metrics_);
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

std::vector<ObjectId> Heap::process_weak_targets(
    const ForwardingTable& forwarding,
    std::vector<std::optional<Object>>& moved_objects,
    std::optional<CollectionKind> collection_kind) const {
    std::vector<ObjectId> rewritten;
    rewritten.reserve(weak_refs_.size());

    for (const auto old_owner : weak_refs_) {
        const auto old_owner_slot = checked_slot(old_owner);
        if (old_owner_slot >= forwarding.size() ||
            !forwarding[old_owner_slot].has_value()) {
            continue;
        }

        const auto new_owner = *forwarding[old_owner_slot];
        const auto new_owner_slot = static_cast<std::size_t>(slot_from(new_owner));
        if (new_owner_slot >= moved_objects.size() ||
            !moved_objects[new_owner_slot].has_value() ||
            moved_objects[new_owner_slot]->kind != ObjectKind::WeakRef) {
            throw std::logic_error(
                "weak registry owner missing from movement forwarding result");
        }

        auto& target = moved_objects[new_owner_slot]->weak_target_;
        ++metrics_.weak_targets_processed;
        if (target.is_object()) {
            const auto old_target_slot = checked_slot(target.as_object());
            if (old_target_slot < forwarding.size() &&
                forwarding[old_target_slot].has_value()) {
                target = Value::object(*forwarding[old_target_slot]);
                ++metrics_.weak_targets_forwarded;
                if (collection_kind == CollectionKind::Major) {
                    ++metrics_.major_weak_targets_forwarded;
                } else if (collection_kind == CollectionKind::Minor) {
                    ++metrics_.minor_weak_targets_forwarded;
                }
            } else {
                target = Value::nil();
                ++metrics_.weak_targets_cleared;
                if (collection_kind == CollectionKind::Major) {
                    ++metrics_.major_weak_targets_cleared;
                } else if (collection_kind == CollectionKind::Minor) {
                    ++metrics_.minor_weak_targets_cleared;
                }
            }
        } else if (target.tag() != Value::Tag::Nil) {
            throw std::logic_error(
                "weak target processing saw a non-reference target value");
        }
        rewritten.push_back(new_owner);
    }

    for (std::size_t i = 1; i < rewritten.size(); ++i) {
        if (slot_from(rewritten[i - 1]) >= slot_from(rewritten[i])) {
            throw std::logic_error(
                "weak registry movement did not preserve strict heap-slot order");
        }
    }
    return rewritten;
}

std::vector<ObjectId> Heap::process_ephemerons(
    const ForwardingTable& forwarding,
    std::vector<std::optional<Object>>& moved_objects) const {
    std::vector<ObjectId> rewritten;
    rewritten.reserve(ephemerons_.size());
    for (const auto old_owner : ephemerons_) {
        const auto owner_slot = checked_slot(old_owner);
        if (owner_slot >= forwarding.size() || !forwarding[owner_slot].has_value()) continue;
        const auto new_owner = *forwarding[owner_slot];
        auto& object = *moved_objects[slot_from(new_owner)];
        auto& key = object.ephemeron_key_;
        auto& value = object.ephemeron_value_;
        if (!key.is_object()) {
            if (key.tag() != Value::Tag::Nil || value.tag() != Value::Tag::Nil)
                throw std::logic_error("invalid cleared ephemeron during movement");
        } else {
            const auto key_slot = checked_slot(key.as_object());
            if (key_slot >= forwarding.size() || !forwarding[key_slot].has_value()) {
                key = Value::nil();
                value = Value::nil();
            } else {
                key = Value::object(*forwarding[key_slot]);
                if (object.ephemeron_value_is_ref_ && value.is_object()) {
                    const auto value_slot = checked_slot(value.as_object());
                    if (value_slot >= forwarding.size() ||
                        !forwarding[value_slot].has_value()) {
                        throw std::logic_error("active ephemeron value missing forwarding entry");
                    }
                    value = Value::object(*forwarding[value_slot]);
                }
            }
        }
        rewritten.push_back(new_owner);
    }
    return rewritten;
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
        }, &metrics_);
        if (has_young_reference) {
            pruned.push_back(make_object_id(static_cast<std::uint32_t>(slot), generations_[slot]));
        }
    }
    remembered_set_ = std::move(pruned);
}

void Heap::validate_heap_storage_layout() const {
    std::vector<bool> covered(objects_.size(), false);
    for (std::size_t base = 0; base < objects_.size(); ++base) {
        ++metrics_.heap_layout_slots_checked;
        if (!objects_[base].has_value()) {
            continue;
        }
        ++metrics_.heap_layout_objects_checked;
        if (covered[base]) {
            throw std::logic_error("heap object header overlaps another object's storage run");
        }
        validate_descriptor_shape(*objects_[base], &metrics_);
        const auto width = storage_slot_count(*objects_[base]);
        if (width == 0 || base + width > objects_.size()) {
            throw std::logic_error("heap object descriptor extends past heap storage");
        }
        for (std::size_t offset = 0; offset < width; ++offset) {
            ++metrics_.heap_layout_slots_checked;
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
        validate_descriptor_shape(*slot, &metrics_);
        visit_reference_fields(*slot, [&](Value value) {
            validate_value(value);
        }, &metrics_);
    }

    validate_remembered_set();
    validate_weak_targets();
    validate_ephemerons();
}

void Heap::validate_remembered_set() const {
    for (const auto remembered : remembered_set_) {
        ++metrics_.remembered_set_entries_checked;
        const auto slot = checked_slot(remembered);
        if (!is_old_slot(slot)) {
            throw std::logic_error("remembered-set entry does not name an old object");
        }
    }

    for (std::size_t slot = 0; slot < objects_.size(); ++slot) {
        ++metrics_.remembered_set_heap_slots_examined;
        if (!objects_[slot].has_value() || !is_old_slot(slot)) {
            continue;
        }

        const auto owner = make_object_id(static_cast<std::uint32_t>(slot), generations_[slot]);
        visit_reference_fields(*objects_[slot], [&](Value value) {
            ++metrics_.remembered_set_reference_fields_checked;
            if (!value.is_object()) {
                return;
            }
            const auto target_slot = checked_slot(value.as_object());
            if (is_young_slot(target_slot) && !remembered_set_contains(owner)) {
                throw std::logic_error(
                    "old-to-young reference missing remembered-set entry");
            }
        }, &metrics_);
    }
}

void Heap::validate_weak_targets() const {
    std::size_t registry_index = 0;
    std::optional<std::uint32_t> previous_slot;
    for (std::size_t slot = 0; slot < objects_.size(); ++slot) {
        if (!objects_[slot].has_value() ||
            objects_[slot]->kind != ObjectKind::WeakRef) {
            continue;
        }
        if (registry_index >= weak_refs_.size()) {
            throw std::logic_error(
                "live WeakRef is missing from weak-target registry");
        }
        const auto registered = weak_refs_[registry_index++];
        const auto registered_slot = slot_from(registered);
        if (registered_slot != slot || checked_slot(registered) != slot) {
            throw std::logic_error(
                "weak-target registry names a stale or wrong WeakRef owner");
        }
        if (previous_slot.has_value() && *previous_slot >= registered_slot) {
            throw std::logic_error(
                "weak-target registry is not in strict heap-slot order");
        }
        previous_slot = registered_slot;

        const auto target = objects_[slot]->weak_target();
        if (target.is_object()) {
            validate_value(target);
        } else if (target.tag() != Value::Tag::Nil) {
            throw std::logic_error(
                "weak target is neither a live object id nor canonical nil");
        }
    }
    if (registry_index != weak_refs_.size()) {
        throw std::logic_error(
            "weak-target registry contains a dead or non-WeakRef owner");
    }
}

void Heap::validate_ephemerons() const {
    std::size_t registry_index = 0;
    for (std::size_t slot = 0; slot < objects_.size(); ++slot) {
        if (!objects_[slot].has_value() || objects_[slot]->kind != ObjectKind::Ephemeron)
            continue;
        if (registry_index >= ephemerons_.size() ||
            checked_slot(ephemerons_[registry_index]) != slot) {
            throw std::logic_error("live Ephemeron is missing from registry");
        }
        if (registry_index > 0 &&
            slot_from(ephemerons_[registry_index - 1]) >=
                slot_from(ephemerons_[registry_index])) {
            throw std::logic_error("ephemeron registry is not in strict slot order");
        }
        const auto& object = *objects_[slot];
        validate_descriptor_shape(object, &metrics_);
        if (object.ephemeron_key().is_object()) validate_value(object.ephemeron_key());
        if (object.ephemeron_value_is_ref() && object.ephemeron_value().is_object())
            validate_value(object.ephemeron_value());
        ++registry_index;
    }
    if (registry_index != ephemerons_.size())
        throw std::logic_error("ephemeron registry contains a dead owner");
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

bool Heap::TEST_ONLY_is_map(ObjectId id) const {
    return object(id).kind == ObjectKind::Map;
}

bool Heap::TEST_ONLY_is_weak_ref(ObjectId id) const {
    return object(id).kind == ObjectKind::WeakRef;
}

bool Heap::TEST_ONLY_is_record(ObjectId id) const {
    return object(id).kind == ObjectKind::Record;
}

bool Heap::TEST_ONLY_is_variant(ObjectId id) const {
    return object(id).kind == ObjectKind::Variant;
}

bool Heap::TEST_ONLY_is_ephemeron(ObjectId id) const {
    return object(id).kind == ObjectKind::Ephemeron;
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
