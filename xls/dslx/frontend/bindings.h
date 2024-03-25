// Copyright 2020 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Bindings class (name to defining AST node tracking) for use in parsing.

#ifndef XLS_DSLX_FRONTEND_BINDINGS_H_
#define XLS_DSLX_FRONTEND_BINDINGS_H_

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/pos.h"

namespace xls::dslx {

// Bindings (name references in the environment that map back to definition
// points in the AST) resolve to this BoundNode variant, which are all kinds of
// definitions.
using BoundNode = std::variant<EnumDef*, TypeAlias*, ConstantDef*, NameDef*,
                               BuiltinNameDef*, StructDef*, Import*>;

// Returns a string, useful for reporting in error messages, for the type of the
// AST node contained inside of the given BoundNode variant; e.g. "Import".
std::string BoundNodeGetTypeString(const BoundNode& bn);

// Encodes ParseError data in a canonical way inside of an invalid argument
// error.
//
// When these propagate up to a Python boundary we throw them as exceptions
// using the data encoded in the absl::Status message. See ParseErrorGetData()
// for the utility used to extract the data from the error message text.
inline absl::Status ParseErrorStatus(const Span& span,
                                     std::string_view message) {
  return absl::InvalidArgumentError(
      absl::StrFormat("ParseError: %s %s", span.ToString(), message));
}

inline absl::Status ParseNameErrorStatus(const Span& span,
                                         std::string_view name) {
  return ParseErrorStatus(
      span, absl::StrFormat("Cannot find a definition for name: \"%s\"", name));
}

// If `status` has a message containing a ParseNameErrorStatus payload as
// created above, extracts the name that the ParseNameError is referring to, or
// returns nullopt (e.g. if the status error code is not as expected, or it's
// not an appropriate error message).
std::optional<std::string_view> MaybeExtractParseNameError(
    const absl::Status& status);

struct PositionalErrorData {
  Span span;
  std::string message;
  std::string error_type;

  std::string GetMessageWithType() const {
    return absl::StrCat(error_type, ": ", message);
  }

  bool operator==(const PositionalErrorData& other) const {
    return span == other.span && message == other.message &&
           error_type == other.error_type;
  }
};

// Returns parsed error data, or an error status if "status" is not of the
// special "positional error" format; e.g. of the formal generated by
// ParseErrorStatus() above.
absl::StatusOr<PositionalErrorData> GetPositionalErrorData(
    const absl::Status& status,
    std::optional<std::string_view> target_type = std::nullopt);

// Maps identifiers to the AST node that bound that identifier (also known as
// the lexical environment).
//
// The datatype is "stackable" so that we can easily take the bindings at a
// given point in the program (say in a function) and extend it with a new scope
// by stacking a fresh Bindings object on top (also sometimes referred to as a
// "scope chain"). For example:
//
//    Binding builtin_bindings;
//    builtin_bindings.Add("range", m.Make<BuiltinNameDef>("range"));
//
//    // Create a fresh scope, with no need to copy the builtin_bindings object.
//    Bindings function_bindings(&builtin_bindings);
//    XLS_ASSIGN_OR_RETURN(Function* f, ParseFunction(&function_bindings));
//
// We can do this because bindings are immutable and stack according to lexical
// scope; new bindings in the worst case only shadow previous bindings.
class Bindings {
 public:
  explicit Bindings(Bindings* parent = nullptr);

  // Too easy to confuse chaining a bindings object with its parent vs copy
  // constructing so we rely on an explicit Clone() call instead.
  Bindings(const Bindings& other) = delete;

  // Moving is ok though.
  Bindings(Bindings&& other) = default;
  Bindings& operator=(Bindings&& other) = default;

  // Returns a copy of this bindings object.
  Bindings Clone() const;

  // The "Cronus" method. This adds a child's bindings to this object, i.e., it
  // "commits" changes made in a child Bindings to this parent object.
  void ConsumeChild(Bindings* child) {
    CHECK_EQ(child->parent_, this);
    for (const auto& [k, v] : child->local_bindings_) {
      local_bindings_[k] = v;
    }
  }

  // Returns whether there are any local bindings (i.e. bindings that are not
  // set in parent/ancestors).
  bool HasLocalBindings() const { return !local_bindings_.empty(); }

  // Adds a local binding.
  void Add(std::string name, BoundNode binding) {
    local_bindings_[std::move(name)] = binding;
  }

  // fail! labels must be unique within a function.
  //
  // The labels are used in Verilog assertion labels, though they are given as
  // strings in the DSLX source.
  //
  // If a fail label is duplicated a parse error is returned.
  absl::Status AddFailLabel(const std::string& label, const Span& span);

  // Returns the AST node bound to 'name'.
  std::optional<BoundNode> ResolveNode(std::string_view name) const {
    for (const Bindings* b = this; b != nullptr; b = b->parent_) {
      auto it = b->local_bindings_.find(name);
      if (it != b->local_bindings_.end()) {
        return it->second;
      }
    }
    return std::nullopt;
  }

  bool ResolveNodeIsTypeDefinition(std::string_view name) const {
    std::optional<BoundNode> bn = ResolveNode(name);
    if (!bn) {
      return false;
    }
    return std::holds_alternative<EnumDef*>(*bn) ||
           std::holds_alternative<TypeAlias*>(*bn) ||
           std::holds_alternative<StructDef*>(*bn);
  }

  // As above, but flags a ParseError() if the binding cannot be resolved,
  // attributing the source of the binding resolution as span.
  absl::StatusOr<BoundNode> ResolveNodeOrError(std::string_view name,
                                               const Span& span) const {
    std::optional<BoundNode> result = ResolveNode(name);
    if (result.has_value()) {
      return *result;
    }
    return ParseNameErrorStatus(span, name);
  }

  // Resolves "name" as an AST binding and returns the associated NameDefNode.
  //
  // Returns nullopt if no AST node binding is found associated with "name".
  std::optional<AnyNameDef> ResolveNameOrNullopt(std::string_view name) const;

  // As above, but returns a ParseError status.
  absl::StatusOr<AnyNameDef> ResolveNameOrError(std::string_view name,
                                                const Span& span) const;

  // Returns whether there is an AST binding associated with "name".
  bool HasName(std::string_view name) const {
    return ResolveNode(name).has_value();
  }

  const absl::flat_hash_map<std::string, BoundNode>& local_bindings() const {
    return local_bindings_;
  }

  // Some properties, such as failure labels, are uniquified at a function
  // scope, so in parsing we mark which bindings are the "roll up point" for a
  // function scope so we can check for uniqueness there.
  void NoteFunctionScoped() {
    function_scoped_ = true;
    fail_labels_.emplace();
  }
  bool IsFunctionScoped() const { return function_scoped_; }

  std::vector<std::string> GetLocalBindings() const {
    std::vector<std::string> result;
    for (const auto& [k, _] : local_bindings_) {
      result.push_back(k);
    }
    std::sort(result.begin(), result.end());
    return result;
  }
  absl::flat_hash_set<std::string> GetAllBindings() const {
    absl::flat_hash_set<std::string> result;
    if (parent_ != nullptr) {
      result = parent_->GetAllBindings();
    }
    for (const auto& [k, v] : local_bindings_) {
      result.insert(k);
    }
    return result;
  }

 private:
  Bindings* GetTop();

  Bindings* parent_;
  absl::flat_hash_map<std::string, BoundNode> local_bindings_;

  // Indicates that these bindings were created for a function scope -- this
  // helps us track properties that should be unique at function scope.
  bool function_scoped_ = false;

  // Note: only the function-scoped bindings will have fail_labels_.
  std::optional<absl::flat_hash_set<std::string>> fail_labels_;
};

// Returns the name definition node (either builtin or user-defined) associated
// with the given binding data.
AnyNameDef BoundNodeToAnyNameDef(BoundNode bn);

// Returns the text span where the binding data is defined.
//
// For a builtin name definition, a "fake" span is returned (that spans no
// characters in the filename "<builtin>").
Span BoundNodeGetSpan(BoundNode bn);

}  // namespace xls::dslx

#endif  // XLS_DSLX_FRONTEND_BINDINGS_H_
