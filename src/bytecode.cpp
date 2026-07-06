#include "lang/bytecode.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace lang {

namespace {

enum class AbstractKind {
    Int64,
    Bool,
    Object,
    Nil,
    Poison,
};

struct PairFields;

struct AbstractValue {
    AbstractKind kind{AbstractKind::Poison};
    std::set<std::size_t> object_sites;
    bool includes_opaque_object{false};
    std::shared_ptr<const PairFields> signature_fields;
};

struct PairFields {
    AbstractValue left;
    AbstractValue right;
};

struct AbstractState {
    std::vector<AbstractValue> stack;
    std::vector<std::optional<AbstractValue>> locals;
    std::map<std::size_t, PairFields> fields_by_site;
};

enum class JoinOutcome {
    Unchanged,
    Changed,
    Invalid,
};

AbstractValue value_with_kind(AbstractKind kind) {
    AbstractValue value;
    value.kind = kind;
    return value;
}

AbstractValue int64_value() { return value_with_kind(AbstractKind::Int64); }
AbstractValue bool_value() { return value_with_kind(AbstractKind::Bool); }
AbstractValue poison_value() { return value_with_kind(AbstractKind::Poison); }

AbstractValue object_value(std::size_t site) {
    AbstractValue value;
    value.kind = AbstractKind::Object;
    value.object_sites.insert(site);
    return value;
}

AbstractValue opaque_object_value() {
    AbstractValue value;
    value.kind = AbstractKind::Object;
    value.includes_opaque_object = true;
    return value;
}

std::shared_ptr<const PairFields> fields_ptr(PairFields fields) {
    return std::make_shared<const PairFields>(std::move(fields));
}

AbstractKind abstract_kind(ValueKind kind) {
    switch (kind) {
    case ValueKind::Int64:
        return AbstractKind::Int64;
    case ValueKind::Bool:
        return AbstractKind::Bool;
    case ValueKind::Object:
        return AbstractKind::Object;
    case ValueKind::Nil:
        return AbstractKind::Nil;
    }
    return AbstractKind::Poison;
}

AbstractValue value_from_signature(ValueKind kind) {
    if (kind == ValueKind::Object) {
        return opaque_object_value();
    }
    return value_with_kind(abstract_kind(kind));
}

AbstractValue value_from_signature(const SignatureValue& signature) {
    if (signature.has_pair_fields()) {
        AbstractValue value;
        value.kind = AbstractKind::Object;
        value.signature_fields = fields_ptr(PairFields{
            value_from_signature(*signature.left),
            value_from_signature(*signature.right),
        });
        return value;
    }
    return value_from_signature(signature.kind);
}

SignatureValue parameter_signature(const FunctionSignature& signature,
                                   std::size_t index) {
    if (signature.parameter_types.empty()) {
        return signature_value(signature.parameters[index]);
    }
    return signature.parameter_types[index];
}

SignatureValue return_signature(const FunctionSignature& signature) {
    if (signature.return_type_detail.has_value()) {
        return *signature.return_type_detail;
    }
    return signature_value(signature.return_type);
}

bool signature_shape_matches_kind(const SignatureValue& signature, ValueKind kind) {
    if (signature.kind != kind) {
        return false;
    }
    if (signature.has_pair_fields()) {
        return signature_shape_matches_kind(*signature.left, signature.left->kind) &&
               signature_shape_matches_kind(*signature.right, signature.right->kind);
    }
    return signature.left == nullptr && signature.right == nullptr;
}

bool signature_is_well_formed(const FunctionSignature& signature) {
    if (!signature.parameter_types.empty() &&
        signature.parameter_types.size() != signature.parameters.size()) {
        return false;
    }
    for (std::size_t i = 0; i < signature.parameter_types.size(); ++i) {
        if (!signature_shape_matches_kind(signature.parameter_types[i],
                                          signature.parameters[i])) {
            return false;
        }
    }
    if (signature.return_type_detail.has_value() &&
        !signature_shape_matches_kind(*signature.return_type_detail,
                                      signature.return_type)) {
        return false;
    }
    return true;
}

bool fields_equal(const std::shared_ptr<const PairFields>& lhs,
                  const std::shared_ptr<const PairFields>& rhs);

bool operator==(const AbstractValue& lhs, const AbstractValue& rhs) {
    return lhs.kind == rhs.kind && lhs.object_sites == rhs.object_sites &&
           lhs.includes_opaque_object == rhs.includes_opaque_object &&
           fields_equal(lhs.signature_fields, rhs.signature_fields);
}

bool operator==(const PairFields& lhs, const PairFields& rhs) {
    return lhs.left == rhs.left && lhs.right == rhs.right;
}

bool fields_equal(const std::shared_ptr<const PairFields>& lhs,
                  const std::shared_ptr<const PairFields>& rhs) {
    if (lhs == nullptr || rhs == nullptr) {
        return lhs == rhs;
    }
    return *lhs == *rhs;
}

bool is_valid_local_index(std::int64_t operand, std::uint32_t local_count) {
    return operand >= 0 && static_cast<std::uint64_t>(operand) < local_count;
}

bool is_valid_target(std::int64_t operand, std::size_t code_size) {
    return operand >= 0 && static_cast<std::uint64_t>(operand) < code_size;
}

bool is_poison(const AbstractValue& value) {
    return value.kind == AbstractKind::Poison;
}

AbstractValue join_values(const AbstractValue& lhs, const AbstractValue& rhs) {
    if (lhs.kind != rhs.kind || is_poison(lhs) || is_poison(rhs)) {
        return poison_value();
    }

    AbstractValue joined = lhs;
    if (joined.kind == AbstractKind::Object) {
        joined.object_sites.insert(rhs.object_sites.begin(), rhs.object_sites.end());
        joined.includes_opaque_object =
            lhs.includes_opaque_object || rhs.includes_opaque_object;
        if (lhs.signature_fields != nullptr && rhs.signature_fields != nullptr) {
            joined.signature_fields = fields_ptr(PairFields{
                join_values(lhs.signature_fields->left, rhs.signature_fields->left),
                join_values(lhs.signature_fields->right, rhs.signature_fields->right),
            });
        } else if (lhs.signature_fields != nullptr) {
            joined.signature_fields = lhs.signature_fields;
        } else {
            joined.signature_fields = rhs.signature_fields;
        }
    }
    return joined;
}

bool join_value_into(AbstractValue& destination, const AbstractValue& incoming) {
    const auto joined = join_values(destination, incoming);
    if (joined == destination) {
        return false;
    }
    destination = joined;
    return true;
}

JoinOutcome join_state_into(AbstractState& destination, const AbstractState& incoming) {
    if (destination.stack.size() != incoming.stack.size() ||
        destination.locals.size() != incoming.locals.size()) {
        return JoinOutcome::Invalid;
    }

    bool changed = false;

    for (std::size_t i = 0; i < destination.stack.size(); ++i) {
        changed = join_value_into(destination.stack[i], incoming.stack[i]) || changed;
    }

    for (std::size_t i = 0; i < destination.locals.size(); ++i) {
        auto& dest_local = destination.locals[i];
        const auto& incoming_local = incoming.locals[i];
        if (!dest_local.has_value() && !incoming_local.has_value()) {
            continue;
        }
        if (!dest_local.has_value() || !incoming_local.has_value()) {
            if (dest_local.has_value()) {
                dest_local.reset();
                changed = true;
            }
            continue;
        }
        changed = join_value_into(*dest_local, *incoming_local) || changed;
    }

    for (const auto& [site, incoming_fields] : incoming.fields_by_site) {
        auto [it, inserted] = destination.fields_by_site.emplace(site, incoming_fields);
        if (inserted) {
            changed = true;
            continue;
        }
        changed = join_value_into(it->second.left, incoming_fields.left) || changed;
        changed = join_value_into(it->second.right, incoming_fields.right) || changed;
    }

    return changed ? JoinOutcome::Changed : JoinOutcome::Unchanged;
}

bool stack_map_matches(const StackMap& map, const std::vector<AbstractValue>& stack) {
    if (map.object_slots.size() != stack.size()) {
        return false;
    }
    for (std::size_t i = 0; i < stack.size(); ++i) {
        if (is_poison(stack[i])) {
            return false;
        }
        const bool is_object = stack[i].kind == AbstractKind::Object;
        if (map.object_slots[i] != is_object) {
            return false;
        }
    }
    return true;
}

std::optional<StackMap> stack_map_from_state(const AbstractState& state) {
    StackMap map;
    map.object_slots.reserve(state.stack.size());
    for (const auto& value : state.stack) {
        if (is_poison(value)) {
            return std::nullopt;
        }
        map.object_slots.push_back(value.kind == AbstractKind::Object);
    }
    return map;
}

bool pop_any(AbstractState& state, AbstractValue* out = nullptr) {
    if (state.stack.empty()) {
        return false;
    }
    const auto value = state.stack.back();
    if (is_poison(value)) {
        return false;
    }
    state.stack.pop_back();
    if (out != nullptr) {
        *out = value;
    }
    return true;
}

bool pop_expect(AbstractState& state, AbstractKind expected, AbstractValue* out = nullptr) {
    AbstractValue actual;
    if (!pop_any(state, &actual)) {
        return false;
    }
    if (actual.kind != expected) {
        return false;
    }
    if (out != nullptr) {
        *out = actual;
    }
    return true;
}

bool pop_expect_signature_kind(AbstractState& state, ValueKind expected,
                               AbstractValue* out = nullptr) {
    return pop_expect(state, abstract_kind(expected), out);
}

bool push_fallthrough(std::size_t pc, std::size_t code_size, const AbstractState& state,
                      std::vector<std::pair<std::size_t, AbstractState>>& successors) {
    const auto next_pc = pc + 1;
    if (next_pc >= code_size) {
        return false;
    }
    successors.push_back({next_pc, state});
    return true;
}

bool load_pair_field(const AbstractState& state, const AbstractValue& receiver, bool left,
                     AbstractValue& out) {
    if (receiver.includes_opaque_object) {
        return false;
    }

    std::optional<AbstractValue> loaded;
    if (receiver.signature_fields != nullptr) {
        loaded = left ? receiver.signature_fields->left : receiver.signature_fields->right;
    }

    for (const auto site : receiver.object_sites) {
        const auto it = state.fields_by_site.find(site);
        if (it == state.fields_by_site.end()) {
            return false;
        }
        const auto& field = left ? it->second.left : it->second.right;
        if (!loaded.has_value()) {
            loaded = field;
        } else {
            *loaded = join_values(*loaded, field);
        }
    }
    if (!loaded.has_value() || is_poison(*loaded)) {
        return false;
    }
    out = *loaded;
    return true;
}

bool value_conforms_to_expected(const AbstractState& state, const AbstractValue& value,
                                const AbstractValue& expected);

bool value_conforms_to_signature(const AbstractState& state, const AbstractValue& value,
                                 const SignatureValue& signature) {
    return value_conforms_to_expected(state, value, value_from_signature(signature));
}

bool value_conforms_to_expected(const AbstractState& state, const AbstractValue& value,
                                const AbstractValue& expected) {
    if (is_poison(value) || is_poison(expected) || value.kind != expected.kind) {
        return false;
    }
    if (expected.kind != AbstractKind::Object) {
        return true;
    }
    if (expected.signature_fields == nullptr) {
        return true;
    }

    AbstractValue value_left;
    AbstractValue value_right;
    return load_pair_field(state, value, true, value_left) &&
           load_pair_field(state, value, false, value_right) &&
           value_conforms_to_expected(state, value_left, expected.signature_fields->left) &&
           value_conforms_to_expected(state, value_right, expected.signature_fields->right);
}

bool store_pair_field(AbstractState& state, const AbstractValue& receiver, bool left,
                      const AbstractValue& value) {
    if (receiver.includes_opaque_object && receiver.signature_fields != nullptr) {
        return false;
    }

    if (receiver.signature_fields != nullptr) {
        const auto& expected =
            left ? receiver.signature_fields->left : receiver.signature_fields->right;
        if (!value_conforms_to_expected(state, value, expected)) {
            return false;
        }
    }

    for (const auto site : receiver.object_sites) {
        const auto it = state.fields_by_site.find(site);
        if (it == state.fields_by_site.end()) {
            return false;
        }
        auto& field = left ? it->second.left : it->second.right;
        (void)join_value_into(field, value);
    }
    return receiver.signature_fields != nullptr || !receiver.object_sites.empty() ||
           receiver.includes_opaque_object;
}

bool transfer_instruction(const Module& module, std::size_t function_index, std::size_t pc,
                          const AbstractState& in,
                          std::vector<std::pair<std::size_t, AbstractState>>& successors) {
    const auto& function = module.functions[function_index];
    const auto& ins = function.code[pc];
    auto state = in;

    switch (ins.op) {
    case OpCode::ConstantI64:
        state.stack.push_back(int64_value());
        return push_fallthrough(pc, function.code.size(), state, successors);
    case OpCode::AddI64:
        if (!pop_expect(state, AbstractKind::Int64) ||
            !pop_expect(state, AbstractKind::Int64)) {
            return false;
        }
        state.stack.push_back(int64_value());
        return push_fallthrough(pc, function.code.size(), state, successors);
    case OpCode::LessI64:
        if (!pop_expect(state, AbstractKind::Int64) ||
            !pop_expect(state, AbstractKind::Int64)) {
            return false;
        }
        state.stack.push_back(bool_value());
        return push_fallthrough(pc, function.code.size(), state, successors);
    case OpCode::AllocPair: {
        AbstractValue right;
        AbstractValue left;
        if (!pop_any(state, &right) || !pop_any(state, &left)) {
            return false;
        }
        const PairFields allocated{left, right};
        auto [it, inserted] = state.fields_by_site.emplace(pc, allocated);
        if (!inserted) {
            (void)join_value_into(it->second.left, allocated.left);
            (void)join_value_into(it->second.right, allocated.right);
        }
        state.stack.push_back(object_value(pc));
        return push_fallthrough(pc, function.code.size(), state, successors);
    }
    case OpCode::GetLeft:
    case OpCode::GetRight: {
        AbstractValue receiver;
        AbstractValue loaded;
        if (!pop_expect(state, AbstractKind::Object, &receiver) ||
            !load_pair_field(state, receiver, ins.op == OpCode::GetLeft, loaded)) {
            return false;
        }
        state.stack.push_back(loaded);
        return push_fallthrough(pc, function.code.size(), state, successors);
    }
    case OpCode::SetLeft:
    case OpCode::SetRight: {
        AbstractValue value;
        AbstractValue receiver;
        if (!pop_any(state, &value) || !pop_expect(state, AbstractKind::Object, &receiver) ||
            !store_pair_field(state, receiver, ins.op == OpCode::SetLeft, value)) {
            return false;
        }
        return push_fallthrough(pc, function.code.size(), state, successors);
    }
    case OpCode::LoadLocal: {
        if (!is_valid_local_index(ins.operand, function.local_count)) {
            return false;
        }
        const auto local_index = static_cast<std::size_t>(ins.operand);
        if (!state.locals[local_index].has_value() || is_poison(*state.locals[local_index])) {
            return false;
        }
        state.stack.push_back(*state.locals[local_index]);
        return push_fallthrough(pc, function.code.size(), state, successors);
    }
    case OpCode::StoreLocal: {
        if (!is_valid_local_index(ins.operand, function.local_count)) {
            return false;
        }
        AbstractValue stored;
        if (!pop_any(state, &stored)) {
            return false;
        }
        state.locals[static_cast<std::size_t>(ins.operand)] = stored;
        return push_fallthrough(pc, function.code.size(), state, successors);
    }
    case OpCode::Jump:
        if (!is_valid_target(ins.operand, function.code.size())) {
            return false;
        }
        successors.push_back({static_cast<std::size_t>(ins.operand), state});
        return true;
    case OpCode::JumpIfFalse:
        if (!is_valid_target(ins.operand, function.code.size()) ||
            !pop_expect(state, AbstractKind::Bool)) {
            return false;
        }
        successors.push_back({static_cast<std::size_t>(ins.operand), state});
        return push_fallthrough(pc, function.code.size(), state, successors);
    case OpCode::Collect:
        return push_fallthrough(pc, function.code.size(), state, successors);
    case OpCode::Call: {
        if (ins.operand < 0 ||
            static_cast<std::uint64_t>(ins.operand) >= module.functions.size()) {
            return false;
        }
        const auto& callee =
            module.functions[static_cast<std::size_t>(ins.operand)].signature;
        for (std::size_t i = callee.parameters.size(); i > 0; --i) {
            AbstractValue argument;
            const auto parameter_index = i - 1;
            if (!pop_expect_signature_kind(state, callee.parameters[parameter_index],
                                           &argument) ||
                !value_conforms_to_signature(state, argument,
                                             parameter_signature(callee,
                                                                 parameter_index))) {
                return false;
            }
        }
        state.stack.push_back(value_from_signature(return_signature(callee)));
        return push_fallthrough(pc, function.code.size(), state, successors);
    }
    case OpCode::Return: {
        AbstractValue returned;
        return pop_any(state, &returned) &&
               value_conforms_to_signature(state, returned,
                                           return_signature(function.signature));
    }
    }
    return false;
}

} // namespace

std::optional<VerificationResult> verify_function_with_stack_maps(const Module& module,
                                                                  std::size_t function_index) {
    const auto& function = module.functions[function_index];
    if (!function.stack_maps.empty() && function.stack_maps.size() != function.code.size()) {
        return std::nullopt;
    }
    if (function.code.empty()) {
        return std::nullopt;
    }
    if (!signature_is_well_formed(function.signature)) {
        return std::nullopt;
    }
    if (function.local_count < function.signature.parameters.size()) {
        return std::nullopt;
    }

    std::vector<std::optional<AbstractState>> states(function.code.size());
    std::deque<std::size_t> worklist;

    AbstractState initial;
    initial.locals.resize(function.local_count);
    for (std::size_t i = 0; i < function.signature.parameters.size(); ++i) {
        initial.locals[i] = value_from_signature(parameter_signature(function.signature, i));
    }
    states[0] = initial;
    worklist.push_back(0);

    while (!worklist.empty()) {
        const auto pc = worklist.front();
        worklist.pop_front();

        std::vector<std::pair<std::size_t, AbstractState>> successors;
        if (!transfer_instruction(module, function_index, pc, *states[pc], successors)) {
            return std::nullopt;
        }

        for (auto& [target, state] : successors) {
            if (!states[target].has_value()) {
                states[target] = state;
                worklist.push_back(target);
                continue;
            }

            const auto outcome = join_state_into(*states[target], state);
            if (outcome == JoinOutcome::Invalid) {
                return std::nullopt;
            }
            if (outcome == JoinOutcome::Changed) {
                worklist.push_back(target);
            }
        }
    }

    VerificationResult result;
    result.stack_maps.resize(function.code.size());
    for (std::size_t pc = 0; pc < function.code.size(); ++pc) {
        if (!states[pc].has_value()) {
            return std::nullopt;
        }
        auto map = stack_map_from_state(*states[pc]);
        if (!map.has_value()) {
            return std::nullopt;
        }
        if (!function.stack_maps.empty() &&
            !stack_map_matches(function.stack_maps[pc], states[pc]->stack)) {
            return std::nullopt;
        }
        result.stack_maps[pc] = *map;
    }

    return result;
}

std::optional<VerificationResult> verify_with_stack_maps(const Function& function) {
    Module module;
    module.entry_function = 0;
    module.functions.push_back(function);
    auto result = verify_with_stack_maps(module);
    if (!result.has_value() || result->functions.empty()) {
        return std::nullopt;
    }
    return result->functions.front();
}

std::optional<ModuleVerificationResult> verify_with_stack_maps(const Module& module) {
    if (module.functions.empty() || module.entry_function >= module.functions.size()) {
        return std::nullopt;
    }

    ModuleVerificationResult result;
    result.functions.reserve(module.functions.size());
    for (std::size_t i = 0; i < module.functions.size(); ++i) {
        auto function_result = verify_function_with_stack_maps(module, i);
        if (!function_result.has_value()) {
            return std::nullopt;
        }
        result.functions.push_back(std::move(*function_result));
    }
    return result;
}

bool verify(const Function& function) {
    return verify_with_stack_maps(function).has_value();
}

bool verify(const Module& module) {
    return verify_with_stack_maps(module).has_value();
}

} // namespace lang
