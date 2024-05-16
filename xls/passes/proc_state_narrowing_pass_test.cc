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

#include "xls/passes/proc_state_narrowing_pass.h"

#include <string_view>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/status/statusor.h"
#include "xls/common/status/matchers.h"
#include "xls/ir/bits.h"
#include "xls/ir/channel_ops.h"
#include "xls/ir/function_builder.h"
#include "xls/ir/ir_matcher.h"
#include "xls/ir/ir_test_base.h"
#include "xls/ir/lsb_or_msb.h"
#include "xls/ir/package.h"
#include "xls/ir/proc.h"
#include "xls/ir/value.h"
#include "xls/passes/pass_base.h"
#include "xls/passes/proc_state_optimization_pass.h"
#include "xls/solvers/z3_ir_equivalence_testutils.h"

namespace m = ::xls::op_matchers;
namespace xls {
namespace {
using status_testing::IsOkAndHolds;
using testing::AllOf;
using testing::UnorderedElementsAre;

class ProcStateNarrowingPassTest : public IrTestBase {
 public:
  absl::StatusOr<bool> RunPass(Proc* p) {
    ScopedRecordIr sri(p->package());
    ProcStateNarrowingPass pass;
    PassResults r;
    return pass.Run(p->package(), {}, &r);
  }

  absl::StatusOr<bool> RunProcStateCleanup(Proc* p) {
    ScopedRecordIr sri(p->package(), "cleanup", /*with_initial=*/false);
    ProcStateOptimizationPass psop;
    PassResults r;
    return psop.Run(p->package(), {}, &r);
  }
};

TEST_F(ProcStateNarrowingPassTest, ZeroExtend) {
  auto p = CreatePackage();
  ProcBuilder fb(TestName(), p.get());
  auto st = fb.StateElement("foo", UBits(0, 32));
  XLS_ASSERT_OK_AND_ASSIGN(
      auto chan, p->CreateStreamingChannel("side_effect", ChannelOps::kSendOnly,
                                           p->GetBitsType(32)));
  fb.Send(chan, fb.Literal(Value::Token()), st);
  fb.Next(st, fb.ZeroExtend(
                  fb.Add(fb.Literal(UBits(1, 3)), fb.BitSlice(st, 0, 3)), 32));

  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, fb.Build());

  EXPECT_THAT(RunPass(proc), IsOkAndHolds(true));
  EXPECT_THAT(RunProcStateCleanup(proc), IsOkAndHolds(true));

  EXPECT_THAT(
      proc->StateParams(),
      UnorderedElementsAre(AllOf(m::Type(p->GetBitsType(3)), m::Param("foo"))));
}

TEST_F(ProcStateNarrowingPassTest, ZeroExtendMultiple) {
  auto p = CreatePackage();
  ProcBuilder fb(TestName(), p.get());
  auto st = fb.StateElement("foo", UBits(0, 32));
  auto onehot = fb.OneHot(st, LsbOrMsb::kLsb);
  XLS_ASSERT_OK_AND_ASSIGN(
      auto chan, p->CreateStreamingChannel("side_effect", ChannelOps::kSendOnly,
                                           p->GetBitsType(32)));
  fb.Send(chan, fb.Literal(Value::Token()), st);
  fb.Next(
      st,
      fb.ZeroExtend(fb.Add(fb.Literal(UBits(1, 3)), fb.BitSlice(st, 0, 3)), 32),
      /*pred=*/fb.BitSlice(onehot, 0, 1));
  fb.Next(
      st,
      fb.ZeroExtend(fb.Add(fb.Literal(UBits(2, 3)), fb.BitSlice(st, 0, 3)), 32),
      /*pred=*/fb.BitSlice(onehot, 1, 1));
  fb.Next(
      st,
      fb.ZeroExtend(fb.Add(fb.Literal(UBits(3, 3)), fb.BitSlice(st, 0, 3)), 32),
      /*pred=*/fb.BitSlice(onehot, 2, 1));

  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, fb.Build());

  EXPECT_THAT(RunPass(proc), IsOkAndHolds(true));
  EXPECT_THAT(RunProcStateCleanup(proc), IsOkAndHolds(true));

  EXPECT_THAT(
      proc->StateParams(),
      UnorderedElementsAre(AllOf(m::Type(p->GetBitsType(3)), m::Param("foo"))));
}

TEST_F(ProcStateNarrowingPassTest, ZeroExtendWithBigInitial) {
  auto p = CreatePackage();
  ProcBuilder fb(TestName(), p.get());
  auto st = fb.StateElement("foo", UBits(0xFF, 32));
  XLS_ASSERT_OK_AND_ASSIGN(
      auto chan, p->CreateStreamingChannel("side_effect", ChannelOps::kSendOnly,
                                           p->GetBitsType(32)));
  fb.Send(chan, fb.Literal(Value::Token()), st);
  fb.Next(st, fb.ZeroExtend(
                  fb.Add(fb.Literal(UBits(1, 3)), fb.BitSlice(st, 0, 3)), 32));

  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, fb.Build());

  EXPECT_THAT(RunPass(proc), IsOkAndHolds(true));
  EXPECT_THAT(RunProcStateCleanup(proc), IsOkAndHolds(true));

  EXPECT_THAT(
      proc->StateParams(),
      UnorderedElementsAre(AllOf(m::Type(p->GetBitsType(8)), m::Param("foo"))));
}

// Basic IR we want proc-state narrowing to improve.
TEST_F(ProcStateNarrowingPassTest, BasicLoop) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(
      auto* chan, p->CreateStreamingChannel("test_chan", ChannelOps::kSendOnly,
                                            p->GetBitsType(32)));
  ProcBuilder pb(TestName(), p.get());
  auto state = pb.StateElement("the_state", UBits(1, 32));
  // State just counts up 1 to 6 then resets to 1.
  // NB Limit is exactly 6 and comparison is LT so that however the transform is
  // done the state fits in 3 bits.
  auto in_loop = pb.ULt(state, pb.Literal(UBits(6, 32)));
  pb.Send(chan, pb.Literal(Value::Token()), state);
  pb.Next(state, pb.Add(state, pb.Literal(UBits(1, 32))), in_loop);
  // Reset value is intentionally not something that could be removed by
  // exploiting overflow
  pb.Next(state, pb.Literal(UBits(1, 32)), pb.Not(in_loop));

  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, pb.Build());

  solvers::z3::ScopedVerifyProcEquivalence svpe(proc, /*activation_count=*/16,
                                                /*include_state=*/false);
  ScopedRecordIr sri(p.get());
  EXPECT_THAT(RunPass(proc), IsOkAndHolds(true));
  EXPECT_THAT(RunProcStateCleanup(proc), IsOkAndHolds(true));

  EXPECT_THAT(proc->StateParams(),
              UnorderedElementsAre(
                  AllOf(m::Param("the_state"), m::Type(p->GetBitsType(3)))));
}
TEST_F(ProcStateNarrowingPassTest, BasicHalt) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(
      auto* chan, p->CreateStreamingChannel("test_chan", ChannelOps::kSendOnly,
                                            p->GetBitsType(32)));
  ProcBuilder pb(TestName(), p.get());
  auto state = pb.StateElement("the_state", UBits(1, 32));
  pb.Send(chan, pb.Literal(Value::Token()), state);
  // State just counts up 1 to 7 then stops updating.
  // NB Limit is exactly 7 and comparison is LT so that however the transform is
  // done the state fits in 3 bits.
  auto in_loop = pb.ULt(state, pb.Literal(UBits(7, 32)));
  pb.Next(state, pb.Add(state, pb.Literal(UBits(1, 32))), in_loop);
  // If we aren't looping the value just stays permanently at the end state.
  pb.Next(state, state, pb.Not(in_loop));

  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, pb.Build());

  solvers::z3::ScopedVerifyProcEquivalence svpe(proc, /*activation_count=*/16,
                                                /*include_state=*/false);
  ScopedRecordIr sri(p.get());
  EXPECT_THAT(RunPass(proc), IsOkAndHolds(true));
  EXPECT_THAT(RunProcStateCleanup(proc), IsOkAndHolds(true));

  EXPECT_THAT(proc->StateParams(),
              UnorderedElementsAre(
                  AllOf(m::Param("the_state"), m::Type(p->GetBitsType(3)))));
}

TEST_F(ProcStateNarrowingPassTest, MultiPath) {
  // loop 1-10 with a reset
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(
      auto* test_chan,
      p->CreateStreamingChannel("test_chan", ChannelOps::kSendOnly,
                                p->GetBitsType(32)));
  XLS_ASSERT_OK_AND_ASSIGN(
      auto* reset,
      p->CreateStreamingChannel("reset_chan", ChannelOps::kReceiveOnly,
                                p->GetBitsType(3)));
  ProcBuilder pb(TestName(), p.get());
  auto state = pb.StateElement("the_state", UBits(1, 32));
  auto send_tok = pb.Send(test_chan, pb.Literal(Value::Token()), state);
  // State just counts up 1 to 10 then stops updating.
  // Limit fits in 4 bits
  auto in_loop = pb.ULt(state, pb.Literal(UBits(10, 32)));
  auto reset_val = pb.ReceiveNonBlocking(reset, send_tok);
  // Either current state or the value in range 0-7 received from the channel.
  // 0-7 fits in 4 bit range of the state element.
  auto state_or_reset =
      pb.Select(pb.TupleIndex(reset_val, 2),
                {state, pb.ZeroExtend(pb.TupleIndex(reset_val, 1), 32)});
  pb.Next(state, pb.Add(state_or_reset, pb.Literal(UBits(1, 32))), in_loop);
  // If we aren't looping the value just stays permanently at the end state.
  pb.Next(state, state, pb.Not(in_loop));

  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, pb.Build());

  solvers::z3::ScopedVerifyProcEquivalence svpe(proc, /*activation_count=*/16,
                                                /*include_state=*/false);
  ScopedRecordIr sri(p.get());
  EXPECT_THAT(RunPass(proc), IsOkAndHolds(true));
  EXPECT_THAT(RunProcStateCleanup(proc), IsOkAndHolds(true));

  EXPECT_THAT(proc->StateParams(),
              UnorderedElementsAre(
                  AllOf(m::Param("the_state"), m::Type(p->GetBitsType(4)))));
}

}  // namespace
}  // namespace xls
