// Copyright 2024 The XLS Authors
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


#include <cstdint>
#include <string>
#include <string_view>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "xls/common/status/matchers.h"
#include "xls/common/status/ret_check.h"
#include "xls/common/status/status_macros.h"
#include "xls/ir/bits.h"
#include "xls/ir/bits_ops.h"
#include "xls/ir/function.h"
#include "xls/ir/function_base.h"
#include "xls/ir/function_builder.h"
#include "xls/ir/ir_matcher.h"
#include "xls/ir/ir_test_base.h"
#include "xls/ir/node.h"
#include "xls/ir/nodes.h"
#include "xls/ir/value.h"
#include "xls/passes/dce_pass.h"
#include "xls/passes/optimization_pass.h"
#include "xls/passes/pass_base.h"
#include "re2/re2.h"

namespace m = ::xls::op_matchers;
namespace xls {
namespace {

using status_testing::IsOkAndHolds;
class PassBaseTest : public IrTestBase {};

class CountPass final : public OptimizationFunctionBasePass {
 public:
  CountPass(std::string_view name, int32_t* counter, int32_t stabilize_req)
      : OptimizationFunctionBasePass(absl::StrCat("count_", name),
                                     absl::StrCat("count_", name)),
        pass_remain_(stabilize_req),
        global_counter_(counter) {}

 protected:
  absl::StatusOr<bool> RunOnFunctionBaseInternal(
      FunctionBase* f, const OptimizationPassOptions& options,
      PassResults* results) const override {
    if (pass_remain_ == 0) {
      return false;
    }
    --pass_remain_;
    ++(*global_counter_);
    // Just increment return literal by 1.
    XLS_RET_CHECK(f->IsFunction());
    XLS_RET_CHECK(f->AsFunctionOrDie()->return_value()->Is<Literal>());
    XLS_RET_CHECK(f->AsFunctionOrDie()->return_value()->GetType()->IsBits());
    Node* n = f->AsFunctionOrDie()->return_value();
    XLS_RETURN_IF_ERROR(
        n->ReplaceUsesWithNew<Literal>(
             Value(bits_ops::Increment(n->As<Literal>()->value().bits())))
            .status());
    return true;
  }

 private:
  mutable int32_t pass_remain_;
  int32_t* global_counter_;
};

class TestPipelineGenerator : public OptimizationPipelineGenerator {
 public:
  TestPipelineGenerator()
      : OptimizationPipelineGenerator("test_pipe", "test_pipe") {}
  int32_t a_count() const { return a_count_; }
  int32_t b_count() const { return b_count_; }

 protected:
  // Pass is count_pass_<a|b>(cnt_to_stable)
  absl::Status AddPassToPipeline(OptimizationCompoundPass* pass,
                                 std::string_view pass_name) const override {
    std::string idx;
    if (pass_name == "dce") {
      pass->Add<DeadCodeEliminationPass>();
      return absl::OkStatus();
    }
    XLS_RET_CHECK(
        RE2::FullMatch(pass_name, "count_pass_[ab]\\(([0-9]+)\\)", &idx))
        << "bad pass " << pass_name;
    int64_t cnt = std::stoi(idx);
    if (absl::StartsWith(pass_name, "count_pass_a")) {
      pass->Add<CountPass>("a", &a_count_, cnt);
    } else if (absl::StartsWith(pass_name, "count_pass_b")) {
      pass->Add<CountPass>("b", &b_count_, cnt);
    } else {
      return absl::InvalidArgumentError(
          absl::StrCat("Bad pass name ", pass_name));
    }
    pass->Add<DeadCodeEliminationPass>();
    return absl::OkStatus();
  }

 private:
  mutable int32_t a_count_ = 0;
  mutable int32_t b_count_ = 0;
};

TEST_F(PassBaseTest, PipelineGeneratorSingle) {
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  fb.Literal(UBits(0, 64));
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());
  TestPipelineGenerator gen;
  XLS_ASSERT_OK_AND_ASSIGN(
      auto pipeline,
      gen.GeneratePipeline(
          "count_pass_a(1) dce count_pass_b(1) dce count_pass_a(1) dce"));
  PassResults res;
  ASSERT_THAT(pipeline->Run(p.get(), OptimizationPassOptions{}, &res),
              IsOkAndHolds(true));

  EXPECT_THAT(f->return_value(), m::Literal(3));
  EXPECT_EQ(gen.a_count(), 2);
  EXPECT_EQ(gen.b_count(), 1);
}

TEST_F(PassBaseTest, PipelineGeneratorFixedPoint) {
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  fb.Literal(UBits(0, 64));
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());
  TestPipelineGenerator gen;
  XLS_ASSERT_OK_AND_ASSIGN(
      auto pipeline,
      gen.GeneratePipeline("[dce count_pass_a(4)] [dce count_pass_b(3)] dce"));
  PassResults res;
  ASSERT_THAT(pipeline->Run(p.get(), OptimizationPassOptions{}, &res),
              IsOkAndHolds(true));

  EXPECT_THAT(f->return_value(), m::Literal(7));
  EXPECT_EQ(gen.a_count(), 4);
  EXPECT_EQ(gen.b_count(), 3);
}

TEST_F(PassBaseTest, PipelineGeneratorMissingPass) {
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  fb.Literal(UBits(0, 64));
  XLS_ASSERT_OK(fb.Build().status());
  TestPipelineGenerator gen;
  EXPECT_THAT(gen.GeneratePipeline("foobar not_present").status(),
              status_testing::StatusIs(
                  absl::StatusCode::kInternal,
                  testing::MatchesRegex(
                      ".*Unable to add pass 'foobar' to pipeline.*")));
}

TEST_F(PassBaseTest, PipelineGeneratorUnmatchedFixedpointOpen) {
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  fb.Literal(UBits(0, 64));
  XLS_ASSERT_OK(fb.Build().status());
  TestPipelineGenerator gen;
  EXPECT_THAT(gen.GeneratePipeline("[ dce dce dce [ dce ] ").status(),
              status_testing::StatusIs(
                  absl::StatusCode::kInternal,
                  testing::MatchesRegex(".*Unmatched '\\[' in pipeline.*")));
}

TEST_F(PassBaseTest, PipelineGeneratorUnmatchedFixedpointClose) {
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  fb.Literal(UBits(0, 64));
  XLS_ASSERT_OK(fb.Build().status());
  TestPipelineGenerator gen;
  EXPECT_THAT(gen.GeneratePipeline("dce dce dce [ dce ] ]").status(),
              status_testing::StatusIs(
                  absl::StatusCode::kInternal,
                  testing::MatchesRegex(".*Unmatched '\\]' in pipeline.*")));
}

}  // namespace
}  // namespace xls
