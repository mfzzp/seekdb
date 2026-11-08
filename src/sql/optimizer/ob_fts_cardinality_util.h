#pragma once

#include "sql/resolver/dml/ob_select_stmt.h"

namespace oceanbase
{
namespace sql
{
// Only the cardinality of this direct derived table is observable. Keep the
// proof deliberately narrow: no row-dependent outer expressions or filters,
// and no DISTINCT/grouping/window/ties/locking semantics in the limited view.
inline bool is_fts_cardinality_only_view(const ObDMLStmt *parent, const ObDMLStmt *child)
{
  if (nullptr == parent || nullptr == child || !parent->is_select_stmt() || !child->is_select_stmt()) {
    return false;
  }
  const ObSelectStmt &outer = *static_cast<const ObSelectStmt *>(parent);
  const ObSelectStmt &inner = *static_cast<const ObSelectStmt *>(child);
  if (outer.get_table_size() != 1 || outer.get_from_item_size() != 1
      || nullptr == outer.get_table_item(0) || !outer.get_table_item(0)->is_generated_table()
      || outer.get_table_item(0)->ref_query_ != child
      || outer.get_select_item_size() != 1 || outer.get_aggr_item_size() != 1
      || outer.get_condition_size() != 0 || outer.has_having() || outer.get_group_expr_size() != 0
      || outer.has_rollup() || outer.has_window_function() || outer.has_distinct()
      || outer.is_set_stmt() || outer.is_contains_assignment() || outer.has_for_update()
      || outer.get_subquery_expr_size() != 0 || outer.has_order_by()
      || inner.get_limit_expr() == nullptr || inner.is_fetch_with_ties()
      || inner.get_table_size() != 1 || inner.get_from_item_size() != 1
      || nullptr == inner.get_table_item(0) || !inner.get_table_item(0)->is_basic_table()
      || inner.has_order_by() || inner.has_group_by() || inner.has_rollup()
      || inner.has_having() || inner.get_aggr_item_size() != 0 || inner.has_distinct()
      || inner.has_window_function() || inner.is_set_stmt() || inner.has_for_update()
      || inner.is_contains_assignment() || inner.get_subquery_expr_size() != 0
      || inner.get_match_exprs().count() != 1 || nullptr == inner.get_match_exprs().at(0)
      || inner.get_match_exprs().at(0)->get_expr_type() != T_FUN_MATCH_AGAINST
      || inner.get_match_exprs().at(0)->get_mode_flag() != NATURAL_LANGUAGE_MODE) {
    return false;
  }
  const ObAggFunRawExpr *agg = outer.get_aggr_item(0);
  if (nullptr == agg || agg != outer.get_select_item(0).expr_
      || agg->get_expr_type() != T_FUN_COUNT || agg->is_param_distinct()
      || agg->get_real_param_count() != 0) {
    return false;
  }
  for (int64_t i = 0; i < inner.get_select_item_size(); ++i) {
    const ObRawExpr *expr = inner.get_select_item(i).expr_;
    if (nullptr == expr || (!expr->is_column_ref_expr() && !expr->is_const_raw_expr())) {
      return false;
    }
  }
  return true;
}
} // namespace sql
} // namespace oceanbase
