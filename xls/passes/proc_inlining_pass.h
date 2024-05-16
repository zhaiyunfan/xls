// Copyright 2022 The XLS Authors
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

#ifndef XLS_PASSES_PROC_INLINING_PASS_H_
#define XLS_PASSES_PROC_INLINING_PASS_H_

#include <string_view>

#include "absl/status/statusor.h"
#include "xls/ir/package.h"
#include "xls/passes/optimization_pass.h"
#include "xls/passes/pass_base.h"

namespace xls {

// Pass which inlines all procs into the top-level proc.
//
// Only supports procs without token-typed state.
class ProcInliningPass : public OptimizationPass {
 public:
  static constexpr std::string_view kName = "proc_inlining";
  ProcInliningPass() : OptimizationPass(kName, "Proc inlining") {}
  ~ProcInliningPass() override = default;

 protected:
  absl::StatusOr<bool> RunInternal(Package* p,
                                   const OptimizationPassOptions& options,
                                   PassResults* results) const override;
};

}  // namespace xls

#endif  // XLS_PASSES_PROC_INLINING_PASS_H_
