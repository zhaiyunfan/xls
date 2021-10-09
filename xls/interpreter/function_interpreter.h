// Copyright 2021 The XLS Authors
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

#ifndef XLS_INTERPRETER_FUNCTION_INTERPRETER_H_
#define XLS_INTERPRETER_FUNCTION_INTERPRETER_H_

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "xls/interpreter/ir_interpreter.h"
#include "xls/ir/function.h"
#include "xls/ir/value.h"

namespace xls {


struct FunctionInterpreterResult {
  Value value;
  InterpreterEvents events;
};

// TODO(https://github.com/google/xls/issues/506) 2021-10-08
// Remove the extra interpreter entry point once there is a standard
// side-channel mechanism to use.

// Runs the interpreter on the given function. 'args' are the argument values
// indexed by parameter name. Returns both the value and any events that
// happened while running.
absl::StatusOr<FunctionInterpreterResult> InterpretFunctionWithEvents(
    Function* function, absl::Span<const Value> args);

// Runs the interpreter on the given function. 'args' are the argument values
// indexed by parameter name. Returns only the result value, dropping any
// collected events..
absl::StatusOr<Value> InterpretFunction(Function* function,
                                        absl::Span<const Value> args);

// Runs the interpreter on the function where the arguments are given by name.
// Returns only the result value, dropping collected events.
absl::StatusOr<Value> InterpretFunctionKwargs(
    Function* function, const absl::flat_hash_map<std::string, Value>& args);

}  // namespace xls

#endif  // XLS_INTERPRETER_FUNCTION_INTERPRETER_H_
