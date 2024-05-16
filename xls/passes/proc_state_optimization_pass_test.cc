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

#include "xls/passes/proc_state_optimization_pass.h"

#include <cstdint>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/optimization.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "xls/common/status/matchers.h"
#include "xls/ir/bits.h"
#include "xls/ir/channel.h"
#include "xls/ir/channel_ops.h"
#include "xls/ir/function_builder.h"
#include "xls/ir/ir_matcher.h"
#include "xls/ir/ir_test_base.h"
#include "xls/ir/package.h"
#include "xls/ir/value.h"
#include "xls/passes/optimization_pass.h"
#include "xls/passes/pass_base.h"

namespace m = ::xls::op_matchers;

namespace xls {
namespace {

using status_testing::IsOkAndHolds;
using ::testing::ElementsAre;
using ::testing::UnorderedElementsAre;

enum class NextValueType : std::uint8_t {
  kNextStateElements,
  kNextValueNodes,
};

template <typename Sink>
void AbslStringify(Sink& sink, NextValueType e) {
  absl::Format(&sink, "%s",
               e == NextValueType::kNextStateElements ? "NextStateElements"
                                                      : "NextValueNodes");
}

class ProcStateOptimizationPassTest
    : public IrTestBase,
      public testing::WithParamInterface<NextValueType> {
 protected:
  ProcStateOptimizationPassTest() = default;

  absl::StatusOr<Proc*> BuildProc(ProcBuilder& pb,
                                  absl::Span<const BValue> next_state) {
    switch (GetParam()) {
      case NextValueType::kNextStateElements:
        return pb.Build(next_state);
      case NextValueType::kNextValueNodes: {
        for (int64_t index = 0; index < next_state.size(); ++index) {
          BValue param = pb.GetStateParam(index);
          BValue next_value = next_state[index];
          pb.Next(param, next_value);
        }
        return pb.Build();
      }
    }
    ABSL_UNREACHABLE();
  }
  absl::StatusOr<Proc*> BuildProc(TokenlessProcBuilder& pb,
                                  absl::Span<const BValue> next_state) {
    switch (GetParam()) {
      case NextValueType::kNextStateElements:
        return pb.Build(next_state);
      case NextValueType::kNextValueNodes: {
        for (int64_t index = 0; index < next_state.size(); ++index) {
          BValue param = pb.GetStateParam(index);
          BValue next_value = next_state[index];
          pb.Next(param, next_value);
        }
        return pb.Build();
      }
    }
    ABSL_UNREACHABLE();
  }

  absl::StatusOr<bool> Run(Package* p) {
    PassResults results;
    return ProcStateOptimizationPass().Run(p, OptimizationPassOptions(),
                                           &results);
  }
};

TEST_P(ProcStateOptimizationPassTest, StatelessProc) {
  auto p = CreatePackage();
  ProcBuilder pb("p", p.get());
  XLS_ASSERT_OK(BuildProc(pb, {}).status());

  EXPECT_THAT(Run(p.get()), IsOkAndHolds(false));
}

TEST_P(ProcStateOptimizationPassTest, SimpleNonoptimizableStateProc) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * out, p->CreateStreamingChannel("out", ChannelOps::kSendOnly,
                                               p->GetBitsType(32)));

  TokenlessProcBuilder pb("p", "tkn", p.get());
  BValue x = pb.StateElement("x", Value(UBits(0, 32)));
  BValue y = pb.StateElement("y", Value(UBits(0, 32)));
  pb.Send(out, pb.Add(x, y));

  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, BuildProc(pb, {pb.Not(x), pb.Not(y)}));

  EXPECT_EQ(proc->GetStateElementCount(), 2);
  EXPECT_THAT(Run(p.get()), IsOkAndHolds(false));
  EXPECT_EQ(proc->GetStateElementCount(), 2);
}

TEST_P(ProcStateOptimizationPassTest, ProcWithDeadElements) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * out, p->CreateStreamingChannel("out", ChannelOps::kSendOnly,
                                               p->GetBitsType(32)));

  TokenlessProcBuilder pb("p", "tkn", p.get());
  BValue x = pb.StateElement("x", Value(UBits(0, 32)));
  BValue y = pb.StateElement("y", Value(UBits(0, 32)));
  BValue z = pb.StateElement("z", Value(UBits(0, 32)));
  pb.Send(out, x);

  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc,
                           BuildProc(pb, {pb.Not(x), y, pb.Not(z)}));

  EXPECT_EQ(proc->GetStateElementCount(), 3);
  EXPECT_THAT(Run(p.get()), IsOkAndHolds(true));
  EXPECT_EQ(proc->GetStateElementCount(), 1);

  EXPECT_EQ(proc->GetStateParam(0)->GetName(), "x");
}

TEST_P(ProcStateOptimizationPassTest, CrissCrossDeadElements) {
  auto p = CreatePackage();
  TokenlessProcBuilder pb("p", "tkn", p.get());
  BValue x = pb.StateElement("x", Value(UBits(0, 32)));
  BValue y = pb.StateElement("y", Value(UBits(0, 32)));

  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, BuildProc(pb, {y, x}));

  EXPECT_EQ(proc->GetStateElementCount(), 2);
  EXPECT_THAT(Run(p.get()), IsOkAndHolds(true));
  EXPECT_EQ(proc->GetStateElementCount(), 0);
}

TEST_P(ProcStateOptimizationPassTest, CrissCrossDeadAndLiveElements) {
  auto p = CreatePackage();

  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * out, p->CreateStreamingChannel("out", ChannelOps::kSendOnly,
                                               p->GetBitsType(32)));

  TokenlessProcBuilder pb("p", "tkn", p.get());
  BValue a = pb.StateElement("a", Value(UBits(0, 32)));
  BValue b = pb.StateElement("b", Value(UBits(0, 32)));
  BValue c = pb.StateElement("c", Value(UBits(0, 32)));
  BValue x = pb.StateElement("x", Value(UBits(0, 32)));
  BValue y = pb.StateElement("y", Value(UBits(0, 32)));

  pb.Send(out, c);

  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, BuildProc(pb, {b, c, a, y, x}));

  EXPECT_EQ(proc->GetStateElementCount(), 5);
  EXPECT_THAT(Run(p.get()), IsOkAndHolds(true));
  EXPECT_THAT(proc->StateParams(),
              ElementsAre(m::Param("a"), m::Param("b"), m::Param("c")));
}

TEST_P(ProcStateOptimizationPassTest, ProcWithZeroWidthElement) {
  auto p = CreatePackage();
  TokenlessProcBuilder pb(NewStyleProc(), "p", "tkn", p.get());
  BValue x = pb.StateElement("x", Value(UBits(0, 0)));
  XLS_ASSERT_OK_AND_ASSIGN(SendChannelReference * out,
                           pb.AddOutputChannel("out", p->GetBitsType(32)));
  BValue y = pb.StateElement("y", Value(UBits(0, 32)));
  BValue send = pb.Send(out, pb.Concat({x, y}));

  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, BuildProc(pb, {pb.Not(x), pb.Not(y)}));

  EXPECT_EQ(proc->GetStateElementCount(), 2);
  EXPECT_THAT(Run(p.get()), IsOkAndHolds(true));
  EXPECT_EQ(proc->GetStateElementCount(), 1);

  EXPECT_EQ(proc->GetStateParam(0)->GetName(), "y");
  EXPECT_THAT(send.node(),
              m::Send(m::Literal(Value::Token()),
                      m::Concat(m::Literal(UBits(0, 0)), m::Param("y"))));
}

TEST_P(ProcStateOptimizationPassTest, StateElementsIntoTuplesAndOut) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * out, p->CreateStreamingChannel("out", ChannelOps::kSendOnly,
                                               p->GetBitsType(32)));

  TokenlessProcBuilder pb("p", "tkn", p.get());
  BValue x = pb.StateElement("x", Value(UBits(0, 32)));
  BValue y = pb.StateElement("y", Value(UBits(0, 32)));
  BValue z = pb.StateElement("z", Value(UBits(0, 32)));

  BValue xy = pb.Tuple({x, y});
  BValue xy_z = pb.Tuple({xy, z});

  // Send element `y` from the tuple.
  pb.Send(out, pb.TupleIndex(xy, 1));

  BValue next_x = y;
  BValue next_y = pb.TupleIndex(pb.TupleIndex(xy_z, 0), 0);
  BValue next_z = pb.TupleIndex(xy_z, 1);

  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc,
                           BuildProc(pb, {next_x, next_y, next_z}));

  EXPECT_EQ(proc->GetStateElementCount(), 3);
  EXPECT_THAT(Run(p.get()), IsOkAndHolds(true));
  EXPECT_EQ(proc->GetStateElementCount(), 2);
  EXPECT_THAT(proc->StateParams(), ElementsAre(x.node(), y.node()));
}

TEST_P(ProcStateOptimizationPassTest, ProcWithPartiallyDeadStateElement) {
  auto p = CreatePackage();
  Type* u32 = p->GetBitsType(32);
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * out,
      p->CreateStreamingChannel("out", ChannelOps::kSendOnly, u32));

  TokenlessProcBuilder pb("p", "tkn", p.get());
  Value zero(UBits(0, 32));
  BValue dead_state = pb.StateElement("dead", Value::Tuple({zero, zero}));
  BValue not_dead_state =
      pb.StateElement("not_dead", Value::Tuple({zero, zero}));
  // Send only one tuple element of the `not_dead` state.
  pb.Send(out, pb.TupleIndex(not_dead_state, 0));
  // Modify the active part of the `not_dead` state so it can't be eliminated.
  BValue next_not_dead_state =
      pb.Tuple({pb.Not(pb.TupleIndex(not_dead_state, 0)),
                pb.TupleIndex(not_dead_state, 1)});

  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc,
                           BuildProc(pb, {dead_state, next_not_dead_state}));

  EXPECT_THAT(proc->StateParams(),
              UnorderedElementsAre(m::Param("dead"), m::Param("not_dead")));
  EXPECT_THAT(Run(p.get()), IsOkAndHolds(true));
  EXPECT_THAT(proc->StateParams(), UnorderedElementsAre(m::Param("not_dead")));
}

TEST_P(ProcStateOptimizationPassTest, ProcWithConstantStateElement) {
  auto p = CreatePackage();
  Type* u32 = p->GetBitsType(32);
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * out,
      p->CreateStreamingChannel("out", ChannelOps::kSendOnly, u32));

  TokenlessProcBuilder pb("p", "tkn", p.get());
  Value zero(UBits(0, 32));
  Value one(UBits(1, 32));
  BValue constant_state =
      pb.StateElement("constant", Value::Tuple({one, zero}));
  BValue not_constant_state =
      pb.StateElement("not_constant", Value::Tuple({zero, zero}));
  // Use one tuple element of both states.
  BValue state_usage = pb.Add(pb.TupleIndex(constant_state, 0),
                              pb.TupleIndex(not_constant_state, 0));
  pb.Send(out, state_usage);
  // Modify the active part of the `not_constant` state so it can't be
  // eliminated.
  BValue next_not_constant_state =
      pb.Tuple({pb.Not(pb.TupleIndex(not_constant_state, 0)),
                pb.TupleIndex(not_constant_state, 1)});

  XLS_ASSERT_OK_AND_ASSIGN(
      Proc * proc, BuildProc(pb, {constant_state, next_not_constant_state}));

  EXPECT_THAT(
      proc->StateParams(),
      UnorderedElementsAre(m::Param("constant"), m::Param("not_constant")));
  EXPECT_THAT(state_usage.node(),
              m::Add(m::TupleIndex(m::Param("constant")),
                     m::TupleIndex(m::Param("not_constant"))));
  EXPECT_THAT(Run(p.get()), IsOkAndHolds(true));
  EXPECT_THAT(proc->StateParams(),
              UnorderedElementsAre(m::Param("not_constant")));
  // Verify the state element has been replaced with its initial value.
  EXPECT_THAT(state_usage.node(),
              m::Add(m::TupleIndex(m::Literal(Value::Tuple({one, zero}))),
                     m::TupleIndex(m::Param("not_constant"))));
}

TEST_P(ProcStateOptimizationPassTest, ProcWithImplicitlyConstantStateElements) {
  auto p = CreatePackage();
  Type* u32 = p->GetBitsType(32);
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * out,
      p->CreateStreamingChannel("out", ChannelOps::kSendOnly, u32));

  TokenlessProcBuilder pb("p", "tkn", p.get());
  Value zero(UBits(0, 32));
  Value one(UBits(1, 32));
  BValue constant_state = pb.StateElement("constant", zero);
  BValue not_constant_state =
      pb.StateElement("not_constant", Value::Tuple({zero, zero}));
  BValue implicit_constant_state_1 =
      pb.StateElement("implicit_constant_1", one);
  BValue implicit_constant_state_2 =
      pb.StateElement("implicit_constant_2", one);
  // Use one element of each state.
  pb.Send(out,
          pb.Or(pb.Or(constant_state, pb.TupleIndex(not_constant_state, 0)),
                pb.Or(implicit_constant_state_1, implicit_constant_state_2)));
  // Modify just one part of the `not_constant` state so it can't be
  // eliminated.
  BValue next_not_constant_state =
      pb.Tuple({pb.Not(pb.TupleIndex(not_constant_state, 0)),
                pb.TupleIndex(not_constant_state, 1)});
  // Modify the implicitly-constant states based on the `constant` state.
  BValue next_implicit_constant_state_1 =
      pb.Xor(constant_state, pb.Literal(one));
  BValue next_implicit_constant_state_2 =
      pb.Xor(constant_state, implicit_constant_state_1);

  XLS_ASSERT_OK_AND_ASSIGN(
      Proc * proc, BuildProc(pb, {constant_state, next_not_constant_state,
                                  next_implicit_constant_state_1,
                                  next_implicit_constant_state_2}));

  EXPECT_THAT(
      proc->StateParams(),
      UnorderedElementsAre(m::Param("constant"), m::Param("not_constant"),
                           m::Param("implicit_constant_1"),
                           m::Param("implicit_constant_2")));
  EXPECT_THAT(Run(p.get()), IsOkAndHolds(true));
  EXPECT_THAT(proc->StateParams(),
              UnorderedElementsAre(m::Param("not_constant")));
}

TEST_P(ProcStateOptimizationPassTest, LiteralChainOfSize1) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * out, p->CreateStreamingChannel("out", ChannelOps::kSendOnly,
                                               p->GetBitsType(32)));

  TokenlessProcBuilder pb("p", "tkn", p.get());
  BValue x = pb.StateElement("x", Value(UBits(100, 32)));
  BValue lit = pb.Literal(Value(UBits(200, 32)));
  BValue send = pb.Send(out, x);

  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, BuildProc(pb, {lit}));

  EXPECT_EQ(proc->GetStateElementCount(), 1);
  EXPECT_THAT(Run(p.get()), IsOkAndHolds(true));
  EXPECT_EQ(proc->GetStateElementCount(), 1);
  EXPECT_EQ(proc->GetStateParam(0)->GetType()->GetFlatBitCount(), 1);

  EXPECT_THAT(
      send.node(),
      m::Send(m::Literal(Value::Token()), m::Select(m::Param("state_machine_x"),
                                         /*cases=*/{m::Literal(100)},
                                         /*default_value=*/m::Literal(200))));
}

INSTANTIATE_TEST_SUITE_P(NextValueTypes, ProcStateOptimizationPassTest,
                         testing::Values(NextValueType::kNextStateElements,
                                         NextValueType::kNextValueNodes),
                         testing::PrintToStringParamName());

}  // namespace
}  // namespace xls
