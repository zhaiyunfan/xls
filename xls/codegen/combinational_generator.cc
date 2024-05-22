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

#include "xls/codegen/combinational_generator.h"

#include <string>

#include "absl/status/statusor.h"
#include "xls/codegen/block_conversion.h"
#include "xls/codegen/block_generator.h"
#include "xls/codegen/codegen_options.h"
#include "xls/codegen/codegen_pass.h"
#include "xls/codegen/codegen_pass_pipeline.h"
#include "xls/codegen/module_signature.h"
#include "xls/codegen/verilog_line_map.pb.h"
#include "xls/common/status/ret_check.h"
#include "xls/common/status/status_macros.h"
#include "xls/delay_model/delay_estimator.h"
#include "xls/ir/node.h"
#include "xls/passes/pass_base.h"

namespace xls {
namespace verilog {

absl::StatusOr<ModuleGeneratorResult> GenerateCombinationalModule(
    FunctionBase* module, const CodegenOptions& options,
    const DelayEstimator* delay_estimator) {
  XLS_ASSIGN_OR_RETURN(CodegenPassUnit unit,
                       FunctionBaseToCombinationalBlock(module, options));

  CodegenPassOptions codegen_pass_options;
  codegen_pass_options.codegen_options = options;
  codegen_pass_options.delay_estimator = delay_estimator;

  PassResults results;
  XLS_RETURN_IF_ERROR(CreateCodegenPassPipeline()
                          ->Run(&unit, codegen_pass_options, &results)
                          .status());
  XLS_RET_CHECK_NE(unit.top_block, nullptr);
  XLS_RET_CHECK(unit.metadata.contains(unit.top_block));
  XLS_RET_CHECK(unit.metadata.at(unit.top_block).signature.has_value());
  VerilogLineMap verilog_line_map;
  XLS_ASSIGN_OR_RETURN(
      std::string verilog,
      GenerateVerilog(unit.top_block, options, &verilog_line_map));

  // TODO: google/xls#1323 - add all block signatures to ModuleGeneratorResult,
  // not just top.
  return ModuleGeneratorResult{
      verilog, verilog_line_map,
      unit.metadata.at(unit.top_block).signature.value()};
}

}  // namespace verilog
}  // namespace xls
