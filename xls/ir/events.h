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

#ifndef XLS_IR_EVENTS_H_
#define XLS_IR_EVENTS_H_

#include <string>
#include <vector>

namespace xls {

// Common structure capturing events that can be produced by any XLS interpreter
// (DSLX, IR, JIT, etc.)
struct InterpreterEvents {
  std::vector<std::string> trace_msgs;
};

}  // namespace xls

#endif  // XLS_IR_EVENTS_H
