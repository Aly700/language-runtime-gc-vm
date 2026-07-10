#include "lang/bytecode.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lang {

namespace {

enum class AbstractKind {
    Int64,
    Bool,
    Object,
    Array,
    RefArray,
    Str,
    Function,
    Map,
    Nil,
    Poison,
};

struct PairFields;

struct AbstractValue {
    AbstractKind kind{AbstractKind::Poison};
    std::set<std::size_t> object_sites;
    std::set<std::size_t> ref_array_sites;
    bool includes_opaque_object{false};
    bool includes_nil{false};
    std::shared_ptr<const PairFields> signature_fields;
    std::shared_ptr<const AbstractValue> signature_array_element;
    std::shared_ptr<const SignatureValue> signature_function;
    std::shared_ptr<const SignatureValue> signature_map;
    std::optional<std::size_t> signature_named_type;
    std::optional<std::size_t> source_local;
    std::optional<std::size_t> nil_test_local;
};

struct PairFields {
    AbstractValue left;
    AbstractValue right;
};

struct AbstractState {
    std::vector<AbstractValue> stack;
    std::vector<std::optional<AbstractValue>> locals;
    std::map<std::size_t, PairFields> fields_by_site;
    std::map<std::size_t, AbstractValue> ref_array_elements_by_site;
};

enum class JoinOutcome {
    Unchanged,
    Changed,
    Invalid,
};

const char* op_name(OpCode op) {
    switch (op) {
    case OpCode::ConstantI64:
        return "ConstantI64";
    case OpCode::AddI64:
        return "AddI64";
    case OpCode::LessI64:
        return "LessI64";
    case OpCode::AllocPair:
        return "AllocPair";
    case OpCode::GetLeft:
        return "GetLeft";
    case OpCode::GetRight:
        return "GetRight";
    case OpCode::SetLeft:
        return "SetLeft";
    case OpCode::SetRight:
        return "SetRight";
    case OpCode::AllocArray:
        return "AllocArray";
    case OpCode::ArrayGet:
        return "ArrayGet";
    case OpCode::ArraySet:
        return "ArraySet";
    case OpCode::ArrayLen:
        return "ArrayLen";
    case OpCode::LoadLocal:
        return "LoadLocal";
    case OpCode::StoreLocal:
        return "StoreLocal";
    case OpCode::Jump:
        return "Jump";
    case OpCode::JumpIfFalse:
        return "JumpIfFalse";
    case OpCode::Collect:
        return "Collect";
    case OpCode::Call:
        return "Call";
    case OpCode::Return:
        return "Return";
    case OpCode::Nil:
        return "Nil";
    case OpCode::IsNil:
        return "IsNil";
    case OpCode::AllocRefArray:
        return "AllocRefArray";
    case OpCode::RefArrayGet:
        return "RefArrayGet";
    case OpCode::RefArraySet:
        return "RefArraySet";
    case OpCode::PushStr:
        return "PushStr";
    case OpCode::StrLen:
        return "StrLen";
    case OpCode::StrEq:
        return "StrEq";
    case OpCode::StrConcat:
        return "StrConcat";
    case OpCode::StrIndex:
        return "StrIndex";
    case OpCode::AllocClosure:
        return "AllocClosure";
    case OpCode::CallClosure:
        return "CallClosure";
    case OpCode::LoadCapture:
        return "LoadCapture";
    case OpCode::AllocMap:
        return "AllocMap";
    case OpCode::MapSet:
        return "MapSet";
    case OpCode::MapGet:
        return "MapGet";
    case OpCode::MapHas:
        return "MapHas";
    case OpCode::MapLen:
        return "MapLen";
    }
    return "<invalid>";
}

const char* value_kind_name(ValueKind kind) {
    switch (kind) {
    case ValueKind::Int64:
        return "Int64";
    case ValueKind::Bool:
        return "Bool";
    case ValueKind::Object:
        return "Object";
    case ValueKind::Array:
        return "Array";
    case ValueKind::Nil:
        return "Nil";
    case ValueKind::Str:
        return "Str";
    case ValueKind::Function:
        return "Function";
    case ValueKind::Map:
        return "Map";
    }
    return "<invalid>";
}

const char* abstract_kind_name(AbstractKind kind) {
    switch (kind) {
    case AbstractKind::Int64:
        return "Int64";
    case AbstractKind::Bool:
        return "Bool";
    case AbstractKind::Object:
        return "Object";
    case AbstractKind::Array:
        return "Array";
    case AbstractKind::RefArray:
        return "RefArray";
    case AbstractKind::Str:
        return "Str";
    case AbstractKind::Function:
        return "Function";
    case AbstractKind::Map:
        return "Map";
    case AbstractKind::Nil:
        return "Nil";
    case AbstractKind::Poison:
        return "Poison";
    }
    return "<invalid>";
}

bool reject(std::vector<VerifierDiagnostic>& diagnostics,
            std::size_t function_index,
            std::optional<std::size_t> pc,
            VerifierReason reason,
            std::string message) {
    diagnostics.push_back(
        VerifierDiagnostic{function_index, pc, reason, std::move(message)});
    return false;
}

std::string instruction_message(const Function& function, std::size_t pc,
                                std::string_view detail) {
    std::ostringstream out;
    out << op_name(function.code[pc].op) << " at pc " << pc << ": " << detail;
    return out.str();
}

AbstractValue value_with_kind(AbstractKind kind) {
    AbstractValue value;
    value.kind = kind;
    return value;
}

AbstractValue int64_value() { return value_with_kind(AbstractKind::Int64); }
AbstractValue bool_value() { return value_with_kind(AbstractKind::Bool); }
AbstractValue array_value() { return value_with_kind(AbstractKind::Array); }
AbstractValue ref_array_value() { return value_with_kind(AbstractKind::RefArray); }
AbstractValue str_value() { return value_with_kind(AbstractKind::Str); }

AbstractValue map_value(SignatureValue signature) {
    AbstractValue value;
    value.kind = AbstractKind::Map;
    value.signature_map =
        std::make_shared<const SignatureValue>(std::move(signature));
    return value;
}

bool signature_values_equal(const SignatureValue& lhs,
                            const SignatureValue& rhs);

bool signature_value_ptr_equal(const std::shared_ptr<SignatureValue>& lhs,
                               const std::shared_ptr<SignatureValue>& rhs) {
    if (lhs == nullptr || rhs == nullptr) {
        return lhs == rhs;
    }
    return signature_values_equal(*lhs, *rhs);
}

bool signature_values_equal(const SignatureValue& lhs,
                            const SignatureValue& rhs) {
    if (lhs.kind != rhs.kind || lhs.named_type != rhs.named_type ||
        lhs.function_parameters.size() != rhs.function_parameters.size() ||
        !signature_value_ptr_equal(lhs.left, rhs.left) ||
        !signature_value_ptr_equal(lhs.right, rhs.right) ||
        !signature_value_ptr_equal(lhs.element, rhs.element) ||
        !signature_value_ptr_equal(lhs.key, rhs.key) ||
        !signature_value_ptr_equal(lhs.value, rhs.value) ||
        !signature_value_ptr_equal(lhs.function_return, rhs.function_return)) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.function_parameters.size(); ++i) {
        if (!signature_values_equal(lhs.function_parameters[i],
                                    rhs.function_parameters[i])) {
            return false;
        }
    }
    return true;
}

AbstractValue ref_array_value(std::size_t site) {
    AbstractValue value;
    value.kind = AbstractKind::RefArray;
    value.ref_array_sites.insert(site);
    return value;
}
AbstractValue poison_value() { return value_with_kind(AbstractKind::Poison); }

AbstractValue nil_object_value() {
    AbstractValue value;
    value.kind = AbstractKind::Object;
    value.includes_nil = true;
    return value;
}

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

AbstractValue named_signature_object_value(std::size_t index) {
    AbstractValue value;
    value.kind = AbstractKind::Object;
    value.includes_nil = true;
    value.signature_named_type = index;
    return value;
}

AbstractValue without_provenance(AbstractValue value) {
    value.source_local.reset();
    value.nil_test_local.reset();
    return value;
}

std::shared_ptr<const PairFields> fields_ptr(PairFields fields) {
    return std::make_shared<const PairFields>(std::move(fields));
}

std::shared_ptr<const AbstractValue> abstract_value_ptr(AbstractValue value) {
    return std::make_shared<const AbstractValue>(std::move(value));
}

AbstractKind abstract_kind(ValueKind kind) {
    switch (kind) {
    case ValueKind::Int64:
        return AbstractKind::Int64;
    case ValueKind::Bool:
        return AbstractKind::Bool;
    case ValueKind::Object:
        return AbstractKind::Object;
    case ValueKind::Array:
        return AbstractKind::Array;
    case ValueKind::Nil:
        return AbstractKind::Nil;
    case ValueKind::Str:
        return AbstractKind::Str;
    case ValueKind::Function:
        return AbstractKind::Function;
    case ValueKind::Map:
        return AbstractKind::Map;
    }
    return AbstractKind::Poison;
}

AbstractValue value_from_signature(ValueKind kind) {
    if (kind == ValueKind::Object) {
        return opaque_object_value();
    }
    return value_with_kind(abstract_kind(kind));
}

bool signature_array_uses_ref_payload(const SignatureValue& signature) {
    return signature.has_array_element() &&
           signature_value_is_reference(*signature.element);
}

AbstractValue value_from_signature(const SignatureValue& signature) {
    if (signature.has_function_signature()) {
        AbstractValue value;
        value.kind = AbstractKind::Function;
        value.signature_function =
            std::make_shared<const SignatureValue>(signature);
        return value;
    }
    if (signature.has_map_entries()) {
        return map_value(signature);
    }
    if (signature.is_named_type_reference()) {
        return named_signature_object_value(*signature.named_type);
    }
    if (signature.has_pair_fields()) {
        AbstractValue value;
        value.kind = AbstractKind::Object;
        value.signature_fields = fields_ptr(PairFields{
            value_from_signature(*signature.left),
            value_from_signature(*signature.right),
        });
        return value;
    }
    if (signature.has_array_element()) {
        if (!signature_array_uses_ref_payload(signature)) {
            return array_value();
        }
        auto value = ref_array_value();
        auto element = value_from_signature(*signature.element);
        if (element.kind == AbstractKind::Object) {
            element.includes_nil = false;
        }
        value.signature_array_element = abstract_value_ptr(std::move(element));
        return value;
    }
    return value_from_signature(signature.kind);
}

bool is_reference_kind(AbstractKind kind) {
    return kind == AbstractKind::Object || kind == AbstractKind::Array ||
           kind == AbstractKind::RefArray || kind == AbstractKind::Str ||
           kind == AbstractKind::Function || kind == AbstractKind::Map;
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

SignatureValue function_type_from_signature(const FunctionSignature& signature) {
    std::vector<SignatureValue> parameters;
    parameters.reserve(signature.parameters.size());
    for (std::size_t i = 0; i < signature.parameters.size(); ++i) {
        parameters.push_back(parameter_signature(signature, i));
    }
    return function_signature(std::move(parameters), return_signature(signature));
}

bool signature_shape_matches_kind(const SignatureValue& signature, ValueKind kind,
                                  const Module& module) {
    if (signature.kind != kind) {
        return false;
    }
    if (signature.is_named_type_reference()) {
        return signature.kind == ValueKind::Object &&
               *signature.named_type < module.named_types.size() &&
               signature.left == nullptr && signature.right == nullptr &&
               signature.element == nullptr && signature.key == nullptr &&
               signature.value == nullptr &&
               signature.function_return == nullptr &&
               signature.function_parameters.empty();
    }
    if (signature.has_pair_fields()) {
        return signature_shape_matches_kind(*signature.left, signature.left->kind,
                                            module) &&
               signature_shape_matches_kind(*signature.right, signature.right->kind,
                                            module) &&
               signature.element == nullptr &&
               signature.key == nullptr && signature.value == nullptr &&
               signature.function_return == nullptr &&
               signature.function_parameters.empty();
    }
    if (signature.has_array_element()) {
        return signature.kind == ValueKind::Array &&
               signature_shape_matches_kind(*signature.element,
                                            signature.element->kind, module) &&
               signature.left == nullptr && signature.right == nullptr &&
               signature.key == nullptr && signature.value == nullptr &&
               signature.function_return == nullptr &&
               signature.function_parameters.empty();
    }
    if (signature.has_function_signature()) {
        if (!signature_shape_matches_kind(*signature.function_return,
                                          signature.function_return->kind,
                                          module)) {
            return false;
        }
        for (const auto& parameter : signature.function_parameters) {
            if (!signature_shape_matches_kind(parameter, parameter.kind, module)) {
                return false;
            }
        }
        return true;
    }
    if (signature.has_map_entries()) {
        return signature_shape_matches_kind(*signature.key,
                                            signature.key->kind, module) &&
               signature_shape_matches_kind(*signature.value,
                                            signature.value->kind, module);
    }
    if (signature.kind == ValueKind::Function) {
        return false;
    }
    if (signature.kind == ValueKind::Map) {
        return false;
    }
    return signature.left == nullptr && signature.right == nullptr &&
           signature.element == nullptr && signature.function_return == nullptr &&
           signature.key == nullptr && signature.value == nullptr &&
           signature.function_parameters.empty() && !signature.named_type.has_value();
}

bool is_valid_map_key_type(const SignatureValue& key) {
    return key.kind == ValueKind::Int64 || key.kind == ValueKind::Bool ||
           key.kind == ValueKind::Str;
}

bool signature_has_invalid_map_key(const SignatureValue& signature) {
    if (signature.has_map_entries()) {
        return !is_valid_map_key_type(*signature.key) ||
               signature_has_invalid_map_key(*signature.key) ||
               signature_has_invalid_map_key(*signature.value);
    }
    if (signature.has_pair_fields()) {
        return signature_has_invalid_map_key(*signature.left) ||
               signature_has_invalid_map_key(*signature.right);
    }
    if (signature.has_array_element()) {
        return signature_has_invalid_map_key(*signature.element);
    }
    if (signature.has_function_signature()) {
        if (signature_has_invalid_map_key(*signature.function_return)) {
            return true;
        }
        for (const auto& parameter : signature.function_parameters) {
            if (signature_has_invalid_map_key(parameter)) {
                return true;
            }
        }
    }
    return false;
}

bool module_map_key_types_are_valid(
    const Module& module, std::vector<VerifierDiagnostic>& diagnostics) {
    for (const auto& type : module.named_types) {
        if (signature_has_invalid_map_key(type.body)) {
            return reject(diagnostics, 0, std::nullopt,
                          VerifierReason::InvalidMapKeyType,
                          "module named type contains a map key other than i64, bool, or str");
        }
    }
    for (std::size_t function_index = 0;
         function_index < module.functions.size(); ++function_index) {
        const auto& signature = module.functions[function_index].signature;
        for (std::size_t i = 0; i < signature.parameters.size(); ++i) {
            if (signature_has_invalid_map_key(parameter_signature(signature, i))) {
                return reject(diagnostics, function_index, std::nullopt,
                              VerifierReason::InvalidMapKeyType,
                              "function parameter contains a map key other than i64, bool, or str");
            }
        }
        if (signature_has_invalid_map_key(return_signature(signature))) {
            return reject(diagnostics, function_index, std::nullopt,
                          VerifierReason::InvalidMapKeyType,
                          "function return contains a map key other than i64, bool, or str");
        }
    }
    for (const auto& layout : module.closure_layouts) {
        if (signature_has_invalid_map_key(layout.function_type)) {
            return reject(diagnostics, layout.function_index, std::nullopt,
                          VerifierReason::InvalidMapKeyType,
                          "closure function type contains an invalid map key");
        }
        for (const auto& capture : layout.capture_types) {
            if (signature_has_invalid_map_key(capture)) {
                return reject(diagnostics, layout.function_index, std::nullopt,
                              VerifierReason::InvalidMapKeyType,
                              "closure capture type contains an invalid map key");
            }
        }
    }
    for (const auto& layout : module.map_layouts) {
        if (!is_valid_map_key_type(layout.key_type) ||
            signature_has_invalid_map_key(layout.value_type)) {
            return reject(diagnostics, 0, std::nullopt,
                          VerifierReason::InvalidMapKeyType,
                          "map layout contains a key type other than i64, bool, or str");
        }
    }
    return true;
}

bool map_layouts_are_well_formed(
    const Module& module, std::vector<VerifierDiagnostic>& diagnostics) {
    for (std::size_t i = 0; i < module.map_layouts.size(); ++i) {
        const auto& layout = module.map_layouts[i];
        if (!is_valid_map_key_type(layout.key_type)) {
            std::ostringstream message;
            message << "map layout " << i
                    << " key type must be i64, bool, or str";
            return reject(diagnostics, 0, std::nullopt,
                          VerifierReason::InvalidMapKeyType, message.str());
        }
        if (!signature_shape_matches_kind(layout.key_type,
                                          layout.key_type.kind, module) ||
            !signature_shape_matches_kind(layout.value_type,
                                          layout.value_type.kind, module) ||
            layout.key_is_ref !=
                signature_value_is_reference(layout.key_type) ||
            layout.value_is_ref !=
                signature_value_is_reference(layout.value_type)) {
            std::ostringstream message;
            message << "map layout " << i
                    << " type shape or derived reference flags are invalid";
            return reject(diagnostics, 0, std::nullopt,
                          VerifierReason::BadMapLayoutIndex, message.str());
        }
    }
    return true;
}

bool named_types_are_well_formed(const Module& module) {
    for (const auto& type : module.named_types) {
        if (!type.body.has_pair_fields()) {
            return false;
        }
        if (!signature_shape_matches_kind(type.body, ValueKind::Object, module)) {
            return false;
        }
    }
    return true;
}

bool signature_is_well_formed(const FunctionSignature& signature,
                              const Module& module) {
    if (!signature.parameter_types.empty() &&
        signature.parameter_types.size() != signature.parameters.size()) {
        return false;
    }
    for (std::size_t i = 0; i < signature.parameter_types.size(); ++i) {
        if (!signature_shape_matches_kind(signature.parameter_types[i],
                                          signature.parameters[i], module)) {
            return false;
        }
    }
    if (signature.return_type_detail.has_value() &&
        !signature_shape_matches_kind(*signature.return_type_detail,
                                      signature.return_type, module)) {
        return false;
    }
    return true;
}

bool closure_layouts_are_well_formed(
    const Module& module, std::vector<VerifierDiagnostic>& diagnostics) {
    for (std::size_t i = 0; i < module.closure_layouts.size(); ++i) {
        const auto& layout = module.closure_layouts[i];
        if (layout.function_index >= module.functions.size() ||
            !module.functions[layout.function_index].closure_layout.has_value() ||
            *module.functions[layout.function_index].closure_layout != i ||
            !layout.function_type.has_function_signature() ||
            !signature_shape_matches_kind(layout.function_type,
                                          ValueKind::Function, module) ||
            !signature_is_well_formed(
                module.functions[layout.function_index].signature, module) ||
            !signature_values_equal(
                layout.function_type,
                function_type_from_signature(
                    module.functions[layout.function_index].signature))) {
            std::ostringstream message;
            message << "closure layout " << i
                    << " has an invalid function target or structural signature";
            return reject(diagnostics, layout.function_index, std::nullopt,
                          VerifierReason::BadClosureLayoutIndex,
                          message.str());
        }
        if (layout.capture_types.size() != layout.capture_map.size()) {
            std::ostringstream message;
            message << "closure layout " << i << " capture type count "
                    << layout.capture_types.size() << " differs from capture map count "
                    << layout.capture_map.size();
            return reject(diagnostics, layout.function_index, std::nullopt,
                          VerifierReason::BadClosureCaptureArity,
                          message.str());
        }
        for (std::size_t capture = 0; capture < layout.capture_types.size(); ++capture) {
            const auto& type = layout.capture_types[capture];
            if (!signature_shape_matches_kind(type, type.kind, module) ||
                layout.capture_map[capture] != signature_value_is_reference(type)) {
                std::ostringstream message;
                message << "closure layout " << i << " capture " << capture
                        << " disagrees with its derived reference bitmap";
                return reject(diagnostics, layout.function_index, std::nullopt,
                              VerifierReason::BadClosureCaptureType,
                              message.str());
            }
        }
    }
    for (std::size_t function_index = 0;
         function_index < module.functions.size(); ++function_index) {
        const auto& function = module.functions[function_index];
        if (!function.closure_layout.has_value()) {
            continue;
        }
        if (*function.closure_layout >= module.closure_layouts.size() ||
            module.closure_layouts[*function.closure_layout].function_index !=
                function_index) {
            return reject(diagnostics, function_index, std::nullopt,
                          VerifierReason::BadClosureLayoutIndex,
                          "function closure-body metadata does not name its own layout");
        }
    }
    return true;
}

bool fields_equal(const std::shared_ptr<const PairFields>& lhs,
                  const std::shared_ptr<const PairFields>& rhs);
bool abstract_value_ptr_equal(const std::shared_ptr<const AbstractValue>& lhs,
                              const std::shared_ptr<const AbstractValue>& rhs);

bool operator==(const AbstractValue& lhs, const AbstractValue& rhs) {
    return lhs.kind == rhs.kind && lhs.object_sites == rhs.object_sites &&
           lhs.ref_array_sites == rhs.ref_array_sites &&
           lhs.includes_opaque_object == rhs.includes_opaque_object &&
           lhs.includes_nil == rhs.includes_nil &&
           fields_equal(lhs.signature_fields, rhs.signature_fields) &&
           abstract_value_ptr_equal(lhs.signature_array_element,
                                    rhs.signature_array_element) &&
           ((lhs.signature_function == nullptr &&
             rhs.signature_function == nullptr) ||
            (lhs.signature_function != nullptr &&
             rhs.signature_function != nullptr &&
             signature_values_equal(*lhs.signature_function,
                                    *rhs.signature_function))) &&
           ((lhs.signature_map == nullptr && rhs.signature_map == nullptr) ||
            (lhs.signature_map != nullptr && rhs.signature_map != nullptr &&
             signature_values_equal(*lhs.signature_map,
                                    *rhs.signature_map))) &&
           lhs.signature_named_type == rhs.signature_named_type &&
           lhs.source_local == rhs.source_local &&
           lhs.nil_test_local == rhs.nil_test_local;
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

bool abstract_value_ptr_equal(const std::shared_ptr<const AbstractValue>& lhs,
                              const std::shared_ptr<const AbstractValue>& rhs) {
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
        joined.includes_nil = lhs.includes_nil || rhs.includes_nil;
        if (lhs.signature_named_type == rhs.signature_named_type) {
            joined.signature_named_type = lhs.signature_named_type;
        } else if (lhs.signature_named_type.has_value() &&
                   !rhs.signature_named_type.has_value()) {
            joined.signature_named_type = lhs.signature_named_type;
        } else if (!lhs.signature_named_type.has_value() &&
                   rhs.signature_named_type.has_value()) {
            joined.signature_named_type = rhs.signature_named_type;
        } else {
            joined.signature_named_type.reset();
        }
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
    } else if (joined.kind == AbstractKind::RefArray) {
        joined.ref_array_sites.insert(rhs.ref_array_sites.begin(),
                                      rhs.ref_array_sites.end());
        if (lhs.signature_array_element != nullptr &&
            rhs.signature_array_element != nullptr) {
            joined.signature_array_element = abstract_value_ptr(
                join_values(*lhs.signature_array_element,
                            *rhs.signature_array_element));
        } else if (lhs.signature_array_element != nullptr) {
            joined.signature_array_element = lhs.signature_array_element;
        } else {
            joined.signature_array_element = rhs.signature_array_element;
        }
    } else if (joined.kind == AbstractKind::Function) {
        if (lhs.signature_function == nullptr ||
            rhs.signature_function == nullptr ||
            !signature_values_equal(*lhs.signature_function,
                                    *rhs.signature_function)) {
            return poison_value();
        }
    } else if (joined.kind == AbstractKind::Map) {
        if (lhs.signature_map == nullptr || rhs.signature_map == nullptr ||
            !signature_values_equal(*lhs.signature_map,
                                    *rhs.signature_map)) {
            return poison_value();
        }
    }
    if (lhs.source_local == rhs.source_local) {
        joined.source_local = lhs.source_local;
    } else {
        joined.source_local.reset();
    }
    if (lhs.nil_test_local == rhs.nil_test_local) {
        joined.nil_test_local = lhs.nil_test_local;
    } else {
        joined.nil_test_local.reset();
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

AbstractValue join_ref_array_element_values(const AbstractValue& lhs,
                                            const AbstractValue& rhs) {
    const auto joined = join_values(lhs, rhs);
    if (!is_poison(joined)) {
        return joined;
    }
    if (lhs.kind == AbstractKind::Function ||
        rhs.kind == AbstractKind::Function) {
        return joined;
    }
    if (is_reference_kind(lhs.kind) && is_reference_kind(rhs.kind)) {
        return opaque_object_value();
    }
    return joined;
}

bool join_ref_array_element_into(AbstractValue& destination,
                                 const AbstractValue& incoming) {
    const auto joined = join_ref_array_element_values(destination, incoming);
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

    for (const auto& [site, incoming_element] : incoming.ref_array_elements_by_site) {
        auto [it, inserted] =
            destination.ref_array_elements_by_site.emplace(site, incoming_element);
        if (inserted) {
            changed = true;
            continue;
        }
        changed = join_ref_array_element_into(it->second, incoming_element) || changed;
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
        const bool is_object = is_reference_kind(stack[i].kind);
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
        map.object_slots.push_back(is_reference_kind(value.kind));
    }
    return map;
}

bool pop_any_or_report(AbstractState& state,
                       std::vector<VerifierDiagnostic>& diagnostics,
                       const Function& function,
                       std::size_t function_index,
                       std::size_t pc,
                       VerifierReason underflow_reason,
                       std::string_view context,
                       AbstractValue* out = nullptr) {
    if (state.stack.empty()) {
        std::ostringstream message;
        message << context << " requires a stack value";
        return reject(diagnostics, function_index, pc, underflow_reason,
                      instruction_message(function, pc, message.str()));
    }
    const auto& value = state.stack.back();
    if (is_poison(value)) {
        std::ostringstream message;
        message << context << " consumed a poison value";
        return reject(diagnostics, function_index, pc, VerifierReason::PoisonUse,
                      instruction_message(function, pc, message.str()));
    }
    if (out != nullptr) {
        *out = std::move(state.stack.back());
    }
    state.stack.pop_back();
    return true;
}

bool pop_expect_or_report(AbstractState& state,
                          std::vector<VerifierDiagnostic>& diagnostics,
                          const Function& function,
                          std::size_t function_index,
                          std::size_t pc,
                          AbstractKind expected,
                          VerifierReason underflow_reason,
                          VerifierReason mismatch_reason,
                          std::string_view context,
                          AbstractValue* out = nullptr) {
    AbstractValue actual;
    if (!pop_any_or_report(state, diagnostics, function, function_index, pc,
                           underflow_reason, context, &actual)) {
        return false;
    }
    if (actual.kind != expected) {
        std::ostringstream message;
        message << context << " expected " << abstract_kind_name(expected)
                << " but found " << abstract_kind_name(actual.kind);
        return reject(diagnostics, function_index, pc, mismatch_reason,
                      instruction_message(function, pc, message.str()));
    }
    if (out != nullptr) {
        *out = std::move(actual);
    }
    return true;
}

bool pop_reference_or_report(AbstractState& state,
                             std::vector<VerifierDiagnostic>& diagnostics,
                             const Function& function,
                             std::size_t function_index,
                             std::size_t pc,
                             VerifierReason underflow_reason,
                             VerifierReason mismatch_reason,
                             std::string_view context,
                             AbstractValue* out = nullptr) {
    AbstractValue actual;
    if (!pop_any_or_report(state, diagnostics, function, function_index, pc,
                           underflow_reason, context, &actual)) {
        return false;
    }
    if (!is_reference_kind(actual.kind) ||
        (actual.kind == AbstractKind::Object && actual.includes_nil)) {
        std::ostringstream message;
        message << context << " expected a non-nil reference but found "
                << abstract_kind_name(actual.kind);
        return reject(diagnostics, function_index, pc, mismatch_reason,
                      instruction_message(function, pc, message.str()));
    }
    if (out != nullptr) {
        *out = std::move(actual);
    }
    return true;
}

bool push_fallthrough_or_report(
    std::size_t pc, const Function& function, std::size_t function_index,
    AbstractState&& state,
    std::vector<std::pair<std::size_t, AbstractState>>& successors,
    std::vector<VerifierDiagnostic>& diagnostics) {
    const auto next_pc = pc + 1;
    if (next_pc >= function.code.size()) {
        return reject(diagnostics, function_index, pc, VerifierReason::FallOffEnd,
                      instruction_message(function, pc,
                                          "instruction would fall through past function end"));
    }
    successors.emplace_back(next_pc, std::move(state));
    return true;
}

bool load_pair_field(const Module& module, const AbstractState& state,
                     const AbstractValue& receiver, bool left, AbstractValue& out) {
    if (receiver.includes_nil || receiver.includes_opaque_object) {
        return false;
    }

    std::optional<AbstractValue> loaded;
    if (receiver.signature_fields != nullptr) {
        loaded = left ? receiver.signature_fields->left : receiver.signature_fields->right;
    }
    if (receiver.signature_named_type.has_value()) {
        if (*receiver.signature_named_type >= module.named_types.size()) {
            return false;
        }
        const auto named_body =
            value_from_signature(module.named_types[*receiver.signature_named_type].body);
        if (named_body.signature_fields == nullptr) {
            return false;
        }
        const auto& field =
            left ? named_body.signature_fields->left : named_body.signature_fields->right;
        if (!loaded.has_value()) {
            loaded = field;
        } else {
            *loaded = join_values(*loaded, field);
        }
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

struct NonnilValueShape {
    std::vector<std::size_t> object_sites;
    bool includes_opaque_object{false};
    std::uintptr_t signature_fields{0};
    std::optional<std::size_t> signature_named_type;

    [[nodiscard]] bool operator<(const NonnilValueShape& other) const {
        if (object_sites != other.object_sites) {
            return object_sites < other.object_sites;
        }
        if (includes_opaque_object != other.includes_opaque_object) {
            return includes_opaque_object < other.includes_opaque_object;
        }
        if (signature_fields != other.signature_fields) {
            return signature_fields < other.signature_fields;
        }
        return signature_named_type < other.signature_named_type;
    }
};

struct ConformanceAssumption {
    NonnilValueShape value;
    std::size_t expected_named_type{0};

    [[nodiscard]] bool operator<(const ConformanceAssumption& other) const {
        if (value < other.value) {
            return true;
        }
        if (other.value < value) {
            return false;
        }
        return expected_named_type < other.expected_named_type;
    }
};

NonnilValueShape nonnil_shape(const AbstractValue& value) {
    return NonnilValueShape{
        std::vector<std::size_t>(value.object_sites.begin(), value.object_sites.end()),
        value.includes_opaque_object,
        reinterpret_cast<std::uintptr_t>(value.signature_fields.get()),
        value.signature_named_type,
    };
}

bool has_nonnil_object_evidence(const AbstractValue& value) {
    return value.includes_opaque_object || !value.object_sites.empty() ||
           value.signature_fields != nullptr || value.signature_named_type.has_value();
}

bool value_conforms_to_expected(const Module& module, const AbstractState& state,
                                const AbstractValue& value,
                                const AbstractValue& expected,
                                std::set<ConformanceAssumption>& assumptions);

bool load_ref_array_element(const AbstractState& state, const AbstractValue& receiver,
                            AbstractValue& out) {
    std::optional<AbstractValue> loaded;
    if (receiver.signature_array_element != nullptr) {
        loaded = *receiver.signature_array_element;
    }
    for (const auto site : receiver.ref_array_sites) {
        const auto it = state.ref_array_elements_by_site.find(site);
        if (it == state.ref_array_elements_by_site.end()) {
            return false;
        }
        if (!loaded.has_value()) {
            loaded = it->second;
        } else {
            *loaded = join_ref_array_element_values(*loaded, it->second);
        }
    }
    if (!loaded.has_value() || is_poison(*loaded)) {
        return false;
    }
    out = *loaded;
    return true;
}

bool value_conforms_to_signature(const Module& module, const AbstractState& state,
                                 const AbstractValue& value,
                                 const SignatureValue& signature) {
    std::set<ConformanceAssumption> assumptions;
    return value_conforms_to_expected(module, state, value,
                                      value_from_signature(signature), assumptions);
}

bool value_conforms_to_expected(const Module& module, const AbstractState& state,
                                const AbstractValue& value,
                                const AbstractValue& expected,
                                std::set<ConformanceAssumption>& assumptions) {
    if (is_poison(value) || is_poison(expected)) {
        return false;
    }
    if (expected.kind == AbstractKind::Nil &&
        value.kind == AbstractKind::Object && value.includes_nil &&
        value.object_sites.empty() && !value.includes_opaque_object &&
        value.signature_fields == nullptr &&
        !value.signature_named_type.has_value()) {
        return true;
    }
    if (value.kind == AbstractKind::RefArray && expected.kind == AbstractKind::Object) {
        return !expected.includes_nil && !expected.signature_named_type.has_value() &&
               expected.signature_fields == nullptr;
    }
    if (value.kind != expected.kind) {
        return false;
    }
    if (expected.kind == AbstractKind::RefArray) {
        if (expected.signature_array_element == nullptr) {
            return true;
        }
        if (value.signature_array_element != nullptr &&
            !value_conforms_to_expected(module, state, *value.signature_array_element,
                                        *expected.signature_array_element,
                                        assumptions)) {
            return false;
        }
        for (const auto site : value.ref_array_sites) {
            const auto it = state.ref_array_elements_by_site.find(site);
            if (it == state.ref_array_elements_by_site.end() ||
                !value_conforms_to_expected(module, state, it->second,
                                            *expected.signature_array_element,
                                            assumptions)) {
                return false;
            }
        }
        return value.signature_array_element != nullptr ||
               !value.ref_array_sites.empty();
    }
    if (expected.kind == AbstractKind::Map) {
        return value.signature_map != nullptr &&
               expected.signature_map != nullptr &&
               signature_values_equal(*value.signature_map,
                                      *expected.signature_map);
    }
    if (expected.kind == AbstractKind::Function) {
        return value.signature_function != nullptr &&
               expected.signature_function != nullptr &&
               signature_values_equal(*value.signature_function,
                                      *expected.signature_function);
    }
    if (expected.kind != AbstractKind::Object) {
        return true;
    }
    if (expected.signature_named_type.has_value()) {
        if (*expected.signature_named_type >= module.named_types.size()) {
            return false;
        }
        if (!has_nonnil_object_evidence(value)) {
            return value.includes_nil;
        }

        auto nonnil = value;
        nonnil.includes_nil = false;
        const ConformanceAssumption assumption{nonnil_shape(nonnil),
                                               *expected.signature_named_type};
        // Termination: recursive signatures can only re-enter through finite
        // Module::named_types references, and verifier values contain finite
        // allocation-site/signature evidence. Once the same non-nil value shape is
        // being checked against the same named type, the remaining obligation is the
        // same coinductive proof, so it is assumed instead of unfolded again.
        if (!assumptions.insert(assumption).second) {
            return true;
        }

        return value_conforms_to_expected(
            module, state, nonnil,
            value_from_signature(module.named_types[*expected.signature_named_type].body),
            assumptions);
    }
    if (value.includes_nil) {
        return false;
    }
    if (expected.signature_fields == nullptr) {
        return true;
    }

    AbstractValue value_left;
    AbstractValue value_right;
    return load_pair_field(module, state, value, true, value_left) &&
           load_pair_field(module, state, value, false, value_right) &&
           value_conforms_to_expected(module, state, value_left,
                                      expected.signature_fields->left, assumptions) &&
           value_conforms_to_expected(module, state, value_right,
                                      expected.signature_fields->right, assumptions);
}

bool store_pair_field(const Module& module, AbstractState& state,
                      const AbstractValue& receiver, bool left,
                      const AbstractValue& value) {
    if (receiver.includes_nil) {
        return false;
    }
    if (receiver.includes_opaque_object && receiver.signature_fields != nullptr) {
        return false;
    }

    std::optional<AbstractValue> expected;
    if (receiver.signature_fields != nullptr) {
        expected = left ? receiver.signature_fields->left : receiver.signature_fields->right;
    }
    if (receiver.signature_named_type.has_value()) {
        if (*receiver.signature_named_type >= module.named_types.size()) {
            return false;
        }
        const auto named_body =
            value_from_signature(module.named_types[*receiver.signature_named_type].body);
        if (named_body.signature_fields == nullptr) {
            return false;
        }
        const auto& named_expected =
            left ? named_body.signature_fields->left : named_body.signature_fields->right;
        if (!expected.has_value()) {
            expected = named_expected;
        } else {
            *expected = join_values(*expected, named_expected);
        }
    }
    if (expected.has_value()) {
        std::set<ConformanceAssumption> assumptions;
        if (!value_conforms_to_expected(module, state, value, *expected, assumptions)) {
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
    return receiver.signature_fields != nullptr ||
           receiver.signature_named_type.has_value() || !receiver.object_sites.empty() ||
           receiver.includes_opaque_object;
}

bool store_ref_array_element(const Module& module, AbstractState& state,
                             const AbstractValue& receiver,
                             const AbstractValue& value) {
    if (receiver.signature_array_element != nullptr) {
        std::set<ConformanceAssumption> assumptions;
        if (!value_conforms_to_expected(module, state, value,
                                        *receiver.signature_array_element,
                                        assumptions)) {
            return false;
        }
    }
    for (const auto site : receiver.ref_array_sites) {
        const auto it = state.ref_array_elements_by_site.find(site);
        if (it == state.ref_array_elements_by_site.end()) {
            return false;
        }
        const auto joined = join_ref_array_element_values(it->second, value);
        if (is_poison(joined)) {
            return false;
        }
        it->second = joined;
    }
    return receiver.signature_array_element != nullptr ||
           !receiver.ref_array_sites.empty();
}

bool transfer_instruction(const Module& module, std::size_t function_index, std::size_t pc,
                          const AbstractState& in,
                          std::vector<std::pair<std::size_t, AbstractState>>& successors,
                          std::vector<VerifierDiagnostic>& diagnostics) {
    const auto& function = module.functions[function_index];
    const auto& ins = function.code[pc];
    auto state = in;

    switch (ins.op) {
    case OpCode::ConstantI64:
        state.stack.push_back(int64_value());
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    case OpCode::PushStr:
        if (ins.operand < 0 ||
            static_cast<std::uint64_t>(ins.operand) >=
                module.string_constants.size()) {
            std::ostringstream message;
            message << "string constant operand " << ins.operand
                    << " is outside pool size " << module.string_constants.size();
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadStringConstantIndex,
                          instruction_message(function, pc, message.str()));
        }
        state.stack.push_back(str_value());
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors,
                                          diagnostics);
    case OpCode::StrLen:
        if (!pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Str,
                                  VerifierReason::StackUnderflow,
                                  VerifierReason::BadStringOperation,
                                  "string receiver")) {
            return false;
        }
        state.stack.push_back(int64_value());
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors,
                                          diagnostics);
    case OpCode::StrEq:
        if (!pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Str,
                                  VerifierReason::StackUnderflow,
                                  VerifierReason::BadStringOperation,
                                  "right string operand") ||
            !pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Str,
                                  VerifierReason::StackUnderflow,
                                  VerifierReason::BadStringOperation,
                                  "left string operand")) {
            return false;
        }
        state.stack.push_back(bool_value());
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors,
                                          diagnostics);
    case OpCode::StrConcat:
        if (!pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Str,
                                  VerifierReason::StackUnderflow,
                                  VerifierReason::BadStringOperation,
                                  "right string operand") ||
            !pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Str,
                                  VerifierReason::StackUnderflow,
                                  VerifierReason::BadStringOperation,
                                  "left string operand")) {
            return false;
        }
        state.stack.push_back(str_value());
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors,
                                          diagnostics);
    case OpCode::StrIndex:
        if (!pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Int64,
                                  VerifierReason::StackUnderflow,
                                  VerifierReason::BadStringOperation,
                                  "string index") ||
            !pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Str,
                                  VerifierReason::StackUnderflow,
                                  VerifierReason::BadStringOperation,
                                  "string receiver")) {
            return false;
        }
        state.stack.push_back(int64_value());
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors,
                                          diagnostics);
    case OpCode::Nil:
        state.stack.push_back(nil_object_value());
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    case OpCode::IsNil: {
        AbstractValue value;
        if (!pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Object, VerifierReason::StackUnderflow,
                                  VerifierReason::TypeMismatch, "is_nil operand",
                                  &value)) {
            return false;
        }
        auto result = bool_value();
        result.nil_test_local = value.source_local;
        state.stack.push_back(result);
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    }
    case OpCode::AddI64:
        if (!pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Int64, VerifierReason::StackUnderflow,
                                  VerifierReason::TypeMismatch, "right operand") ||
            !pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Int64, VerifierReason::StackUnderflow,
                                  VerifierReason::TypeMismatch, "left operand")) {
            return false;
        }
        state.stack.push_back(int64_value());
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    case OpCode::LessI64:
        if (!pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Int64, VerifierReason::StackUnderflow,
                                  VerifierReason::TypeMismatch, "right operand") ||
            !pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Int64, VerifierReason::StackUnderflow,
                                  VerifierReason::TypeMismatch, "left operand")) {
            return false;
        }
        state.stack.push_back(bool_value());
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    case OpCode::AllocPair: {
        AbstractValue right;
        AbstractValue left;
        if (!pop_any_or_report(state, diagnostics, function, function_index, pc,
                               VerifierReason::StackUnderflow, "right field", &right) ||
            !pop_any_or_report(state, diagnostics, function, function_index, pc,
                               VerifierReason::StackUnderflow, "left field", &left)) {
            return false;
        }
        const PairFields allocated{without_provenance(left), without_provenance(right)};
        auto [it, inserted] = state.fields_by_site.emplace(pc, allocated);
        if (!inserted) {
            (void)join_value_into(it->second.left, allocated.left);
            (void)join_value_into(it->second.right, allocated.right);
        }
        state.stack.push_back(object_value(pc));
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    }
    case OpCode::AllocArray:
        if (!pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Int64, VerifierReason::StackUnderflow,
                                  VerifierReason::BadArrayOperation, "array init value") ||
            !pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Int64, VerifierReason::StackUnderflow,
                                  VerifierReason::BadArrayOperation, "array length")) {
            return false;
        }
        state.stack.push_back(array_value());
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    case OpCode::ArrayGet:
        if (!pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Int64, VerifierReason::StackUnderflow,
                                  VerifierReason::BadArrayOperation, "array index") ||
            !pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Array, VerifierReason::StackUnderflow,
                                  VerifierReason::BadArrayOperation, "array receiver")) {
            return false;
        }
        state.stack.push_back(int64_value());
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    case OpCode::ArraySet:
        if (!pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Int64, VerifierReason::StackUnderflow,
                                  VerifierReason::BadArrayOperation, "array stored value") ||
            !pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Int64, VerifierReason::StackUnderflow,
                                  VerifierReason::BadArrayOperation, "array index") ||
            !pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Array, VerifierReason::StackUnderflow,
                                  VerifierReason::BadArrayOperation, "array receiver")) {
            return false;
        }
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    case OpCode::ArrayLen:
    {
        AbstractValue receiver;
        if (!pop_any_or_report(state, diagnostics, function, function_index, pc,
                               VerifierReason::StackUnderflow, "array receiver",
                               &receiver)) {
            return false;
        }
        if (receiver.kind != AbstractKind::Array &&
            receiver.kind != AbstractKind::RefArray) {
            std::ostringstream message;
            message << "array receiver expected Array or RefArray but found "
                    << abstract_kind_name(receiver.kind);
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadArrayOperation,
                          instruction_message(function, pc, message.str()));
        }
        state.stack.push_back(int64_value());
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    }
    case OpCode::AllocRefArray:
    {
        AbstractValue init;
        if (!pop_reference_or_report(state, diagnostics, function, function_index, pc,
                                     VerifierReason::StackUnderflow,
                                     VerifierReason::BadArrayOperation,
                                     "ref array init value", &init) ||
            !pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Int64, VerifierReason::StackUnderflow,
                                  VerifierReason::BadArrayOperation,
                                  "ref array length")) {
            return false;
        }
        state.ref_array_elements_by_site[pc] = without_provenance(init);
        state.stack.push_back(ref_array_value(pc));
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    }
    case OpCode::RefArrayGet:
    {
        AbstractValue receiver;
        AbstractValue loaded;
        if (!pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Int64, VerifierReason::StackUnderflow,
                                  VerifierReason::BadArrayOperation,
                                  "ref array index") ||
            !pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::RefArray, VerifierReason::StackUnderflow,
                                  VerifierReason::BadArrayOperation,
                                  "ref array receiver", &receiver)) {
            return false;
        }
        if (!load_ref_array_element(state, receiver, loaded)) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadArrayOperation,
                          instruction_message(function, pc,
                                              "ref array element facts are unavailable or incompatible"));
        }
        state.stack.push_back(std::move(loaded));
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    }
    case OpCode::RefArraySet:
    {
        AbstractValue value;
        AbstractValue receiver;
        if (!pop_reference_or_report(state, diagnostics, function, function_index, pc,
                                     VerifierReason::StackUnderflow,
                                     VerifierReason::BadArrayOperation,
                                     "ref array stored value", &value) ||
            !pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Int64, VerifierReason::StackUnderflow,
                                  VerifierReason::BadArrayOperation,
                                  "ref array index") ||
            !pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::RefArray, VerifierReason::StackUnderflow,
                                  VerifierReason::BadArrayOperation,
                                  "ref array receiver", &receiver)) {
            return false;
        }
        if (!store_ref_array_element(module, state, receiver,
                                     without_provenance(value))) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadArrayOperation,
                          instruction_message(function, pc,
                                              "stored value does not satisfy ref array element facts"));
        }
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    }
    case OpCode::AllocMap: {
        if (ins.operand < 0 ||
            static_cast<std::uint64_t>(ins.operand) >=
                module.map_layouts.size()) {
            std::ostringstream message;
            message << "map layout operand " << ins.operand
                    << " is outside layout count " << module.map_layouts.size();
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadMapLayoutIndex,
                          instruction_message(function, pc, message.str()));
        }
        const auto& layout =
            module.map_layouts[static_cast<std::size_t>(ins.operand)];
        state.stack.push_back(map_value(map_signature(layout.key_type,
                                                      layout.value_type)));
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors,
                                          diagnostics);
    }
    case OpCode::MapGet:
    case OpCode::MapHas: {
        AbstractValue key;
        AbstractValue receiver;
        if (!pop_any_or_report(state, diagnostics, function, function_index, pc,
                               VerifierReason::StackUnderflow, "map key", &key) ||
            !pop_any_or_report(state, diagnostics, function, function_index, pc,
                               VerifierReason::StackUnderflow, "map receiver",
                               &receiver)) {
            return false;
        }
        if (receiver.kind != AbstractKind::Map ||
            receiver.signature_map == nullptr ||
            !receiver.signature_map->has_map_entries()) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::MapOperationOnNonMap,
                          instruction_message(function, pc,
                                              "receiver is not a typed map"));
        }
        if (!value_conforms_to_signature(module, state, key,
                                         *receiver.signature_map->key)) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::MapKeyTypeMismatch,
                          instruction_message(function, pc,
                                              "key does not match map key type"));
        }
        state.stack.push_back(
            ins.op == OpCode::MapHas
                ? bool_value()
                : value_from_signature(*receiver.signature_map->value));
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors,
                                          diagnostics);
    }
    case OpCode::MapSet: {
        AbstractValue stored;
        AbstractValue key;
        AbstractValue receiver;
        if (!pop_any_or_report(state, diagnostics, function, function_index, pc,
                               VerifierReason::StackUnderflow,
                               "map stored value", &stored) ||
            !pop_any_or_report(state, diagnostics, function, function_index, pc,
                               VerifierReason::StackUnderflow, "map key", &key) ||
            !pop_any_or_report(state, diagnostics, function, function_index, pc,
                               VerifierReason::StackUnderflow, "map receiver",
                               &receiver)) {
            return false;
        }
        if (receiver.kind != AbstractKind::Map ||
            receiver.signature_map == nullptr ||
            !receiver.signature_map->has_map_entries()) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::MapOperationOnNonMap,
                          instruction_message(function, pc,
                                              "receiver is not a typed map"));
        }
        if (!value_conforms_to_signature(module, state, key,
                                         *receiver.signature_map->key)) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::MapKeyTypeMismatch,
                          instruction_message(function, pc,
                                              "key does not match map key type"));
        }
        if (!value_conforms_to_signature(module, state, stored,
                                         *receiver.signature_map->value)) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::MapValueTypeMismatch,
                          instruction_message(function, pc,
                                              "value does not match map value type"));
        }
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors,
                                          diagnostics);
    }
    case OpCode::MapLen: {
        AbstractValue receiver;
        if (!pop_any_or_report(state, diagnostics, function, function_index, pc,
                               VerifierReason::StackUnderflow, "map receiver",
                               &receiver)) {
            return false;
        }
        if (receiver.kind != AbstractKind::Map ||
            receiver.signature_map == nullptr ||
            !receiver.signature_map->has_map_entries()) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::MapOperationOnNonMap,
                          instruction_message(function, pc,
                                              "receiver is not a typed map"));
        }
        state.stack.push_back(int64_value());
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors,
                                          diagnostics);
    }
    case OpCode::GetLeft:
    case OpCode::GetRight: {
        AbstractValue receiver;
        AbstractValue loaded;
        if (!pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Object, VerifierReason::StackUnderflow,
                                  VerifierReason::TypeMismatch, "pair receiver",
                                  &receiver)) {
            return false;
        }
        if (!load_pair_field(module, state, receiver, ins.op == OpCode::GetLeft,
                             loaded)) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadPairFieldRead,
                          instruction_message(function, pc,
                                              "pair field facts are unavailable or incompatible"));
        }
        if (is_poison(loaded)) {
            return reject(diagnostics, function_index, pc, VerifierReason::PoisonUse,
                          instruction_message(function, pc,
                                              "pair field read produced a poison value"));
        }
        state.stack.push_back(std::move(loaded));
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    }
    case OpCode::SetLeft:
    case OpCode::SetRight: {
        AbstractValue value;
        AbstractValue receiver;
        if (!pop_any_or_report(state, diagnostics, function, function_index, pc,
                               VerifierReason::StackUnderflow, "stored field value",
                               &value) ||
            !pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Object, VerifierReason::StackUnderflow,
                                  VerifierReason::TypeMismatch, "pair receiver",
                                  &receiver)) {
            return false;
        }
        if (!store_pair_field(module, state, receiver, ins.op == OpCode::SetLeft,
                              without_provenance(value))) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadPairFieldWrite,
                          instruction_message(function, pc,
                                              "stored value does not satisfy pair field facts"));
        }
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    }
    case OpCode::LoadLocal: {
        if (!is_valid_local_index(ins.operand, function.local_count)) {
            std::ostringstream message;
            message << "local operand " << ins.operand
                    << " is outside local_count " << function.local_count;
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadLocalIndex,
                          instruction_message(function, pc, message.str()));
        }
        const auto local_index = static_cast<std::size_t>(ins.operand);
        if (!state.locals[local_index].has_value() || is_poison(*state.locals[local_index])) {
            if (!state.locals[local_index].has_value()) {
                std::ostringstream message;
                message << "local " << local_index
                        << " is uninitialized on at least one incoming path";
                return reject(diagnostics, function_index, pc,
                              VerifierReason::UninitializedLocal,
                              instruction_message(function, pc, message.str()));
            }
            std::ostringstream message;
            message << "local " << local_index << " contains a poison value";
            return reject(diagnostics, function_index, pc, VerifierReason::PoisonUse,
                          instruction_message(function, pc, message.str()));
        }
        auto loaded = *state.locals[local_index];
        loaded.source_local = local_index;
        loaded.nil_test_local.reset();
        state.stack.push_back(std::move(loaded));
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    }
    case OpCode::StoreLocal: {
        if (!is_valid_local_index(ins.operand, function.local_count)) {
            std::ostringstream message;
            message << "local operand " << ins.operand
                    << " is outside local_count " << function.local_count;
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadLocalIndex,
                          instruction_message(function, pc, message.str()));
        }
        AbstractValue stored;
        if (!pop_any_or_report(state, diagnostics, function, function_index, pc,
                               VerifierReason::StackUnderflow, "stored local value",
                               &stored)) {
            return false;
        }
        state.locals[static_cast<std::size_t>(ins.operand)] =
            without_provenance(stored);
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    }
    case OpCode::Jump:
        if (!is_valid_target(ins.operand, function.code.size())) {
            std::ostringstream message;
            message << "target " << ins.operand << " is outside code size "
                    << function.code.size();
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadJumpTarget,
                          instruction_message(function, pc, message.str()));
        }
        successors.emplace_back(static_cast<std::size_t>(ins.operand), std::move(state));
        return true;
    case OpCode::JumpIfFalse: {
        if (!is_valid_target(ins.operand, function.code.size())) {
            std::ostringstream message;
            message << "target " << ins.operand << " is outside code size "
                    << function.code.size();
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadJumpTarget,
                          instruction_message(function, pc, message.str()));
        }
        AbstractValue condition;
        if (!pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Bool, VerifierReason::StackUnderflow,
                                  VerifierReason::TypeMismatch, "branch condition",
                                  &condition)) {
            return false;
        }
        auto false_state = state;
        auto true_state = state;
        if (condition.nil_test_local.has_value()) {
            const auto local = *condition.nil_test_local;
            if (local < true_state.locals.size() && true_state.locals[local].has_value()) {
                true_state.locals[local] = nil_object_value();
            }
            if (local < false_state.locals.size() &&
                false_state.locals[local].has_value()) {
                false_state.locals[local]->includes_nil = false;
            }
        }
        successors.emplace_back(static_cast<std::size_t>(ins.operand),
                                std::move(false_state));
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(true_state),
                                          successors, diagnostics);
    }
    case OpCode::Collect:
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    case OpCode::AllocClosure: {
        if (ins.operand < 0 ||
            static_cast<std::uint64_t>(ins.operand) >=
                module.closure_layouts.size()) {
            std::ostringstream message;
            message << "layout " << ins.operand << " is outside closure layout count "
                    << module.closure_layouts.size();
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadClosureLayoutIndex,
                          instruction_message(function, pc, message.str()));
        }
        const auto& layout =
            module.closure_layouts[static_cast<std::size_t>(ins.operand)];
        for (std::size_t i = layout.capture_types.size(); i > 0; --i) {
            AbstractValue capture;
            const auto capture_index = i - 1;
            std::ostringstream context;
            context << "capture " << capture_index;
            if (!pop_any_or_report(state, diagnostics, function, function_index, pc,
                                   VerifierReason::BadClosureCaptureArity,
                                   context.str(), &capture)) {
                return false;
            }
            if (!value_conforms_to_signature(module, state, capture,
                                             layout.capture_types[capture_index])) {
                std::ostringstream message;
                message << "capture " << capture_index
                        << " does not conform to closure layout type "
                        << value_kind_name(
                               layout.capture_types[capture_index].kind);
                return reject(diagnostics, function_index, pc,
                              VerifierReason::BadClosureCaptureType,
                              instruction_message(function, pc, message.str()));
            }
        }
        state.stack.push_back(value_from_signature(layout.function_type));
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors,
                                          diagnostics);
    }
    case OpCode::CallClosure: {
        AbstractValue callee;
        if (!pop_any_or_report(state, diagnostics, function, function_index, pc,
                               VerifierReason::BadClosureCallArity,
                               "closure callee", &callee)) {
            return false;
        }
        if (callee.kind != AbstractKind::Function ||
            callee.signature_function == nullptr ||
            !callee.signature_function->has_function_signature()) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::CallClosureOnNonFunction,
                          instruction_message(
                              function, pc,
                              "callee stack slot is not a structural function"));
        }
        const auto& signature = *callee.signature_function;
        for (std::size_t i = signature.function_parameters.size(); i > 0; --i) {
            AbstractValue argument;
            const auto parameter_index = i - 1;
            std::ostringstream context;
            context << "closure argument " << parameter_index;
            if (!pop_any_or_report(state, diagnostics, function, function_index, pc,
                                   VerifierReason::BadClosureCallArity,
                                   context.str(), &argument)) {
                return false;
            }
            if (!value_conforms_to_signature(
                    module, state, argument,
                    signature.function_parameters[parameter_index])) {
                std::ostringstream message;
                message << "closure argument " << parameter_index
                        << " does not conform to parameter type "
                        << value_kind_name(
                               signature.function_parameters[parameter_index].kind);
                return reject(diagnostics, function_index, pc,
                              VerifierReason::BadClosureCallArgKind,
                              instruction_message(function, pc, message.str()));
            }
        }
        state.stack.push_back(value_from_signature(*signature.function_return));
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors,
                                          diagnostics);
    }
    case OpCode::LoadCapture: {
        if (!function.closure_layout.has_value()) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::LoadCaptureOutsideClosureBody,
                          instruction_message(
                              function, pc,
                              "current function has no closure-body layout"));
        }
        assert(*function.closure_layout < module.closure_layouts.size() &&
               "module closure layouts are validated before function analysis");
        const auto& layout = module.closure_layouts[*function.closure_layout];
        if (ins.operand < 0 ||
            static_cast<std::uint64_t>(ins.operand) >=
                layout.capture_types.size()) {
            std::ostringstream message;
            message << "capture " << ins.operand << " is outside capture count "
                    << layout.capture_types.size();
            return reject(diagnostics, function_index, pc,
                          VerifierReason::LoadCaptureOutOfRange,
                          instruction_message(function, pc, message.str()));
        }
        state.stack.push_back(value_from_signature(
            layout.capture_types[static_cast<std::size_t>(ins.operand)]));
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors,
                                          diagnostics);
    }
    case OpCode::Call: {
        if (ins.operand < 0 ||
            static_cast<std::uint64_t>(ins.operand) >= module.functions.size()) {
            std::ostringstream message;
            message << "callee " << ins.operand << " is outside function count "
                    << module.functions.size();
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadCallTarget,
                          instruction_message(function, pc, message.str()));
        }
        const auto callee_index = static_cast<std::size_t>(ins.operand);
        const auto& callee_function = module.functions[callee_index];
        if (callee_function.closure_layout.has_value()) {
            assert(*callee_function.closure_layout <
                       module.closure_layouts.size() &&
                   "closure-body layout is validated before transfer");
            if (!module.closure_layouts[*callee_function.closure_layout]
                     .capture_types.empty()) {
                return reject(
                    diagnostics, function_index, pc,
                    VerifierReason::BadCallTarget,
                    instruction_message(
                        function, pc,
                        "direct Call cannot target a capture-bearing closure body"));
            }
        }
        const auto& callee = callee_function.signature;
        for (std::size_t i = callee.parameters.size(); i > 0; --i) {
            AbstractValue argument;
            const auto parameter_index = i - 1;
            std::ostringstream context;
            context << "argument " << parameter_index;
            if (!pop_any_or_report(state, diagnostics, function, function_index, pc,
                                   VerifierReason::BadCallArity, context.str(),
                                   &argument)) {
                return false;
            }
            if (!value_conforms_to_signature(module, state, argument,
                                             parameter_signature(callee,
                                                                 parameter_index))) {
                std::ostringstream message;
                message << "argument " << parameter_index
                        << " does not conform to callee parameter "
                        << value_kind_name(callee.parameters[parameter_index]);
                return reject(diagnostics, function_index, pc,
                              VerifierReason::BadCallArgKind,
                              instruction_message(function, pc, message.str()));
            }
        }
        state.stack.push_back(value_from_signature(return_signature(callee)));
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    }
    case OpCode::Return: {
        AbstractValue returned;
        if (!pop_any_or_report(state, diagnostics, function, function_index, pc,
                               VerifierReason::StackUnderflow, "return value",
                               &returned)) {
            return false;
        }
        if (!value_conforms_to_signature(module, state, returned,
                                         return_signature(function.signature))) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadReturnKind,
                          instruction_message(function, pc,
                                              "return value does not conform to function signature"));
        }
        return true;
    }
    }
    return reject(diagnostics, function_index, pc, VerifierReason::InvalidOpcode,
                  instruction_message(function, pc, "unknown opcode"));
}

std::optional<VerificationResult> verify_function_with_stack_maps(
    const Module& module, std::size_t function_index,
    std::vector<VerifierDiagnostic>& diagnostics) {
    const auto& function = module.functions[function_index];
    if (!function.stack_maps.empty() && function.stack_maps.size() != function.code.size()) {
        std::ostringstream message;
        message << "stack_maps size " << function.stack_maps.size()
                << " does not match code size " << function.code.size();
        reject(diagnostics, function_index, std::nullopt, VerifierReason::BadStackMap,
               message.str());
        return std::nullopt;
    }
    if (function.code.empty()) {
        reject(diagnostics, function_index, std::nullopt, VerifierReason::EmptyFunction,
               "function has no bytecode");
        return std::nullopt;
    }
    if (!signature_is_well_formed(function.signature, module)) {
        reject(diagnostics, function_index, std::nullopt,
               VerifierReason::SignatureShapeMismatch,
               "function signature detail shape does not match coarse kinds");
        return std::nullopt;
    }
    if (function.local_count < function.signature.parameters.size()) {
        std::ostringstream message;
        message << "local_count " << function.local_count
                << " is smaller than parameter count "
                << function.signature.parameters.size();
        reject(diagnostics, function_index, std::nullopt,
               VerifierReason::LocalCountMismatch, message.str());
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

    std::vector<std::pair<std::size_t, AbstractState>> successors;
    successors.reserve(2);
    while (!worklist.empty()) {
        const auto pc = worklist.front();
        worklist.pop_front();

        successors.clear();
        if (!transfer_instruction(module, function_index, pc, *states[pc], successors,
                                  diagnostics)) {
            return std::nullopt;
        }

        for (auto& [target, state] : successors) {
            if (!states[target].has_value()) {
                states[target] = std::move(state);
                worklist.push_back(target);
                continue;
            }

            if (states[target]->stack.size() != state.stack.size()) {
                std::ostringstream message;
                message << "target pc " << target << " has incoming stack height "
                        << state.stack.size() << " but existing height is "
                        << states[target]->stack.size();
                reject(diagnostics, function_index, target,
                       VerifierReason::StackHeightMergeMismatch, message.str());
                return std::nullopt;
            }
            if (states[target]->locals.size() != state.locals.size()) {
                std::ostringstream message;
                message << "target pc " << target << " has incoming local count "
                        << state.locals.size() << " but existing count is "
                        << states[target]->locals.size();
                reject(diagnostics, function_index, target,
                       VerifierReason::LocalCountMismatch, message.str());
                return std::nullopt;
            }
            const auto outcome = join_state_into(*states[target], state);
            if (outcome == JoinOutcome::Invalid) {
                reject(diagnostics, function_index, target,
                       VerifierReason::StackHeightMergeMismatch,
                       "control-flow merge has incompatible state shape");
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
            reject(diagnostics, function_index, pc, VerifierReason::UnreachableCode,
                   "bytecode pc is unreachable from function entry");
            return std::nullopt;
        }
        auto map = stack_map_from_state(*states[pc]);
        if (!map.has_value()) {
            reject(diagnostics, function_index, pc, VerifierReason::PoisonUse,
                   "cannot generate stack map for poison stack value");
            return std::nullopt;
        }
        if (!function.stack_maps.empty() &&
            !stack_map_matches(function.stack_maps[pc], states[pc]->stack)) {
            reject(diagnostics, function_index, pc, VerifierReason::BadStackMap,
                   "supplied stack map does not match verifier state");
            return std::nullopt;
        }
        result.stack_maps[pc] = *map;
    }

    return result;
}

} // namespace

const char* verifier_reason_name(VerifierReason reason) {
    switch (reason) {
    case VerifierReason::ModuleShapeMismatch:
        return "ModuleShapeMismatch";
    case VerifierReason::EmptyFunction:
        return "EmptyFunction";
    case VerifierReason::SignatureShapeMismatch:
        return "SignatureShapeMismatch";
    case VerifierReason::LocalCountMismatch:
        return "LocalCountMismatch";
    case VerifierReason::BadStackMap:
        return "BadStackMap";
    case VerifierReason::StackUnderflow:
        return "StackUnderflow";
    case VerifierReason::TypeMismatch:
        return "TypeMismatch";
    case VerifierReason::PoisonUse:
        return "PoisonUse";
    case VerifierReason::UninitializedLocal:
        return "UninitializedLocal";
    case VerifierReason::BadLocalIndex:
        return "BadLocalIndex";
    case VerifierReason::BadJumpTarget:
        return "BadJumpTarget";
    case VerifierReason::FallOffEnd:
        return "FallOffEnd";
    case VerifierReason::StackHeightMergeMismatch:
        return "StackHeightMergeMismatch";
    case VerifierReason::UnreachableCode:
        return "UnreachableCode";
    case VerifierReason::BadPairFieldRead:
        return "BadPairFieldRead";
    case VerifierReason::BadPairFieldWrite:
        return "BadPairFieldWrite";
    case VerifierReason::BadArrayOperation:
        return "BadArrayOperation";
    case VerifierReason::BadCallTarget:
        return "BadCallTarget";
    case VerifierReason::BadCallArity:
        return "BadCallArity";
    case VerifierReason::BadCallArgKind:
        return "BadCallArgKind";
    case VerifierReason::BadReturnKind:
        return "BadReturnKind";
    case VerifierReason::InvalidOpcode:
        return "InvalidOpcode";
    case VerifierReason::BadStringConstantIndex:
        return "BadStringConstantIndex";
    case VerifierReason::BadStringOperation:
        return "BadStringOperation";
    case VerifierReason::BadClosureLayoutIndex:
        return "BadClosureLayoutIndex";
    case VerifierReason::BadClosureCaptureArity:
        return "BadClosureCaptureArity";
    case VerifierReason::BadClosureCaptureType:
        return "BadClosureCaptureType";
    case VerifierReason::CallClosureOnNonFunction:
        return "CallClosureOnNonFunction";
    case VerifierReason::BadClosureCallArity:
        return "BadClosureCallArity";
    case VerifierReason::BadClosureCallArgKind:
        return "BadClosureCallArgKind";
    case VerifierReason::LoadCaptureOutOfRange:
        return "LoadCaptureOutOfRange";
    case VerifierReason::LoadCaptureOutsideClosureBody:
        return "LoadCaptureOutsideClosureBody";
    case VerifierReason::BadMapLayoutIndex:
        return "BadMapLayoutIndex";
    case VerifierReason::InvalidMapKeyType:
        return "InvalidMapKeyType";
    case VerifierReason::MapOperationOnNonMap:
        return "MapOperationOnNonMap";
    case VerifierReason::MapKeyTypeMismatch:
        return "MapKeyTypeMismatch";
    case VerifierReason::MapValueTypeMismatch:
        return "MapValueTypeMismatch";
    }
    return "<unknown>";
}

std::string format_verifier_diagnostic(const VerifierDiagnostic& diagnostic) {
    std::ostringstream out;
    out << "function=" << diagnostic.function_index << " ";
    if (diagnostic.pc.has_value()) {
        out << "pc=" << *diagnostic.pc;
    } else {
        out << "pc=<none>";
    }
    out << " reason=" << verifier_reason_name(diagnostic.reason);
    if (!diagnostic.message.empty()) {
        out << ": " << diagnostic.message;
    }
    return out.str();
}

FunctionVerifierReport verify_with_diagnostics(const Function& function) {
    Module module;
    module.entry_function = 0;
    module.functions.push_back(function);

    auto module_report = verify_with_diagnostics(module);
    FunctionVerifierReport report;
    if (module_report.result.has_value() && !module_report.result->functions.empty()) {
        report.result = module_report.result->functions.front();
    }
    report.diagnostics = std::move(module_report.diagnostics);
    return report;
}

ModuleVerifierReport verify_with_diagnostics(const Module& module) {
    ModuleVerifierReport report;
    if (module.functions.empty()) {
        reject(report.diagnostics, 0, std::nullopt,
               VerifierReason::ModuleShapeMismatch,
               "module has no functions");
        return report;
    }
    if (module.entry_function >= module.functions.size()) {
        std::ostringstream message;
        message << "entry function " << module.entry_function
                << " is outside function count " << module.functions.size();
        reject(report.diagnostics, module.entry_function, std::nullopt,
               VerifierReason::ModuleShapeMismatch, message.str());
        return report;
    }
    if (!module_map_key_types_are_valid(module, report.diagnostics)) {
        return report;
    }
    if (!map_layouts_are_well_formed(module, report.diagnostics)) {
        return report;
    }
    if (!named_types_are_well_formed(module)) {
        reject(report.diagnostics, 0, std::nullopt,
               VerifierReason::SignatureShapeMismatch,
               "module named type table contains a malformed recursive type");
        return report;
    }
    if (!closure_layouts_are_well_formed(module, report.diagnostics)) {
        return report;
    }
    const auto& entry = module.functions[module.entry_function];
    if (entry.closure_layout.has_value() &&
        !module.closure_layouts[*entry.closure_layout].capture_types.empty()) {
        reject(report.diagnostics, module.entry_function, std::nullopt,
               VerifierReason::BadCallTarget,
               "module entry cannot be a capture-bearing closure body");
        return report;
    }

    ModuleVerificationResult result;
    result.functions.reserve(module.functions.size());
    for (std::size_t i = 0; i < module.functions.size(); ++i) {
        auto function_result = verify_function_with_stack_maps(module, i,
                                                               report.diagnostics);
        if (!function_result.has_value()) {
            return report;
        }
        result.functions.push_back(std::move(*function_result));
    }
    report.result = std::move(result);
    return report;
}

VerifiedModuleReport verify_module_with_diagnostics(Module module) {
    auto verification_report = verify_with_diagnostics(module);

    VerifiedModuleReport report;
    report.diagnostics = std::move(verification_report.diagnostics);
    if (!verification_report.result.has_value()) {
        return report;
    }

    auto verification = std::move(*verification_report.result);
    for (std::size_t i = 0; i < module.functions.size(); ++i) {
        module.functions[i].stack_maps = verification.functions[i].stack_maps;
    }
    report.module = VerifiedModule(std::move(module), std::move(verification));
    return report;
}

std::optional<VerificationResult> verify_with_stack_maps(const Function& function) {
    auto report = verify_with_diagnostics(function);
    if (!report.result.has_value()) {
        return std::nullopt;
    }
    return std::move(report.result);
}

std::optional<ModuleVerificationResult> verify_with_stack_maps(const Module& module) {
    auto report = verify_with_diagnostics(module);
    if (!report.result.has_value()) {
        return std::nullopt;
    }
    return std::move(report.result);
}

std::optional<VerifiedModule> verify_module(Module module) {
    auto report = verify_module_with_diagnostics(std::move(module));
    if (!report.module.has_value()) {
        return std::nullopt;
    }
    return std::move(report.module);
}

bool verify(const Function& function) {
    return verify_with_stack_maps(function).has_value();
}

bool verify(const Module& module) {
    return verify_with_stack_maps(module).has_value();
}

} // namespace lang
