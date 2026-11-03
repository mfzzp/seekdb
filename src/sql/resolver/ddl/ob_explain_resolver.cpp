/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/ddl/ob_explain_resolver.h"
#include "sql/resolver/ob_resolver.h"
#include "sql/parser/ob_parser.h"
#include "lib/string/ob_sql_string.h"
#include "sql/table_function/table_function.h"

using namespace oceanbase;
using namespace sql;
using namespace common;
const static int64_t EXPLAIN_FORMAT = 0;
const static int64_t EXPLAIN_DISPLAY_OPTION = 1;
const static int64_t EXPLAIN_INTO_TABLE = 2;
const static int64_t EXPLAIN_STMT_ID = 3;
const static int64_t EXPLAIN_CHILD_STMT = 4;

static const char *describe_type_name(const ObRawExprResType &type)
{
  const char *name = "VARCHAR";
  switch (type.get_type()) {
    case ObTinyIntType:
      name = type.get_accuracy().get_precision() <= 1 ? "BOOLEAN" : "TINYINT";
      break;
    case ObSmallIntType: name = "SMALLINT"; break;
    case ObInt32Type:
    case ObIntType: name = "BIGINT"; break;
    case ObFloatType: name = "FLOAT"; break;
    case ObDoubleType: name = "DOUBLE"; break;
    case ObDateType: name = "DATE"; break;
    case ObDateTimeType: name = "DATETIME"; break;
    case ObTimestampType: name = "TIMESTAMP"; break;
    case ObVarcharType:
    case ObCharType:
    case ObTinyTextType:
    case ObTextType:
    case ObMediumTextType:
    case ObLongTextType: name = "VARCHAR"; break;
    default: name = "VARCHAR"; break;
  }
  return name;
}

static bool select_has_file_function(const ObSelectStmt &stmt)
{
  bool found = false;
  for (int64_t i = 0; !found && i < stmt.get_table_size(); ++i) {
    const TableItem *item = stmt.get_table_item(i);
    if (OB_NOT_NULL(item) && item->is_function_table()) {
      const uint64_t id = item->table_function_id_;
      found = id >= READ_CSV_FUNCTION_ID && id <= GLOB_FUNCTION_ID;
    }
  }
  return found;
}

static int build_describe_sql(const ObSelectStmt &stmt, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  if (0 == stmt.get_select_item_size()) {
    ret = OB_INVALID_ARGUMENT;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < stmt.get_select_item_size(); ++i) {
    const SelectItem &item = stmt.get_select_item(i);
    if (OB_ISNULL(item.expr_)) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      ObString name = item.alias_name_.empty() ? item.expr_name_ : item.alias_name_;
      const bool nullable = !item.expr_->get_result_type().has_result_flag(NOT_NULL_FLAG);
      if (i > 0 && OB_FAIL(sql.append(" union all "))) {
      } else if (OB_FAIL(sql.append_fmt(
          "select '%.*s' as column_name, '%s' as column_type, '%s' as `null`, "
          "NULL as `key`, NULL as `default`, NULL as `extra`",
          name.length(), name.ptr(), describe_type_name(item.expr_->get_result_type()),
          nullable ? "YES" : "NO"))) {
      }
    }
  }
  return ret;
}
int ObExplainResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  ObExplainStmt *explain_stmt = NULL;
  if (!(T_EXPLAIN == parse_tree.type_  && 5 == parse_tree.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid EXPLAIN syntax",
             "type", parse_tree.type_,
             "# of child", parse_tree.num_child_);
  } else if (NULL == (explain_stmt = create_stmt<ObExplainStmt>())) {
    ret = OB_SQL_RESOLVER_NO_MEMORY;
    LOG_WARN("failed to create explain stmt");
  } else {
    ParseNode *child_node = parse_tree.children_[EXPLAIN_CHILD_STMT];
    ObStmt *child_stmt = NULL;
    ObExplainDisplayOpt opt;
    opt.with_tree_line_ = true;
    if (OB_ISNULL(child_node)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("empty parse tree for stmt");
    } else if (!(T_SELECT == child_node->type_
                 || T_DELETE == child_node->type_
                 || T_INSERT == child_node->type_
                 || T_UPDATE == child_node->type_
                 || T_REPLACE == child_node->type_)) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("invalid stmt type for EXPLAIN", "type", child_node->type_);
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "stmt type for EXPLAIN");
    } else {
      ObResolver resolver(params_);
      if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                   *child_node,
                                   child_stmt))) {
      } else if (OB_ISNULL(child_stmt)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to generate stmt for child stmt");
      } else if (T_SELECT == child_node->type_
                 && select_has_file_function(*static_cast<ObSelectStmt *>(child_stmt))) {
        ObSqlString metadata_sql;
        ParseResult metadata_parse_result;
        ObParser parser(*params_.allocator_, params_.session_info_->get_sql_mode(),
                        params_.session_info_->get_charsets4parser());
        ObResolver metadata_resolver(params_);
        ObStmt *metadata_stmt = nullptr;
        if (OB_FAIL(build_describe_sql(*static_cast<ObSelectStmt *>(child_stmt), metadata_sql))) {
        } else if (OB_FAIL(parser.parse(metadata_sql.string(), metadata_parse_result))) {
        } else if (OB_ISNULL(metadata_parse_result.result_tree_)
                   || OB_ISNULL(metadata_parse_result.result_tree_->children_)
                   || metadata_parse_result.result_tree_->num_child_ < 1) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(metadata_resolver.resolve(
                     ObResolver::IS_NOT_PREPARED_STMT,
                     *metadata_parse_result.result_tree_->children_[0], metadata_stmt))) {
        } else if (OB_ISNULL(metadata_stmt)) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          set_basic_stmt(metadata_stmt);
          return OB_SUCCESS;
        }
        ret = OB_SUCCESS;
        explain_stmt->set_explain_query_stmt(static_cast<ObDMLStmt *>(child_stmt));
      } else {
        explain_stmt->set_explain_query_stmt(static_cast<ObDMLStmt *>(child_stmt));
        ParseNode *format = parse_tree.children_[EXPLAIN_FORMAT];
        if (NULL != format) {
          switch (format->type_) {
          case T_OUTLINE:
          {
            explain_stmt->set_explain_format(EXPLAIN_OUTLINE);
            opt.with_tree_line_ = true;
          } break;
          case T_EXTENDED:
          {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED);
            opt.with_tree_line_ = true;
          } break;
          case T_PARTITIONS:
          {
            explain_stmt->set_explain_format(EXPLAIN_PARTITIONS);
            opt.with_tree_line_ = true;
          } break;
          case T_TRADITIONAL:
          {
            explain_stmt->set_explain_format(EXPLAIN_TRADITIONAL);
            opt.with_tree_line_ = true;
          } break;
          case T_FORMAT_JSON:
          {
            explain_stmt->set_explain_format(EXPLAIN_FORMAT_JSON);
            opt.with_tree_line_ = false;
          } break;
          case T_BASIC: {
            explain_stmt->set_explain_format(EXPLAIN_BASIC);
            opt.with_tree_line_ = true;
            break;
          }
          case T_EXTENDED_NOADDR: {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED_NOADDR);
            opt.with_tree_line_ = true;
            break;
          }
          case T_PLANREGRESS: {
            explain_stmt->set_explain_format(EXPLAIN_PLANREGRESS);
            opt.with_tree_line_ = true;
            break;
          }
          default:
            ret = OB_ERROR;
            LOG_WARN("unknown explain format", "format", format->type_);
            break;
          }
        }
      }
    }
    ParseNode *opt_node = parse_tree.children_[EXPLAIN_DISPLAY_OPTION];
    if (OB_SUCC(ret) && NULL != opt_node) {
      switch (opt_node->type_) {
      case T_PRETTY_COLOR:
        opt.with_color_ = true;
        // pass
      case T_PRETTY:
        opt.with_tree_line_ = true;
        break;
      default:
        ret = OB_ERR_UNEXPECTED;
      }
    }
    if (OB_SUCC(ret)) {
      explain_stmt->set_display_opt(opt);
    }
    ParseNode *into_table_node = parse_tree.children_[EXPLAIN_INTO_TABLE];
    if (OB_SUCC(ret) && NULL != into_table_node) {
      ObString into_table(into_table_node->str_len_, into_table_node->str_value_);
      explain_stmt->set_into_table(into_table);
    }
    ParseNode *statement_id_node = parse_tree.children_[EXPLAIN_STMT_ID];
    if (OB_SUCC(ret) && NULL != statement_id_node) {
      ObString statement_id(statement_id_node->str_len_, statement_id_node->str_value_);
      explain_stmt->set_statement_id(statement_id);
    }
  }

  return ret;
}
