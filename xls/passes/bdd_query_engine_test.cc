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

#include "xls/passes/bdd_query_engine.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xls/common/status/matchers.h"
#include "xls/ir/bits.h"
#include "xls/ir/function.h"
#include "xls/ir/function_builder.h"
#include "xls/ir/ir_test_base.h"
#include "xls/ir/op.h"
#include "xls/ir/package.h"

namespace xls {
namespace {

class BddQueryEngineTest : public IrTestBase {
 protected:
  // Convenience methods for testing implication, equality, and inverse for
  // single-bit node values.
  bool Implies(const QueryEngine& engine, Node* a, Node* b) {
    return engine.Implies(TreeBitLocation(a, 0), TreeBitLocation(b, 0));
  }
  bool KnownEquals(const QueryEngine& engine, Node* a, Node* b) {
    return engine.KnownEquals(TreeBitLocation(a, 0), TreeBitLocation(b, 0));
  }
  bool KnownNotEquals(const QueryEngine& engine, Node* a, Node* b) {
    return engine.KnownNotEquals(TreeBitLocation(a, 0), TreeBitLocation(b, 0));
  }
};

TEST_F(BddQueryEngineTest, EqualToPredicates) {
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  BValue x = fb.Param("x", p->GetBitsType(8));
  BValue y = fb.Param("y", p->GetBitsType(8));
  BValue x_eq_0 = fb.Eq(x, fb.Literal(UBits(0, 8)));
  BValue x_eq_0_2 = fb.Eq(x, fb.Literal(UBits(0, 8)));
  BValue x_ne_0 = fb.Not(x_eq_0);
  BValue x_eq_42 = fb.Eq(x, fb.Literal(UBits(7, 8)));
  BValue y_eq_42 = fb.Eq(y, fb.Literal(UBits(7, 8)));
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());
  BddQueryEngine query_engine;
  XLS_ASSERT_OK(query_engine.Populate(f).status());

  EXPECT_TRUE(query_engine.AtMostOneNodeTrue({}));
  EXPECT_FALSE(query_engine.AtMostOneBitTrue(x.node()));
  EXPECT_TRUE(query_engine.AtMostOneNodeTrue({x_eq_0.node(), x_eq_42.node()}));
  EXPECT_TRUE(query_engine.AtLeastOneNodeTrue({x_eq_0.node(), x_ne_0.node()}));

  EXPECT_TRUE(KnownEquals(query_engine, x_eq_0.node(), x_eq_0.node()));
  EXPECT_TRUE(KnownEquals(query_engine, x_eq_0.node(), x_eq_0_2.node()));
  EXPECT_FALSE(KnownNotEquals(query_engine, x_eq_0.node(), x_eq_0_2.node()));
  EXPECT_TRUE(KnownNotEquals(query_engine, x_eq_0.node(), x_ne_0.node()));

  EXPECT_TRUE(Implies(query_engine, x_eq_0.node(), x_eq_0.node()));
  EXPECT_TRUE(Implies(query_engine, x_eq_0.node(), x_eq_0_2.node()));
  EXPECT_FALSE(Implies(query_engine, x_eq_0.node(), x_eq_42.node()));

  // Unrelated values 'x' and 'y' should have no relationships.
  EXPECT_FALSE(Implies(query_engine, x_eq_42.node(), y_eq_42.node()));
  EXPECT_FALSE(KnownEquals(query_engine, x_eq_42.node(), y_eq_42.node()));
  EXPECT_FALSE(KnownNotEquals(query_engine, x_eq_42.node(), y_eq_42.node()));
  EXPECT_FALSE(
      query_engine.AtMostOneNodeTrue({x_eq_42.node(), y_eq_42.node()}));
  EXPECT_FALSE(
      query_engine.AtLeastOneNodeTrue({x_eq_42.node(), y_eq_42.node()}));
}

TEST_F(BddQueryEngineTest, VariousComparisonPredicates) {
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  BValue x = fb.Param("x", p->GetBitsType(32));
  BValue x_eq_42 = fb.Eq(x, fb.Literal(UBits(42, 32)));
  BValue x_lt_42 = fb.ULt(x, fb.Literal(UBits(42, 32)));
  BValue x_ge_20 = fb.UGe(x, fb.Literal(UBits(20, 32)));
  BValue x_lt_20 = fb.ULt(x, fb.Literal(UBits(20, 32)));
  BValue x_eq_7 = fb.Eq(x, fb.Literal(UBits(7, 32)));
  BValue x_eq_999 = fb.Eq(x, fb.Literal(UBits(999, 32)));
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());
  BddQueryEngine query_engine;
  XLS_ASSERT_OK(query_engine.Populate(f).status());

  EXPECT_TRUE(query_engine.AtMostOneNodeTrue(
      {x_eq_42.node(), x_eq_7.node(), x_eq_999.node()}));
  EXPECT_FALSE(
      query_engine.AtMostOneNodeTrue({x_lt_42.node(), x_ge_20.node()}));

  EXPECT_TRUE(
      query_engine.AtLeastOneNodeTrue({x_lt_42.node(), x_ge_20.node()}));
  EXPECT_TRUE(
      query_engine.AtLeastOneNodeTrue({x_ge_20.node(), x_lt_20.node()}));

  EXPECT_TRUE(Implies(query_engine, x_eq_7.node(), x_lt_42.node()));
  EXPECT_FALSE(Implies(query_engine, x_lt_42.node(), x_eq_7.node()));
}

TEST_F(BddQueryEngineTest, BitValuesImplyNodeValueSimple) {
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  BValue x = fb.Param("x", p->GetBitsType(1));
  BValue x_not = fb.Not(x);
  BValue concat = fb.Concat({x, x_not});

  XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());
  BddQueryEngine query_engine;
  XLS_ASSERT_OK(query_engine.Populate(f).status());

  auto result =
      query_engine.ImpliedNodeValue({{{x.node(), 0}, true}}, concat.node());
  EXPECT_TRUE(result.has_value());
  EXPECT_THAT(result.value().ToBitVector(), testing::ElementsAre(false, true));
}

TEST_F(BddQueryEngineTest, BitValuesImplyNodeValueComplex) {
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  BValue a = fb.Param("a", p->GetBitsType(1));
  BValue b = fb.Param("b", p->GetBitsType(1));
  BValue c = fb.Param("c", p->GetBitsType(1));
  BValue d = fb.Param("d", p->GetBitsType(1));
  BValue a_or_b = fb.Or(a, b);
  BValue a_and_b = fb.And(a, b);
  BValue c_and_d = fb.And(c, d);
  BValue c_xor_d = fb.Xor(c, d);
  BValue concat = fb.Concat({a_or_b, c_and_d});

  XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());
  BddQueryEngine query_engine;
  XLS_ASSERT_OK(query_engine.Populate(f).status());

  auto result = query_engine.ImpliedNodeValue(
      {{{a_and_b.node(), 0}, true}, {{c_xor_d.node(), 0}, true}},
      concat.node());
  EXPECT_TRUE(result.has_value());
  EXPECT_THAT(result.value().ToBitVector(), testing::ElementsAre(false, true));
}

TEST_F(BddQueryEngineTest, BitValuesImplyNodeValueFalsePredice) {
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  BValue a = fb.Param("a", p->GetBitsType(1));
  BValue b = fb.Param("b", p->GetBitsType(1));
  BValue a_or_b = fb.Or(a, b);
  BValue a_and_b = fb.And(a, b);
  BValue a_xor_b = fb.Xor(a, b);

  XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());
  BddQueryEngine query_engine;
  XLS_ASSERT_OK(query_engine.Populate(f).status());

  auto result = query_engine.ImpliedNodeValue(
      {{{a_and_b.node(), 0}, false}, {{a_or_b.node(), 0}, true}},
      a_xor_b.node());
  EXPECT_TRUE(result.has_value());
  EXPECT_THAT(result.value().ToBitVector(), testing::ElementsAre(true));
}

TEST_F(BddQueryEngineTest, BitValuesImplyNodeValueNoValueImpliedLogical) {
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  BValue a = fb.Param("a", p->GetBitsType(1));
  BValue b = fb.Param("b", p->GetBitsType(1));
  BValue a_or_b = fb.Or(a, b);
  BValue a_and_b = fb.And(a, b);

  XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());
  BddQueryEngine query_engine;
  XLS_ASSERT_OK(query_engine.Populate(f).status());

  auto result = query_engine.ImpliedNodeValue({{{a_or_b.node(), 0}, true}},
                                              a_and_b.node());
  EXPECT_FALSE(result.has_value());
}

TEST_F(BddQueryEngineTest, BitValuesImplyNodeValueNotImpliedUnrelated) {
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  BValue a = fb.Param("a", p->GetBitsType(1));
  BValue b = fb.Param("b", p->GetBitsType(1));
  BValue c = fb.Param("c", p->GetBitsType(1));
  BValue d = fb.Param("d", p->GetBitsType(1));
  BValue a_or_b = fb.Or(a, b);
  BValue a_and_b = fb.And(a, b);
  BValue c_and_d = fb.And(c, d);
  BValue c_xor_d = fb.Xor(c, d);

  BValue q = fb.Param("q", p->GetBitsType(1));
  BValue concat = fb.Concat({a_or_b, c_and_d, q});

  XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());
  BddQueryEngine query_engine;
  XLS_ASSERT_OK(query_engine.Populate(f).status());

  auto result = query_engine.ImpliedNodeValue(
      {{{a_and_b.node(), 0}, true}, {{c_xor_d.node(), 0}, true}},
      concat.node());
  EXPECT_FALSE(result.has_value());
}

TEST_F(BddQueryEngineTest, BitValuesImplyNodeValueNoValueImpliedNonBit) {
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  BValue a = fb.Param("a", p->GetBitsType(1));
  BValue b = fb.Param("b", p->GetBitsType(1));
  BValue a_and_b = fb.And(a, b);
  BValue array = fb.Array({a, b}, a.node()->GetType());

  XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());
  BddQueryEngine query_engine;
  XLS_ASSERT_OK(query_engine.Populate(f).status());

  auto result = query_engine.ImpliedNodeValue({{{a_and_b.node(), 0}, true}},
                                              array.node());
  EXPECT_FALSE(result.has_value());
}

TEST_F(BddQueryEngineTest,
       BitValuesImplyNodeValueNoValueImpliedEmptyPredicate) {
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  BValue a = fb.Param("a", p->GetBitsType(1));

  XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());
  BddQueryEngine query_engine;
  XLS_ASSERT_OK(query_engine.Populate(f).status());

  auto result = query_engine.ImpliedNodeValue({}, a.node());
  EXPECT_FALSE(result.has_value());
}

TEST_F(BddQueryEngineTest, ForceNodeToBeModeledAsVariable) {
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  BValue x = fb.Param("x", p->GetBitsType(32));
  BValue x_not = fb.Not(x);
  BValue andop = fb.And(x, x_not);
  BValue orop = fb.Or(x, x_not);
  BValue my_one = fb.Literal(UBits(1, 1));
  BValue my_zero = fb.Literal(UBits(0, 1));

  XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());
  BddQueryEngine query_engine(/*path_limit=*/0,
                              [](const Node* n) { return n->op() != Op::kOr; });
  XLS_ASSERT_OK(query_engine.Populate(f).status());
  EXPECT_FALSE(KnownEquals(query_engine, andop.node(), my_one.node()));
  EXPECT_TRUE(KnownEquals(query_engine, andop.node(), my_zero.node()));
  EXPECT_FALSE(KnownEquals(query_engine, orop.node(), my_one.node()));
  EXPECT_FALSE(KnownEquals(query_engine, orop.node(), my_zero.node()));
  BddQueryEngine query_engine_empty_op_set(/*path_limit=*/0);
  XLS_ASSERT_OK(query_engine_empty_op_set.Populate(f).status());
  EXPECT_FALSE(
      KnownEquals(query_engine_empty_op_set, andop.node(), my_one.node()));
  EXPECT_TRUE(
      KnownEquals(query_engine_empty_op_set, andop.node(), my_zero.node()));
  EXPECT_TRUE(
      KnownEquals(query_engine_empty_op_set, orop.node(), my_one.node()));
  EXPECT_FALSE(
      KnownEquals(query_engine_empty_op_set, orop.node(), my_zero.node()));
}

TEST_F(BddQueryEngineTest, BitValuesImplyNodeValuePredicateAlwaysFalse) {
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  BValue a = fb.Param("a", p->GetBitsType(1));
  BValue a_not = fb.Not(a);
  BValue orop = fb.Or(a, a_not);

  XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());
  BddQueryEngine query_engine;
  XLS_ASSERT_OK(query_engine.Populate(f).status());

  auto result = query_engine.ImpliedNodeValue(
      {{{a.node(), 0}, true}, {{a_not.node(), 0}, true}}, orop.node());
  EXPECT_FALSE(result.has_value());
}

}  // namespace
}  // namespace xls
