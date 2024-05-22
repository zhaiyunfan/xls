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

#include "xls/codegen/block_generator.h"

#include <cstdint>
#include <filesystem>  // NOLINT
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/container/flat_hash_map.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "xls/codegen/block_conversion.h"
#include "xls/codegen/codegen_options.h"
#include "xls/codegen/codegen_pass.h"
#include "xls/codegen/codegen_pass_pipeline.h"
#include "xls/codegen/module_signature.h"
#include "xls/codegen/op_override_impls.h"
#include "xls/codegen/signature_generator.h"
#include "xls/common/logging/log_lines.h"
#include "xls/common/status/matchers.h"
#include "xls/common/status/status_macros.h"
#include "xls/delay_model/delay_estimator.h"
#include "xls/delay_model/delay_estimators.h"
#include "xls/ir/bits.h"
#include "xls/ir/block.h"
#include "xls/ir/channel.h"
#include "xls/ir/channel_ops.h"
#include "xls/ir/function_builder.h"
#include "xls/ir/instantiation.h"
#include "xls/ir/ir_parser.h"
#include "xls/ir/op.h"
#include "xls/ir/package.h"
#include "xls/ir/register.h"
#include "xls/ir/source_location.h"
#include "xls/ir/type.h"
#include "xls/ir/value.h"
#include "xls/passes/pass_base.h"
#include "xls/scheduling/pipeline_schedule.h"
#include "xls/scheduling/run_pipeline_schedule.h"
#include "xls/scheduling/scheduling_options.h"
#include "xls/simulation/module_simulator.h"
#include "xls/simulation/module_testbench.h"
#include "xls/simulation/module_testbench_thread.h"
#include "xls/simulation/testbench_signal_capture.h"
#include "xls/simulation/verilog_test_base.h"
#include "xls/tools/verilog_include.h"

namespace xls {
namespace verilog {
namespace {

using status_testing::StatusIs;
using ::testing::HasSubstr;

constexpr char kTestName[] = "block_generator_test";
constexpr char kTestdataPath[] = "xls/codegen/testdata";

inline constexpr std::string_view kFifoRTLText =
    R"(// simple fifo implementation
module xls_fifo_wrapper (
clk, rst,
push_ready, push_data, push_valid,
pop_ready,  pop_data,  pop_valid);
  parameter Width = 32,
            Depth = 32,
            EnableBypass = 0,
            RegisterPushOutputs = 1,
            RegisterPopOutputs = 1;
  localparam AddrWidth = $clog2(Depth) + 1;
  input  wire             clk;
  input  wire             rst;
  output wire             push_ready;
  input  wire [Width-1:0] push_data;
  input  wire             push_valid;
  input  wire             pop_ready;
  output wire [Width-1:0] pop_data;
  output wire             pop_valid;

  // Require depth be 1 and bypass disabled.
  initial begin
    if (EnableBypass || Depth != 1 || !RegisterPushOutputs) begin
      // FIFO configuration not supported.
      $fatal(1);
    end
  end


  reg [Width-1:0] mem;
  reg full;

  assign push_ready = !full;
  assign pop_valid = full;
  assign pop_data = mem;

  always @(posedge clk) begin
    if (rst == 1'b1) begin
      full <= 1'b0;
    end else begin
      if (push_valid && push_ready) begin
        mem <= push_data;
        full <= 1'b1;
      end else if (pop_valid && pop_ready) begin
        mem <= mem;
        full <= 1'b0;
      end else begin
        mem <= mem;
        full <= full;
      end
    end
  end
endmodule
)";

class BlockGeneratorTest : public VerilogTestBase {
 protected:
  CodegenOptions codegen_options(
      std::optional<std::string> clock_name = std::nullopt) {
    CodegenOptions options;
    options.use_system_verilog(UseSystemVerilog());
    if (clock_name.has_value()) {
      options.clock_name(clock_name.value());
    }
    return options;
  }

  // Make and return a block which adds two u32 numbers.
  absl::StatusOr<Block*> MakeSubtractBlock(std::string_view name,
                                           Package* package) {
    Type* u32 = package->GetBitsType(32);
    BlockBuilder bb(name, package);
    BValue a = bb.InputPort("a", u32);
    BValue b = bb.InputPort("b", u32);
    bb.OutputPort("result", bb.Subtract(a, b));
    return bb.Build();
  }

  // Make and return a register block.
  absl::StatusOr<Block*> MakeRegisterBlock(std::string_view name,
                                           std::string_view clock_name,
                                           Package* package) {
    Type* u32 = package->GetBitsType(32);
    BlockBuilder bb(name, package);
    BValue a = bb.InputPort("a", u32);
    BValue reg_a = bb.InsertRegister(name, a);
    bb.OutputPort("result", reg_a);
    XLS_RETURN_IF_ERROR(bb.block()->AddClockPort(clock_name));
    return bb.Build();
  }

  // Make and return a block which instantiates the given block. Given block
  // should take two u32s (`a` and `b`) and return a u32 (`result`).
  absl::StatusOr<Block*> MakeDelegatingBlock(std::string_view name,
                                             Block* sub_block,
                                             Package* package) {
    Type* u32 = package->GetBitsType(32);
    BlockBuilder bb(name, package);
    BValue x = bb.InputPort("x", u32);
    BValue y = bb.InputPort("y", u32);
    XLS_ASSIGN_OR_RETURN(
        xls::Instantiation * instantiation,
        bb.block()->AddBlockInstantiation(
            absl::StrFormat("%s_instantiation", sub_block->name()), sub_block));
    bb.InstantiationInput(instantiation, "a", x);
    bb.InstantiationInput(instantiation, "b", y);
    BValue result = bb.InstantiationOutput(instantiation, "result");
    bb.OutputPort("z", result);
    return bb.Build();
  }
};

TEST_P(BlockGeneratorTest, AandB) {
  Package package(TestBaseName());

  Type* u32 = package.GetBitsType(32);
  BlockBuilder bb(TestBaseName(), &package);
  BValue a = bb.InputPort("a", u32);
  BValue b = bb.InputPort("b", u32);
  bb.OutputPort("sum", bb.And(a, b));
  XLS_ASSERT_OK_AND_ASSIGN(Block * block, bb.Build());

  XLS_ASSERT_OK_AND_ASSIGN(std::string verilog,
                           GenerateVerilog(block, codegen_options()));

  XLS_ASSERT_OK_AND_ASSIGN(ModuleSignature sig,
                           GenerateSignature(codegen_options(), block));

  ExpectVerilogEqualToGoldenFile(GoldenFilePath(kTestName, kTestdataPath),
                                 verilog);

  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<ModuleTestbench> tb,
                           NewModuleTestbench(verilog, sig));
  XLS_ASSERT_OK_AND_ASSIGN(
      ModuleTestbenchThread * tbt,
      tb->CreateThreadDrivingAllInputs("main", ZeroOrX::kX));
  SequentialBlock& seq = tbt->MainBlock();

  seq.AtEndOfCycle().ExpectX("sum");
  // The combinational module doesn't a connected clock, but the clock can still
  // be used to sequence events in time.
  seq.Set("a", 0).Set("b", 0);
  seq.AtEndOfCycle().ExpectEq("sum", 0);
  seq.Set("a", 0x11ff).Set("b", 0x77bb);
  seq.AtEndOfCycle().ExpectEq("sum", 0x11bb);

  XLS_ASSERT_OK(tb->Run());
}

TEST_P(BlockGeneratorTest, PipelinedAandB) {
  Package package(TestBaseName());

  Type* u32 = package.GetBitsType(32);
  BlockBuilder bb(TestBaseName(), &package);
  BValue a = bb.InputPort("a", u32);
  BValue b = bb.InputPort("b", u32);
  BValue rst = bb.InputPort("the_reset", package.GetBitsType(1));

  // Pipeline register 0.
  BValue p0_a = bb.InsertRegister("p0_a", a, rst,
                                  xls::Reset{.reset_value = Value(UBits(0, 32)),
                                             .asynchronous = false,
                                             .active_low = false});
  BValue p0_b = bb.InsertRegister("p0_b", b, rst,
                                  xls::Reset{.reset_value = Value(UBits(0, 32)),
                                             .asynchronous = false,
                                             .active_low = false});

  // Pipeline register 1.
  BValue p1_sum =
      bb.InsertRegister("p1_sum", bb.And(p0_a, p0_b), rst,
                        xls::Reset{.reset_value = Value(UBits(0, 32)),
                                   .asynchronous = false,
                                   .active_low = false});

  bb.OutputPort("sum", p1_sum);
  XLS_ASSERT_OK(bb.block()->AddClockPort("the_clock"));
  XLS_ASSERT_OK_AND_ASSIGN(Block * block, bb.Build());

  XLS_ASSERT_OK_AND_ASSIGN(
      std::string verilog,
      GenerateVerilog(block, codegen_options().emit_as_pipeline(true)));
  XLS_ASSERT_OK_AND_ASSIGN(
      ModuleSignature sig,
      GenerateSignature(codegen_options("the_clock"), block));

  ExpectVerilogEqualToGoldenFile(GoldenFilePath(kTestName, kTestdataPath),
                                 verilog);

  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<ModuleTestbench> tb,
                           NewModuleTestbench(verilog, sig));
  XLS_ASSERT_OK_AND_ASSIGN(
      ModuleTestbenchThread * tbt,
      tb->CreateThreadDrivingAllInputs("main", ZeroOrX::kX));
  SequentialBlock& seq = tbt->MainBlock();

  seq.AtEndOfCycle().ExpectX("sum");
  seq.Set("a", 0).Set("b", 0);
  seq.AdvanceNCycles(2);
  seq.AtEndOfCycle().ExpectEq("sum", 0);

  seq.Set("a", 0x11ff).Set("b", 0x77bb);
  seq.AdvanceNCycles(2);
  seq.AtEndOfCycle().ExpectEq("sum", 0x11bb);

  seq.Set("the_reset", 1);
  seq.NextCycle();
  seq.AtEndOfCycle().ExpectEq("sum", 0);

  seq.Set("the_reset", 0);
  seq.NextCycle();
  seq.AtEndOfCycle().ExpectEq("sum", 0);
  seq.AtEndOfCycle().ExpectEq("sum", 0x11bb);

  XLS_ASSERT_OK(tb->Run());
}

TEST_P(BlockGeneratorTest, PipelinedAandBNoReset) {
  Package package(TestBaseName());

  Type* u32 = package.GetBitsType(32);
  BlockBuilder bb(TestBaseName(), &package);
  BValue a = bb.InputPort("a", u32);
  BValue b = bb.InputPort("b", u32);

  // Pipeline register 0.
  BValue p0_a = bb.InsertRegister("p0_a", a);
  BValue p0_b = bb.InsertRegister("p0_b", b);

  // Pipeline register 1.
  BValue p1_sum = bb.InsertRegister("p1_sum", bb.And(p0_a, p0_b));

  bb.OutputPort("sum", p1_sum);
  XLS_ASSERT_OK(bb.block()->AddClockPort("the_clock"));
  XLS_ASSERT_OK_AND_ASSIGN(Block * block, bb.Build());

  XLS_ASSERT_OK_AND_ASSIGN(std::string verilog,
                           GenerateVerilog(block, codegen_options()));
  XLS_ASSERT_OK_AND_ASSIGN(
      ModuleSignature sig,
      GenerateSignature(codegen_options("the_clock"), block));

  ExpectVerilogEqualToGoldenFile(GoldenFilePath(kTestName, kTestdataPath),
                                 verilog);

  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<ModuleTestbench> tb,
                           NewModuleTestbench(verilog, sig));
  XLS_ASSERT_OK_AND_ASSIGN(
      ModuleTestbenchThread * tbt,
      tb->CreateThreadDrivingAllInputs("main", ZeroOrX::kX));
  SequentialBlock& seq = tbt->MainBlock();

  seq.AtEndOfCycle().ExpectX("sum");
  seq.Set("a", 0).Set("b", 0);
  seq.AtEndOfCycle().ExpectX("sum");
  seq.AtEndOfCycle().ExpectX("sum");
  seq.AtEndOfCycle().ExpectEq("sum", 0);

  seq.Set("a", 0x11ff).Set("b", 0x77bb);
  seq.AdvanceNCycles(2);
  seq.AtEndOfCycle().ExpectEq("sum", 0x11bb);

  XLS_ASSERT_OK(tb->Run());
}

TEST_P(BlockGeneratorTest, Accumulator) {
  Package package(TestBaseName());

  Type* u32 = package.GetBitsType(32);
  BlockBuilder bb(TestBaseName(), &package);
  BValue in = bb.InputPort("in", u32);
  BValue rst_n = bb.InputPort("rst_n", package.GetBitsType(1));

  XLS_ASSERT_OK_AND_ASSIGN(
      Register * accum_reg,
      bb.block()->AddRegister("accum", u32,
                              xls::Reset{.reset_value = Value(UBits(10, 32)),
                                         .asynchronous = false,
                                         .active_low = true}));
  BValue accum = bb.RegisterRead(accum_reg);
  bb.RegisterWrite(accum_reg, bb.Add(in, accum), /*load_enable=*/std::nullopt,
                   rst_n);
  bb.OutputPort("out", accum);
  XLS_ASSERT_OK(bb.block()->AddClockPort("clk"));
  XLS_ASSERT_OK_AND_ASSIGN(Block * block, bb.Build());

  XLS_ASSERT_OK_AND_ASSIGN(std::string verilog,
                           GenerateVerilog(block, codegen_options()));
  XLS_ASSERT_OK_AND_ASSIGN(ModuleSignature sig,
                           GenerateSignature(codegen_options("clk"), block));

  ExpectVerilogEqualToGoldenFile(GoldenFilePath(kTestName, kTestdataPath),
                                 verilog);

  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<ModuleTestbench> tb,
                           NewModuleTestbench(verilog, sig));
  XLS_ASSERT_OK_AND_ASSIGN(
      ModuleTestbenchThread * tbt,
      tb->CreateThreadDrivingAllInputs("main", ZeroOrX::kX));
  SequentialBlock& seq = tbt->MainBlock();

  seq.Set("in", 0).Set("rst_n", 0);
  seq.NextCycle();
  seq.Set("rst_n", 1);
  seq.NextCycle();

  seq.Set("in", 42);
  seq.AtEndOfCycle().ExpectEq("out", 10);
  seq.Set("in", 100);
  seq.AtEndOfCycle().ExpectEq("out", 52);
  seq.Set("in", 0);
  seq.AtEndOfCycle().ExpectEq("out", 152);

  seq.Set("in", 0).Set("rst_n", 0);
  seq.NextCycle();
  seq.Set("rst_n", 1);
  seq.AtEndOfCycle().ExpectEq("out", 10);

  XLS_ASSERT_OK(tb->Run());
}

TEST_P(BlockGeneratorTest, RegisterWithoutClockPort) {
  Package package(TestBaseName());
  Type* u32 = package.GetBitsType(32);

  BlockBuilder bb(TestBaseName(), &package);
  BValue a = bb.InputPort("a", u32);
  XLS_ASSERT_OK_AND_ASSIGN(Block * block, bb.Build());

  XLS_ASSERT_OK_AND_ASSIGN(Register * reg,
                           block->AddRegister("reg", a.node()->GetType()));
  XLS_ASSERT_OK(block
                    ->MakeNode<RegisterWrite>(SourceInfo(), a.node(),
                                              /*load_enable=*/std::nullopt,
                                              /*reset=*/std::nullopt, reg)
                    .status());
  XLS_ASSERT_OK(block->MakeNode<RegisterRead>(SourceInfo(), reg).status());

  EXPECT_THAT(GenerateVerilog(block, codegen_options()).status(),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Block has registers but no clock port")));
}

TEST_P(BlockGeneratorTest, RegisterWithDifferentResetBehavior) {
  Package package(TestBaseName());
  Type* u32 = package.GetBitsType(32);

  BlockBuilder bb(TestBaseName(), &package);
  BValue a = bb.InputPort("a", u32);
  XLS_ASSERT_OK(bb.block()->AddClockPort("clk"));
  BValue rst = bb.InputPort("the_reset", package.GetBitsType(1));
  BValue a_d = bb.InsertRegister("a_d", a, rst,
                                 xls::Reset{.reset_value = Value(UBits(0, 32)),
                                            .asynchronous = false,
                                            .active_low = true});
  bb.InsertRegister("a_d_d", a_d, rst,
                    xls::Reset{.reset_value = Value(UBits(0, 32)),
                               .asynchronous = false,
                               .active_low = false});
  XLS_ASSERT_OK_AND_ASSIGN(Block * block, bb.Build());

  EXPECT_THAT(
      GenerateVerilog(block, codegen_options()).status(),
      StatusIs(
          absl::StatusCode::kInvalidArgument,
          HasSubstr("Block has active low and active high reset signals")));
}

TEST_P(BlockGeneratorTest, BlockWithAssertNoLabel) {
  Package package(TestBaseName());
  BlockBuilder b(TestBaseName(), &package);
  BValue rst = b.InputPort("my_rst", package.GetBitsType(1));
  BValue a = b.InputPort("a", package.GetBitsType(32));
  BValue a_d = b.InsertRegister(
      "a_d", a, rst,
      xls::Reset{/*reset_value=*/Value(UBits(123, 32)),
                 /*asynchronous=*/false, /*active_low=*/false});
  b.Assert(b.AfterAll({}), b.ULt(a_d, b.Literal(UBits(42, 32))),
           "a is not greater than 42");
  XLS_ASSERT_OK(b.block()->AddClockPort("my_clk"));
  XLS_ASSERT_OK_AND_ASSIGN(Block * block, b.Build());
  {
    // No format string.
    XLS_ASSERT_OK_AND_ASSIGN(std::string verilog,
                             GenerateVerilog(block, codegen_options("my_clk")));
    if (UseSystemVerilog()) {
      EXPECT_THAT(
          verilog,
          HasSubstr(
              R"(assert property (@(posedge my_clk) disable iff ($sampled(my_rst)) a_d < 32'h0000_002a) else $fatal(0, "a is not greater than 42");)"));
    } else {
      EXPECT_THAT(verilog, Not(HasSubstr("assert")));
    }
  }

  {
    // With format string, no label.
    XLS_ASSERT_OK_AND_ASSIGN(
        std::string verilog,
        GenerateVerilog(
            block,
            codegen_options("my_clk").SetOpOverride(
                Op::kAssert,
                std::make_unique<OpOverrideAssertion>(
                    R"(`MY_ASSERT({condition}, "{message}", {clk}, {rst}))"))));
    if (UseSystemVerilog()) {
      EXPECT_THAT(
          verilog,
          HasSubstr(
              R"(`MY_ASSERT(a_d < 32'h0000_002a, "a is not greater than 42", my_clk, my_rst))"));
    } else {
      EXPECT_THAT(verilog, Not(HasSubstr("assert")));
    }
  }

  // Format string with label but assert doesn't have label.
  EXPECT_THAT(
      GenerateVerilog(block,
                      codegen_options("my_clk").SetOpOverride(
                          Op::kAssert, std::make_unique<OpOverrideAssertion>(
                                           R"({label} foobar)"))),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("Assert format string has {label} placeholder, "
                         "but assert operation has no label")));

  // Format string with invalid placeholder.
  EXPECT_THAT(
      GenerateVerilog(block,
                      codegen_options("my_clk").SetOpOverride(
                          Op::kAssert, std::make_unique<OpOverrideAssertion>(
                                           R"({foobar} blargfoobar)"))),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("Invalid placeholder {foobar} in format string. "
                         "Valid placeholders: {clk}, {condition}, {label}, "
                         "{message}, {rst}")));
}

TEST_P(BlockGeneratorTest, BlockWithAssertWithLabel) {
  Package package(TestBaseName());
  BlockBuilder b(TestBaseName(), &package);
  BValue a = b.InputPort("a", package.GetBitsType(32));
  b.Assert(b.AfterAll({}), b.ULt(a, b.Literal(UBits(42, 32))),
           "a is not greater than 42", "the_label");
  XLS_ASSERT_OK(b.block()->AddClockPort("my_clk"));
  XLS_ASSERT_OK_AND_ASSIGN(Block * block, b.Build());

  {
    // No format string.
    XLS_ASSERT_OK_AND_ASSIGN(std::string verilog,
                             GenerateVerilog(block, codegen_options("my_clk")));
    if (UseSystemVerilog()) {
      EXPECT_THAT(
          verilog,
          HasSubstr(
              R"(assert property (@(posedge my_clk) disable iff ($sampled($isunknown(a < 32'h0000_002a))) a < 32'h0000_002a) else $fatal(0, "a is not greater than 42");)"));
    } else {
      EXPECT_THAT(verilog, Not(HasSubstr("assert")));
    }
  }

  {
    // With format string.
    XLS_ASSERT_OK_AND_ASSIGN(
        std::string verilog,
        GenerateVerilog(
            block,
            codegen_options("my_clk").SetOpOverride(
                Op::kAssert,
                std::make_unique<OpOverrideAssertion>(
                    R"({label}: `MY_ASSERT({condition}, "{message}") // {label})"))));
    if (UseSystemVerilog()) {
      EXPECT_THAT(
          verilog,
          HasSubstr(
              R"(the_label: `MY_ASSERT(a < 32'h0000_002a, "a is not greater than 42") // the_label)"));
    } else {
      EXPECT_THAT(verilog, Not(HasSubstr("assert")));
    }
  }

  // Format string with reset but block doesn't have reset.
  EXPECT_THAT(GenerateVerilog(
                  block, codegen_options("my_clk").SetOpOverride(
                             Op::kAssert, std::make_unique<OpOverrideAssertion>(
                                              R"({rst} foobar)"))),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Assert format string has {rst} placeholder, "
                                 "but block has no reset signal")));
}

TEST_P(BlockGeneratorTest, AssertCombinationalOrMissingClock) {
  if (!UseSystemVerilog()) {
    return;
  }
  Package package(TestBaseName());
  BlockBuilder b(TestBaseName(), &package);
  BValue a = b.InputPort("a", package.GetBitsType(32));
  b.Assert(b.AfterAll({}), b.ULt(a, b.Literal(UBits(42, 32))),
           "a is not greater than 42", "the_label");
  XLS_ASSERT_OK_AND_ASSIGN(Block * block, b.Build());

  {
    XLS_ASSERT_OK_AND_ASSIGN(std::string verilog,
                             GenerateVerilog(block, codegen_options()));
    EXPECT_THAT(
        verilog,
        HasSubstr(
            R"(assert final ($isunknown(a < 32'h0000_002a) || a < 32'h0000_002a))"));
  }

  {
    XLS_ASSERT_OK_AND_ASSIGN(
        std::string verilog,
        GenerateVerilog(
            block, codegen_options().SetOpOverride(
                       Op::kAssert,
                       std::make_unique<OpOverrideAssertion>(
                           R"(ASSERT({label}, {condition}, "{message}"))"))));
    EXPECT_THAT(
        verilog,
        HasSubstr(
            R"(ASSERT(the_label, a < 32'h0000_002a, "a is not greater than 42"))"));
  }

  EXPECT_THAT(GenerateVerilog(
                  block, codegen_options().SetOpOverride(
                             Op::kAssert, std::make_unique<OpOverrideAssertion>(
                                              R"({clk} foobar)"))),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Assert format string has {clk} placeholder, "
                                 "but block has no clock signal")));
}

TEST_P(BlockGeneratorTest, BlockWithTrace) {
  Package package(TestBaseName());
  BlockBuilder b(TestBaseName(), &package);
  BValue a = b.InputPort("a", package.GetBitsType(32));
  b.Trace(b.AfterAll({}), b.ULt(a, b.Literal(UBits(42, 32))), {a},
          "a ({}) is not greater than 42");
  XLS_ASSERT_OK_AND_ASSIGN(Block * block, b.Build());

  {
    // No format string.
    XLS_ASSERT_OK_AND_ASSIGN(std::string verilog,
                             GenerateVerilog(block, codegen_options()));
    EXPECT_THAT(verilog,
                HasSubstr(R"($display("a (%d) is not greater than 42", a)"));
  }
}

TEST_P(BlockGeneratorTest, BlockWithExtraBracesTrace) {
  Package package(TestBaseName());
  BlockBuilder b(TestBaseName(), &package);
  BValue a = b.InputPort("a", package.GetBitsType(32));
  b.Trace(b.AfterAll({}), b.ULt(a, b.Literal(UBits(42, 32))), {a},
          "{{st0{{a: {}}}}} is not greater than 42");
  XLS_ASSERT_OK_AND_ASSIGN(Block * block, b.Build());

  {
    // No format string.
    XLS_ASSERT_OK_AND_ASSIGN(std::string verilog,
                             GenerateVerilog(block, codegen_options()));
    EXPECT_THAT(
        verilog,
        HasSubstr(R"($display("{st0{a: %d}} is not greater than 42", a)"));
  }
}

TEST_P(BlockGeneratorTest, PortOrderTest) {
  Package package(TestBaseName());
  Type* u32 = package.GetBitsType(32);

  BlockBuilder bb(TestBaseName(), &package);
  BValue a = bb.InputPort("a", u32);
  bb.OutputPort("b", a);
  BValue c = bb.InputPort("c", u32);
  bb.OutputPort("d", c);
  XLS_ASSERT_OK_AND_ASSIGN(Block * block, bb.Build());

  XLS_ASSERT_OK_AND_ASSIGN(std::string verilog,
                           GenerateVerilog(block, codegen_options()));
  EXPECT_THAT(verilog,
              HasSubstr("input wire [31:0] a,\n  input wire [31:0] c,\n  "
                        "output wire [31:0] b,\n  output wire [31:0] d"));
}

TEST_P(BlockGeneratorTest, LoadEnables) {
  // Construct a block with two parallel data paths: "a" and "b". Each consists
  // of a single register with a load enable. Verify that the two load enables
  // work as expected.
  Package package(TestBaseName());

  Type* u1 = package.GetBitsType(1);
  Type* u32 = package.GetBitsType(32);
  BlockBuilder bb(TestBaseName(), &package);
  BValue a = bb.InputPort("a", u32);
  BValue a_le = bb.InputPort("a_le", u1);
  BValue b = bb.InputPort("b", u32);
  BValue b_le = bb.InputPort("b_le", u1);
  BValue rst = bb.InputPort("rst", u1);

  BValue a_reg =
      bb.InsertRegister("a_reg", a, rst,
                        xls::Reset{.reset_value = Value(UBits(42, 32)),
                                   .asynchronous = false,
                                   .active_low = false},
                        a_le);
  BValue b_reg =
      bb.InsertRegister("b_reg", b, rst,
                        xls::Reset{.reset_value = Value(UBits(43, 32)),
                                   .asynchronous = false,
                                   .active_low = false},
                        b_le);

  bb.OutputPort("a_out", a_reg);
  bb.OutputPort("b_out", b_reg);

  XLS_ASSERT_OK(bb.block()->AddClockPort("clk"));
  XLS_ASSERT_OK_AND_ASSIGN(Block * block, bb.Build());

  XLS_ASSERT_OK_AND_ASSIGN(std::string verilog,
                           GenerateVerilog(block, codegen_options()));
  XLS_ASSERT_OK_AND_ASSIGN(ModuleSignature sig,
                           GenerateSignature(codegen_options("clk"), block));
  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<ModuleTestbench> tb,
                           NewModuleTestbench(verilog, sig));
  XLS_ASSERT_OK_AND_ASSIGN(
      ModuleTestbenchThread * tbt,
      tb->CreateThreadDrivingAllInputs("main", ZeroOrX::kX));
  SequentialBlock& seq = tbt->MainBlock();

  // Set inputs to zero and disable load-enables.
  seq.Set("a", 100).Set("b", 200).Set("a_le", 0).Set("b_le", 0).Set("rst", 1);
  seq.NextCycle();
  seq.Set("rst", 0);

  // Outputs should be at the reset value.
  seq.AtEndOfCycle().ExpectEq("a_out", 42).ExpectEq("b_out", 43);

  // Outputs should remain at reset values after clocking because load enables
  // are unasserted.
  seq.AtEndOfCycle().ExpectEq("a_out", 42).ExpectEq("b_out", 43);

  // Assert load enable of 'a'. Load enable of 'b' remains unasserted.
  seq.Set("a_le", 1);
  seq.NextCycle();
  seq.AtEndOfCycle().ExpectEq("a_out", 100).ExpectEq("b_out", 43);

  // Assert load enable of 'b'. Deassert load enable of 'a' and change a's
  // input. New input of 'a' should not propagate.
  seq.Set("a", 101).Set("a_le", 0).Set("b_le", 1);
  seq.NextCycle();
  seq.AtEndOfCycle().ExpectEq("a_out", 100).ExpectEq("b_out", 200);

  // Assert both load enables.
  seq.Set("b", 201).Set("a_le", 1).Set("b_le", 1);
  seq.NextCycle();
  seq.AtEndOfCycle().ExpectEq("a_out", 101).ExpectEq("b_out", 201);

  XLS_ASSERT_OK(tb->Run());
}

TEST_P(BlockGeneratorTest, GatedBitsType) {
  Package package(TestBaseName());
  BlockBuilder b(TestBaseName(), &package);
  BValue cond = b.InputPort("cond", package.GetBitsType(1));
  BValue x = b.InputPort("x", package.GetBitsType(32));
  BValue y = b.InputPort("y", package.GetBitsType(32));
  b.Add(b.Gate(cond, x, SourceInfo(), "gated_x"), y);
  XLS_ASSERT_OK_AND_ASSIGN(Block * block, b.Build());

  {
    // No format string.
    XLS_ASSERT_OK_AND_ASSIGN(std::string verilog,
                             GenerateVerilog(block, codegen_options()));
    EXPECT_THAT(verilog, HasSubstr(R"(wire [31:0] gated_x;)"));
    EXPECT_THAT(verilog, HasSubstr(R"(assign gated_x = {32{cond}} & x;)"));
  }

  {
    // With format string.
    XLS_ASSERT_OK_AND_ASSIGN(
        std::string verilog,
        GenerateVerilog(
            block,
            codegen_options().SetOpOverride(
                Op::kGate,
                std::make_unique<OpOverrideGateAssignment>(
                    R"(my_and {output} [{width}-1:0] = my_and({condition}, {input}))"))));
    EXPECT_THAT(verilog, Not(HasSubstr(R"(wire gated_x [31:0];)")));
    EXPECT_THAT(verilog,
                HasSubstr(R"(my_and gated_x [32-1:0] = my_and(cond, x);)"));
  }
}

TEST_P(BlockGeneratorTest, SmulpWithFormat) {
  Package package(TestBaseName());
  BlockBuilder b(TestBaseName(), &package);
  Type* u32 = package.GetBitsType(32);
  BValue x = b.InputPort("x", u32);
  BValue y = b.InputPort("y", u32);
  BValue x_smulp_y = b.SMulp(x, y, SourceInfo(), "x_smulp_y");
  BValue z = b.InputPort("z", u32);
  BValue z_smulp_z = b.SMulp(z, z, SourceInfo(), "z_smulp_z");
  b.OutputPort("out", b.Tuple({x_smulp_y, z_smulp_z}));
  XLS_ASSERT_OK_AND_ASSIGN(Block * block, b.Build());

  CodegenOptions options = codegen_options().SetOpOverride(
      Op::kSMulp, std::make_unique<OpOverrideInstantiation>(
                      R"(HardMultp #(
  .lhs_width({input0_width}),
  .rhs_width({input1_width}),
  .output_width({output_width})
) {output}_inst (
  .lhs({input0}),
  .rhs({input1}),
  .do_signed(1'b1),
  .output0({output}[({output_width}>>1)-1:0]),
  .output1({output}[({output_width}>>1)*2-1:({output_width}>>1)])
);)"));

  XLS_ASSERT_OK_AND_ASSIGN(std::string verilog,
                           GenerateVerilog(block, options));
  verilog = absl::StrCat("`include \"hardmultp.v\"\n\n", verilog);

  VerilogInclude hardmultp_definition;
  hardmultp_definition.relative_path = "hardmultp.v";
  hardmultp_definition.verilog_text =
      R"(module HardMultp (lhs, rhs, do_signed, output0, output1);
  parameter lhs_width = 32,
    rhs_width = 32,
    output_width = 32;
  input wire [lhs_width-1:0] lhs;
  input wire [rhs_width-1:0] rhs;
  input wire do_signed;
  output wire [output_width-1:0] output0;
  output wire [output_width-1:0] output1;

  assign output0 = 1'b0;
  assign output1 = lhs * rhs;
endmodule
)";

  ExpectVerilogEqualToGoldenFile(GoldenFilePath(kTestName, kTestdataPath),
                                 verilog, /*macro_definitions=*/{},
                                 {hardmultp_definition});
}

TEST_P(BlockGeneratorTest, GatedSingleBitType) {
  Package package(TestBaseName());
  BlockBuilder b(TestBaseName(), &package);
  BValue cond = b.InputPort("cond", package.GetBitsType(1));
  BValue x = b.InputPort("x", package.GetBitsType(1));
  b.Gate(cond, x, SourceInfo(), "gated_x");
  XLS_ASSERT_OK_AND_ASSIGN(Block * block, b.Build());

  XLS_ASSERT_OK_AND_ASSIGN(std::string verilog,
                           GenerateVerilog(block, codegen_options()));
  EXPECT_THAT(verilog, HasSubstr(R"(assign gated_x = cond & x;)"));
}

TEST_P(BlockGeneratorTest, GatedTupleType) {
  Package package(TestBaseName());
  BlockBuilder b(TestBaseName(), &package);
  BValue cond = b.InputPort("cond", package.GetBitsType(1));
  BValue x = b.InputPort("x", package.GetTupleType({package.GetBitsType(32),
                                                    package.GetBitsType(8)}));
  b.Gate(cond, x, SourceInfo(), "gated_x");
  XLS_ASSERT_OK_AND_ASSIGN(Block * block, b.Build());

  XLS_ASSERT_OK_AND_ASSIGN(std::string verilog,
                           GenerateVerilog(block, codegen_options()));
  EXPECT_THAT(verilog, HasSubstr(R"(wire [39:0] gated_x;)"));
  EXPECT_THAT(verilog, HasSubstr(R"(assign gated_x = {40{cond}} & x;)"));
}

TEST_P(BlockGeneratorTest, GatedArrayType) {
  Package package(TestBaseName());
  BlockBuilder b(TestBaseName(), &package);
  BValue cond = b.InputPort("cond", package.GetBitsType(1));
  BValue x = b.InputPort("x", package.GetArrayType(7, package.GetBitsType(32)));
  b.Gate(cond, x, SourceInfo(), "gated_x");
  XLS_ASSERT_OK_AND_ASSIGN(Block * block, b.Build());

  EXPECT_THAT(GenerateVerilog(block, codegen_options()),
              StatusIs(absl::StatusCode::kUnimplemented,
                       HasSubstr("Gate operation only supported for bits and "
                                 "tuple types, has type: bits[32][7]")));
}

TEST_P(BlockGeneratorTest, InstantiatedBlock) {
  Package package(TestBaseName());
  Type* u32 = package.GetBitsType(32);

  XLS_ASSERT_OK_AND_ASSIGN(Block * sub_block,
                           MakeSubtractBlock("subtractor", &package));

  BlockBuilder bb("my_block", &package);
  XLS_ASSERT_OK_AND_ASSIGN(xls::Instantiation * subtractor,
                           bb.block()->AddBlockInstantiation("sub", sub_block));
  BValue x = bb.InputPort("x", u32);
  BValue y = bb.InputPort("y", u32);
  BValue one = bb.Literal(UBits(1, 32));
  bb.InstantiationInput(subtractor, "a", bb.Add(x, one));
  bb.InstantiationInput(subtractor, "b", bb.Subtract(y, one));
  BValue sum = bb.InstantiationOutput(subtractor, "result");
  bb.OutputPort("out", bb.Shll(sum, one));
  XLS_ASSERT_OK_AND_ASSIGN(Block * block, bb.Build());

  XLS_ASSERT_OK_AND_ASSIGN(std::string verilog,
                           GenerateVerilog(block, codegen_options()));
  XLS_ASSERT_OK_AND_ASSIGN(ModuleSignature sig,
                           GenerateSignature(codegen_options(), block));

  ExpectVerilogEqualToGoldenFile(GoldenFilePath(kTestName, kTestdataPath),
                                 verilog);

  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<ModuleTestbench> tb,
                           NewModuleTestbench(verilog, sig));
  XLS_ASSERT_OK_AND_ASSIGN(
      ModuleTestbenchThread * tbt,
      tb->CreateThreadDrivingAllInputs("main", ZeroOrX::kX));
  SequentialBlock& seq = tbt->MainBlock();

  seq.AtEndOfCycle().ExpectX("out");
  // The module doesn't a connected clock, but the clock can still
  // be used to sequence events in time.
  // `out` should be: ((x + 1) - (y - 1)) << 1
  seq.Set("x", 0).Set("y", 0);
  seq.AtEndOfCycle().ExpectEq("out", 4);
  seq.Set("x", 100).Set("y", 42);
  seq.AtEndOfCycle().ExpectEq("out", 120);

  XLS_ASSERT_OK(tb->Run());
}

TEST_P(BlockGeneratorTest, InstantiatedBlockWithClockButNoClock) {
  Package package(TestBaseName());
  Type* u32 = package.GetBitsType(32);

  XLS_ASSERT_OK_AND_ASSIGN(Block * sub_block,
                           MakeRegisterBlock("my_register", "clk", &package));

  BlockBuilder bb("my_block", &package);
  XLS_ASSERT_OK_AND_ASSIGN(
      xls::Instantiation * my_reg,
      bb.block()->AddBlockInstantiation("my_reg", sub_block));
  BValue x = bb.InputPort("x", u32);
  bb.InstantiationInput(my_reg, "a", x);
  BValue result = bb.InstantiationOutput(my_reg, "result");
  bb.OutputPort("out", result);
  XLS_ASSERT_OK_AND_ASSIGN(Block * block, bb.Build());

  EXPECT_THAT(GenerateVerilog(block, codegen_options()).status(),
              StatusIs(absl::StatusCode::kInternal,
                       HasSubstr("The instantiated block requires a clock but "
                                 "the instantiating block has no clock.")));
}

TEST_P(BlockGeneratorTest, InstantiatedBlockWithClock) {
  Package package(TestBaseName());
  Type* u32 = package.GetBitsType(32);

  XLS_ASSERT_OK_AND_ASSIGN(Block * sub_block,
                           MakeRegisterBlock("my_register", "clk", &package));

  BlockBuilder bb("my_block", &package);
  XLS_ASSERT_OK_AND_ASSIGN(
      xls::Instantiation * my_reg,
      bb.block()->AddBlockInstantiation("my_reg", sub_block));
  BValue x = bb.InputPort("x", u32);
  bb.InstantiationInput(my_reg, "a", x);
  BValue result = bb.InstantiationOutput(my_reg, "result");
  bb.OutputPort("out", result);
  XLS_ASSERT_OK(bb.block()->AddClockPort("the_clock"));
  XLS_ASSERT_OK_AND_ASSIGN(Block * block, bb.Build());

  XLS_ASSERT_OK_AND_ASSIGN(std::string verilog,
                           GenerateVerilog(block, codegen_options()));
  XLS_ASSERT_OK_AND_ASSIGN(
      ModuleSignature sig,
      GenerateSignature(codegen_options("the_clock"), block));

  ExpectVerilogEqualToGoldenFile(GoldenFilePath(kTestName, kTestdataPath),
                                 verilog);

  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<ModuleTestbench> tb,
                           NewModuleTestbench(verilog, sig));
  XLS_ASSERT_OK_AND_ASSIGN(
      ModuleTestbenchThread * tbt,
      tb->CreateThreadDrivingAllInputs("main", ZeroOrX::kX));
  SequentialBlock& seq = tbt->MainBlock();

  seq.AtEndOfCycle().ExpectX("out");
  seq.Set("x", 100);
  seq.NextCycle();
  seq.AtEndOfCycle().ExpectEq("out", 100);
  seq.Set("x", 101);
  seq.NextCycle();
  seq.AtEndOfCycle().ExpectEq("out", 101);
  seq.Set("x", 102);
  seq.NextCycle();
  seq.AtEndOfCycle().ExpectEq("out", 102);
  seq.Set("x", 0);

  XLS_ASSERT_OK(tb->Run());
}

TEST_P(BlockGeneratorTest, MultiplyInstantiatedBlock) {
  Package package(TestBaseName());
  Type* u32 = package.GetBitsType(32);

  XLS_ASSERT_OK_AND_ASSIGN(Block * sub_block,
                           MakeSubtractBlock("subtractor", &package));

  BlockBuilder bb("my_block", &package);
  XLS_ASSERT_OK_AND_ASSIGN(
      xls::Instantiation * subtractor0,
      bb.block()->AddBlockInstantiation("sub0", sub_block));
  XLS_ASSERT_OK_AND_ASSIGN(
      xls::Instantiation * subtractor1,
      bb.block()->AddBlockInstantiation("sub1", sub_block));
  XLS_ASSERT_OK_AND_ASSIGN(
      xls::Instantiation * subtractor2,
      bb.block()->AddBlockInstantiation("sub2", sub_block));
  BValue x = bb.InputPort("x", u32);
  BValue y = bb.InputPort("y", u32);

  bb.InstantiationInput(subtractor0, "a", x);
  bb.InstantiationInput(subtractor0, "b", y);
  BValue x_minus_y = bb.InstantiationOutput(subtractor0, "result");

  bb.InstantiationInput(subtractor1, "a", y);
  bb.InstantiationInput(subtractor1, "b", x);
  BValue y_minus_x = bb.InstantiationOutput(subtractor1, "result");

  bb.InstantiationInput(subtractor2, "a", x);
  bb.InstantiationInput(subtractor2, "b", x);
  BValue x_minus_x = bb.InstantiationOutput(subtractor2, "result");

  bb.OutputPort("x_minus_y", x_minus_y);
  bb.OutputPort("y_minus_x", y_minus_x);
  bb.OutputPort("x_minus_x", x_minus_x);

  XLS_ASSERT_OK_AND_ASSIGN(Block * block, bb.Build());

  XLS_ASSERT_OK_AND_ASSIGN(std::string verilog,
                           GenerateVerilog(block, codegen_options()));
  XLS_ASSERT_OK_AND_ASSIGN(ModuleSignature sig,
                           GenerateSignature(codegen_options(), block));

  ExpectVerilogEqualToGoldenFile(GoldenFilePath(kTestName, kTestdataPath),
                                 verilog);

  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<ModuleTestbench> tb,
                           NewModuleTestbench(verilog, sig));
  XLS_ASSERT_OK_AND_ASSIGN(
      ModuleTestbenchThread * tbt,
      tb->CreateThreadDrivingAllInputs("main", ZeroOrX::kX));
  SequentialBlock& seq = tbt->MainBlock();

  seq.AtEndOfCycle()
      .ExpectX("x_minus_y")
      .ExpectX("y_minus_x")
      .ExpectX("x_minus_x");

  // The module doesn't a connected clock, but the clock can still
  // be used to sequence events in time.
  seq.NextCycle();
  seq.Set("x", 0).Set("y", 0);
  seq.AtEndOfCycle()
      .ExpectEq("x_minus_y", 0)
      .ExpectEq("y_minus_x", 0)
      .ExpectEq("x_minus_x", 0);

  seq.NextCycle();
  seq.Set("x", 0xabcd).Set("y", 0x4242);
  seq.AtEndOfCycle()
      .ExpectEq("x_minus_y", 0x698b)
      .ExpectEq("y_minus_x", 0xffff9675)
      .ExpectEq("x_minus_x", 0);

  XLS_ASSERT_OK(tb->Run());
}

TEST_P(BlockGeneratorTest, DiamondDependencyInstantiations) {
  Package package(TestBaseName());
  Type* u32 = package.GetBitsType(32);

  XLS_ASSERT_OK_AND_ASSIGN(Block * sub_block,
                           MakeSubtractBlock("subtractor", &package));
  XLS_ASSERT_OK_AND_ASSIGN(
      Block * delegator0,
      MakeDelegatingBlock("delegator0", sub_block, &package));
  XLS_ASSERT_OK_AND_ASSIGN(
      Block * delegator1,
      MakeDelegatingBlock("delegator1", sub_block, &package));

  BlockBuilder bb("my_block", &package);
  XLS_ASSERT_OK_AND_ASSIGN(
      xls::Instantiation * instantiation0,
      bb.block()->AddBlockInstantiation("deleg0", delegator0));
  XLS_ASSERT_OK_AND_ASSIGN(
      xls::Instantiation * instantiation1,
      bb.block()->AddBlockInstantiation("deleg1", delegator1));

  BValue j = bb.InputPort("j", u32);
  BValue k = bb.InputPort("k", u32);

  bb.InstantiationInput(instantiation0, "x", j);
  bb.InstantiationInput(instantiation0, "y", k);
  BValue j_minus_k = bb.InstantiationOutput(instantiation0, "z");

  bb.InstantiationInput(instantiation1, "x", k);
  bb.InstantiationInput(instantiation1, "y", j);
  BValue k_minus_j = bb.InstantiationOutput(instantiation1, "z");

  bb.OutputPort("j_minus_k", j_minus_k);
  bb.OutputPort("k_minus_j", k_minus_j);

  XLS_ASSERT_OK_AND_ASSIGN(Block * block, bb.Build());

  XLS_ASSERT_OK_AND_ASSIGN(std::string verilog,
                           GenerateVerilog(block, codegen_options()));
  XLS_ASSERT_OK_AND_ASSIGN(ModuleSignature sig,
                           GenerateSignature(codegen_options(), block));

  ExpectVerilogEqualToGoldenFile(GoldenFilePath(kTestName, kTestdataPath),
                                 verilog);

  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<ModuleTestbench> tb,
                           NewModuleTestbench(verilog, sig));
  XLS_ASSERT_OK_AND_ASSIGN(
      ModuleTestbenchThread * tbt,
      tb->CreateThreadDrivingAllInputs("main", ZeroOrX::kX));
  SequentialBlock& seq = tbt->MainBlock();

  seq.AtEndOfCycle().ExpectX("j_minus_k").ExpectX("k_minus_j");

  // The module doesn't a connected clock, but the clock can still
  // be used to sequence events in time.
  seq.NextCycle();
  seq.Set("j", 0).Set("k", 0);
  seq.AtEndOfCycle().ExpectEq("j_minus_k", 0).ExpectEq("k_minus_j", 0);

  seq.NextCycle();
  seq.Set("j", 0xabcd).Set("k", 0x4242);
  seq.AtEndOfCycle()
      .ExpectEq("j_minus_k", 0x698b)
      .ExpectEq("k_minus_j", 0xffff9675);

  XLS_ASSERT_OK(tb->Run());
}

TEST_P(BlockGeneratorTest, LoopbackFifoInstantiation) {
  constexpr std::string_view ir_text = R"(package test

chan in(bits[32], id=0, kind=streaming, ops=receive_only, flow_control=ready_valid, metadata="")
chan out(bits[32], id=1, kind=streaming, ops=send_only, flow_control=ready_valid, metadata="")
chan loopback(bits[32], id=2, kind=streaming, ops=send_receive, flow_control=ready_valid, fifo_depth=1, bypass=false, register_push_outputs=true, metadata="")

proc running_sum(first_cycle: bits[1], init={1}) {
  tkn: token = literal(value=token, id=1000)
  in_recv: (token, bits[32]) = receive(tkn, channel=in)
  in_tkn: token = tuple_index(in_recv, index=0)
  in_data: bits[32] = tuple_index(in_recv, index=1)
  lit1: bits[32] = literal(value=1)
  not_first_cycle: bits[1] = not(first_cycle)
  loopback_recv: (token, bits[32]) = receive(tkn, predicate=not_first_cycle, channel=loopback)
  loopback_tkn: token = tuple_index(loopback_recv, index=0)
  loopback_data: bits[32] = tuple_index(loopback_recv, index=1)
  sum: bits[32] = add(loopback_data, in_data)
  all_recv_tkn: token = after_all(in_tkn, loopback_tkn)
  out_send: token = send(all_recv_tkn, sum, channel=out)
  loopback_send: token = send(out_send, sum, channel=loopback)
  lit0: bits[1] = literal(value=0)
  next_first_cycle: () = next_value(param=first_cycle, value=lit0)
}
)";

  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Package> package,
                           Parser::ParsePackage(ir_text));

  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, package->GetProc("running_sum"));

  XLS_ASSERT_OK_AND_ASSIGN(DelayEstimator * estimator,
                           GetDelayEstimator("unit"));
  XLS_ASSERT_OK_AND_ASSIGN(
      PipelineSchedule schedule,
      RunPipelineSchedule(
          proc, *estimator,
          SchedulingOptions().pipeline_stages(3).add_constraint(IOConstraint(
              "loopback", IODirection::kReceive, "loopback", IODirection::kSend,
              /*minimum_latency=*/1, /*maximum_latency=*/1))));

  CodegenOptions options = codegen_options();
  options.flop_inputs(false).flop_outputs(false).clock_name("clk");
  options.valid_control("input_valid", "output_valid");
  options.reset("rst", /*asynchronous=*/false, /*active_low=*/false,
                /*reset_data_path=*/true);
  options.streaming_channel_data_suffix("_data");
  options.streaming_channel_valid_suffix("_valid");
  options.streaming_channel_ready_suffix("_ready");
  options.module_name("running_sum");

  XLS_ASSERT_OK_AND_ASSIGN(CodegenPassUnit unit, FunctionBaseToPipelinedBlock(
                                                     schedule, options, proc));

  XLS_ASSERT_OK_AND_ASSIGN(std::string verilog,
                           GenerateVerilog(unit.top_block, options));
  XLS_ASSERT_OK_AND_ASSIGN(ModuleSignature sig,
                           GenerateSignature(options, unit.top_block));

  verilog = absl::StrCat("`include \"fifo.v\"\n\n", verilog);

  VerilogInclude fifo_definition{.relative_path = "fifo.v",
                                 .verilog_text = std::string{kFifoRTLText}};

  ExpectVerilogEqualToGoldenFile(GoldenFilePath(kTestName, kTestdataPath),
                                 verilog, /*macro_definitions=*/{},
                                 {fifo_definition});

  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<ModuleTestbench> tb,
                           NewModuleTestbench(verilog, sig, {fifo_definition}));
  XLS_ASSERT_OK_AND_ASSIGN(
      ModuleTestbenchThread * push_tbt,
      tb->CreateThread(
          "push",
          {DutInput{.port_name = "in_valid", .initial_value = UBits(0, 1)},
           DutInput{.port_name = "in_data", .initial_value = IsX()}}));
  XLS_ASSERT_OK_AND_ASSIGN(
      ModuleTestbenchThread * pop_tbt,
      tb->CreateThread("pop", {DutInput{.port_name = "out_ready",
                                        .initial_value = UBits(0, 1)}}));
  SequentialBlock& push_block = push_tbt->MainBlock();
  SequentialBlock& pop_block = pop_tbt->MainBlock();

  auto push = [&push_block](int64_t data) {
    push_block.Set("in_valid", 1).Set("in_data", data);
    push_block.WaitForCycleAfter("in_ready");
    push_block.Set("in_valid", 0);
    push_block.NextCycle();
  };
  auto pop = [&pop_block](int64_t expected) {
    pop_block.Set("out_ready", 1)
        .AtEndOfCycleWhen("out_valid")
        .ExpectEq("out_valid", 1)
        .ExpectEq("out_data", expected);
    pop_block.Set("out_ready", 0);
    pop_block.NextCycle();
  };

  for (int64_t i = 0; i < 25; ++i) {
    push(i);
    pop((i * (i + 1)) / 2);  // output is the next triangular number.
  }

  XLS_ASSERT_OK(tb->Run());
}

TEST_P(BlockGeneratorTest, RecvDataFeedingSendPredicate) {
  Package package(TestName());
  Type* u32 = package.GetBitsType(32);
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * in,
      package.CreateStreamingChannel("in", ChannelOps::kReceiveOnly, u32, {},
                                     std::nullopt, FlowControl::kReadyValid));
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * out0,
      package.CreateStreamingChannel("out0", ChannelOps::kSendOnly, u32, {},
                                     std::nullopt, FlowControl::kReadyValid));
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * out1,
      package.CreateStreamingChannel("out1", ChannelOps::kSendOnly, u32, {},
                                     std::nullopt, FlowControl::kReadyValid));

  TokenlessProcBuilder pb(TestName(), "tkn", &package);
  BValue recv = pb.Receive(in);

  BValue two_five = pb.Literal(UBits(25, 32));
  BValue one_five = pb.Literal(UBits(15, 32));

  BValue lt_two_five = pb.ULt(recv, two_five);
  BValue gt_one_five = pb.UGt(recv, one_five);

  pb.SendIf(out0, lt_two_five, recv);
  pb.SendIf(out1, gt_one_five, recv);

  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, pb.Build({}));

  XLS_ASSERT_OK_AND_ASSIGN(DelayEstimator * estimator,
                           GetDelayEstimator("unit"));

  XLS_ASSERT_OK_AND_ASSIGN(
      PipelineSchedule schedule,
      RunPipelineSchedule(proc, *estimator,
                          SchedulingOptions().pipeline_stages(1)));
  CodegenOptions options;
  options.flop_inputs(false).flop_outputs(true).clock_name("clk");
  options.reset("rst", /*asynchronous=*/false, /*active_low=*/false,
                /*reset_data_path=*/true);
  options.streaming_channel_data_suffix("_data");
  options.streaming_channel_valid_suffix("_valid");
  options.streaming_channel_ready_suffix("_ready");
  options.module_name("pipelined_proc");
  options.use_system_verilog(UseSystemVerilog());

  XLS_ASSERT_OK_AND_ASSIGN(CodegenPassUnit unit, FunctionBaseToPipelinedBlock(
                                                     schedule, options, proc));

  XLS_ASSERT_OK_AND_ASSIGN(std::string verilog,
                           GenerateVerilog(unit.top_block, options));

  VLOG(3) << "Verilog:";
  XLS_VLOG_LINES(3, verilog);

  XLS_ASSERT_OK_AND_ASSIGN(ModuleSignature sig,
                           GenerateSignature(options, unit.top_block));

  ModuleSimulator simulator = NewModuleSimulator(verilog, sig);

  // Setup input
  absl::flat_hash_map<std::string, std::vector<Bits>> input_values;
  input_values["in"] = {UBits(0, 32), UBits(20, 32), UBits(30, 32)};

  std::vector<ValidHoldoff> valid_holdoffs = {
      ValidHoldoff{.cycles = 2, .driven_values = {IsX(), IsX()}},
      ValidHoldoff{.cycles = 2, .driven_values = {IsX(), IsX()}},
      ValidHoldoff{.cycles = 2, .driven_values = {IsX(), IsX()}},
  };

  auto ready_valid_holdoffs =
      ReadyValidHoldoffs{.valid_holdoffs = {{"in", valid_holdoffs}}};

  // Expected output values
  absl::flat_hash_map<std::string, int64_t> output_channel_counts;
  output_channel_counts["out0"] = 2;
  output_channel_counts["out1"] = 2;

  absl::flat_hash_map<std::string, std::vector<Bits>> expected_output_values;
  expected_output_values["out0"] = {UBits(0, 32), UBits(20, 32)};
  expected_output_values["out1"] = {UBits(20, 32), UBits(30, 32)};

  EXPECT_THAT(simulator.RunInputSeriesProc(input_values, output_channel_counts,
                                           ready_valid_holdoffs),
              status_testing::IsOkAndHolds(expected_output_values));
}

TEST_P(BlockGeneratorTest, DynamicStateFeedbackWithNonUpdateCase) {
  const std::string ir_text = R"(package test
chan out(bits[32], id=1, kind=streaming, ops=send_only, flow_control=ready_valid, metadata="")

proc slow_counter(counter: bits[32], odd_iteration: bits[1], init={0, 0}) {
  tkn: token = literal(value=token, id=1000)
  lit1: bits[32] = literal(value=1)
  incremented_counter: bits[32] = add(counter, lit1)
  even_iteration: bits[1] = not(odd_iteration)
  send.1: token = send(tkn, counter, channel=out, id=1)
  next_counter_odd: () = next_value(param=counter, value=counter, predicate=odd_iteration)
  next_counter_even: () = next_value(param=counter, value=incremented_counter, predicate=even_iteration)
  next_value.2: () = next_value(param=odd_iteration, value=even_iteration, id=2)
}
)";

  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Package> package,
                           Parser::ParsePackage(ir_text));

  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, package->GetProc("slow_counter"));

  XLS_ASSERT_OK_AND_ASSIGN(DelayEstimator * estimator,
                           GetDelayEstimator("unit"));

  XLS_ASSERT_OK_AND_ASSIGN(
      PipelineSchedule schedule,
      RunPipelineSchedule(proc, *estimator,
                          SchedulingOptions()
                              .pipeline_stages(2)
                              .worst_case_throughput(2)
                              .add_constraint(NodeInCycleConstraint(
                                  *proc->GetNode("next_counter_odd"), 0))
                              .add_constraint(NodeInCycleConstraint(
                                  *proc->GetNode("next_counter_even"), 1))));

  CodegenOptions options;
  options.flop_inputs(false).flop_outputs(true).clock_name("clk");
  options.reset("rst", /*asynchronous=*/false, /*active_low=*/false,
                /*reset_data_path=*/true);
  options.streaming_channel_data_suffix("_data");
  options.streaming_channel_valid_suffix("_valid");
  options.streaming_channel_ready_suffix("_ready");
  options.module_name("pipelined_proc");
  options.use_system_verilog(UseSystemVerilog());

  XLS_ASSERT_OK_AND_ASSIGN(CodegenPassUnit unit, FunctionBaseToPipelinedBlock(
                                                     schedule, options, proc));

  XLS_ASSERT_OK_AND_ASSIGN(std::string verilog,
                           GenerateVerilog(unit.top_block, options));

  XLS_ASSERT_OK_AND_ASSIGN(ModuleSignature sig,
                           GenerateSignature(options, unit.top_block));

  ExpectVerilogEqualToGoldenFile(GoldenFilePath(kTestName, kTestdataPath),
                                 verilog);
}

TEST_P(BlockGeneratorTest, DynamicStateFeedbackWithOnlyUpdateCases) {
  const std::string ir_text = R"(package test
chan out(bits[32], id=1, kind=streaming, ops=send_only, flow_control=ready_valid, metadata="")

proc bad_alternator(counter: bits[32], odd_iteration: bits[1], init={0, 0}) {
  tkn: token = literal(value=token, id=1000)
  lit1: bits[32] = literal(value=1)
  incremented_counter: bits[32] = add(counter, lit1)
  even_iteration: bits[1] = not(odd_iteration)
  send.1: token = send(tkn, counter, channel=out, id=1)
  next_counter_odd: () = next_value(param=counter, value=lit1, predicate=odd_iteration)
  next_counter_even: () = next_value(param=counter, value=incremented_counter, predicate=even_iteration)
  next_value.2: () = next_value(param=odd_iteration, value=even_iteration, id=2)
}
)";

  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Package> package,
                           Parser::ParsePackage(ir_text));

  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, package->GetProc("bad_alternator"));

  XLS_ASSERT_OK_AND_ASSIGN(DelayEstimator * estimator,
                           GetDelayEstimator("unit"));

  XLS_ASSERT_OK_AND_ASSIGN(
      PipelineSchedule schedule,
      RunPipelineSchedule(proc, *estimator,
                          SchedulingOptions()
                              .pipeline_stages(2)
                              .worst_case_throughput(2)
                              .add_constraint(NodeInCycleConstraint(
                                  *proc->GetNode("next_counter_odd"), 0))
                              .add_constraint(NodeInCycleConstraint(
                                  *proc->GetNode("next_counter_even"), 1))));

  CodegenOptions options;
  options.flop_inputs(false).flop_outputs(true).clock_name("clk");
  options.reset("rst", /*asynchronous=*/false, /*active_low=*/false,
                /*reset_data_path=*/true);
  options.streaming_channel_data_suffix("_data");
  options.streaming_channel_valid_suffix("_valid");
  options.streaming_channel_ready_suffix("_ready");
  options.module_name("pipelined_proc");
  options.use_system_verilog(UseSystemVerilog());

  XLS_ASSERT_OK_AND_ASSIGN(CodegenPassUnit unit, FunctionBaseToPipelinedBlock(
                                                     schedule, options, proc));

  XLS_ASSERT_OK_AND_ASSIGN(std::string verilog,
                           GenerateVerilog(unit.top_block, options));

  XLS_ASSERT_OK_AND_ASSIGN(ModuleSignature sig,
                           GenerateSignature(options, unit.top_block));

  ExpectVerilogEqualToGoldenFile(GoldenFilePath(kTestName, kTestdataPath),
                                 verilog);
}

TEST_P(BlockGeneratorTest, TruncatedArrayIndices) {
  const std::string ir_text = R"(package test
chan out(bits[7], id=10, kind=streaming, ops=send_only, flow_control=ready_valid, strictness=proven_mutually_exclusive, metadata="""""")

proc lookup_proc(x: bits[1], z: bits[1], init={0, 0}) {
  tkn: token = literal(value=token, id=1000)
  literal.1: bits[33] = literal(value=1, id=1)
  literal.2: bits[33] = literal(value=2, id=2)
  sel.3: bits[33] = sel(x, cases=[literal.1], default=literal.2, id=3)
  literal.4: bits[4] = literal(value=4, id=4)
  literal.5: bits[4] = literal(value=5, id=5)
  sel.6: bits[4] = sel(z, cases=[literal.4], default=literal.5, id=6)
  lookup_table: bits[7][4][1] = literal(value=[[0, 0, 0, 0]], id=7)
  entry: bits[7] = array_index(lookup_table, indices=[sel.3, sel.6], id=8)
  send.9: token = send(tkn, entry, channel=out, id=9)
  next_value.10: () = next_value(param=x, value=x, id=10)
  next_value.11: () = next_value(param=z, value=z, id=11)
}
)";

  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Package> package,
                           Parser::ParsePackage(ir_text));

  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, package->GetProc("lookup_proc"));

  XLS_ASSERT_OK_AND_ASSIGN(DelayEstimator * estimator,
                           GetDelayEstimator("unit"));

  XLS_ASSERT_OK_AND_ASSIGN(
      PipelineSchedule schedule,
      RunPipelineSchedule(proc, *estimator,
                          SchedulingOptions().pipeline_stages(1)));

  CodegenOptions options;
  options.flop_inputs(false).flop_outputs(true).clock_name("clk");
  options.reset("rst", /*asynchronous=*/false, /*active_low=*/false,
                /*reset_data_path=*/true);
  options.streaming_channel_data_suffix("_data");
  options.streaming_channel_valid_suffix("_valid");
  options.streaming_channel_ready_suffix("_ready");
  options.module_name("pipelined_proc");
  options.use_system_verilog(UseSystemVerilog());

  XLS_ASSERT_OK_AND_ASSIGN(CodegenPassUnit unit, FunctionBaseToPipelinedBlock(
                                                     schedule, options, proc));

  XLS_ASSERT_OK_AND_ASSIGN(std::string verilog,
                           GenerateVerilog(unit.top_block, options));

  XLS_ASSERT_OK_AND_ASSIGN(ModuleSignature sig,
                           GenerateSignature(options, unit.top_block));

  ExpectVerilogEqualToGoldenFile(GoldenFilePath(kTestName, kTestdataPath),
                                 verilog);
}

INSTANTIATE_TEST_SUITE_P(BlockGeneratorTestInstantiation, BlockGeneratorTest,
                         testing::ValuesIn(kDefaultSimulationTargets),
                         ParameterizedTestName<BlockGeneratorTest>);

std::string_view ParameterizedFloppingName(
    std::tuple<bool, CodegenOptions::IOKind> param) {
  if (std::get<0>(param)) {
    return CodegenOptions::IOKindToString(std::get<1>(param));
  }
  return "NoFlop";
}

template <typename TestT>
std::string ParameterizedTestNameWithFlopping(
    const testing::TestParamInfo<typename TestT::ParamType>& info) {
  // Underscores and dashes not allowed in test names. Strip them out and
  // replace string with camel case. For example, "fancy-sim" becomes
  // "FancySim".
  std::vector<std::string> parts =
      absl::StrSplit(std::get<0>(info.param).simulator, absl::ByAnyChar("-_"));
  for (std::string& part : parts) {
    part[0] = absl::ascii_toupper(part[0]);
  }
  parts.push_back(std::get<0>(info.param).use_system_verilog ? "SystemVerilog"
                                                             : "Verilog");
  parts.push_back(absl::StrCat("Input", ParameterizedFloppingName(std::get<0>(
                                            std::get<1>(info.param)))));
  parts.push_back(absl::StrCat("Output", ParameterizedFloppingName(std::get<1>(
                                             std::get<1>(info.param)))));

  return absl::StrJoin(parts, "");
}

class ZeroWidthBlockGeneratorTest
    : public VerilogTestBaseWithParam<std::tuple<
          SimulationTarget, std::tuple<
                                // (flop_inputs, flop_inputs_kind)
                                std::tuple<bool, CodegenOptions::IOKind>,
                                // (flop_outputs, flop_outputs_kind)
                                std::tuple<bool, CodegenOptions::IOKind>>>> {
 public:
  SimulationTarget GetSimulationTarget() const final {
    return std::get<0>(GetParam());
  }
  CodegenOptions codegen_options() const {
    CodegenOptions options;
    options.clock_name("clk");
    options.reset("rst", /*asynchronous=*/false, /*active_low=*/false,
                  /*reset_data_path=*/true);
    options.streaming_channel_data_suffix("_data");
    options.streaming_channel_valid_suffix("_valid");
    options.streaming_channel_ready_suffix("_ready");
    options.module_name("pipelined_proc");
    options.use_system_verilog(UseSystemVerilog());
    options.flop_inputs(std::get<0>(std::get<0>(std::get<1>(GetParam()))));
    options.flop_inputs_kind(std::get<1>(std::get<0>(std::get<1>(GetParam()))));
    options.flop_outputs(std::get<0>(std::get<1>(std::get<1>(GetParam()))));
    options.flop_outputs_kind(
        std::get<1>(std::get<1>(std::get<1>(GetParam()))));
    return options;
  }
  std::filesystem::path GoldenFilePath(
      std::string_view test_file_name,
      const std::filesystem::path& testdata_dir) override {
    // We suffix the golden reference files with "txt" on top of the extension
    // just to indicate they're compiler byproduct comparison points and not
    // Verilog files that have been written by hand.
    std::string filename = absl::StrCat(
        test_file_name, "_", TestBaseName(), "Input",
        ParameterizedFloppingName(std::get<0>(std::get<1>(GetParam()))),
        "Output",
        ParameterizedFloppingName(std::get<1>(std::get<1>(GetParam()))), ".",
        UseSystemVerilog() ? "svtxt" : "vtxt");
    return testdata_dir / filename;
  }
};

TEST_P(ZeroWidthBlockGeneratorTest, ZeroWidthRecvChannel) {
  Package package(TestName());
  Type* u0 = package.GetBitsType(0);
  Type* u32 = package.GetBitsType(32);
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * in,
      package.CreateStreamingChannel("in", ChannelOps::kReceiveOnly, u0, {},
                                     std::nullopt, FlowControl::kReadyValid));
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * out,
      package.CreateStreamingChannel("out", ChannelOps::kSendOnly, u32, {},
                                     std::nullopt, FlowControl::kReadyValid));
  TokenlessProcBuilder pb(TestName(), "tkn", &package);
  pb.Receive(in);

  BValue two_five = pb.Literal(UBits(25, 32));

  pb.Send(out, two_five);

  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, pb.Build({}));

  XLS_ASSERT_OK_AND_ASSIGN(DelayEstimator * estimator,
                           GetDelayEstimator("unit"));

  XLS_ASSERT_OK_AND_ASSIGN(
      PipelineSchedule schedule,
      RunPipelineSchedule(proc, *estimator,
                          SchedulingOptions().pipeline_stages(1)));
  CodegenOptions options = codegen_options();

  XLS_ASSERT_OK_AND_ASSIGN(CodegenPassUnit unit, FunctionBaseToPipelinedBlock(
                                                     schedule, options, proc));
  std::unique_ptr<CodegenPass> passes = CreateCodegenPassPipeline();
  PassResults results;
  CodegenPassOptions codegen_pass_options{.codegen_options = options,
                                          .schedule = schedule,
                                          .delay_estimator = estimator};
  XLS_ASSERT_OK(passes->Run(&unit, codegen_pass_options, &results));

  XLS_ASSERT_OK_AND_ASSIGN(std::string verilog,
                           GenerateVerilog(unit.top_block, options));

  ExpectVerilogEqualToGoldenFile(GoldenFilePath(kTestName, kTestdataPath),
                                 verilog);
}

TEST_P(ZeroWidthBlockGeneratorTest, ZeroWidthSendChannel) {
  Package package(TestName());
  Type* u0 = package.GetBitsType(0);
  Type* u32 = package.GetBitsType(32);
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * in,
      package.CreateStreamingChannel("in", ChannelOps::kReceiveOnly, u32, {},
                                     std::nullopt, FlowControl::kReadyValid));
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * out,
      package.CreateStreamingChannel("out", ChannelOps::kSendOnly, u0, {},
                                     std::nullopt, FlowControl::kReadyValid));
  TokenlessProcBuilder pb(TestName(), "tkn", &package);
  pb.Receive(in);
  pb.Send(out, pb.Literal(UBits(0, 0)));

  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, pb.Build({}));

  XLS_ASSERT_OK_AND_ASSIGN(DelayEstimator * estimator,
                           GetDelayEstimator("unit"));

  XLS_ASSERT_OK_AND_ASSIGN(
      PipelineSchedule schedule,
      RunPipelineSchedule(proc, *estimator,
                          SchedulingOptions().pipeline_stages(1)));
  CodegenOptions options = codegen_options();
  XLS_ASSERT_OK_AND_ASSIGN(CodegenPassUnit unit, FunctionBaseToPipelinedBlock(
                                                     schedule, options, proc));

  std::unique_ptr<CodegenPass> passes = CreateCodegenPassPipeline();
  PassResults results;
  CodegenPassOptions codegen_pass_options{.codegen_options = options,
                                          .schedule = schedule,
                                          .delay_estimator = estimator};
  XLS_ASSERT_OK(passes->Run(&unit, codegen_pass_options, &results));

  XLS_ASSERT_OK_AND_ASSIGN(std::string verilog,
                           GenerateVerilog(unit.top_block, options));

  ExpectVerilogEqualToGoldenFile(GoldenFilePath(kTestName, kTestdataPath),
                                 verilog);
}

constexpr std::initializer_list<std::tuple<bool, CodegenOptions::IOKind>>
    kFloppingParams = {{false, CodegenOptions::IOKind::kFlop},
                       {true, CodegenOptions::IOKind::kFlop},
                       {true, CodegenOptions::IOKind::kSkidBuffer},
                       {true, CodegenOptions::IOKind::kZeroLatencyBuffer}};

INSTANTIATE_TEST_SUITE_P(
    ZeroWidthBlockGeneratorTestInstantiation, ZeroWidthBlockGeneratorTest,
    testing::Combine(testing::ValuesIn(kDefaultSimulationTargets),
                     testing::Combine(
                         // input flopping
                         testing::ValuesIn(kFloppingParams),
                         // output flopping
                         testing::ValuesIn(kFloppingParams))),
    ParameterizedTestNameWithFlopping<ZeroWidthBlockGeneratorTest>);

}  // namespace
}  // namespace verilog
}  // namespace xls
