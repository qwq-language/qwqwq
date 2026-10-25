#include "checker.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

#include "error.hpp"
#include "stdlib.hpp"

namespace qwq {
namespace {

StreamType checkPort(Port& port, Environment& environment);
void checkExpr(Expr& expr, Environment& environment, const StreamType* inputHint,
               const StreamType* outputHint);
std::optional<StreamType> knownInput(const Expr& expr, const Environment& environment);
std::optional<StreamType> knownOutput(const Expr& expr, const Environment& environment);

// ---- helpers ------------------------------------------------------------

/// The definition whose body is currently being checked without a declared
/// signature. A call to it there cannot be typed, so it gets a precise error.
const std::string* selfName = nullptr;

struct SelfScope {
  explicit SelfScope(const std::string& name) : previous_(selfName) {
    selfName = &name;
  }
  ~SelfScope() { selfName = previous_; }
  SelfScope(const SelfScope&) = delete;
  SelfScope& operator=(const SelfScope&) = delete;

 private:
  const std::string* previous_;
};

std::string describe(const Expr& expr) {
  return toString(expr.in) + " -> " + toString(expr.out);
}

[[noreturn]] void fail(const Expr& expr, const std::string& message) {
  throw Error(message + " (face type: " + describe(expr) + ")", expr.position);
}

std::optional<std::size_t> conversionCost(const StreamType& from,
                                          const StreamType& to);

std::optional<std::size_t> conversionCost(const TypePtr& from,
                                          const TypePtr& to) {
  if (equals(from, to)) return 0;
  if (!from || !to) return std::nullopt;

  // Distinct nominal types never convert directly, even when their layouts
  // happen to be identical. This keeps them useful for overload selection.
  if (from->kind == Type::Kind::Nominal &&
      to->kind == Type::Kind::Nominal) {
    return std::nullopt;
  }

  if (from->kind == Type::Kind::Nominal) {
    const auto nested = conversionCost(from->representation, StreamType(to));
    if (nested) return *nested + 1;
    return std::nullopt;
  }
  if (to->kind == Type::Kind::Nominal) {
    const auto nested = conversionCost(StreamType(from), to->representation);
    if (nested) return *nested + 1;
    return std::nullopt;
  }
  if (from->kind != to->kind) return std::nullopt;

  switch (from->kind) {
    case Type::Kind::Int:
    case Type::Kind::Str:
    case Type::Kind::Bool:
      return std::nullopt;
    case Type::Kind::Array:
      return conversionCost(from->elements, to->elements);
    case Type::Kind::Pack: {
      const auto left = conversionCost(StreamType(from->left), StreamType(to->left));
      const auto right = conversionCost(StreamType(from->right), StreamType(to->right));
      if (!left || !right) return std::nullopt;
      return *left + *right;
    }
    case Type::Kind::Nominal:
    case Type::Kind::Variable:
      return std::nullopt;
  }
  return std::nullopt;
}

std::optional<std::size_t> conversionCost(const StreamType& from,
                                          const StreamType& to) {
  if (from.empty() || to.empty()) {
    return from.empty() == to.empty() ? std::optional<std::size_t>(0)
                                      : std::nullopt;
  }
  return conversionCost(from.value, to.value);
}

bool canConvert(const StreamType& from, const StreamType& to) {
  return equals(from, to);
}

bool canExplicitlyConvert(const StreamType& from, const StreamType& to) {
  return conversionCost(from, to).has_value();
}

bool isKind(const StreamType& streamType, Type::Kind kind) {
  return !streamType.empty() && streamType.value->kind == kind;
}

StreamType arrayElements(const StreamType& streamType) {
  return !streamType.empty() && streamType.value->kind == Type::Kind::Array
             ? streamType.value->elements
             : StreamType{};
}

bool isInt(const StreamType& streamType) {
  return isKind(streamType, Type::Kind::Int);
}

bool isArray(const StreamType& streamType) {
  return isKind(streamType, Type::Kind::Array);
}

std::optional<StreamType> constrainType(
    const Expr& expr, const std::optional<StreamType>& annotation,
    const StreamType* hint, const char* side) {
  if (annotation && hint) {
    const bool compatible =
        std::string(side) == "input" ? canConvert(*hint, *annotation)
                                      : canConvert(*annotation, *hint);
    if (!compatible) {
      throw Error(std::string("explicit ") + side + " type " +
                      toString(*annotation) + " conflicts with inferred type " +
                      toString(*hint),
                  expr.position);
    }
  }
  if (annotation) return annotation;
  if (hint != nullptr) return *hint;
  return std::nullopt;
}

bool isBool(const StreamType& streamType) {
  return isKind(streamType, Type::Kind::Bool);
}

bool isPack(const StreamType& streamType) {
  return isKind(streamType, Type::Kind::Pack);
}

void collectPackLeaves(const TypePtr& type, std::vector<TypePtr>& leaves) {
  // Arrays and nominal types are leaves; only structural Packs are regrouped.
  if (type->kind == Type::Kind::Pack) {
    collectPackLeaves(type->left, leaves);
    collectPackLeaves(type->right, leaves);
  } else {
    leaves.push_back(type);
  }
}

bool sameOrderedPackLeaves(const StreamType& input, const StreamType& output) {
  if (!isPack(input) || !isPack(output)) return false;

  std::vector<TypePtr> inputLeaves;
  std::vector<TypePtr> outputLeaves;
  collectPackLeaves(input.value, inputLeaves);
  collectPackLeaves(output.value, outputLeaves);
  if (inputLeaves.size() != outputLeaves.size()) return false;
  for (std::size_t i = 0; i < inputLeaves.size(); ++i) {
    if (!equals(inputLeaves[i], outputLeaves[i])) return false;
  }
  return true;
}

StreamType firstOf(const StreamType& streamType) {
  if (!isPack(streamType)) return {};
  return StreamType(streamType.value->left);
}

StreamType secondOf(const StreamType& streamType) {
  if (!isPack(streamType)) return {};
  return StreamType(streamType.value->right);
}

bool isEqualPair(const StreamType& streamType) {
  return isPack(streamType) &&
         equals(firstOf(streamType), secondOf(streamType));
}

/// `:` permits the identity, an explicit representation conversion, or
/// discarding a non-empty intermediate into `o`.
bool canCoerce(const StreamType& from, const StreamType& to) {
  return equals(from, to) || to.empty() || canExplicitlyConvert(from, to);
}

// ---- ports --------------------------------------------------------------

StreamType resolveTypePort(const Port& port, const Environment& environment,
                           const TypeBindings& bindings,
                           std::vector<std::string>& activeAliases);

StreamType resolveNamedType(const PortPart& part, const Environment& environment,
                            const TypeBindings& bindings,
                            std::vector<std::string>& activeAliases) {
  const auto binding = bindings.find(part.name);
  if (binding != bindings.end()) {
    if (!part.arguments.empty()) {
      throw Error("type parameter '" + part.name +
                      "' cannot be applied to type arguments",
                  part.position);
    }
    return binding->second;
  }

  const auto found = environment.typeAliases.find(part.name);
  if (found == environment.typeAliases.end()) {
    throw Error("unknown class type '" + part.name + "'", part.position);
  }
  const TypeAliasDefinition& alias = *found->second;
  if (part.arguments.size() != alias.parameters.size()) {
    throw Error("class '" + part.name + "' expects " +
                    std::to_string(alias.parameters.size()) +
                    " type argument(s), found " +
                    std::to_string(part.arguments.size()),
                part.position);
  }

  const auto cycle = std::find(activeAliases.begin(), activeAliases.end(), part.name);
  if (cycle != activeAliases.end()) {
    std::string path;
    for (auto it = cycle; it != activeAliases.end(); ++it) {
      if (!path.empty()) path += " -> ";
      path += *it;
    }
    path += " -> " + part.name;
    throw Error("recursive class representation: " + path, part.position);
  }

  TypeBindings bindingsForClass;
  std::vector<StreamType> typeArguments;
  typeArguments.reserve(alias.parameters.size());
  for (std::size_t i = 0; i < alias.parameters.size(); ++i) {
    StreamType argument = resolveTypePort(*part.arguments[i], environment,
                                          bindings, activeAliases);
    bindingsForClass.emplace(alias.parameters[i], argument);
    typeArguments.push_back(std::move(argument));
  }

  activeAliases.push_back(part.name);
  StreamType representation = resolveTypePort(alias.body, environment,
                                               bindingsForClass, activeAliases);
  activeAliases.pop_back();
  if (representation.empty()) {
    throw Error("class '" + part.name + "' must have a non-empty representation",
                part.position);
  }
  return StreamType(nominalType(part.name, std::move(typeArguments),
                                std::move(representation)));
}

StreamType resolveTypePart(const PortPart& part, const Environment& environment,
                           const TypeBindings& bindings,
                           std::vector<std::string>& activeAliases) {
  switch (part.kind) {
    case PortPart::Kind::Primitive:
      return part.type;
    case PortPart::Kind::Array:
      return StreamType(arrayType(resolveTypePort(*part.inner, environment,
                                                  bindings, activeAliases)));
    case PortPart::Kind::Group:
      return resolveTypePort(*part.inner, environment, bindings, activeAliases);
    case PortPart::Kind::Named:
      return resolveNamedType(part, environment, bindings, activeAliases);
    case PortPart::Kind::Expression:
      throw Error("a class representation may contain only types, Packs, and arrays",
                  part.position);
  }
  throw Error("internal error: unknown port part", part.position);
}

StreamType resolveTypePort(const Port& port, const Environment& environment,
                           const TypeBindings& bindings,
                           std::vector<std::string>& activeAliases) {
  StreamType result;
  for (const PortPart& part : port.parts) {
    result = pack(result,
                  resolveTypePart(part, environment, bindings, activeAliases));
  }
  return result;
}

StreamType checkPort(Port& port, Environment& environment) {
  StreamType result;
  const TypeBindings& bindings = environment.typeBindings;
  std::vector<std::string> activeAliases;

  for (PortPart& part : port.parts) {
    StreamType next;
    switch (part.kind) {
      case PortPart::Kind::Primitive:
      case PortPart::Kind::Named: {
        next = resolveTypePart(part, environment, bindings, activeAliases);
        break;
      }

      case PortPart::Kind::Array:
        next = StreamType(arrayType(checkPort(*part.inner, environment)));
        break;

      case PortPart::Kind::Group:
        next = checkPort(*part.inner, environment);
        break;

      case PortPart::Kind::Expression:
        checkExpr(*part.expression, environment, nullptr, nullptr);
        if (!equals(part.expression->in, part.expression->out)) {
          throw Error(
              "expected a port (a type), but found the morphism " +
                  describe(*part.expression) +
                  "; the eyes of a face are types, so connect a program first: "
                  "A ~ (X . Y)",
              part.position);
        }
        next = part.expression->in;
        break;
    }

    // In a port, underscores are the strict binary Pack constructor,
    // associated to the left.
    // Empty streams are its unit, so o,q and q,o both reduce to q.
    result = pack(result, next);
  }

  return result;
}

std::optional<StreamType> knownPortType(const Port& port,
                                        const Environment& environment) {
  StreamType result;
  const TypeBindings& bindings = environment.typeBindings;
  std::vector<std::string> activeAliases;
  for (const PortPart& part : port.parts) {
    std::optional<StreamType> next;
    switch (part.kind) {
      case PortPart::Kind::Primitive:
      case PortPart::Kind::Named:
        next = resolveTypePart(part, environment, bindings, activeAliases);
        break;
      case PortPart::Kind::Array: {
        const auto element = knownPortType(*part.inner, environment);
        if (!element) return std::nullopt;
        next = StreamType(arrayType(*element));
        break;
      }
      case PortPart::Kind::Group:
        next = knownPortType(*part.inner, environment);
        break;
      case PortPart::Kind::Expression: {
        const auto input = knownInput(*part.expression, environment);
        const auto output = knownOutput(*part.expression, environment);
        if (!input || !output || !equals(*input, *output)) return std::nullopt;
        next = input;
        break;
      }
    }
    if (!next) return std::nullopt;
    result = pack(result, *next);
  }
  return result;
}

std::optional<StreamType> knownZeroArgumentAlias(
    const std::string& name, std::size_t position,
    const Environment& environment) {
  const auto found = environment.typeAliases.find(name);
  if (found == environment.typeAliases.end() ||
      !found->second->parameters.empty()) {
    return std::nullopt;
  }
  PortPart reference;
  reference.kind = PortPart::Kind::Named;
  reference.name = name;
  reference.position = position;
  const TypeBindings& bindings = environment.typeBindings;
  std::vector<std::string> activeAliases;
  return resolveNamedType(reference, environment, bindings, activeAliases);
}

Port clonePort(const Port& source);
ExprPtr cloneExpr(const Expr& source);

Port clonePort(const Port& source) {
  Port result;
  result.parts.reserve(source.parts.size());
  for (const PortPart& part : source.parts) {
    PortPart copy;
    copy.kind = part.kind;
    copy.type = part.type;
    copy.name = part.name;
    copy.position = part.position;
    if (part.inner) copy.inner = std::make_unique<Port>(clonePort(*part.inner));
    if (part.expression) copy.expression = cloneExpr(*part.expression);
    copy.arguments.reserve(part.arguments.size());
    for (const std::unique_ptr<Port>& argument : part.arguments) {
      copy.arguments.push_back(std::make_unique<Port>(clonePort(*argument)));
    }
    result.parts.push_back(std::move(copy));
  }
  return result;
}

ExprPtr cloneExpr(const Expr& source) {
  auto result = std::make_unique<Expr>();
  result->kind = source.kind;
  result->domain = clonePort(source.domain);
  result->codomain = clonePort(source.codomain);
  result->hasDomain = source.hasDomain;
  result->hasCodomain = source.hasCodomain;
  result->core = source.core;
  result->corePosition = source.corePosition;
  result->integer = source.integer;
  result->text = source.text;
  result->templateInstanceKey = source.templateInstanceKey;
  result->templateArguments.reserve(source.templateArguments.size());
  for (const Port& argument : source.templateArguments) {
    result->templateArguments.push_back(clonePort(argument));
  }
  result->elements.reserve(source.elements.size());
  for (const ExprPtr& element : source.elements) {
    result->elements.push_back(element ? cloneExpr(*element) : nullptr);
  }
  if (source.left) result->left = cloneExpr(*source.left);
  if (source.right) result->right = cloneExpr(*source.right);
  result->in = source.in;
  result->out = source.out;
  result->implicitComposition = source.implicitComposition;
  result->parenthesized = source.parenthesized;
  result->position = source.position;
  return result;
}

bool isTemplateParameter(const std::vector<std::string>& parameters,
                         const std::string& name) {
  return std::find(parameters.begin(), parameters.end(), name) != parameters.end();
}

bool bindTemplateType(const TypePtr& pattern, const TypePtr& actual,
                      const std::vector<std::string>& parameters,
                      TypeBindings& bindings);

bool bindTemplateStream(const StreamType& pattern, const StreamType& actual,
                        const std::vector<std::string>& parameters,
                        TypeBindings& bindings) {
  if (!pattern.empty() && pattern.value->kind == Type::Kind::Variable &&
      isTemplateParameter(parameters, pattern.value->name)) {
    const auto found = bindings.find(pattern.value->name);
    if (found != bindings.end()) return equals(found->second, actual);
    bindings.emplace(pattern.value->name, actual);
    return true;
  }
  if (pattern.empty() || actual.empty()) return pattern.empty() == actual.empty();
  return bindTemplateType(pattern.value, actual.value, parameters, bindings);
}

bool bindTemplateType(const TypePtr& pattern, const TypePtr& actual,
                      const std::vector<std::string>& parameters,
                      TypeBindings& bindings) {
  if (pattern && pattern->kind == Type::Kind::Variable &&
      isTemplateParameter(parameters, pattern->name)) {
    const StreamType value(actual);
    const auto found = bindings.find(pattern->name);
    if (found != bindings.end()) return equals(found->second, value);
    bindings.emplace(pattern->name, value);
    return true;
  }
  if (!pattern || !actual || pattern->kind != actual->kind) return false;
  switch (pattern->kind) {
    case Type::Kind::Int:
    case Type::Kind::Str:
    case Type::Kind::Bool:
      return true;
    case Type::Kind::Array:
      return bindTemplateStream(pattern->elements, actual->elements,
                                parameters, bindings);
    case Type::Kind::Pack:
      return bindTemplateType(pattern->left, actual->left, parameters, bindings) &&
             bindTemplateType(pattern->right, actual->right, parameters, bindings);
    case Type::Kind::Nominal:
      if (pattern->name != actual->name ||
          pattern->arguments.size() != actual->arguments.size()) {
        return false;
      }
      for (std::size_t i = 0; i < pattern->arguments.size(); ++i) {
        if (!bindTemplateStream(pattern->arguments[i], actual->arguments[i],
                                parameters, bindings)) {
          return false;
        }
      }
      return true;
    case Type::Kind::Variable:
      return pattern->name == actual->name;
  }
  return false;
}

StreamType substituteTemplateStream(const StreamType& source,
                                    const TypeBindings& bindings);

StreamType substituteTemplateType(const TypePtr& source,
                                  const TypeBindings& bindings) {
  if (source->kind == Type::Kind::Variable) {
    const auto found = bindings.find(source->name);
    return found == bindings.end() ? StreamType(source) : found->second;
  }
  switch (source->kind) {
    case Type::Kind::Int:
    case Type::Kind::Str:
    case Type::Kind::Bool:
    case Type::Kind::Variable:
      return StreamType(source);
    case Type::Kind::Array:
      return StreamType(arrayType(
          substituteTemplateStream(source->elements, bindings)));
    case Type::Kind::Pack:
      return pack(substituteTemplateType(source->left, bindings),
                  substituteTemplateType(source->right, bindings));
    case Type::Kind::Nominal: {
      std::vector<StreamType> arguments;
      arguments.reserve(source->arguments.size());
      for (const StreamType& argument : source->arguments) {
        arguments.push_back(substituteTemplateStream(argument, bindings));
      }
      return StreamType(nominalType(
          source->name, std::move(arguments),
          substituteTemplateStream(source->representation, bindings)));
    }
  }
  return {};
}

StreamType substituteTemplateStream(const StreamType& source,
                                    const TypeBindings& bindings) {
  if (source.empty()) return {};
  return substituteTemplateType(source.value, bindings);
}

struct FunctionMatch {
  const FunctionOverload* overload = nullptr;
  Signature signature;
  TypeBindings bindings;
  std::string instanceKey;
};

std::string makeTemplateInstanceKey(const FunctionOverload& overload,
                                    const TypeBindings& bindings,
                                    const Signature& signature) {
  std::string key = overload.definition ? overload.definition->name : "";
  key += "<";
  for (const std::string& parameter : overload.templateParameters) {
    const auto found = bindings.find(parameter);
    key += std::to_string(parameter.size()) + ":" + parameter + "=";
    if (found != bindings.end()) {
      const std::string value = toString(found->second);
      key += std::to_string(value.size()) + ":" + value;
    }
    key += ";";
  }
  key += ">@" + toString(signature.first) + "->" +
         toString(signature.second);
  return key;
}

std::optional<FunctionMatch> matchFunctionOverload(
    const FunctionOverload& overload,
    const std::vector<StreamType>& explicitArguments,
    const std::optional<StreamType>& input,
    const std::optional<StreamType>& output) {
  if (overload.templateParameters.empty()) {
    if (!explicitArguments.empty()) return std::nullopt;
    if (input && !equals(*input, overload.signature.first)) return std::nullopt;
    if (output && !equals(overload.signature.second, *output)) return std::nullopt;
    return FunctionMatch{&overload, overload.signature, {}, {}};
  }
  if (!explicitArguments.empty() &&
      explicitArguments.size() != overload.templateParameters.size()) {
    return std::nullopt;
  }

  TypeBindings bindings;
  for (std::size_t i = 0; i < explicitArguments.size(); ++i) {
    bindings.emplace(overload.templateParameters[i], explicitArguments[i]);
  }
  if (input && !bindTemplateStream(overload.signature.first, *input,
                                   overload.templateParameters, bindings)) {
    return std::nullopt;
  }
  if (output && !bindTemplateStream(overload.signature.second, *output,
                                    overload.templateParameters, bindings)) {
    return std::nullopt;
  }
  for (const std::string& parameter : overload.templateParameters) {
    if (bindings.find(parameter) == bindings.end()) return std::nullopt;
  }

  Signature signature{
      substituteTemplateStream(overload.signature.first, bindings),
      substituteTemplateStream(overload.signature.second, bindings)};
  if (input && !equals(*input, signature.first)) return std::nullopt;
  if (output && !equals(signature.second, *output)) return std::nullopt;
  std::string instanceKey =
      makeTemplateInstanceKey(overload, bindings, signature);
  return FunctionMatch{&overload, std::move(signature), std::move(bindings),
                       std::move(instanceKey)};
}

bool patternEquals(const Signature& left,
                   const std::vector<std::string>& leftParameters,
                   const Signature& right,
                   const std::vector<std::string>& rightParameters) {
  TypeBindings leftNames;
  TypeBindings rightNames;
  for (std::size_t i = 0; i < leftParameters.size(); ++i) {
    const std::string canonical = "$" + std::to_string(i);
    leftNames.emplace(leftParameters[i],
                      StreamType(templateVariableType(canonical)));
  }
  for (std::size_t i = 0; i < rightParameters.size(); ++i) {
    const std::string canonical = "$" + std::to_string(i);
    rightNames.emplace(rightParameters[i],
                       StreamType(templateVariableType(canonical)));
  }
  return equals(substituteTemplateStream(left.first, leftNames),
                substituteTemplateStream(right.first, rightNames)) &&
         equals(substituteTemplateStream(left.second, leftNames),
                substituteTemplateStream(right.second, rightNames));
}

struct TypeBindingScope {
  TypeBindingScope(Environment& environment, const TypeBindings& bindings)
      : environment_(environment), previous_(std::move(environment.typeBindings)) {
    environment_.typeBindings = bindings;
  }
  ~TypeBindingScope() { environment_.typeBindings = std::move(previous_); }
  TypeBindingScope(const TypeBindingScope&) = delete;
  TypeBindingScope& operator=(const TypeBindingScope&) = delete;

 private:
  Environment& environment_;
  TypeBindings previous_;
};

void ensureTemplateSpecialization(const FunctionMatch& match,
                                 Environment& environment,
                                 std::size_t position) {
  if (match.overload == nullptr || match.overload->templateParameters.empty() ||
      match.overload->definition == nullptr ||
      environment.specializations == nullptr) {
    return;
  }
  for (const Definition& specialization : *environment.specializations) {
    if (specialization.templateInstanceKey == match.instanceKey) return;
  }
  if (std::find(environment.activeSpecializations.begin(),
                environment.activeSpecializations.end(), match.instanceKey) !=
      environment.activeSpecializations.end()) {
    return;
  }
  if (environment.specializations->size() >= 4096) {
    throw Error("too many function-template specializations", position);
  }

  environment.activeSpecializations.push_back(match.instanceKey);
  ExprPtr body = cloneExpr(*match.overload->definition->body);
  {
    TypeBindingScope scope(environment, match.bindings);
    checkExpr(*body, environment, &match.signature.first, &match.signature.second);
  }
  if (!canConvert(match.signature.first, body->in) ||
      !canConvert(body->out, match.signature.second)) {
    environment.activeSpecializations.pop_back();
    throw Error("function template '" + match.overload->definition->name +
                    "' instantiated as " + toString(match.signature.first) +
                    " -> " + toString(match.signature.second) +
                    " but its body is " + describe(*body),
                body->position);
  }

  Definition instance;
  instance.name = match.overload->definition->name;
  instance.hasSignature = true;
  instance.body = std::move(body);
  instance.in = match.signature.first;
  instance.out = match.signature.second;
  instance.templateInstanceKey = match.instanceKey;
  instance.position = match.overload->definition->position;
  instance.end = match.overload->definition->end;
  environment.specializations->push_back(std::move(instance));
  environment.activeSpecializations.pop_back();
}

/// Infer a syntactically available input/output type without committing an
/// overloaded call. These summaries help Pack products split a known value.
std::optional<StreamType> knownInput(const Expr& expr,
                                     const Environment& environment) {
  switch (expr.kind) {
    case Expr::Kind::Wire:
      return knownPortType(expr.domain, environment);

    case Expr::Kind::Face:
      if (expr.hasDomain) return knownPortType(expr.domain, environment);
      switch (expr.core) {
        case Core::IntLiteral:
        case Core::StringLiteral:
        case Core::BoolLiteral:
          return StreamType{};
        case Core::ArrayLiteral:
          return expr.elements.empty() ? std::nullopt
                                       : std::optional<StreamType>(StreamType{});
        case Core::Not:
          return StreamType(boolType());
        default:
          return std::nullopt;
      }

    case Expr::Kind::Call: {
      if (expr.hasDomain) return knownPortType(expr.domain, environment);
      const auto typeBinding = environment.typeBindings.find(expr.text);
      if (typeBinding != environment.typeBindings.end() &&
          expr.templateArguments.empty()) {
        return typeBinding->second;
      }
      const auto found = environment.functions.find(expr.text);
      if (found == environment.functions.end() || found->second.empty()) {
        return knownZeroArgumentAlias(expr.text, expr.position, environment);
      }
      if (std::any_of(found->second.begin(), found->second.end(),
                      [](const FunctionOverload& overload) {
                        return !overload.templateParameters.empty();
                      })) {
        return std::nullopt;
      }
      const StreamType& input = found->second.front().signature.first;
      for (const FunctionOverload& overload : found->second) {
        if (!equals(overload.signature.first, input)) return std::nullopt;
      }
      return input;
    }

    case Expr::Kind::Compose:
    case Expr::Kind::Coerce:
      return knownInput(*expr.left, environment);

    case Expr::Kind::Append: {
      const auto left = knownInput(*expr.left, environment);
      const auto right = knownInput(*expr.right, environment);
      if (!left || !right) return std::nullopt;
      return pack(*left, *right);
    }

    case Expr::Kind::Loop:
    case Expr::Kind::Branch:
      return knownInput(*expr.right, environment);
  }
  return std::nullopt;
}

std::optional<StreamType> knownOutput(const Expr& expr,
                                      const Environment& environment) {
  switch (expr.kind) {
    case Expr::Kind::Wire:
      return knownPortType(expr.domain, environment);

    case Expr::Kind::Face:
      if (expr.hasCodomain) return knownPortType(expr.codomain, environment);
      switch (expr.core) {
        case Core::IntLiteral: return StreamType(intType());
        case Core::StringLiteral: return StreamType(strType());
        case Core::BoolLiteral: return StreamType(boolType());
        case Core::Not: return StreamType(boolType());
        case Core::Mul:
        case Core::Div:
        case Core::Mod: return StreamType(intType());
        case Core::ArrayLiteral:
          if (!expr.elements.empty()) {
            const auto first = knownOutput(*expr.elements.front(), environment);
            if (first) return StreamType(arrayType(*first));
          }
          return std::nullopt;
        default: return std::nullopt;
      }

    case Expr::Kind::Call: {
      if (expr.hasCodomain) return knownPortType(expr.codomain, environment);
      const auto typeBinding = environment.typeBindings.find(expr.text);
      if (typeBinding != environment.typeBindings.end() &&
          expr.templateArguments.empty()) {
        return typeBinding->second;
      }
      const auto found = environment.functions.find(expr.text);
      if (found == environment.functions.end() || found->second.empty()) {
        return knownZeroArgumentAlias(expr.text, expr.position, environment);
      }
      if (std::any_of(found->second.begin(), found->second.end(),
                      [](const FunctionOverload& overload) {
                        return !overload.templateParameters.empty();
                      })) {
        return std::nullopt;
      }
      const StreamType& output = found->second.front().signature.second;
      for (const FunctionOverload& overload : found->second) {
        if (!equals(overload.signature.second, output)) return std::nullopt;
      }
      return output;
    }

    case Expr::Kind::Compose:
    case Expr::Kind::Coerce:
      return knownOutput(*expr.right, environment);

    case Expr::Kind::Append: {
      const auto left = knownOutput(*expr.left, environment);
      const auto right = knownOutput(*expr.right, environment);
      if (!left || !right) return std::nullopt;
      return pack(*left, *right);
    }

    case Expr::Kind::Loop:
    case Expr::Kind::Branch:
      return knownOutput(*expr.right, environment);
  }
  return std::nullopt;
}

// ---- faces --------------------------------------------------------------

void checkLiteral(Expr& expr, const std::optional<StreamType>& input,
                  const std::optional<StreamType>& output) {
  if (input && !input->empty()) {
    fail(expr, "a literal has an empty input state");
  }

  StreamType result;
  if (expr.core == Core::StringLiteral) {
    result = StreamType(strType());
  } else if (expr.core == Core::BoolLiteral) {
    result = StreamType(boolType());
  } else if (output && isBool(*output)) {
    if (expr.integer != 0 && expr.integer != 1) {
      fail(expr, "a boolean literal must be 0 or 1");
    }
    expr.core = Core::BoolLiteral;
    result = StreamType(boolType());
  } else {
    result = StreamType(intType());
  }

  if (output) {
    if (!canConvert(result, *output)) {
      fail(expr, "literal type " + toString(result) +
                     " conflicts with expected type " + toString(*output));
    }
    result = *output;
  }
  expr.in = {};
  expr.out = result;
}

bool isLiteralExpression(const Expr& expr) {
  if (expr.kind == Expr::Kind::Append) {
    return isLiteralExpression(*expr.left) && isLiteralExpression(*expr.right);
  }
  if (expr.kind != Expr::Kind::Face) return false;

  switch (expr.core) {
    case Core::IntLiteral:
    case Core::BoolLiteral:
    case Core::StringLiteral:
      return true;
    case Core::ArrayLiteral:
      for (const ExprPtr& item : expr.elements) {
        if (!isLiteralExpression(*item)) return false;
      }
      return true;
    default:
      return false;
  }
}

/// Empty `{}` is context-sensitive: with input o it is an empty literal, with
/// q it allocates a default-filled array, and with q,T it fills from T.
void checkArrayLiteral(Expr& expr, const std::optional<StreamType>& input,
                       const std::optional<StreamType>& output,
                       Environment& environment) {
  if (expr.elements.empty()) {
    if (!input || input->empty()) {
      if (!output || !isArray(*output)) {
        fail(expr, "an empty array literal needs an expected array element type");
      }
      expr.core = Core::ArrayLiteral;
      expr.in = {};
      expr.out = *output;
      return;
    }

    if (isInt(*input)) {
      if (!output || !isArray(*output)) {
        fail(expr, "a default-filled array needs an expected array element type");
      }
      expr.core = Core::ArrayDefault;
      expr.in = *input;
      expr.out = *output;
      return;
    }

    if (isPack(*input) && isInt(firstOf(*input))) {
      const StreamType element = secondOf(*input);
      const StreamType expected = StreamType(arrayType(element));
      if (output && !canConvert(expected, *output)) {
        fail(expr, "array fill input has element type " + toString(element) +
                       ", but the expected array type is " + toString(*output));
      }
      expr.core = Core::ArrayFill;
      expr.in = *input;
      expr.out = output ? *output : expected;
      return;
    }

    fail(expr, "empty '{}' expects o, q, or q,T as its input state");
  }

  if (input && !input->empty()) {
    fail(expr, "a fixed array literal has an empty input state");
  }
  if (output && !isArray(*output)) {
    fail(expr, "an array literal must produce an array");
  }

  std::optional<StreamType> element;
  if (output) element = arrayElements(*output);
  const StreamType empty;

  for (const ExprPtr& item : expr.elements) {
    checkExpr(*item, environment, &empty, element ? &*element : nullptr);
    if (!isLiteralExpression(*item)) {
      throw Error("an array element must be a closed literal expression",
                  item->position);
    }
    if (!item->in.empty()) {
      throw Error("an array element must have an empty input state",
                  item->position);
    }
    if (!element) element = item->out;
    if (!canConvert(item->out, *element)) {
      throw Error("array literal elements must all have compatible types; expected " +
                      toString(*element) + ", found " + toString(item->out),
                  item->position);
    }
  }

  if (!element) {
    fail(expr, "cannot infer the element type of an empty array literal");
  }
  const StreamType result(arrayType(*element));
  if (output && !canConvert(result, *output)) {
    fail(expr, "array literal has type " + toString(result) +
                   ", but the expected type is " + toString(*output));
  }
  expr.in = {};
  expr.out = output ? *output : result;
}

struct CoreCandidate {
  Core core;
  Signature signature;
  const FunctionOverload* overload = nullptr;
  TypeBindings bindings;
  std::string instanceKey;
};

void addCandidate(std::vector<CoreCandidate>& candidates, Core core,
                  const StreamType& input, const StreamType& output) {
  for (const CoreCandidate& candidate : candidates) {
    if (candidate.overload == nullptr &&
        equals(candidate.signature.first, input) &&
        equals(candidate.signature.second, output)) {
      return;
    }
  }
  candidates.push_back(CoreCandidate{core, Signature{input, output}, nullptr, {}, {}});
}

std::string operatorFunctionName(Core core) {
  switch (core) {
    case Core::Add: return "operator(+)";
    case Core::Sub:
    case Core::Negate: return "operator(-)";
    case Core::Mul: return "operator(*)";
    case Core::Div: return "operator(/)";
    case Core::Mod: return "operator(%)";
    case Core::BitOr: return "operator(|)";
    case Core::BitAnd: return "operator(&)";
    case Core::BitXor: return "operator(^)";
    case Core::Not: return "operator(!)";
    case Core::Equal: return "operator(==)";
    case Core::NotEqual: return "operator(!=)";
    case Core::Greater: return "operator(>)";
    case Core::Less: return "operator(<)";
    case Core::GreaterEqual: return "operator(>=)";
    case Core::LessEqual: return "operator(<=)";
    case Core::Duplicate: return "operator($)";
    case Core::Swap: return "operator(.)";
    case Core::Hash:
    case Core::Reshape: return "operator(#)";
    case Core::Index:
    case Core::ArrayWrite:
    case Core::ArrayLength: return "operator([])";
    default: return {};
  }
}

void addUserOperatorCandidates(std::vector<CoreCandidate>& candidates,
                               Core core,
                               const std::optional<StreamType>& input,
                               const std::optional<StreamType>& output,
                               Environment& environment) {
  const std::string functionName = operatorFunctionName(core);
  if (functionName.empty()) return;
  const auto found = environment.functions.find(functionName);
  if (found == environment.functions.end()) return;

  for (const FunctionOverload& overload : found->second) {
    const auto match = matchFunctionOverload(overload, {}, input, output);
    if (!match) continue;
    candidates.push_back(CoreCandidate{
        core, match->signature, &overload, match->bindings,
        match->instanceKey});
  }
}

void selectCoreCandidate(Expr& expr, std::vector<CoreCandidate> candidates,
                         const std::optional<StreamType>& input,
                         const std::optional<StreamType>& output,
                         Environment& environment) {
  addUserOperatorCandidates(candidates, expr.core, input, output, environment);
  std::vector<const CoreCandidate*> matching;
  for (const CoreCandidate& candidate : candidates) {
    if (input && !equals(*input, candidate.signature.first)) continue;
    if (output && !equals(candidate.signature.second, *output)) continue;
    matching.push_back(&candidate);
  }

  if (matching.empty()) {
    throw Error("operator '" + std::string(name(expr.core)) +
                    "' has no signature matching the available input/output types",
                expr.position);
  }
  if (matching.size() != 1) {
    throw Error("cannot infer which overload of operator '" +
                    std::string(name(expr.core)) +
                    "' is intended; provide an input or output type",
                expr.position);
  }
  const CoreCandidate& selected = *matching.front();
  expr.core = selected.core;
  expr.in = selected.signature.first;
  expr.out = selected.signature.second;
  if (selected.overload != nullptr) {
    expr.kind = Expr::Kind::Call;
    expr.text = selected.overload->definition->name;
    expr.templateInstanceKey = selected.instanceKey;
    const FunctionMatch match{selected.overload, selected.signature,
                              selected.bindings, selected.instanceKey};
    ensureTemplateSpecialization(match, environment, expr.position);
  }
}

void checkIndex(Expr& expr, const std::optional<StreamType>& input,
                const std::optional<StreamType>& output,
                Environment& environment) {
  std::vector<CoreCandidate> candidates;
  if (input) {
    if (isArray(*input)) {
      addCandidate(candidates, Core::ArrayLength, *input,
                   StreamType(intType()));
    }
    if (isPack(*input)) {
      const StreamType first = firstOf(*input);
      const StreamType second = secondOf(*input);
      if (isArray(first) && isInt(second)) {
        addCandidate(candidates, Core::Index, *input, arrayElements(first));
      }
      if (isPack(first)) {
        const StreamType array = firstOf(first);
        const StreamType index = secondOf(first);
        const StreamType element = arrayElements(array);
        if (isArray(array) && isInt(index) && !element.empty() &&
            canConvert(second, element)) {
          addCandidate(candidates, Core::ArrayWrite, *input,
                       StreamType(arrayType(element)));
        }
      }
    }
  }

  if (output && isArray(*output)) {
    const StreamType element = arrayElements(*output);
    if (!element.empty()) {
      addCandidate(candidates, Core::ArrayWrite,
                   pack(pack(*output, StreamType(intType())), element), *output);
    }
  } else if (output && !equals(*output, StreamType(intType()))) {
    addCandidate(candidates, Core::Index,
                 pack(StreamType(arrayType(*output)), StreamType(intType())),
                 *output);
  }

  selectCoreCandidate(expr, std::move(candidates), input, output, environment);
}

void checkFace(Expr& expr, const std::optional<StreamType>& input,
               const std::optional<StreamType>& output,
               Environment& environment) {
  switch (expr.core) {
    case Core::IntLiteral:
    case Core::StringLiteral:
    case Core::BoolLiteral:
      checkLiteral(expr, input, output);
      return;

    case Core::ArrayLiteral:
      checkArrayLiteral(expr, input, output, environment);
      return;

    case Core::Index:
      checkIndex(expr, input, output, environment);
      return;

    case Core::Hash: {
      // `#` only regroups Pack structure. Reading a value is a library call.
      std::vector<CoreCandidate> candidates;
      if (input && output && isPack(*input) && isPack(*output) &&
          sameOrderedPackLeaves(*input, *output)) {
        addCandidate(candidates, Core::Reshape, *input, *output);
      }
      selectCoreCandidate(expr, std::move(candidates), input, output, environment);
      return;
    }

    case Core::Negate: {
      std::vector<CoreCandidate> candidates;
      const StreamType q(intType());
      addCandidate(candidates, Core::Negate, q, q);
      addCandidate(candidates, Core::Sub, pack(q, q), q);
      selectCoreCandidate(expr, std::move(candidates), input, output, environment);
      return;
    }

    case Core::Not: {
      const StreamType a(boolType());
      selectCoreCandidate(expr, {CoreCandidate{Core::Not, {a, a}, nullptr, {}, {}}}, input, output, environment);
      return;
    }

    case Core::Duplicate: {
      std::vector<CoreCandidate> candidates;
      if (input && !input->empty()) {
        addCandidate(candidates, Core::Duplicate, *input, pack(*input, *input));
      }
      if (output && isPack(*output) &&
          equals(firstOf(*output), secondOf(*output))) {
        addCandidate(candidates, Core::Duplicate, firstOf(*output), *output);
      }
      selectCoreCandidate(expr, std::move(candidates), input, output, environment);
      return;
    }

    case Core::Swap: {
      std::vector<CoreCandidate> candidates;
      if (input && isPack(*input)) {
        addCandidate(candidates, Core::Swap, *input,
                     pack(secondOf(*input), firstOf(*input)));
      }
      if (output && isPack(*output)) {
        addCandidate(candidates, Core::Swap,
                     pack(secondOf(*output), firstOf(*output)), *output);
      }
      selectCoreCandidate(expr, std::move(candidates), input, output, environment);
      return;
    }

    case Core::Reshape: {
      if (!input || !output) {
        throw Error("cannot infer the input and output for '#'; provide both "
                    "Pack types",
                    expr.position);
      }
      expr.in = *input;
      expr.out = *output;
      if (!isPack(*input) || !isPack(*output)) {
        fail(expr, "'#' regrouping requires Pack input and output types");
      }
      if (!sameOrderedPackLeaves(*input, *output)) {
        fail(expr, "'#' can only regroup Pack structure while preserving the "
                   "ordered leaf types");
      }
      return;
    }

    case Core::Add: {
      std::vector<CoreCandidate> candidates;
      const StreamType q(intType());
      const StreamType s(strType());
      addCandidate(candidates, Core::Add, pack(q, q), q);
      addCandidate(candidates, Core::Add, pack(s, s), s);
      if (input && isPack(*input) && isArray(firstOf(*input)) &&
          canConvert(secondOf(*input), firstOf(*input))) {
        addCandidate(candidates, Core::Add, *input, firstOf(*input));
      }
      if (output && isArray(*output)) {
        addCandidate(candidates, Core::Add, pack(*output, *output), *output);
      }
      selectCoreCandidate(expr, std::move(candidates), input, output, environment);
      return;
    }

    case Core::Sub:
    case Core::Mul:
    case Core::Div:
    case Core::Mod: {
      const StreamType q(intType());
      selectCoreCandidate(expr, {CoreCandidate{expr.core, {pack(q, q), q}, nullptr, {}, {}}}, input, output, environment);
      return;
    }

    case Core::BitOr:
    case Core::BitAnd:
    case Core::BitXor: {
      std::vector<CoreCandidate> candidates;
      const StreamType q(intType());
      const StreamType a(boolType());
      addCandidate(candidates, expr.core, pack(q, q), q);
      addCandidate(candidates, expr.core, pack(a, a), a);
      selectCoreCandidate(expr, std::move(candidates), input, output, environment);
      return;
    }

    case Core::Equal:
    case Core::NotEqual: {
      std::vector<CoreCandidate> candidates;
      if (input && isEqualPair(*input)) {
        addCandidate(candidates, expr.core, *input, StreamType(boolType()));
      }
      selectCoreCandidate(expr, std::move(candidates), input, output, environment);
      return;
    }

    case Core::Greater:
    case Core::Less:
    case Core::GreaterEqual:
    case Core::LessEqual: {
      std::vector<CoreCandidate> candidates;
      const StreamType q(intType());
      const StreamType s(strType());
      const StreamType a(boolType());
      addCandidate(candidates, expr.core, pack(q, q), a);
      addCandidate(candidates, expr.core, pack(s, s), a);
      selectCoreCandidate(expr, std::move(candidates), input, output, environment);
      return;
    }

    case Core::ArrayFill:
    case Core::ArrayDefault:
    case Core::ArrayWrite:
    case Core::ArrayLength:
      throw Error("internal error: unresolved array operator", expr.position);
  }
}

// ---- calls and overloading ----------------------------------------------

std::string describeOverloads(const std::string& name, const Overloads& overloads) {
  std::string text;
  for (const FunctionOverload& overload : overloads) {
    if (!text.empty()) text += " ";
    text += name;
    if (!overload.templateParameters.empty()) {
      text += "<";
      for (std::size_t i = 0; i < overload.templateParameters.size(); ++i) {
        if (i != 0) text += ", ";
        text += overload.templateParameters[i];
      }
      text += ">";
    }
    text += "(" + toString(overload.signature.first) + " -> " +
            toString(overload.signature.second) + ");";
  }
  if (!overloads.empty()) text.pop_back();
  return text;
}

std::vector<StreamType> resolveExplicitTemplateArguments(
    const std::vector<Port>& arguments, Environment& environment) {
  std::vector<StreamType> result;
  result.reserve(arguments.size());
  for (const Port& argument : arguments) {
    Port copy = clonePort(argument);
    result.push_back(checkPort(copy, environment));
  }
  return result;
}

void checkCall(Expr& expr, Environment& environment,
               const StreamType* inputHint, const StreamType* outputHint) {
  const auto found = environment.functions.find(expr.text);
  if (found == environment.functions.end()) {
    if (selfName != nullptr && expr.text == *selfName) {
      throw Error("'" + expr.text +
                      "' cannot be called in its own body: without a signature its "
                      "type is not known yet; write `" + expr.text +
                      "(A -> B) => body`",
                  expr.position);
    }

    const auto typeBinding = environment.typeBindings.find(expr.text);
    const auto alias = environment.typeAliases.find(expr.text);
    if (typeBinding != environment.typeBindings.end() ||
        alias != environment.typeAliases.end()) {
      Port reference;
      PortPart part;
      part.kind = PortPart::Kind::Named;
      part.name = expr.text;
      part.position = expr.position;
      for (const Port& argument : expr.templateArguments) {
        part.arguments.push_back(std::make_unique<Port>(clonePort(argument)));
      }
      reference.parts.push_back(std::move(part));
      const StreamType type = checkPort(reference, environment);

      std::optional<StreamType> annotatedInput;
      std::optional<StreamType> annotatedOutput;
      if (expr.hasDomain) annotatedInput = checkPort(expr.domain, environment);
      if (expr.hasCodomain) annotatedOutput = checkPort(expr.codomain, environment);
      const auto input = constrainType(expr, annotatedInput, inputHint, "input");
      const auto output = constrainType(expr, annotatedOutput, outputHint, "output");
      if ((input && !canConvert(*input, type)) ||
          (output && !canConvert(type, *output))) {
        throw Error("class or type parameter '" + expr.text +
                        "' denotes the identity on " + toString(type),
                    expr.position);
      }

      expr.kind = Expr::Kind::Wire;
      expr.domain = std::move(reference);
      expr.hasDomain = true;
      expr.templateArguments.clear();
      expr.in = type;
      expr.out = type;
      return;
    }

    throw Error("unknown function '" + expr.text + "'", expr.position);
  }

  std::optional<StreamType> annotatedInput;
  std::optional<StreamType> annotatedOutput;
  if (expr.hasDomain) annotatedInput = checkPort(expr.domain, environment);
  if (expr.hasCodomain) annotatedOutput = checkPort(expr.codomain, environment);
  const auto input = constrainType(expr, annotatedInput, inputHint, "input");
  const auto output = constrainType(expr, annotatedOutput, outputHint, "output");
  const std::vector<StreamType> explicitArguments =
      resolveExplicitTemplateArguments(expr.templateArguments, environment);

  std::vector<FunctionMatch> matching;
  for (const FunctionOverload& overload : found->second) {
    auto match = matchFunctionOverload(overload, explicitArguments,
                                       input, output);
    if (match) matching.push_back(std::move(*match));
  }

  if (matching.empty()) {
    throw Error("no overload of '" + expr.text +
                    "' matches the available input/output types; candidates are " +
                    describeOverloads(expr.text, found->second),
                expr.position);
  }
  if (matching.size() != 1) {
    throw Error("ambiguous call to '" + expr.text +
                    "'; provide an input or output type or explicit template "
                    "arguments to choose between " +
                    describeOverloads(expr.text, found->second),
                expr.position);
  }

  const FunctionMatch& selected = matching.front();
  expr.in = selected.signature.first;
  expr.out = selected.signature.second;
  if (!selected.overload->templateParameters.empty()) {
    expr.templateInstanceKey = selected.instanceKey;
    ensureTemplateSpecialization(selected, environment, expr.position);
  }
}

// ---- connectors ---------------------------------------------------------

void checkCompose(Expr& expr, Environment& environment,
                  const StreamType* inputHint, const StreamType* outputHint,
                  const char* symbol) {
  const auto rightInput = knownInput(*expr.right, environment);
  const auto leftOutput = knownOutput(*expr.left, environment);

  if (rightInput) {
    checkExpr(*expr.left, environment, inputHint, &*rightInput);
    checkExpr(*expr.right, environment, &expr.left->out, outputHint);
  } else if (leftOutput) {
    checkExpr(*expr.left, environment, inputHint, nullptr);
    checkExpr(*expr.right, environment, &expr.left->out, outputHint);
  } else if (outputHint != nullptr) {
    // Propagate an expected result backwards through the right-hand side first.
    checkExpr(*expr.right, environment, nullptr, outputHint);
    checkExpr(*expr.left, environment, inputHint, &expr.right->in);
  } else {
    checkExpr(*expr.left, environment, inputHint, nullptr);
    checkExpr(*expr.right, environment, &expr.left->out, nullptr);
  }

  if (!canConvert(expr.left->out, expr.right->in)) {
    throw Error(std::string("type mismatch at '") + symbol + "': " +
                    toString(expr.left->out) + " cannot connect to " +
                    toString(expr.right->in),
                expr.position);
  }
  expr.in = expr.left->in;
  expr.out = expr.right->out;

  if (inputHint && !canConvert(*inputHint, expr.in)) {
    throw Error("expression input type is " + toString(expr.in) +
                    ", but the surrounding context requires " + toString(*inputHint),
                expr.position);
  }
  if (outputHint && !canConvert(expr.out, *outputHint)) {
    throw Error("expression output type is " + toString(expr.out) +
                    ", but the surrounding context requires " + toString(*outputHint),
                expr.position);
  }
}

void checkCoerce(Expr& expr, Environment& environment,
                 const StreamType* inputHint, const StreamType* outputHint) {
  checkExpr(*expr.right, environment, nullptr, outputHint);
  checkExpr(*expr.left, environment, inputHint, nullptr);

  if (!canCoerce(expr.left->out, expr.right->in)) {
    throw Error("no coercion from " + toString(expr.left->out) + " to " +
                    toString(expr.right->in),
                expr.position);
  }
  expr.in = expr.left->in;
  expr.out = expr.right->out;

  if (inputHint && !canConvert(*inputHint, expr.in)) {
    throw Error("expression input type is " + toString(expr.in) +
                    ", but the surrounding context requires " + toString(*inputHint),
                expr.position);
  }
  if (outputHint && !canConvert(expr.out, *outputHint)) {
    throw Error("expression output type is " + toString(expr.out) +
                    ", but the surrounding context requires " + toString(*outputHint),
                expr.position);
  }
}

void checkAppend(Expr& expr, Environment& environment,
                 const StreamType* inputHint, const StreamType* outputHint) {
  std::optional<StreamType> leftInput;
  std::optional<StreamType> rightInput;
  std::optional<StreamType> leftOutput;
  std::optional<StreamType> rightOutput;
  const auto leftKnownInput = knownInput(*expr.left, environment);
  const auto rightKnownInput = knownInput(*expr.right, environment);
  const auto leftKnownOutput = knownOutput(*expr.left, environment);
  const auto rightKnownOutput = knownOutput(*expr.right, environment);

  if (inputHint != nullptr) {
    if (inputHint->empty()) {
      leftInput = StreamType{};
      rightInput = StreamType{};
    } else if (leftKnownInput && leftKnownInput->empty()) {
      leftInput = StreamType{};
      rightInput = *inputHint;
    } else if (rightKnownInput && rightKnownInput->empty()) {
      leftInput = *inputHint;
      rightInput = StreamType{};
    } else if (isPack(*inputHint)) {
      leftInput = firstOf(*inputHint);
      rightInput = secondOf(*inputHint);
    } else if (leftKnownInput && !leftKnownInput->empty() && !rightKnownInput) {
      leftInput = *inputHint;
      rightInput = StreamType{};
    } else if (rightKnownInput && !rightKnownInput->empty() && !leftKnownInput) {
      leftInput = StreamType{};
      rightInput = *inputHint;
    }
  }

  if (outputHint != nullptr) {
    if (outputHint->empty()) {
      leftOutput = StreamType{};
      rightOutput = StreamType{};
    } else if (leftKnownOutput && leftKnownOutput->empty()) {
      leftOutput = StreamType{};
      rightOutput = *outputHint;
    } else if (rightKnownOutput && rightKnownOutput->empty()) {
      leftOutput = *outputHint;
      rightOutput = StreamType{};
    } else if (isPack(*outputHint)) {
      leftOutput = firstOf(*outputHint);
      rightOutput = secondOf(*outputHint);
    } else if (leftKnownOutput && !leftKnownOutput->empty() && !rightKnownOutput) {
      leftOutput = *outputHint;
      rightOutput = StreamType{};
    } else if (rightKnownOutput && !rightKnownOutput->empty() && !leftKnownOutput) {
      leftOutput = StreamType{};
      rightOutput = *outputHint;
    }
  }

  checkExpr(*expr.left, environment,
            leftInput ? &*leftInput : nullptr,
            leftOutput ? &*leftOutput : nullptr);
  checkExpr(*expr.right, environment,
            rightInput ? &*rightInput : nullptr,
            rightOutput ? &*rightOutput : nullptr);

  expr.in = pack(expr.left->in, expr.right->in);
  expr.out = pack(expr.left->out, expr.right->out);
  if (inputHint && !canConvert(*inputHint, expr.in)) {
    throw Error("underscore append input type is " + toString(expr.in) +
                    ", but the context requires " + toString(*inputHint),
                expr.position);
  }
  if (outputHint && !canConvert(expr.out, *outputHint)) {
    throw Error("underscore append output type is " + toString(expr.out) +
                    ", but the context requires " + toString(*outputHint),
                expr.position);
  }
}

void checkControl(Expr& expr, Environment& environment,
                  const StreamType* inputHint, const StreamType* outputHint) {
  std::optional<StreamType> payload;
  if (inputHint != nullptr) payload = *inputHint;
  if (outputHint != nullptr) {
    if (payload && !canConvert(*payload, *outputHint)) {
      throw Error("control expression must preserve its stream type", expr.position);
    }
    payload = *outputHint;
  }
  if (!payload) payload = knownInput(*expr.right, environment);
  if (!payload) payload = knownOutput(*expr.right, environment);
  if (!payload) {
    throw Error("cannot infer the state of this control expression; provide an "
                "input or output type",
                expr.position);
  }

  const StreamType conditionOutput = pack(StreamType(boolType()), *payload);
  checkExpr(*expr.left, environment, &*payload, &conditionOutput);
  checkExpr(*expr.right, environment, &*payload, &*payload);

  const bool isLoop = expr.kind == Expr::Kind::Loop;
  const std::string label = isLoop ? "@ (loop)" : "? (branch)";
  if (!canConvert(*payload, expr.right->in) ||
      !canConvert(expr.right->out, *payload)) {
    throw Error(label + " body must preserve its state, found " +
                    describe(*expr.right),
                expr.right->position);
  }
  if (!canConvert(*payload, expr.left->in)) {
    throw Error(label + " condition must accept " + toString(*payload) +
                    ", found " + toString(expr.left->in),
                expr.left->position);
  }
  if (!canConvert(expr.left->out, conditionOutput)) {
    throw Error(label + " condition must produce " + toString(conditionOutput) +
                    " (a boolean packed with the state), found " +
                    toString(expr.left->out),
                expr.left->position);
  }
  expr.in = *payload;
  expr.out = *payload;
}

// ---- entry --------------------------------------------------------------

void checkExpr(Expr& expr, Environment& environment,
               const StreamType* inputHint, const StreamType* outputHint) {
  switch (expr.kind) {
    case Expr::Kind::Wire: {
      expr.in = checkPort(expr.domain, environment);
      expr.out = expr.in;
      if (inputHint && !canConvert(*inputHint, expr.in)) {
        throw Error("wire has type " + toString(expr.in) +
                        ", but the context requires input " + toString(*inputHint),
                    expr.position);
      }
      if (outputHint && !canConvert(expr.out, *outputHint)) {
        throw Error("wire has type " + toString(expr.out) +
                        ", but the context requires output " + toString(*outputHint),
                    expr.position);
      }
      return;
    }

    case Expr::Kind::Face: {
      std::optional<StreamType> annotationInput;
      std::optional<StreamType> annotationOutput;
      if (expr.hasDomain) annotationInput = checkPort(expr.domain, environment);
      if (expr.hasCodomain) annotationOutput = checkPort(expr.codomain, environment);
      const auto input = constrainType(expr, annotationInput, inputHint, "input");
      const auto output = constrainType(expr, annotationOutput, outputHint, "output");
      checkFace(expr, input, output, environment);
      return;
    }

    case Expr::Kind::Call:
      checkCall(expr, environment, inputHint, outputHint);
      return;

    case Expr::Kind::Compose:
      checkCompose(expr, environment, inputHint, outputHint, "~");
      return;

    case Expr::Kind::Coerce:
      checkCoerce(expr, environment, inputHint, outputHint);
      return;

    case Expr::Kind::Append:
      checkAppend(expr, environment, inputHint, outputHint);
      return;

    case Expr::Kind::Loop:
    case Expr::Kind::Branch:
      checkControl(expr, environment, inputHint, outputHint);
      return;
  }
}

void validateTypeAliases(const Program& program, Environment& environment) {
  for (const TypeAliasDefinition& alias : program.typeAliases) {
    if (environment.typeAliases.find(alias.name) != environment.typeAliases.end()) {
      throw Error("class '" + alias.name + "' is already defined",
                  alias.position);
    }
    environment.typeAliases.emplace(alias.name, &alias);
  }

  for (const TypeAliasDefinition& alias : program.typeAliases) {
    TypeBindings bindings;
    for (const std::string& parameter : alias.parameters) {
      if (bindings.find(parameter) != bindings.end()) {
        throw Error("duplicate type parameter '" + parameter + "'",
                    alias.position);
      }
      bindings.emplace(parameter, StreamType(intType()));
    }

    std::vector<std::string> activeAliases{alias.name};
    const StreamType representation =
        resolveTypePort(alias.body, environment, bindings, activeAliases);
    if (representation.empty()) {
      throw Error("class '" + alias.name +
                      "' must have a non-empty representation",
                  alias.position);
    }
  }
}

}  // namespace

void check(Expr& expression, const Environment& environment) {
  Environment copy = environment;
  const StreamType empty;
  checkExpr(expression, copy, &empty, nullptr);
}

void check(Program& program) {
  Environment environment;
  environment.specializations = &program.specializations;
  program.specializations.clear();
  validateTypeAliases(program, environment);

  const auto registerDefinition = [&](Definition& definition) {
    FunctionOverload candidate;
    candidate.signature = Signature{definition.in, definition.out};
    candidate.templateParameters = definition.templateParameters;
    candidate.definition = &definition;
    Overloads& overloads = environment.functions[definition.name];
    for (const FunctionOverload& overload : overloads) {
      if (patternEquals(overload.signature, overload.templateParameters,
                        candidate.signature, candidate.templateParameters)) {
        throw Error("function '" + definition.name +
                        "' already has the signature " +
                        toString(definition.in) + " -> " +
                        toString(definition.out),
                    definition.position);
      }
    }
    overloads.push_back(std::move(candidate));
  };

  for (Definition& definition : program.definitions) {
    if (definition.external) {
      // A `.awa` declaration: the signature is the whole definition and the
      // body is provided by the runtime.
      if (!definition.templateParameters.empty()) {
        throw Error("a declaration cannot be a template: '" + definition.name +
                        "'",
                    definition.position);
      }
      definition.in = checkPort(definition.domain, environment);
      definition.out = checkPort(definition.codomain, environment);
      if (!hasNative(definition.name, definition.in, definition.out)) {
        // The declaration usually lives in another file, so its offset would
        // point into the wrong line here.
        throw Error("'" + definition.name + "' is declared as " +
                        toString(definition.in) + " -> " + toString(definition.out) +
                        ", but the runtime has no implementation for it",
                    Error::npos);
      }
      registerDefinition(definition);
      continue;
    }

    if (!definition.body) {
      throw Error("function '" + definition.name + "' has no body",
                  definition.position);
    }

    if (!definition.templateParameters.empty()) {
      if (!definition.hasSignature) {
        throw Error("a function template requires an explicit signature",
                    definition.position);
      }
      TypeBindings bindings;
      for (const std::string& parameter : definition.templateParameters) {
        bindings.emplace(parameter,
                         StreamType(templateVariableType(parameter)));
      }
      {
        TypeBindingScope scope(environment, bindings);
        definition.in = checkPort(definition.domain, environment);
        definition.out = checkPort(definition.codomain, environment);
      }
      registerDefinition(definition);
      continue;
    }

    if (!definition.hasSignature) {
      // `name => body`: the body is checked first and its two eyes become the
      // signature. The name is invisible until the body has been checked.
      const SelfScope scope(definition.name);
      checkExpr(*definition.body, environment, nullptr, nullptr);
      definition.in = definition.body->in;
      definition.out = definition.body->out;
      registerDefinition(definition);
      continue;
    }

    definition.in = checkPort(definition.domain, environment);
    definition.out = checkPort(definition.codomain, environment);

    // Bind the name before checking the body, so the body may call itself.
    registerDefinition(definition);
    checkExpr(*definition.body, environment, &definition.in, &definition.out);

    if (!canConvert(definition.in, definition.body->in) ||
        !canConvert(definition.body->out, definition.out)) {
      throw Error("function '" + definition.name + "' is declared as " +
                      toString(definition.in) + " -> " + toString(definition.out) +
                      " but its body is " + describe(*definition.body),
                  definition.body->position);
    }
  }

  // A program starts from the empty stream, but that is reported once the
  // whole program is typed: hinting `o` here would blame a face's own domain
  // instead of saying that the program is not closed.
  if (program.main) checkExpr(*program.main, environment, nullptr, nullptr);
}

}  // namespace qwq
