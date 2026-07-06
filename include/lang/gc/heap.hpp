#pragma once

#include "lang/value.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace lang::gc {

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
    std::uint64_t collect_every_n_instructions{0};
};

struct Object {
    // Object layout assumption: a pair stores two full tagged Values.
    // The marker must inspect the Value tag before following a field as an ObjectId.
    bool marked{false};
    Value left{Value::nil()};
    Value right{Value::nil()};
};

class Heap {
public:
    ObjectId allocate_pair(Value left, Value right);
    void set_root_provider(RootProvider* provider) { root_provider_ = provider; }
    void collect();
    void collect(RootProvider& roots);
    void set_stress_config(StressConfig config) { stress_config_ = config; }

    [[nodiscard]] const Object& object(ObjectId id) const;
    [[nodiscard]] Object& object(ObjectId id);
    [[nodiscard]] Value left(ObjectId id) const;
    [[nodiscard]] Value right(ObjectId id) const;
    void set_left(ObjectId id, Value value);
    void set_right(ObjectId id, Value value);
    [[nodiscard]] std::size_t live_count() const;
    [[nodiscard]] std::size_t capacity_slots() const { return objects_.size(); }
    [[nodiscard]] StressConfig stress_config() const { return stress_config_; }

private:
    class MarkingVisitor;
    enum class PairField { Left, Right };

    ObjectId allocate_slot(Value left, Value right);
    [[nodiscard]] std::size_t checked_slot(ObjectId id) const;
    void collect_with_extra_roots(std::span<Value*> extra_roots);
    void store_pair_field(ObjectId id, PairField field, Value value);
    void mark_value(Value value);
    void mark_object(ObjectId id);
    void sweep();

    RootProvider* root_provider_{nullptr};
    StressConfig stress_config_{};
    std::vector<std::optional<Object>> objects_;
    std::vector<std::uint32_t> generations_;
};

} // namespace lang::gc
