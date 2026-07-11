#pragma once

#include "lang/value.hpp"

#include <cstdint>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace lang::gc {

class Heap;
struct HeapLifetime;

class RootVisitor {
public:
    virtual ~RootVisitor() = default;
    virtual void visit(Value& root) = 0;
};

class RootProvider {
public:
    virtual ~RootProvider() = default;
    virtual void trace_roots(RootVisitor& visitor) = 0;
};

struct StressConfig {
    bool collect_before_every_allocation{false};
    bool collect_after_every_allocation{false};
    bool collect_minor_after_every_write_barrier{false};
    std::uint64_t collect_every_n_instructions{0};
    std::uint64_t collect_minor_every_n_instructions{0};
};

struct HeapMetrics {
    std::uint64_t allocations{0};
    std::uint64_t major_collections{0};
    std::uint64_t minor_collections{0};
    std::uint64_t objects_moved{0};
    std::uint64_t write_barrier_hits{0};
    std::uint64_t remembered_set_peak{0};
    std::uint64_t heap_peak_slots{0};
    std::uint64_t map_lookup_entries_examined{0};
    std::uint64_t map_descriptor_entries_scanned{0};
    std::uint64_t closure_capture_slots_scanned{0};
    std::uint64_t weak_targets_processed{0};
    std::uint64_t weak_targets_forwarded{0};
    std::uint64_t weak_targets_cleared{0};
    std::uint64_t major_weak_targets_forwarded{0};
    std::uint64_t major_weak_targets_cleared{0};
    std::uint64_t minor_weak_targets_forwarded{0};
    std::uint64_t minor_weak_targets_cleared{0};
    std::uint64_t allocation_candidate_slots_examined{0};
    std::uint64_t allocation_storage_slots_checked{0};
    std::uint64_t storage_occupancy_headers_examined{0};
    std::uint64_t heap_layout_objects_checked{0};
    std::uint64_t heap_layout_slots_checked{0};
    std::uint64_t remembered_set_entries_checked{0};
    std::uint64_t remembered_set_heap_slots_examined{0};
    std::uint64_t remembered_set_reference_fields_checked{0};
    std::uint64_t compaction_objects_copied{0};
    std::uint64_t compaction_pair_bytes{0};
    std::uint64_t compaction_scalar_array_bytes{0};
    std::uint64_t compaction_ref_array_bytes{0};
    std::uint64_t compaction_string_bytes{0};
    std::uint64_t compaction_closure_bytes{0};
    std::uint64_t compaction_map_bytes{0};
    std::uint64_t compaction_weak_ref_bytes{0};
};

enum class ObjectGeneration {
    Young,
    Old,
};

enum class ObjectKind {
    Pair,
    ScalarArray,
    RefArray,
    Str,
    Closure,
    Map,
    WeakRef,
};

struct MapEntry {
    Value key{Value::nil()};
    Value value{Value::nil()};
};

struct Object {
    static Object pair(Value left, Value right);
    static Object scalar_array(std::size_t length, std::int64_t init);
    static Object ref_array(std::size_t length, Value init);
    static Object string(std::span<const std::uint8_t> bytes);
    static Object closure(std::size_t layout_index, std::size_t function_index,
                          std::vector<Value> captures,
                          std::vector<bool> capture_map);
    static Object map(std::size_t layout_index, bool key_is_ref,
                      bool value_is_ref);
    static Object weak_ref(Value target);

    [[nodiscard]] Value weak_target() const { return weak_target_; }

    bool marked{false};
    ObjectGeneration generation{ObjectGeneration::Young};
    ObjectKind kind{ObjectKind::Pair};
    std::uint32_t length{2};
    Value left{Value::nil()};
    Value right{Value::nil()};
    std::vector<std::int64_t> scalar_elements;
    std::vector<Value> ref_elements;
    std::vector<std::uint8_t> string_bytes;
    std::uint32_t closure_layout_index{0};
    std::uint32_t closure_function_index{0};
    std::vector<Value> closure_captures;
    std::vector<bool> closure_capture_map;
    std::uint32_t map_layout_index{0};
    bool map_key_is_ref{false};
    bool map_value_is_ref{false};
    std::vector<MapEntry> map_entries;

private:
    friend class Heap;
    Value weak_target_{Value::nil()};
};

class Handle {
public:
    ~Handle();

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept;
    Handle& operator=(Handle&& other) noexcept;

    [[nodiscard]] Value value() const;
    [[nodiscard]] ObjectId object() const;

private:
    friend class Heap;

    Handle(Heap& heap, Value value);

    void release() noexcept;
    void ensure_usable() const;

    Heap* heap_{nullptr};
    std::shared_ptr<HeapLifetime> lifetime_;
    Value slot_{Value::nil()};
};

class Heap {
public:
    Heap();
    ~Heap() noexcept;

    Heap(const Heap&) = delete;
    Heap& operator=(const Heap&) = delete;
    Heap(Heap&&) = delete;
    Heap& operator=(Heap&&) = delete;

    ObjectId allocate_pair(Value left, Value right);
    ObjectId allocate_scalar_array(std::size_t length, std::int64_t init);
    ObjectId allocate_string(std::span<const std::uint8_t> bytes);
    ObjectId allocate_string_concat(Value left, Value right);
    ObjectId allocate_string_substring(Value source, std::size_t lo,
                                       std::size_t hi);
    ObjectId allocate_closure(std::size_t layout_index,
                              std::size_t function_index,
                              std::vector<Value> captures,
                              std::vector<bool> capture_map);
    ObjectId allocate_map(std::size_t layout_index, bool key_is_ref,
                          bool value_is_ref);
    ObjectId allocate_weak(Value target);
    [[nodiscard]] Handle make_handle(Value value);
    [[nodiscard]] Handle make_handle(ObjectId id);
    void set_root_provider(RootProvider* provider) { root_provider_ = provider; }
    void collect();
    void collect(RootProvider& roots);
    void collect_minor();
    void collect_minor(RootProvider& roots);
    void set_stress_config(StressConfig config) { stress_config_ = config; }

    [[nodiscard]] const Object& object(ObjectId id) const;
    [[nodiscard]] Value left(ObjectId id) const;
    [[nodiscard]] Value right(ObjectId id) const;
    void set_left(ObjectId id, Value value);
    void set_right(ObjectId id, Value value);
    [[nodiscard]] std::size_t array_length(ObjectId id) const;
    [[nodiscard]] std::int64_t array_get(ObjectId id, std::size_t index) const;
    void array_set(ObjectId id, std::size_t index, std::int64_t value);
    ObjectId allocate_ref_array(std::size_t length, Value init);
    [[nodiscard]] std::size_t ref_array_length(ObjectId id) const;
    [[nodiscard]] Value ref_array_get(ObjectId id, std::size_t index) const;
    void ref_array_set(ObjectId id, std::size_t index, Value value);
    [[nodiscard]] std::size_t string_length(ObjectId id) const;
    [[nodiscard]] std::span<const std::uint8_t> string_bytes(ObjectId id) const;
    [[nodiscard]] bool string_equal(ObjectId left, ObjectId right) const;
    [[nodiscard]] std::uint8_t string_index(ObjectId id, std::size_t index) const;
    [[nodiscard]] std::size_t closure_layout_index(ObjectId id) const;
    [[nodiscard]] std::size_t closure_function_index(ObjectId id) const;
    [[nodiscard]] std::size_t closure_capture_count(ObjectId id) const;
    [[nodiscard]] Value closure_capture(ObjectId id, std::size_t index) const;
    [[nodiscard]] std::size_t map_layout_index(ObjectId id) const;
    [[nodiscard]] std::size_t map_length(ObjectId id) const;
    [[nodiscard]] bool map_has(ObjectId id, Value key) const;
    [[nodiscard]] Value map_get(ObjectId id, Value key) const;
    void map_set(ObjectId id, Value key, Value value);
    [[nodiscard]] Value map_key_at(ObjectId id, std::size_t index) const;
    [[nodiscard]] Value map_value_at(ObjectId id, std::size_t index) const;
    [[nodiscard]] Value weak_get(ObjectId id) const;
    [[nodiscard]] std::size_t live_count() const;
    [[nodiscard]] std::size_t capacity_slots() const { return objects_.size(); }
    [[nodiscard]] StressConfig stress_config() const { return stress_config_; }
    [[nodiscard]] HeapMetrics metrics() const { return metrics_; }

    [[nodiscard]] bool TEST_ONLY_is_young_object(ObjectId id) const;
    [[nodiscard]] bool TEST_ONLY_is_old_object(ObjectId id) const;
    [[nodiscard]] bool TEST_ONLY_is_scalar_array(ObjectId id) const;
    [[nodiscard]] bool TEST_ONLY_is_ref_array(ObjectId id) const;
    [[nodiscard]] bool TEST_ONLY_is_string(ObjectId id) const;
    [[nodiscard]] bool TEST_ONLY_is_closure(ObjectId id) const;
    [[nodiscard]] bool TEST_ONLY_is_map(ObjectId id) const;
    [[nodiscard]] bool TEST_ONLY_is_weak_ref(ObjectId id) const;
    [[nodiscard]] std::size_t TEST_ONLY_remembered_set_size() const {
        return remembered_set_.size();
    }
    [[nodiscard]] std::uint64_t TEST_ONLY_validation_count() const {
        return TEST_ONLY_validation_count_;
    }
    [[nodiscard]] std::size_t TEST_ONLY_handle_root_count() const {
        return handle_roots_.size();
    }
    void TEST_ONLY_skip_next_write_barrier_for_barrier_validator();
    void TEST_ONLY_promote_object_through_collector_path(ObjectId id);
    void TEST_ONLY_validate_gc_invariants() const;

private:
    friend class Handle;

    class MarkingVisitor;
    class ForwardingVisitor;
    class ValidatingVisitor;
    enum class PairField { Left, Right };
    enum class CollectionKind { Major, Minor };
    using ForwardingTable = std::vector<std::optional<ObjectId>>;

    struct CompactionResult {
        ForwardingTable forwarding;
        std::vector<std::optional<Object>> objects;
        std::vector<std::uint32_t> generations;
        std::vector<std::size_t> promoted_slots;
        std::uint64_t objects_moved{0};
    };

    ObjectId allocate_object(Object object);
    [[nodiscard]] std::optional<std::size_t> find_free_storage_run(
        std::size_t required_slots) const;
    [[nodiscard]] bool is_storage_slot_free(std::size_t slot) const;
    [[nodiscard]] std::size_t checked_slot(ObjectId id) const;
    [[nodiscard]] Object& mutable_object(ObjectId id);
    [[nodiscard]] const Object& checked_pair(ObjectId id) const;
    [[nodiscard]] Object& checked_pair(ObjectId id);
    [[nodiscard]] const Object& checked_scalar_array(ObjectId id) const;
    [[nodiscard]] Object& checked_scalar_array(ObjectId id);
    [[nodiscard]] const Object& checked_ref_array(ObjectId id) const;
    [[nodiscard]] Object& checked_ref_array(ObjectId id);
    [[nodiscard]] const Object& checked_string(ObjectId id) const;
    [[nodiscard]] const Object& checked_closure(ObjectId id) const;
    [[nodiscard]] const Object& checked_map(ObjectId id) const;
    [[nodiscard]] Object& checked_map(ObjectId id);
    [[nodiscard]] const Object& checked_weak_ref(ObjectId id) const;
    void register_handle_root(Value* slot);
    void deregister_handle_root(Value* slot) noexcept;
    void move_handle_root(Value* from, Value* to) noexcept;
    void trace_handle_roots(RootVisitor& visitor) const;
    void collect_impl(CollectionKind kind, RootProvider* roots, std::span<Value*> extra_roots);
    void collect_with_extra_roots(std::span<Value*> extra_roots);
    void trace_collection_roots(RootVisitor& visitor, RootProvider* roots,
                                std::span<Value*> extra_roots) const;
    void store_pair_field(ObjectId id, PairField field, Value value);
    void store_ref_array_element(ObjectId id, std::size_t index, Value value);
    void store_map_entry(ObjectId id, Value key, Value value);
    void ensure_map_growth_storage(Value& owner, Value& key, Value& value,
                                   std::size_t required_width);
    void relocate_map_for_growth(Value& owner, Value& key, Value& value,
                                 std::size_t required_width);
    [[nodiscard]] std::optional<std::size_t> find_map_entry(
        ObjectId id, Value key) const;
    void validate_map_key(const Object& map, Value key) const;
    void validate_map_value(const Object& map, Value value) const;
    [[nodiscard]] bool record_write_barrier_if_needed(ObjectId owner, Value value);
    [[nodiscard]] bool record_map_write_barrier_if_needed(
        ObjectId owner, std::optional<Value> inserted_key, Value value);
    void record_remembered_object(ObjectId id);
    void record_promoted_object_edges(std::span<const std::size_t> promoted_slots);
    [[nodiscard]] bool remembered_set_contains(ObjectId id) const;
    [[nodiscard]] bool is_young_slot(std::size_t slot) const;
    [[nodiscard]] bool is_old_slot(std::size_t slot) const;
    void enqueue_mark_value(Value value, std::vector<ObjectId>& worklist, CollectionKind kind);
    void enqueue_young_references_from_remembered_set(std::vector<ObjectId>& worklist);
    void drain_mark_worklist(std::vector<ObjectId>& worklist, CollectionKind kind);
    [[nodiscard]] CompactionResult compact_live_objects(CollectionKind kind) const;
    void rewrite_references(const ForwardingTable& forwarding,
                            std::vector<std::optional<Object>>& compacted_objects,
                            RootProvider* roots, std::span<Value*> extra_roots) const;
    void rewrite_value(Value& value, const ForwardingTable& forwarding) const;
    [[nodiscard]] std::vector<ObjectId> process_weak_targets(
        const ForwardingTable& forwarding,
        std::vector<std::optional<Object>>& moved_objects,
        std::optional<CollectionKind> collection_kind) const;
    [[nodiscard]] std::vector<ObjectId> rewrite_remembered_set(
        const ForwardingTable& forwarding) const;
    void prune_remembered_set();
    void validate_heap_storage_layout() const;
    void validate_after_collection(RootProvider* roots, std::span<Value*> extra_roots) const;
    void validate_remembered_set() const;
    void validate_weak_targets() const;
    void validate_value(Value value) const;

    RootProvider* root_provider_{nullptr};
    std::shared_ptr<HeapLifetime> lifetime_;
    StressConfig stress_config_{};
    std::vector<std::optional<Object>> objects_;
    std::vector<std::uint32_t> generations_;
    std::vector<ObjectId> remembered_set_;
    std::vector<ObjectId> weak_refs_;
    std::vector<Value*> handle_roots_;
    // Performance metrics are passive observations, including logically-const
    // validation and lookup work. No metric participates in runtime control flow.
    mutable HeapMetrics metrics_{};
    bool TEST_ONLY_skip_next_write_barrier_{false};
    mutable std::uint64_t TEST_ONLY_validation_count_{0};
};

} // namespace lang::gc
