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
};

enum class ObjectGeneration {
    Young,
    Old,
};

struct Object {
    // Object layout assumption: a pair stores two full tagged Values.
    // The marker must inspect the Value tag before following a field as an ObjectId.
    bool marked{false};
    ObjectGeneration generation{ObjectGeneration::Young};
    Value left{Value::nil()};
    Value right{Value::nil()};
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
    [[nodiscard]] std::size_t live_count() const;
    [[nodiscard]] std::size_t capacity_slots() const { return objects_.size(); }
    [[nodiscard]] StressConfig stress_config() const { return stress_config_; }
    [[nodiscard]] HeapMetrics metrics() const { return metrics_; }

    [[nodiscard]] bool TEST_ONLY_is_young_object(ObjectId id) const;
    [[nodiscard]] bool TEST_ONLY_is_old_object(ObjectId id) const;
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
        std::uint64_t objects_moved{0};
    };

    ObjectId allocate_slot(Value left, Value right);
    [[nodiscard]] std::size_t checked_slot(ObjectId id) const;
    [[nodiscard]] Object& mutable_object(ObjectId id);
    void register_handle_root(Value* slot);
    void deregister_handle_root(Value* slot) noexcept;
    void move_handle_root(Value* from, Value* to) noexcept;
    void trace_handle_roots(RootVisitor& visitor) const;
    void collect_impl(CollectionKind kind, RootProvider* roots, std::span<Value*> extra_roots);
    void collect_with_extra_roots(std::span<Value*> extra_roots);
    void trace_collection_roots(RootVisitor& visitor, RootProvider* roots,
                                std::span<Value*> extra_roots) const;
    void store_pair_field(ObjectId id, PairField field, Value value);
    [[nodiscard]] bool record_write_barrier_if_needed(ObjectId owner, Value value);
    void record_remembered_object(ObjectId id);
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
    [[nodiscard]] std::vector<ObjectId> rewrite_remembered_set(
        const ForwardingTable& forwarding) const;
    void prune_remembered_set();
    void validate_after_collection(RootProvider* roots, std::span<Value*> extra_roots) const;
    void validate_remembered_set() const;
    void validate_value(Value value) const;

    RootProvider* root_provider_{nullptr};
    std::shared_ptr<HeapLifetime> lifetime_;
    StressConfig stress_config_{};
    std::vector<std::optional<Object>> objects_;
    std::vector<std::uint32_t> generations_;
    std::vector<ObjectId> remembered_set_;
    std::vector<Value*> handle_roots_;
    HeapMetrics metrics_{};
    bool TEST_ONLY_skip_next_write_barrier_{false};
    mutable std::uint64_t TEST_ONLY_validation_count_{0};
};

} // namespace lang::gc
