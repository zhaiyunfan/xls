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

#include "xls/solvers/z3_ir_translator.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/cleanup/cleanup.h"
#include "absl/container/flat_hash_map.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/memory/memory.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "xls/common/status/ret_check.h"
#include "xls/common/status/status_macros.h"
#include "xls/data_structures/leaf_type_tree.h"
#include "xls/ir/abstract_evaluator.h"
#include "xls/ir/abstract_node_evaluator.h"
#include "xls/ir/bits.h"
#include "xls/ir/function.h"
#include "xls/ir/function_base.h"
#include "xls/ir/node.h"
#include "xls/ir/nodes.h"
#include "xls/ir/type.h"
#include "xls/ir/value.h"
#include "xls/solvers/z3_op_translator.h"
#include "xls/solvers/z3_utils.h"
#include "../z3/src/api/z3_api.h"
#include "../z3/src/api/z3_fpa.h"

namespace xls {
namespace solvers {
namespace z3 {

/* static */ Predicate Predicate::IsEqualTo(Node* other) {
  return Predicate(PredicateKind::kEqualToNode, other);
}
/* static */ Predicate Predicate::EqualToZero() {
  return Predicate(PredicateKind::kEqualToZero);
}
/* static */ Predicate Predicate::NotEqualToZero() {
  return Predicate(PredicateKind::kNotEqualToZero);
}
/* static */ Predicate Predicate::UnsignedGreaterOrEqual(Bits lower_bound) {
  return Predicate(PredicateKind::kUnsignedGreaterOrEqual, nullptr,
                   std::move(lower_bound));
}
/* static */ Predicate Predicate::UnsignedLessOrEqual(Bits upper_bound) {
  return Predicate(PredicateKind::kUnsignedLessOrEqual, nullptr,
                   std::move(upper_bound));
}

Predicate::Predicate(PredicateKind kind) : kind_(kind) {}

Predicate::Predicate(PredicateKind kind, Node* node)
    : kind_(kind), node_(node) {}

Predicate::Predicate(PredicateKind kind, Node* node, Bits value)
    : kind_(kind), node_(node), value_(std::move(value)) {}

std::string Predicate::ToString() const {
  switch (kind_) {
    case PredicateKind::kEqualToZero:
      return "eq zero";
    case PredicateKind::kNotEqualToZero:
      return "ne zero";
    case PredicateKind::kEqualToNode:
      return "eq " + node()->GetName();
    case PredicateKind::kUnsignedGreaterOrEqual:
      return "uge " + value_->ToDebugString();
    case PredicateKind::kUnsignedLessOrEqual:
      return "ule " + value_->ToDebugString();
  }
  return absl::StrFormat("<invalid predicate kind %d>",
                         static_cast<int>(kind_));
}

namespace {

// Helper class for using the AbstractNodeEvaluator to enqueue Z3 expressions.
class Z3AbstractEvaluator
    : public AbstractEvaluator<Z3_ast, Z3AbstractEvaluator> {
 public:
  explicit Z3AbstractEvaluator(Z3_context z3_ctx) : translator_(z3_ctx) {}
  Element One() const { return translator_.Fill(true, 1); }
  Element Zero() const { return translator_.Fill(false, 1); }
  Element Not(const Element& a) const { return translator_.Not(a); }
  Element And(const Element& a, const Element& b) const {
    return translator_.And(a, b);
  }
  Element Or(const Element& a, const Element& b) const {
    return translator_.Or(a, b);
  }

 private:
  mutable Z3OpTranslator translator_;
};

// Returns the index with the proper bitwidth for the given array_type.
Z3_ast GetAsFormattedArrayIndex(Z3_context ctx, Z3_ast index,
                                ArrayType* array_type) {
  // In XLS, array indices can be of any sort, whereas in Z3, index types need
  // to be declared w/the array (the "domain" argument - we declare that to be
  // the smallest bit vector that covers all indices. Thus, we need to "cast"
  // appropriately here.
  uint32_t target_width = static_cast<uint32_t>(
      Bits::MinBitCountUnsigned(static_cast<uint64_t>(array_type->size())));
  int z3_width = Z3_get_bv_sort_size(ctx, Z3_get_sort(ctx, index));
  if (z3_width < target_width) {
    index = Z3_mk_zero_ext(ctx, target_width - z3_width, index);
  } else if (z3_width > target_width) {
    index = Z3_mk_extract(ctx, target_width - 1, /*low=*/0, index);
  }

  return index;
}

}  // namespace

absl::StatusOr<std::unique_ptr<IrTranslator>> IrTranslator::CreateAndTranslate(
    FunctionBase* source, bool allow_unsupported) {
  Z3_config config = Z3_mk_config();
  Z3_set_param_value(config, "proof", "true");
  auto translator = absl::WrapUnique(new IrTranslator(config, source));
  translator->allow_unsupported_ = allow_unsupported;
  if (source != nullptr) {
    XLS_RET_CHECK(!source->IsBlock());
    XLS_RETURN_IF_ERROR(source->Accept(translator.get()));
  }
  return translator;
}

absl::StatusOr<std::unique_ptr<IrTranslator>> IrTranslator::CreateAndTranslate(
    Z3_context ctx, Node* source, bool allow_unsupported) {
  auto translator = absl::WrapUnique(new IrTranslator(
      ctx, nullptr, std::optional<absl::Span<const Z3_ast>>()));
  translator->allow_unsupported_ = allow_unsupported;
  if (source != nullptr) {
    XLS_RETURN_IF_ERROR(source->Accept(translator.get()));
  }
  return translator;
}

absl::StatusOr<std::unique_ptr<IrTranslator>> IrTranslator::CreateAndTranslate(
    Z3_context ctx, FunctionBase* function_base,
    absl::Span<const Z3_ast> imported_params, bool allow_unsupported) {
  auto translator =
      absl::WrapUnique(new IrTranslator(ctx, function_base, imported_params));
  translator->allow_unsupported_ = allow_unsupported;
  XLS_RET_CHECK(!function_base->IsBlock());
  XLS_RETURN_IF_ERROR(function_base->Accept(translator.get()));
  return translator;
}

absl::Status IrTranslator::Retranslate(
    const absl::flat_hash_map<const Node*, Z3_ast>& replacements) {
  ResetVisitedState();
  translations_ = replacements;
  return xls_function_->Accept(this);
}

IrTranslator::IrTranslator(Z3_config config, FunctionBase* source)
    : config_(config),
      ctx_(Z3_mk_context(config_)),
      borrowed_context_(false),
      xls_function_(source),
      current_symbol_(0) {}

IrTranslator::IrTranslator(
    Z3_context ctx, FunctionBase* source,
    std::optional<absl::Span<const Z3_ast>> imported_params)
    : ctx_(ctx),
      borrowed_context_(true),
      imported_params_(imported_params),
      xls_function_(source),
      current_symbol_(0) {}

IrTranslator::~IrTranslator() {
  if (!borrowed_context_) {
    Z3_del_context(ctx_);
    Z3_del_config(config_);
  }
}

Z3_ast IrTranslator::GetTranslation(const Node* source) {
  return translations_.at(source);
}

Z3_ast IrTranslator::GetReturnNode() {
  CHECK_NE(xls_function_, nullptr);
  CHECK(xls_function_->IsFunction());
  return GetTranslation(xls_function_->AsFunctionOrDie()->return_value());
}

Z3_sort_kind IrTranslator::GetValueKind(Z3_ast value) {
  Z3_sort sort = Z3_get_sort(ctx_, value);
  return Z3_get_sort_kind(ctx_, sort);
}

void IrTranslator::SetTimeout(absl::Duration timeout) {
  std::string timeout_str = absl::StrCat(absl::ToInt64Milliseconds(timeout));
  Z3_update_param_value(ctx_, "timeout", timeout_str.c_str());
}

void IrTranslator::SetRlimit(int64_t rlimit) {
  const std::string rlimit_str = absl::StrCat(rlimit);
  Z3_update_param_value(ctx_, "rlimit", rlimit_str.c_str());
}

Z3_ast IrTranslator::FloatZero(Z3_sort sort) {
  return Z3_mk_fpa_zero(ctx_, sort, /*negative=*/false);
}

absl::StatusOr<Z3_ast> IrTranslator::FloatFlushSubnormal(Z3_ast value) {
  Z3_sort sort = Z3_get_sort(ctx_, value);
  Z3_sort_kind sort_kind = Z3_get_sort_kind(ctx_, sort);
  if (sort_kind != Z3_FLOATING_POINT_SORT) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Wrong sort for floating-point operations: %d.",
                        static_cast<int>(sort_kind)));
  }
  Z3_ast is_subnormal(Z3_mk_fpa_is_subnormal(ctx_, value));
  return Z3_mk_ite(ctx_, is_subnormal, FloatZero(sort), value);
}

absl::StatusOr<Z3_ast> IrTranslator::ToFloat32(absl::Span<const Z3_ast> nodes) {
  if (nodes.size() != 3) {
    return absl::InvalidArgumentError(absl::StrCat(
        "Incorrect number of arguments - need 3, got ", nodes.size()));
  }

  // Does some validation and returns the node of interest.
  auto get_fp_component =
      [this, nodes](int64_t index,
                    int64_t expected_width) -> absl::StatusOr<Z3_ast> {
    Z3_sort sort = Z3_get_sort(ctx_, nodes[index]);
    Z3_sort_kind sort_kind = Z3_get_sort_kind(ctx_, sort);
    if (sort_kind != Z3_BV_SORT) {
      return absl::InvalidArgumentError(absl::StrCat(
          "Wrong sort for floating-point components: need Z3_BV_SORT, got ",
          static_cast<int>(sort_kind)));
    }

    int bit_width = Z3_get_bv_sort_size(ctx_, sort);
    if (bit_width != expected_width) {
      return absl::InvalidArgumentError(
          absl::StrFormat("Invalid width for FP component %d: got %d, need %d",
                          index, bit_width, expected_width));
    }
    return nodes[index];
  };

  XLS_ASSIGN_OR_RETURN(Z3_ast sign, get_fp_component(0, 1));
  XLS_ASSIGN_OR_RETURN(Z3_ast exponent, get_fp_component(1, 8));
  XLS_ASSIGN_OR_RETURN(Z3_ast fraction, get_fp_component(2, 23));

  return Z3_mk_fpa_fp(ctx_, sign, exponent, fraction);
}

absl::StatusOr<Z3_ast> IrTranslator::ToFloat32(Z3_ast tuple) {
  std::vector<Z3_ast> components;
  Z3_sort tuple_sort = Z3_get_sort(ctx_, tuple);
  for (int i = 0; i < 3; i++) {
    Z3_func_decl func_decl = Z3_get_tuple_sort_field_decl(ctx_, tuple_sort, i);
    components.push_back(Z3_mk_app(ctx_, func_decl, 1, &tuple));
  }

  return ToFloat32(components);
}

template <typename OpT, typename FnT>
absl::Status IrTranslator::HandleBinary(OpT* op, FnT f) {
  ScopedErrorHandler seh(ctx_);
  Z3_ast result = f(ctx_, GetBitVec(op->operand(0)), GetBitVec(op->operand(1)));
  NoteTranslation(op, result);
  return seh.status();
}

absl::Status IrTranslator::HandleAdd(BinOp* add) {
  return HandleBinary(add, Z3_mk_bvadd);
}
absl::Status IrTranslator::HandleSub(BinOp* sub) {
  return HandleBinary(sub, Z3_mk_bvsub);
}

absl::Status IrTranslator::HandleULe(CompareOp* ule) {
  auto f = [](Z3_context ctx, Z3_ast lhs, Z3_ast rhs) {
    Z3OpTranslator op_translator(ctx);
    std::vector<Z3_ast> args;
    Z3_ast ult = op_translator.ULt(lhs, rhs);
    Z3_ast eq = op_translator.Eq(lhs, rhs);
    Z3_ast result = Z3_mk_bvor(ctx, ult, eq);
    return Z3_mk_bvredor(ctx, result);
  };
  return HandleBinary(ule, f);
}

absl::Status IrTranslator::HandleULt(CompareOp* lt) {
  auto f = [this](Z3_context ctx, Z3_ast lhs, Z3_ast rhs) {
    return Z3OpTranslator(ctx_).ULt(lhs, rhs);
  };
  return HandleBinary(lt, f);
}

absl::Status IrTranslator::HandleUDiv(BinOp* div) {
  auto f = [this](Z3_context ctx, Z3_ast lhs, Z3_ast rhs) {
    return Z3OpTranslator(ctx_).UDiv(lhs, rhs);
  };
  return HandleBinary(div, f);
}

absl::Status IrTranslator::HandleUMod(BinOp* mod) {
  auto f = [this](Z3_context ctx, Z3_ast lhs, Z3_ast rhs) {
    return Z3OpTranslator(ctx_).UMod(lhs, rhs);
  };
  return HandleBinary(mod, f);
}

absl::Status IrTranslator::HandleUGe(CompareOp* uge) {
  auto f = [](Z3_context ctx, Z3_ast lhs, Z3_ast rhs) {
    Z3OpTranslator t(ctx);
    return t.Not(t.ULt(lhs, rhs));
  };
  return HandleBinary(uge, f);
}

absl::Status IrTranslator::HandleUGt(CompareOp* gt) {
  auto f = [this](Z3_context ctx, Z3_ast lhs, Z3_ast rhs) {
    // If the msb of the subtraction result is set, that means we underflowed,
    // so RHS is > LHS (that is LHS < RHS)
    //
    // Since we're trying to determine whether LHS > RHS we ask whether:
    //    (LHS == RHS) => false
    //    (LHS < RHS) => false
    //    _ => true
    Z3OpTranslator t(ctx_);
    return t.Not(t.Or(t.Eq(lhs, rhs), t.ULt(lhs, rhs)));
  };
  return HandleBinary(gt, f);
}

absl::Status IrTranslator::HandleSGt(CompareOp* sgt) {
  auto f = [](Z3_context ctx, Z3_ast lhs, Z3_ast rhs) {
    Z3OpTranslator op_translator(ctx);
    Z3_ast slt = op_translator.SLt(lhs, rhs);
    Z3_ast eq = op_translator.Eq(lhs, rhs);

    Z3_ast result = Z3_mk_bvor(ctx, slt, eq);
    result = Z3_mk_bvredor(ctx, result);
    return Z3_mk_bvnot(ctx, result);
  };
  return HandleBinary(sgt, f);
}

absl::Status IrTranslator::HandleSLe(CompareOp* sle) {
  auto f = [](Z3_context ctx, Z3_ast lhs, Z3_ast rhs) {
    Z3OpTranslator op_translator(ctx);
    std::vector<Z3_ast> args;
    Z3_ast slt = op_translator.SLt(lhs, rhs);
    Z3_ast eq = op_translator.Eq(lhs, rhs);
    Z3_ast result = Z3_mk_bvor(ctx, slt, eq);
    return Z3_mk_bvredor(ctx, result);
  };
  return HandleBinary(sle, f);
}

absl::Status IrTranslator::HandleSLt(CompareOp* slt) {
  auto f = [](Z3_context ctx, Z3_ast lhs, Z3_ast rhs) {
    return Z3OpTranslator(ctx).SLt(lhs, rhs);
  };
  return HandleBinary(slt, f);
}

absl::Status IrTranslator::HandleSDiv(BinOp* div) {
  auto f = [this](Z3_context ctx, Z3_ast lhs, Z3_ast rhs) {
    return Z3OpTranslator(ctx_).SDiv(lhs, rhs);
  };
  return HandleBinary(div, f);
}

absl::Status IrTranslator::HandleSMod(BinOp* mod) {
  auto f = [this](Z3_context ctx, Z3_ast lhs, Z3_ast rhs) {
    return Z3OpTranslator(ctx_).SMod(lhs, rhs);
  };
  return HandleBinary(mod, f);
}

absl::Status IrTranslator::HandleSGe(CompareOp* sge) {
  auto f = [](Z3_context ctx, Z3_ast lhs, Z3_ast rhs) {
    Z3OpTranslator t(ctx);
    return t.Not(t.SLt(lhs, rhs));
  };
  return HandleBinary(sge, f);
}

namespace {
// Returns the Z3 value addressed by the given indices.
// If `indices` is not empty, it is expected that `type` (and the corresponding
// sort of `value`) is an aggregate (tuple or array). For each index in
// `indices`, this will check if the value is an array or tuple and perform the
// appropriate Z3 operations to resolve the value at that index. Returns an
// error if the value is not an array/or tuple and an index remains to resolve.
// Also returns an error if the Z3 sort and XLS type being indexed are not
// compatible (i.e. the type is a tuple but Z3 sort is a bitvector).
absl::StatusOr<Z3_ast> GetValueAtIndices(Type* type, Z3_context ctx,
                                         Z3_ast value,
                                         absl::Span<int64_t const> indices) {
  // Chase indices one at a time.
  while (!indices.empty()) {
    Z3_sort value_sort = Z3_get_sort(ctx, value);
    Z3_sort_kind value_kind = Z3_get_sort_kind(ctx, value_sort);
    switch (value_kind) {
      case Z3_ARRAY_SORT: {
        XLS_ASSIGN_OR_RETURN(ArrayType * array_type, type->AsArray());
        // Need to take care to get the right sort/width for Z3 array indexing.
        Z3_sort index_sort =
            Z3_mk_bv_sort(ctx, static_cast<uint32_t>(Bits::MinBitCountUnsigned(
                                   static_cast<uint64_t>(array_type->size()))));
        Z3_ast index_z3 = Z3_mk_int64(ctx, indices.front(), index_sort);
        index_z3 = GetAsFormattedArrayIndex(ctx, index_z3, array_type);
        value = Z3_mk_select(ctx, value, index_z3);
        type = array_type->element_type();
        break;
      }
      case Z3_DATATYPE_SORT: {
        XLS_ASSIGN_OR_RETURN(TupleType * tuple_type, type->AsTuple());
        Z3_func_decl proj_fn = Z3_get_tuple_sort_field_decl(
            ctx, value_sort, static_cast<unsigned int>(indices.front()));
        value = Z3_mk_app(ctx, proj_fn, 1, &value);
        type = tuple_type->element_type(indices.front());
        break;
      }
      default:
        return absl::InvalidArgumentError(
            absl::StrFormat("Z3 sort %s cannot be indexed",
                            Z3_sort_to_string(ctx, value_sort)));
    }
    indices.remove_prefix(1);
  }
  return value;
}

// Helper for computing Ne on potentially-aggregate-typed operands.
// Shared by both Eq and Ne handlers.
absl::StatusOr<Z3_ast> ComputeNe(Z3_context ctx, Z3_ast lhs, Z3_ast rhs,
                                 Type* operand_type, Z3OpTranslator& t) {
  XLS_ASSIGN_OR_RETURN(
      LeafTypeTree<Z3_ast> ltt,
      LeafTypeTree<Z3_ast>::CreateFromFunction(
          operand_type,
          [&](Type* leaf_type,
              absl::Span<int64_t const> indices) -> absl::StatusOr<Z3_ast> {
            XLS_ASSIGN_OR_RETURN(
                Z3_ast lhs_at_indices,
                GetValueAtIndices(operand_type, ctx, lhs, indices));
            XLS_ASSIGN_OR_RETURN(
                Z3_ast rhs_at_indices,
                GetValueAtIndices(operand_type, ctx, rhs, indices));
            return t.Xor(lhs_at_indices, rhs_at_indices);
          }));
  XLS_RET_CHECK(!ltt.elements().empty());
  Z3_ast concat = ltt.elements().front();
  for (Z3_ast element : ltt.elements().subspan(1)) {
    concat = Z3_mk_concat(ctx, concat, element);
  }
  return t.ReduceOr(concat);
}
}  // namespace

absl::Status IrTranslator::HandleEq(CompareOp* eq) {
  Z3OpTranslator t(ctx_);
  ScopedErrorHandler seh(ctx_);
  XLS_ASSIGN_OR_RETURN(Z3_ast result, ComputeNe(ctx_, GetValue(eq->operand(0)),
                                                GetValue(eq->operand(1)),
                                                eq->operand(0)->GetType(), t));
  result = t.Not(result);
  NoteTranslation(eq, result);
  return seh.status();
}

absl::Status IrTranslator::HandleNe(CompareOp* ne) {
  Z3OpTranslator t(ctx_);
  ScopedErrorHandler seh(ctx_);
  XLS_ASSIGN_OR_RETURN(Z3_ast result, ComputeNe(ctx_, GetValue(ne->operand(0)),
                                                GetValue(ne->operand(1)),
                                                ne->operand(0)->GetType(), t));
  NoteTranslation(ne, result);
  return seh.status();
}

template <typename FnT>
absl::Status IrTranslator::HandleShift(BinOp* shift, FnT fshift) {
  auto f = [shift, fshift](Z3_context ctx, Z3_ast lhs, Z3_ast rhs) {
    int64_t lhs_bit_count = shift->operand(0)->BitCountOrDie();
    int64_t rhs_bit_count = shift->operand(1)->BitCountOrDie();
    auto extend = (fshift == Z3_mk_bvashr) ? Z3_mk_sign_ext : Z3_mk_zero_ext;
    if (rhs_bit_count < lhs_bit_count) {
      rhs = Z3_mk_zero_ext(ctx, lhs_bit_count - rhs_bit_count, rhs);
    } else if (rhs_bit_count > lhs_bit_count) {
      lhs = extend(ctx, rhs_bit_count - lhs_bit_count, lhs);
    }
    Z3_ast shifted = fshift(ctx, lhs, rhs);
    return Z3_mk_extract(ctx, lhs_bit_count - 1, 0, shifted);
  };
  return HandleBinary(shift, f);
}

absl::Status IrTranslator::HandleShra(BinOp* shra) {
  return HandleShift(shra, Z3_mk_bvashr);
}

absl::Status IrTranslator::HandleShrl(BinOp* shrl) {
  return HandleShift(shrl, Z3_mk_bvlshr);
}

absl::Status IrTranslator::HandleShll(BinOp* shll) {
  return HandleShift(shll, Z3_mk_bvshl);
}

template <typename OpT, typename FnT>
absl::Status IrTranslator::HandleNary(OpT* op, FnT f, bool invert_result) {
  ScopedErrorHandler seh(ctx_);
  int64_t operands = op->operands().size();
  XLS_RET_CHECK_GT(operands, 0) << op->ToString();
  Z3_ast accum = GetBitVec(op->operand(0));
  for (int64_t i = 1; i < operands; ++i) {
    accum = f(ctx_, accum, GetBitVec(op->operand(i)));
  }
  if (invert_result) {
    accum = Z3OpTranslator(ctx_).Not(accum);
  }
  NoteTranslation(op, accum);
  return seh.status();
}

absl::Status IrTranslator::HandleNaryAnd(NaryOp* and_op) {
  return HandleNary(and_op, Z3_mk_bvand, /*invert_result=*/false);
}

absl::Status IrTranslator::HandleNaryNand(NaryOp* nand_op) {
  return HandleNary(nand_op, Z3_mk_bvand, /*invert_result=*/true);
}

absl::Status IrTranslator::HandleNaryNor(NaryOp* nor_op) {
  return HandleNary(nor_op, Z3_mk_bvor, /*invert_result=*/true);
}

absl::Status IrTranslator::HandleNaryOr(NaryOp* or_op) {
  return HandleNary(or_op, Z3_mk_bvor, /*invert_result=*/false);
}

absl::Status IrTranslator::HandleNaryXor(NaryOp* op) {
  return HandleNary(op, Z3_mk_bvxor, /*invert_result=*/false);
}

absl::Status IrTranslator::HandleConcat(Concat* concat) {
  return HandleNary(concat, Z3_mk_concat, /*invert_result=*/false);
}

Z3_ast IrTranslator::CreateTuple(Z3_sort tuple_sort,
                                 absl::Span<const Z3_ast> elements) {
  Z3_func_decl mk_tuple_decl = Z3_get_tuple_sort_mk_decl(ctx_, tuple_sort);
  return Z3_mk_app(ctx_, mk_tuple_decl, elements.size(), elements.data());
}

Z3_ast IrTranslator::CreateTuple(Type* tuple_type,
                                 absl::Span<const Z3_ast> elements) {
  Z3_sort tuple_sort = TypeToSort(ctx_, *tuple_type);
  Z3_func_decl mk_tuple_decl = Z3_get_tuple_sort_mk_decl(ctx_, tuple_sort);
  return Z3_mk_app(ctx_, mk_tuple_decl, elements.size(), elements.data());
}

absl::StatusOr<Z3_ast> IrTranslator::CreateZ3Param(
    Type* type, std::string_view param_name) {
  return Z3_mk_const(ctx_,
                     Z3_mk_string_symbol(ctx_, std::string(param_name).c_str()),
                     TypeToSort(ctx_, *type));
}

absl::Status IrTranslator::HandleParam(Param* param) {
  ScopedErrorHandler seh(ctx_);
  Type* type = param->GetType();

  // If in "Use existing" mode, then all params must have been encountered
  // already - just copy them over.
  Z3_ast value;
  if (imported_params_) {
    // Find the index of this param in the function, and pull that one out of
    // the imported set.
    XLS_ASSIGN_OR_RETURN(int64_t param_index,
                         param->function_base()->GetParamIndex(param));
    value = imported_params_.value().at(param_index);
  } else {
    XLS_ASSIGN_OR_RETURN(value, CreateZ3Param(type, param->name()));
  }
  NoteTranslation(param, value);
  return seh.status();
}

Z3_ast IrTranslator::ZeroOfSort(Z3_sort sort) {
  // We represent tuples as bit vectors.
  Z3_sort_kind sort_kind = Z3_get_sort_kind(ctx_, sort);
  switch (sort_kind) {
    case Z3_BV_SORT:
      return Z3_mk_int(ctx_, 0, sort);
    case Z3_ARRAY_SORT: {
      // it's an array, so we need to create an array of zero-valued elements.
      Z3_sort index_sort = Z3_get_array_sort_domain(ctx_, sort);
      Z3_ast element = ZeroOfSort(Z3_get_array_sort_range(ctx_, sort));
      return Z3_mk_const_array(ctx_, index_sort, element);
    }
    case Z3_DATATYPE_SORT: {
      int num_elements = Z3_get_tuple_sort_num_fields(ctx_, sort);
      std::vector<Z3_ast> elements;
      elements.reserve(num_elements);
      for (int i = 0; i < num_elements; i++) {
        elements.push_back(ZeroOfSort(
            Z3_get_range(ctx_, Z3_get_tuple_sort_field_decl(ctx_, sort, i))));
      }
      return CreateTuple(sort, elements);
    }
    default:
      LOG(FATAL) << "Unknown/unsupported sort kind: "
                 << static_cast<int>(sort_kind);
  }
}

Z3_symbol IrTranslator::GetNewSymbol() {
  return Z3_mk_int_symbol(ctx_, current_symbol_++);
}

Z3_ast IrTranslator::CreateArray(ArrayType* type,
                                 absl::Span<const Z3_ast> elements) {
  Z3_sort element_sort = TypeToSort(ctx_, *type->element_type());

  // Zero-element arrays are A Thing, so we need to synthesize a Z3 zero value
  // for all our array element types.
  Z3_ast default_value = ZeroOfSort(element_sort);
  Z3_sort index_sort =
      Z3_mk_bv_sort(ctx_, Bits::MinBitCountUnsigned(type->size()));
  Z3_ast z3_array = Z3_mk_const_array(ctx_, index_sort, default_value);
  Z3OpTranslator op_translator(ctx_);
  for (int i = 0; i < type->size(); i++) {
    Z3_ast index = Z3_mk_int64(ctx_, i, index_sort);
    z3_array = Z3_mk_store(ctx_, z3_array, index, elements[i]);
  }

  return z3_array;
}

absl::Status IrTranslator::HandleAfterAll(AfterAll* after_all) {
  ScopedErrorHandler seh(ctx_);
  // Token types don't contain any data. A 0-field tuple is a convenient
  // way to let (most of) the rest of the z3 infrastructure treat a
  // token like a normal data-type.
  NoteTranslation(after_all,
                  CreateTuple(TypeToSort(ctx_, *after_all->GetType()),
                              /*elements=*/{}));
  return seh.status();
}

absl::Status IrTranslator::HandleMinDelay(MinDelay* min_delay) {
  ScopedErrorHandler seh(ctx_);
  // Token types don't contain any data. A 0-field tuple is a convenient
  // way to let (most of) the rest of the z3 infrastructure treat a
  // token like a normal data-type.
  NoteTranslation(min_delay,
                  CreateTuple(TypeToSort(ctx_, *min_delay->GetType()),
                              /*elements=*/{}));
  return seh.status();
}

absl::Status IrTranslator::HandleArray(Array* array) {
  ScopedErrorHandler seh(ctx_);
  std::vector<Z3_ast> elements;
  elements.reserve(array->size());
  for (int i = 0; i < array->size(); i++) {
    elements.push_back(GetValue(array->operand(i)));
  }

  NoteTranslation(array,
                  CreateArray(array->GetType()->AsArrayOrDie(), elements));
  return seh.status();
}

absl::Status IrTranslator::HandleTuple(Tuple* tuple) {
  std::vector<Z3_ast> elements;
  elements.reserve(tuple->operand_count());
  for (int i = 0; i < tuple->operand_count(); i++) {
    elements.push_back(GetValue(tuple->operand(i)));
  }
  NoteTranslation(tuple, CreateTuple(tuple->GetType(), elements));

  return absl::OkStatus();
}

Z3_ast IrTranslator::GetArrayElement(ArrayType* array_type, Z3_ast array,
                                     Z3_ast index) {
  index = GetAsFormattedArrayIndex(ctx_, index, array_type);
  // To follow XLS semantics, if the index exceeds the array size, then return
  // the element at the max index.
  Z3OpTranslator t(ctx_);
  Z3_ast array_max_index =
      Z3_mk_int64(ctx_, array_type->size() - 1, Z3_get_sort(ctx_, index));
  index = t.Min(index, array_max_index);
  return Z3_mk_select(ctx_, array, index);
}

absl::Status IrTranslator::HandleArrayIndex(ArrayIndex* array_index) {
  ScopedErrorHandler seh(ctx_);
  Type* array_type = array_index->array()->GetType();
  Z3_ast element = GetValue(array_index->array());
  for (Node* index : array_index->indices()) {
    element =
        GetArrayElement(array_type->AsArrayOrDie(), element, GetValue(index));
    array_type = array_type->AsArrayOrDie()->element_type();
  }
  NoteTranslation(array_index, element);
  return seh.status();
}

Z3_ast IrTranslator::UpdateArrayElement(Type* type, Z3_ast array, Z3_ast value,
                                        Z3_ast cond,
                                        absl::Span<const Z3_ast> indices) {
  if (indices.empty()) {
    return Z3_mk_ite(ctx_, cond, value, array);
  }
  ArrayType* array_type = type->AsArrayOrDie();
  Z3_sort index_sort =
      Z3_mk_bv_sort(ctx_, Bits::MinBitCountUnsigned(array_type->size()));
  std::vector<Z3_ast> elements;
  for (int64_t i = 0; i < array_type->size(); ++i) {
    Z3_ast this_index = GetAsFormattedArrayIndex(
        ctx_, Z3_mk_int64(ctx_, i, index_sort), array_type);
    Z3_ast updated_index =
        GetAsFormattedArrayIndex(ctx_, indices.front(), array_type);
    // In the recursive call, the condition is updated by whether the current
    // index matches.
    Z3_ast and_args[] = {cond, Z3_mk_eq(ctx_, this_index, updated_index)};
    Z3_ast new_cond = Z3_mk_and(ctx_, 2, and_args);
    elements.push_back(UpdateArrayElement(
        /*type=*/array_type->element_type(),
        /*array=*/Z3_mk_select(ctx_, array, this_index),
        /*value=*/value, /*cond=*/new_cond, indices.subspan(1)));
  }
  return CreateArray(array_type, elements);
}

absl::Status IrTranslator::HandleArrayUpdate(ArrayUpdate* array_update) {
  ScopedErrorHandler seh(ctx_);

  std::vector<Z3_ast> indices;
  for (Node* index : array_update->indices()) {
    indices.push_back(GetValue(index));
  }
  Z3_ast new_array = UpdateArrayElement(
      /*type=*/array_update->GetType(),
      /*array=*/GetValue(array_update->array_to_update()),
      /*value=*/GetValue(array_update->update_value()),
      /*cond=*/Z3_mk_true(ctx_),
      /*indices=*/indices);
  NoteTranslation(array_update, new_array);
  return seh.status();
}

absl::Status IrTranslator::HandleArrayConcat(ArrayConcat* array_concat) {
  ScopedErrorHandler seh(ctx_);

  std::vector<Z3_ast> elements;
  for (Node* operand : array_concat->operands()) {
    // Get number of elements in this operand (which is an array)
    ArrayType* array_type = operand->GetType()->AsArrayOrDie();
    int64_t element_count = array_type->size();

    Z3_sort index_sort =
        Z3_mk_bv_sort(ctx_, Bits::MinBitCountUnsigned(element_count));

    Z3_ast array = GetValue(operand);

    for (int64_t i = 0; i < element_count; ++i) {
      Z3_ast index = Z3_mk_int64(ctx_, i, index_sort);
      Z3_ast element = Z3_mk_select(ctx_, array, index);
      elements.push_back(element);
    }
  }

  NoteTranslation(
      array_concat,
      CreateArray(array_concat->GetType()->AsArrayOrDie(), elements));
  return seh.status();
}

absl::Status IrTranslator::HandleArraySlice(ArraySlice* array_slice) {
  ScopedErrorHandler seh(ctx_);
  Z3_ast array_ast = GetValue(array_slice->array());
  Z3_ast start_ast = GetValue(array_slice->start());
  ArrayType* input_type = array_slice->array()->GetType()->AsArrayOrDie();
  ArrayType result_type(array_slice->width(), input_type->element_type());
  Z3_ast formatted_start_ast =
      GetAsFormattedArrayIndex(ctx_, start_ast, input_type);

  std::vector<Z3_ast> elements;
  for (uint64_t i = 0; i < array_slice->width(); ++i) {
    Z3_ast i_ast = Z3_mk_int64(ctx_, i, Z3_get_sort(ctx_, formatted_start_ast));
    Z3_ast index_ast = Z3_mk_bvadd(ctx_, i_ast, formatted_start_ast);
    elements.push_back(GetArrayElement(input_type, array_ast, index_ast));
  }

  NoteTranslation(array_slice, CreateArray(&result_type, elements));
  return seh.status();
}

absl::Status IrTranslator::HandleTupleIndex(TupleIndex* tuple_index) {
  ScopedErrorHandler seh(ctx_);
  Z3_ast tuple = GetValue(tuple_index->operand(0));
  Z3_sort tuple_sort = Z3_get_sort(ctx_, tuple);
  Z3_func_decl proj_fn =
      Z3_get_tuple_sort_field_decl(ctx_, tuple_sort, tuple_index->index());
  Z3_ast result = Z3_mk_app(ctx_, proj_fn, 1, &tuple);

  NoteTranslation(tuple_index, result);
  return seh.status();
}

// Handles the translation of unary node "op" by using the abstract node
// evaluator.
absl::Status IrTranslator::HandleUnaryViaAbstractEval(Node* op) {
  CHECK_EQ(op->operand_count(), 1);
  ScopedErrorHandler seh(ctx_);
  Z3AbstractEvaluator evaluator(ctx_);

  Z3_ast operand = GetBitVec(op->operand(0));
  Z3OpTranslator t(ctx_);
  CHECK_EQ(op->operand(0)->BitCountOrDie(), t.GetBvBitCount(operand));
  std::vector<Z3_ast> input_bits = t.ExplodeBits(operand);

  XLS_ASSIGN_OR_RETURN(
      std::vector<Z3_ast> output_bits,
      AbstractEvaluate(op, std::vector<Z3AbstractEvaluator::Vector>{input_bits},
                       &evaluator, nullptr));
  // The "output_bits" we are given have LSb in index 0, but ConcatN puts
  // argument 0 in the MSb position, so we must reverse.
  std::reverse(output_bits.begin(), output_bits.end());
  Z3_ast result = t.ConcatN(output_bits);
  CHECK_EQ(op->BitCountOrDie(), t.GetBvBitCount(result));
  NoteTranslation(op, result);
  return seh.status();
}

// Translates a unary `UnOp` or `BitwiseReductionOp` operator to a Z3 AST format
// by invoking `f`, the Z3 AST-node generator corresponding to the desired op.
template <typename FnT>
absl::Status IrTranslator::HandleUnary(Node* op, FnT f) {
  CHECK_EQ(op->operand_count(), 1);
  ScopedErrorHandler seh(ctx_);
  Z3_ast result = f(ctx_, GetBitVec(op->operand(0)));
  NoteTranslation(op, result);
  return seh.status();
}

absl::Status IrTranslator::HandleDecode(Decode* decode) {
  return HandleUnaryViaAbstractEval(decode);
}

absl::Status IrTranslator::HandleEncode(Encode* encode) {
  return HandleUnaryViaAbstractEval(encode);
}

absl::Status IrTranslator::HandleOneHot(OneHot* one_hot) {
  return HandleUnaryViaAbstractEval(one_hot);
}

absl::Status IrTranslator::HandleNeg(UnOp* neg) {
  return HandleUnary(neg, Z3_mk_bvneg);
}

absl::Status IrTranslator::HandleNext(Next* next) {
  // We don't model inductive propagation of next values - so just handle the
  // empty-tuple placeholder value for this node.
  NoteTranslation(next, CreateTuple(next->GetType(), {}));
  return absl::OkStatus();
}

absl::Status IrTranslator::HandleNot(UnOp* not_op) {
  return HandleUnary(not_op, Z3_mk_bvnot);
}

absl::Status IrTranslator::HandleReverse(UnOp* reverse) {
  return HandleUnaryViaAbstractEval(reverse);
}

absl::Status IrTranslator::HandleIdentity(UnOp* identity) {
  NoteTranslation(identity, GetValue(identity->operand(0)));
  return absl::OkStatus();
}

absl::Status IrTranslator::HandleSignExtend(ExtendOp* sign_ext) {
  ScopedErrorHandler seh(ctx_);
  int64_t input_bit_count = sign_ext->operand(0)->BitCountOrDie();
  Z3_ast result =
      Z3_mk_sign_ext(ctx_, sign_ext->new_bit_count() - input_bit_count,
                     GetBitVec(sign_ext->operand(0)));
  NoteTranslation(sign_ext, result);
  return seh.status();
}

absl::Status IrTranslator::HandleZeroExtend(ExtendOp* zero_ext) {
  ScopedErrorHandler seh(ctx_);
  int64_t input_bit_count = zero_ext->operand(0)->BitCountOrDie();
  Z3_ast result =
      Z3_mk_zero_ext(ctx_, zero_ext->new_bit_count() - input_bit_count,
                     GetBitVec(zero_ext->operand(0)));
  NoteTranslation(zero_ext, result);
  return seh.status();
}

absl::Status IrTranslator::HandleBitSlice(BitSlice* bit_slice) {
  ScopedErrorHandler seh(ctx_);
  int64_t low = bit_slice->start();
  int64_t high = low + bit_slice->width() - 1;
  Z3_ast result =
      Z3_mk_extract(ctx_, high, low, GetBitVec(bit_slice->operand(0)));
  NoteTranslation(bit_slice, result);
  return seh.status();
}

absl::Status IrTranslator::HandleBitSliceUpdate(BitSliceUpdate* update) {
  if (update->start()->GetType()->GetFlatBitCount() > 130) {
    VLOG(3) << "Losing some precision in Z3 analysis because of wide bit "
            << "slice update start index";
    return IrTranslator::DefaultHandler(update);
  }
  if (update->to_update()->GetType()->GetFlatBitCount() > 1000) {
    VLOG(3) << "Losing some precision in Z3 analysis because of wide bit "
            << "slice update to_update value ("
            << update->to_update()->GetType()->GetFlatBitCount() << " bits)";
    return IrTranslator::DefaultHandler(update);
  }
  ScopedErrorHandler seh(ctx_);
  Z3AbstractEvaluator evaluator(ctx_);
  std::vector<Z3_ast> to_update =
      Z3OpTranslator(ctx_).ExplodeBits(GetBitVec(update->to_update()));
  std::vector<Z3_ast> start =
      Z3OpTranslator(ctx_).ExplodeBits(GetBitVec(update->start()));
  std::vector<Z3_ast> update_value =
      Z3OpTranslator(ctx_).ExplodeBits(GetBitVec(update->update_value()));

  std::vector<Z3_ast> flat_results =
      evaluator.BitSliceUpdate(to_update, start, update_value);

  std::reverse(flat_results.begin(), flat_results.end());
  Z3_ast result = UnflattenZ3Ast(update->GetType(), flat_results);

  NoteTranslation(update, result);
  return seh.status();
}

absl::Status IrTranslator::HandleDynamicBitSlice(
    DynamicBitSlice* dynamic_bit_slice) {
  ScopedErrorHandler seh(ctx_);
  Z3_ast value = GetBitVec(dynamic_bit_slice->operand(0));
  Z3_ast start = GetBitVec(dynamic_bit_slice->operand(1));
  int64_t value_width = dynamic_bit_slice->operand(0)->BitCountOrDie();
  int64_t start_width = dynamic_bit_slice->operand(1)->BitCountOrDie();

  int64_t max_width = std::max(value_width, start_width);
  Z3_ast value_ext = Z3_mk_zero_ext(ctx_, max_width - value_width, value);
  Z3_ast start_ext = Z3_mk_zero_ext(ctx_, max_width - start_width, start);

  Value operand_width(UBits(value_width, max_width));
  BitsType max_width_type(max_width);
  XLS_ASSIGN_OR_RETURN(
      Z3_ast bit_width,
      TranslateLiteralValue(false, &max_width_type, operand_width));

  // Indicates whether slice is completely out of bounds.
  Z3_ast out_of_bounds = Z3_mk_bvuge(ctx_, start_ext, bit_width);
  BitsType return_type(dynamic_bit_slice->width());
  XLS_ASSIGN_OR_RETURN(
      Z3_ast zeros,
      TranslateLiteralValue(false, &return_type,
                            Value(Bits(dynamic_bit_slice->width()))));
  Z3_ast shifted_value = Z3_mk_bvlshr(ctx_, value_ext, start_ext);
  Z3_ast truncated_value =
      Z3_mk_extract(ctx_, dynamic_bit_slice->width() - 1, 0, shifted_value);
  Z3_ast result = Z3_mk_ite(ctx_, out_of_bounds, zeros, truncated_value);
  NoteTranslation(dynamic_bit_slice, result);
  return seh.status();
}

absl::StatusOr<Z3_ast> IrTranslator::TranslateLiteralBits(const Bits& bits) {
  std::unique_ptr<bool[]> booleans(new bool[bits.bit_count()]);
  for (int64_t i = 0; i < bits.bit_count(); ++i) {
    booleans[i] = bits.Get(i);
  }
  return Z3_mk_bv_numeral(ctx_, static_cast<unsigned int>(bits.bit_count()),
                          &booleans[0]);
}

absl::StatusOr<Z3_ast> IrTranslator::TranslateLiteralValue(bool has_uses,
                                                           Type* value_type,
                                                           const Value& value) {
  bool is_zero_bit_vector = value.IsBits() && value.GetFlatBitCount() == 0;

  if (value.IsBits() && !is_zero_bit_vector) {
    return TranslateLiteralBits(value.bits());
  }

  // We translate zero length bitvectors to empty tuples.
  // This will cause errors if the bitvectors are used in any nontrivial way,
  // but fixes fuzzer errors in the mutual_exclusion_pass.
  if (is_zero_bit_vector) {
    if (has_uses) {
      return absl::UnimplementedError(
          "Zero length bitvectors must not have "
          "uses in the IR graph when translating "
          "to Z3");
    }
    TupleType tuple_type({});
    return CreateTuple(&tuple_type, {});
  }

  // Tokens are essentially opaque empty tuples.
  if (value.IsToken()) {
    TupleType tuple_type({});
    return CreateTuple(&tuple_type, {});
  }

  if (value.IsArray()) {
    ArrayType* array_type = value_type->AsArrayOrDie();
    int64_t num_elements = array_type->size();
    std::vector<Z3_ast> elements;
    elements.reserve(num_elements);

    for (int i = 0; i < value.elements().size(); i++) {
      XLS_ASSIGN_OR_RETURN(
          Z3_ast translated,
          TranslateLiteralValue(has_uses, array_type->element_type(),
                                value.elements()[i]));
      elements.push_back(translated);
    }

    return CreateArray(array_type, elements);
  }

  // Tuples!
  TupleType* tuple_type = value_type->AsTupleOrDie();
  int num_elements = tuple_type->size();
  std::vector<Z3_ast> elements;
  elements.reserve(num_elements);
  for (int i = 0; i < num_elements; i++) {
    XLS_ASSIGN_OR_RETURN(
        Z3_ast translated,
        TranslateLiteralValue(has_uses, tuple_type->element_type(i),
                              value.elements()[i]));
    elements.push_back(translated);
  }

  return CreateTuple(tuple_type, elements);
}

absl::Status IrTranslator::HandleLiteral(Literal* literal) {
  ScopedErrorHandler seh(ctx_);
  XLS_ASSIGN_OR_RETURN(
      Z3_ast result,
      TranslateLiteralValue(!literal->users().empty(), literal->GetType(),
                            literal->value()));
  NoteTranslation(literal, result);
  return seh.status();
}

std::vector<Z3_ast> IrTranslator::FlattenValue(Type* type, Z3_ast value,
                                               bool little_endian) {
  Z3OpTranslator op_translator(ctx_);

  switch (type->kind()) {
    case TypeKind::kBits: {
      std::vector<Z3_ast> boom = op_translator.ExplodeBits(value);
      if (little_endian) {
        std::reverse(boom.begin(), boom.end());
      }
      return boom;
    }
    case TypeKind::kArray: {
      ArrayType* array_type = type->AsArrayOrDie();
      std::vector<Z3_ast> flattened;
      Z3_sort index_sort =
          Z3_mk_bv_sort(ctx_, Bits::MinBitCountUnsigned(array_type->size()));
      for (int i = 0; i < array_type->size(); i++) {
        Z3_ast index = Z3_mk_int64(ctx_, i, index_sort);
        Z3_ast element = GetArrayElement(array_type, value, index);
        std::vector<Z3_ast> flat_child =
            FlattenValue(array_type->element_type(), element, little_endian);
        flattened.insert(flattened.end(), flat_child.begin(), flat_child.end());
      }
      return flattened;
    }
    case TypeKind::kTuple: {
      TupleType* tuple_type = type->AsTupleOrDie();
      Z3_sort tuple_sort = Z3_get_sort(ctx_, value);

      std::vector<Z3_ast> flattened;
      for (int i = 0; i < tuple_type->size(); i++) {
        Z3_func_decl child_accessor =
            Z3_get_tuple_sort_field_decl(ctx_, tuple_sort, i);
        Z3_ast child = Z3_mk_app(ctx_, child_accessor, 1, &value);
        std::vector<Z3_ast> flat_child =
            FlattenValue(tuple_type->element_type(i), child, little_endian);
        flattened.insert(flattened.end(), flat_child.begin(), flat_child.end());
      }
      return flattened;
    }
    default:
      LOG(FATAL) << "Unsupported type kind: " << TypeKindToString(type->kind());
  }
}

Z3_ast IrTranslator::UnflattenZ3Ast(Type* type, absl::Span<const Z3_ast> flat,
                                    bool little_endian) {
  Z3OpTranslator op_translator(ctx_);
  switch (type->kind()) {
    case TypeKind::kBits:
      if (little_endian) {
        std::vector<Z3_ast> flat_vec(flat.begin(), flat.end());
        std::reverse(flat_vec.begin(), flat_vec.end());
        return op_translator.ConcatN(flat_vec);
      } else {
        return op_translator.ConcatN(flat);
      }
    case TypeKind::kArray: {
      ArrayType* array_type = type->AsArrayOrDie();
      int num_elements = array_type->size();

      Type* element_type = array_type->element_type();
      int element_bits = element_type->GetFlatBitCount();
      std::vector<Z3_ast> elements;
      elements.reserve(num_elements);

      int high = array_type->GetFlatBitCount();
      for (int i = 0; i < num_elements; i++) {
        absl::Span<const Z3_ast> subspan =
            flat.subspan(high - element_bits, element_bits);
        elements.push_back(
            UnflattenZ3Ast(element_type, subspan, little_endian));
        high -= element_bits;
      }
      return CreateArray(array_type, elements);
    }
    case TypeKind::kTuple: {
      // For each tuple element, extract the sub-type's bits and unflatten, then
      // munge into a tuple.
      TupleType* tuple_type = type->AsTupleOrDie();
      std::vector<Z3_ast> elements;
      int high = tuple_type->GetFlatBitCount();
      for (Type* element_type : tuple_type->element_types()) {
        int64_t element_bits = element_type->GetFlatBitCount();
        absl::Span<const Z3_ast> subspan =
            flat.subspan(high - element_bits, element_bits);
        elements.push_back(
            UnflattenZ3Ast(element_type, subspan, little_endian));
        high -= element_bits;
      }
      return CreateTuple(tuple_type, elements);
    }
    default:
      LOG(FATAL) << "Unsupported type kind: " << TypeKindToString(type->kind());
  }
}

template <typename NodeT>
absl::Status IrTranslator::HandleSelect(
    NodeT* node, std::function<FlatValue(const FlatValue& selector,
                                         const std::vector<FlatValue>& cases)>
                     evaluator) {
  // HandleSel could be implemented on its own terms (and not in the same way
  // as one-hot), if there's concern that flattening to bitwise Z3_asts loses
  // any semantic info.
  ScopedErrorHandler seh(ctx_);
  Z3OpTranslator op_translator(ctx_);
  std::vector<Z3_ast> selector =
      Z3OpTranslator(ctx_).ExplodeBits(GetBitVec(node->selector()));

  std::vector<std::vector<Z3_ast>> case_elements;
  for (Node* element : node->cases()) {
    case_elements.push_back(
        FlattenValue(element->GetType(), GetValue(element)));
  }

  std::vector<Z3_ast> flat_results = evaluator(selector, case_elements);
  std::reverse(flat_results.begin(), flat_results.end());
  Z3_ast result = UnflattenZ3Ast(node->GetType(), flat_results);

  NoteTranslation(node, result);
  return seh.status();
}

absl::Status IrTranslator::HandleOneHotSel(OneHotSelect* one_hot) {
  Z3AbstractEvaluator evaluator(ctx_);
  return HandleSelect(
      one_hot, [&evaluator](const std::vector<Z3_ast>& selector,
                            const std::vector<std::vector<Z3_ast>>& cases) {
        return evaluator.OneHotSelect(
            selector, evaluator.SpanOfVectorsToVectorOfSpans(cases),
            /*selector_can_be_zero=*/true);
      });
}

absl::Status IrTranslator::HandlePrioritySel(PrioritySelect* sel) {
  Z3AbstractEvaluator evaluator(ctx_);
  return HandleSelect(
      sel, [&evaluator](const std::vector<Z3_ast>& selector,
                        const std::vector<std::vector<Z3_ast>>& cases) {
        return evaluator.PrioritySelect(
            selector, evaluator.SpanOfVectorsToVectorOfSpans(cases),
            /*selector_can_be_zero=*/true);
      });
}

absl::Status IrTranslator::HandleSel(Select* sel) {
  Z3AbstractEvaluator evaluator(ctx_);
  Z3OpTranslator op_translator(ctx_);
  return HandleSelect(sel, [this, sel, &evaluator](
                               const std::vector<Z3_ast>& selector,
                               const std::vector<std::vector<Z3_ast>>& cases) {
    // Calculate the Z3-ified default value, if any.
    std::optional<std::vector<Z3_ast>> default_value = std::nullopt;
    if (sel->default_value()) {
      default_value = FlattenValue(sel->default_value().value()->GetType(),
                                   GetValue(sel->default_value().value()));
    }
    return evaluator.Select(
        selector, evaluator.SpanOfVectorsToVectorOfSpans(cases), default_value);
  });
}

absl::Status IrTranslator::HandleAndReduce(BitwiseReductionOp* and_reduce) {
  return HandleUnary(and_reduce, Z3_mk_bvredand);
}

absl::Status IrTranslator::HandleOrReduce(BitwiseReductionOp* or_reduce) {
  return HandleUnary(or_reduce, Z3_mk_bvredor);
}

absl::Status IrTranslator::HandleXorReduce(BitwiseReductionOp* xor_reduce) {
  return HandleUnaryViaAbstractEval(xor_reduce);
}

static Z3_ast DoMul(Z3_context ctx, Z3_ast lhs, Z3_ast rhs, bool is_signed,
                    int result_size) {
  Z3_ast result;
  // Do the mul at maximum width, then truncate if necessary to the result
  // width.
  if (is_signed) {
    int lhs_size = Z3_get_bv_sort_size(ctx, Z3_get_sort(ctx, lhs));
    int rhs_size = Z3_get_bv_sort_size(ctx, Z3_get_sort(ctx, rhs));

    int operation_size = std::max({result_size, lhs_size, rhs_size});

    if (lhs_size < operation_size) {
      lhs = Z3_mk_sign_ext(ctx, operation_size - lhs_size, lhs);
    }
    if (rhs_size < operation_size) {
      rhs = Z3_mk_sign_ext(ctx, operation_size - rhs_size, rhs);
    }
    result = Z3_mk_bvmul(ctx, lhs, rhs);
    if (operation_size > result_size) {
      result = Z3_mk_extract(ctx, result_size - 1, 0, result);
    }
  } else {
    result = DoUnsignedMul(ctx, lhs, rhs, result_size);
  }
  return result;
}

void IrTranslator::HandleMul(ArithOp* mul, bool is_signed) {
  // In XLS IR, multiply operands can potentially be of different widths. In Z3,
  // they can't, so we need to zext (for a umul) the operands to the size of the
  // result.
  Z3_ast lhs = GetValue(mul->operand(0));
  Z3_ast rhs = GetValue(mul->operand(1));
  int result_size = mul->BitCountOrDie();

  Z3_ast result = DoMul(ctx_, lhs, rhs, is_signed, result_size);
  NoteTranslation(mul, result);
}

absl::Status IrTranslator::HandleSMul(ArithOp* mul) {
  ScopedErrorHandler seh(ctx_);
  HandleMul(mul, /*is_signed=*/true);
  return seh.status();
}

absl::Status IrTranslator::HandleUMul(ArithOp* mul) {
  ScopedErrorHandler seh(ctx_);
  HandleMul(mul, /*is_signed=*/false);
  return seh.status();
}

// Partial product ops are unusual in that the output of the operation isn't
// fully specified. The output is a 2-tuple with the property that the elements
// of the tuple sum to the product of the inputs. The outputs can take any
// values that satisfy this property. We model this in Z3 by creating two bound
// variables for each element in the output tuple and adding a constraint that
// the sum of these variables equals the desired product.
void IrTranslator::HandleMulp(PartialProductOp* mul, bool is_signed) {
  // In XLS IR, multiply operands can potentially be of different widths. In Z3,
  // they can't, so we need to zext (for a umul) the operands to the size of the
  // result.
  Z3_ast lhs = GetValue(mul->operand(0));
  Z3_ast rhs = GetValue(mul->operand(1));
  int lhs_size = Z3_get_bv_sort_size(ctx_, Z3_get_sort(ctx_, lhs));
  int rhs_size = Z3_get_bv_sort_size(ctx_, Z3_get_sort(ctx_, rhs));
  int result_size = mul->width();
  int operation_size = std::max({result_size, lhs_size, rhs_size});
  Z3_sort op_sort = Z3_mk_bv_sort(ctx_, operation_size);

  Z3_ast result = DoMul(ctx_, lhs, rhs, /*is_signed=*/is_signed, result_size);

  Z3_symbol product0_symbol = GetNewSymbol();
  Z3_ast product0 = Z3_mk_const(ctx_, product0_symbol, op_sort);
  Z3_symbol product1_symbol = GetNewSymbol();
  Z3_ast product1 = Z3_mk_const(ctx_, product1_symbol, op_sort);

  Z3_ast sum = Z3_mk_bvadd(ctx_, product0, product1);
  Z3_ast eq = Z3_mk_eq(ctx_, sum, result);
  Z3_app vars[] = {reinterpret_cast<Z3_app>(product0),
                   reinterpret_cast<Z3_app>(product1)};
  Z3_mk_forall_const(ctx_, /*weight=*/0, /*num_bound=*/2, /*bound=*/vars,
                     /*num_patterns=*/0, /*patterns=*/nullptr,
                     /*body=*/eq);
  NoteTranslation(mul, CreateTuple(mul->GetType(), {product0, product1}));
}

absl::Status IrTranslator::HandleSMulp(PartialProductOp* mul) {
  ScopedErrorHandler seh(ctx_);
  HandleMulp(mul, /*is_signed=*/true);
  return seh.status();
}

absl::Status IrTranslator::HandleUMulp(PartialProductOp* mul) {
  ScopedErrorHandler seh(ctx_);
  HandleMulp(mul, /*is_signed=*/false);
  return seh.status();
}

absl::Status IrTranslator::DefaultHandler(Node* node) {
  if (allow_unsupported_) {
    XLS_ASSIGN_OR_RETURN(Z3_ast fresh,
                         CreateZ3Param(node->GetType(), node->GetName()));
    NoteTranslation(node, fresh);
    VLOG(1) << "Unhandled node for conversion from XLS IR to Z3, "
               "defaulting to variable: "
            << node->ToString();
    return absl::OkStatus();
  }
  return absl::UnimplementedError(
      "Unhandled node for conversion from XLS IR to Z3: " + node->ToString());
}

absl::Status IrTranslator::HandleInvoke(Invoke* invoke) {
  CHECK_EQ(invoke->operands().size(), invoke->to_apply()->params().size());

  std::vector<Z3_ast> z3_params;
  for (const Node* param_node : invoke->operands()) {
    z3_params.push_back(GetValue(param_node));
  }

  XLS_ASSIGN_OR_RETURN(
      std::unique_ptr<IrTranslator> sub_translator,
      CreateAndTranslate(ctx(), invoke->to_apply(),
                         /*imported_params=*/z3_params, allow_unsupported_));

  Z3_ast z3_ret = sub_translator->GetValue(invoke->to_apply()->return_value());

  NoteTranslation(invoke, z3_ret);

  return absl::OkStatus();
}

Z3_ast IrTranslator::GetValue(const Node* node) {
  auto it = translations_.find(node);
  CHECK(it != translations_.end()) << "Node not translated: " << node;
  return it->second;
}

// Wrapper around the above that verifies we're accessing a Bits value.
Z3_ast IrTranslator::GetBitVec(Node* node) {
  Z3_ast value = GetValue(node);
  Z3_sort value_sort = Z3_get_sort(ctx_, value);
  CHECK_EQ(Z3_get_sort_kind(ctx_, value_sort), Z3_BV_SORT);
  CHECK_EQ(node->GetType()->GetFlatBitCount(),
           Z3_get_bv_sort_size(ctx_, value_sort));
  return value;
}

void IrTranslator::NoteTranslation(Node* node, Z3_ast translated) {
  if (translations_.contains(node)) {
    VLOG(2) << "Skipping translation of " << node->GetName()
            << ", as it's already been recorded "
            << "(expected if we're retranslating).";
    return;
  }
  translations_[node] = translated;
}

// Converts the predicate into a boolean objective that can be fed to the
// Z3 solver.
//
// Args:
//  p: predicate that the user is attempting to prove on the subject a_node
//  a_node: the node that is the subject of the predicate
//  a: the Z3 AST value that a_node resolves to
//  translator: The IR translator being used for the proof
//
// Implementation note: if the predicate we want to prove is "equal to zero" we
// return that "not equal to zero" is not satisfiable. That is, this routine
// inverts the condition we're attempting to prove, so that we can try to
// demonstrate an example for our attempted assertion "there exists no value
// where this (the inverse of what we're expecting to be the case, i.e.  inverse
// of our assertion) holds".
static absl::StatusOr<Z3_ast> PredicateToNegatedObjective(
    const Predicate& p, Node* a_node, Z3_ast a, IrTranslator* translator) {
  Z3_ast objective = nullptr;
  Z3OpTranslator t(translator->ctx());

  auto validate_bv_sort = [&]() -> absl::Status {
    if (translator->GetValueKind(a) != Z3_BV_SORT) {
      return absl::InvalidArgumentError(
          absl::StrFormat("Cannot evaluate predicate %s vs non-bit-vector Z3 "
                          "value for %s with Z3 sort: %s",
                          p.ToString(), a_node->ToString(), t.GetSortName(a)));
    }
    return absl::OkStatus();
  };

  VLOG(3) << "predicate: " << p.ToString()
          << " Z3_ast sort kind: " << t.GetSortName(a);
  switch (p.kind()) {
    case PredicateKind::kEqualToZero: {
      XLS_RETURN_IF_ERROR(validate_bv_sort());
      ScopedErrorHandler seh(translator->ctx());
      objective = t.NeZeroBool(a);
      XLS_RETURN_IF_ERROR(seh.status());
      break;
    }
    case PredicateKind::kNotEqualToZero: {
      XLS_RETURN_IF_ERROR(validate_bv_sort());
      ScopedErrorHandler seh(translator->ctx());
      objective = t.EqZeroBool(a);
      XLS_RETURN_IF_ERROR(seh.status());
      break;
    }
    case PredicateKind::kEqualToNode: {
      ScopedErrorHandler seh(translator->ctx());
      // Tokens always compare equal.
      if (p.node()->GetType()->IsToken() && a_node->GetType()->IsToken()) {
        XLS_RET_CHECK_EQ(t.GetSortName(a), "()");
        objective = t.False();
        break;
      }

      XLS_RETURN_IF_ERROR(validate_bv_sort());

      // Validate that the node to compare is also bit-vector valued.
      Z3_ast value = translator->GetTranslation(p.node());
      if (translator->GetValueKind(value) != Z3_BV_SORT) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "Cannot compare to non-bits-valued node: %s sort: %s",
            p.node()->ToString(), t.GetSortName(value)));
      }
      Z3_ast b = value;
      objective = t.NeBool(a, b);
      XLS_RETURN_IF_ERROR(seh.status());
      break;
    }
    case PredicateKind::kUnsignedGreaterOrEqual: {
      XLS_RETURN_IF_ERROR(validate_bv_sort());
      ScopedErrorHandler seh(translator->ctx());
      XLS_ASSIGN_OR_RETURN(Z3_ast b,
                           translator->TranslateLiteralBits(p.value()));
      objective = t.EqZeroBool(t.UGe(a, b));
      XLS_RETURN_IF_ERROR(seh.status());
      break;
    }
    case PredicateKind::kUnsignedLessOrEqual: {
      XLS_RETURN_IF_ERROR(validate_bv_sort());
      ScopedErrorHandler seh(translator->ctx());
      XLS_ASSIGN_OR_RETURN(Z3_ast b,
                           translator->TranslateLiteralBits(p.value()));
      objective = t.EqZeroBool(t.ULe(a, b));
      XLS_RETURN_IF_ERROR(seh.status());
      break;
    }
    default:
      return absl::UnimplementedError("Unhandled predicate.");
  }

  XLS_RET_CHECK(objective != nullptr) << p.ToString();
  XLS_RET_CHECK_EQ(t.GetSortKind(objective), Z3_BOOL_SORT);

  return objective;
}

namespace {

enum class PredicateCombination : std::uint8_t { kDisjunction, kConjunction };

absl::StatusOr<ProverResult> TryProveCombination(
    FunctionBase* f, std::unique_ptr<IrTranslator> translator,
    absl::Span<const PredicateOfNode> terms,
    PredicateCombination predicate_combination) {
  Z3OpTranslator t(translator->ctx());
  std::optional<Z3_ast> objective;

  for (const PredicateOfNode& term : terms) {
    Z3_ast value = translator->GetTranslation(term.subject);
    XLS_RET_CHECK(value != nullptr);

    // Translate the predicate to a term we can throw into the conjunction.
    XLS_ASSIGN_OR_RETURN(Z3_ast objective_term,
                         PredicateToNegatedObjective(term.p, term.subject,
                                                     value, translator.get()));
    XLS_RET_CHECK(objective_term != nullptr);

    if (objective.has_value()) {
      XLS_RET_CHECK(objective.value() != nullptr);
      switch (predicate_combination) {
        case PredicateCombination::kConjunction:
          objective = t.OrBool(objective.value(), objective_term);
          break;
        case PredicateCombination::kDisjunction:
          objective = t.AndBool(objective.value(), objective_term);
          break;
      }
      XLS_RET_CHECK(objective != nullptr);
    } else {
      objective = objective_term;
    }
  }

  CHECK(objective.has_value());
  CHECK(objective.value() != nullptr);

  Z3_context ctx = translator->ctx();
  VLOG(1) << "objective:\n" << Z3_ast_to_string(ctx, objective.value());
  Z3_solver solver = solvers::z3::CreateSolver(ctx, /*num_threads=*/1);
  auto cleanup = absl::Cleanup([&] { Z3_solver_dec_ref(ctx, solver); });

  Z3_solver_assert(ctx, solver, objective.value());
  Z3_lbool satisfiable = Z3_solver_check(ctx, solver);

  VLOG(1) << solvers::z3::SolverResultToString(ctx, solver, satisfiable);
  switch (satisfiable) {
    case Z3_L_FALSE:
      // Unsatisfiable; no value contradicts the claim, so the result is true.
      return ProvenTrue();
    case Z3_L_TRUE: {
      // Satisfiable; found a value that contradicts the claim.
      absl::StatusOr<absl::flat_hash_map<const Param*, Value>> counterexample =
          absl::flat_hash_map<const Param*, Value>();
      auto model = Z3_solver_get_model(ctx, solver);
      for (const Param* param : f->params()) {
        absl::StatusOr<Value> value = NodeValue(
            ctx, model, translator->GetTranslation(param), param->GetType());
        if (value.ok()) {
          counterexample->emplace(param, *std::move(value));
        } else {
          counterexample = std::move(value).status();
          break;
        }
      }
      return ProvenFalse{
          .counterexample = std::move(counterexample),
          .message =
              solvers::z3::SolverResultToString(ctx, solver, satisfiable),
      };
    }
    case Z3_L_UNDEF:
      // No result; timeout.
      return absl::DeadlineExceededError("Z3 solver timed out");
  }

  return absl::InternalError(absl::StrCat("Invalid Z3 result: ", satisfiable));
}

}  // namespace

absl::StatusOr<ProverResult> TryProveConjunction(
    FunctionBase* f, absl::Span<const PredicateOfNode> terms,
    absl::Duration timeout, bool allow_unsupported) {
  XLS_RET_CHECK(!terms.empty());
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<IrTranslator> translator,
                       IrTranslator::CreateAndTranslate(f, allow_unsupported));
  translator->SetTimeout(timeout);
  return TryProveCombination(f, std::move(translator), terms,
                             PredicateCombination::kConjunction);
}

absl::StatusOr<ProverResult> TryProveConjunction(
    FunctionBase* f, absl::Span<const PredicateOfNode> terms, int64_t rlimit,
    bool allow_unsupported) {
  XLS_RET_CHECK(!terms.empty());
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<IrTranslator> translator,
                       IrTranslator::CreateAndTranslate(f, allow_unsupported));
  translator->SetRlimit(rlimit);
  return TryProveCombination(f, std::move(translator), terms,
                             PredicateCombination::kConjunction);
}

absl::StatusOr<ProverResult> TryProveDisjunction(
    FunctionBase* f, absl::Span<const PredicateOfNode> terms,
    absl::Duration timeout, bool allow_unsupported) {
  XLS_RET_CHECK(!terms.empty());
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<IrTranslator> translator,
                       IrTranslator::CreateAndTranslate(f, allow_unsupported));
  translator->SetTimeout(timeout);
  return TryProveCombination(f, std::move(translator), terms,
                             PredicateCombination::kDisjunction);
}

absl::StatusOr<ProverResult> TryProveDisjunction(
    FunctionBase* f, absl::Span<const PredicateOfNode> terms, int64_t rlimit,
    bool allow_unsupported) {
  XLS_RET_CHECK(!terms.empty());
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<IrTranslator> translator,
                       IrTranslator::CreateAndTranslate(f, allow_unsupported));
  translator->SetRlimit(rlimit);
  return TryProveCombination(f, std::move(translator), terms,
                             PredicateCombination::kDisjunction);
}

absl::StatusOr<ProverResult> TryProve(FunctionBase* f, Node* subject,
                                      Predicate p, absl::Duration timeout,
                                      bool allow_unsupported) {
  PredicateOfNode term = {.subject = subject, .p = std::move(p)};
  return TryProveConjunction(f, absl::MakeConstSpan(&term, 1), timeout,
                             allow_unsupported);
}

absl::StatusOr<ProverResult> TryProve(FunctionBase* f, Node* subject,
                                      Predicate p, int64_t rlimit,
                                      bool allow_unsupported) {
  PredicateOfNode term = {.subject = subject, .p = std::move(p)};
  return TryProveConjunction(f, absl::MakeConstSpan(&term, 1), rlimit,
                             allow_unsupported);
}

}  // namespace z3
}  // namespace solvers
}  // namespace xls
