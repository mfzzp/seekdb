/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SERVER
#include <cmath>
#include "observer/ai_service/ai_column_worker.h"
#include "observer/ob_server.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "lib/string/ob_sql_string.h"
#include "share/object/ob_obj_cast.h"
#include "share/object/ob_array_cast.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/schema/ob_multi_version_schema_service.h"

namespace oceanbase
{
namespace observer
{
using namespace common;
using namespace share::schema;
namespace
{
constexpr int64_t AI_CALL_TIMEOUT = 30L * 1000 * 1000;
constexpr int64_t AI_LEASE_US = 60L * 1000 * 1000;
// Staged SQL hex literals take two bytes per result byte, plus their wrapper.
constexpr int64_t AI_RESULT_LIMIT = (OB_MAX_VARCHAR_LENGTH - 64) / 2;
const char *AI_TASKS = "oceanbase.__all_ai_column_task";

struct AiWorkItem
{
  int64_t table_id_ = 0;
  int64_t column_id_ = 0;
  int64_t definition_version_ = 0;
  int64_t retry_count_ = 0;
  int64_t state_ = 0;
  ObString task_id_;
  ObString request_id_;
  ObString predicate_;
  ObString result_;
};

// Hex literals are independent of NO_BACKSLASH_ESCAPES and preserve NUL bytes.
int ai_worker_literal(ObSqlString &sql, const ObString &value)
{
  int ret = sql.append("CONVERT(X'");
  const char hex[] = "0123456789ABCDEF";
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    const unsigned char c = static_cast<unsigned char>(value.ptr()[i]);
    const char pair[] = {hex[c >> 4], hex[c & 15]};
    OZ (sql.append(pair, 2));
  }
  OZ (sql.append("' USING utf8mb4)"));
  return ret;
}

int ai_worker_identifier(ObSqlString &sql, const ObString &name)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    OZ (sql.append(name.ptr() + i, 1));
    if (OB_SUCC(ret) && name.ptr()[i] == '`') { OZ (sql.append("`")); }
  }
  OZ (sql.append("`"));
  return ret;
}

int ai_worker_copy(sqlclient::ObMySQLResult &row, const char *name,
                   ObIAllocator &alloc, ObString &out)
{
  int ret = OB_SUCCESS;
  ObString value;
  OZ (row.get_varchar(name, value));
  OZ (ob_write_string(alloc, value, out));
  return ret;
}

int ai_worker_read_items(ObISQLClient &client, const ObSqlString &sql,
                        ObIAllocator &alloc, ObIArray<AiWorkItem> &items)
{
  int ret = OB_SUCCESS;
  alignas(16) ObMySQLProxy::MySQLResult result;
  sqlclient::ObMySQLResult *row = nullptr;
  if (OB_FAIL(client.read(result, sql.ptr()))) {
  } else if (OB_ISNULL(row = result.get_result())) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    while (OB_SUCC(ret) && OB_SUCC(ret = row->next())) {
      AiWorkItem item;
      OZ (row->get_int("table_id", item.table_id_));
      OZ (row->get_int("column_id", item.column_id_));
      OZ (row->get_int("definition_version", item.definition_version_));
      OZ (row->get_int("retry_count", item.retry_count_));
      OZ (row->get_int("state", item.state_));
      OZ (ai_worker_copy(*row, "task_id", alloc, item.task_id_));
      OZ (ai_worker_copy(*row, "request_id", alloc, item.request_id_));
      OZ (ai_worker_copy(*row, "rowkey_predicate", alloc, item.predicate_));
      OZ (ai_worker_copy(*row, "result_value", alloc, item.result_));
      OZ (items.push_back(item));
    }
    if (OB_ITER_END == ret) { ret = OB_SUCCESS; }
  }
  return ret;
}

int ai_worker_key(ObSqlString &sql, const AiWorkItem &item, bool group)
{
  int ret = sql.append_fmt("table_id=%ld AND request_id=", item.table_id_);
  OZ (ai_worker_literal(sql, item.request_id_));
  OZ (sql.append(group ? " AND rowkey_predicate=" : " AND task_id="));
  OZ (ai_worker_literal(sql, group ? item.predicate_ : item.task_id_));
  return ret;
}

int ai_worker_table(ObSchemaGetterGuard &guard, const ObTableSchema &table, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  const ObDatabaseSchema *database = nullptr;
  OZ (guard.get_database_schema(table.get_database_id(), database));
  if (OB_SUCC(ret) && OB_ISNULL(database)) { ret = OB_SCHEMA_ERROR; }
  if (OB_SUCC(ret)) {
    OZ (ai_worker_identifier(sql, database->get_database_name_str()));
    OZ (sql.append("."));
    OZ (ai_worker_identifier(sql, table.get_table_name_str()));
  }
  return ret;
}

// Use the same strict conversion and accuracy checks as SQL column assignment.
// Store only a generated SQL literal, never model output as executable SQL.
int ai_worker_convert(const ObString &raw, bool is_null,
                      const ObColumnSchemaV2 &column, ObIAllocator &alloc,
                      ObSqlString &literal)
{
  int ret = OB_SUCCESS;
  if (is_null) {
    OZ (literal.append("NULL"));
  } else if (raw.length() > AI_RESULT_LIMIT) {
    ret = OB_SIZE_OVERFLOW;
  } else if (column.get_data_type() == ObCollectionSQLType) {
    // The native vector assignment conversion already validated this value.
    OZ (ai_worker_literal(literal, raw));
  } else {
    ObObj input, converted;
    // Accuracy checks leave this pointer unchanged for integer/JSON types.
    const ObObj *accurate = &converted;
    ObAccuracy accuracy = column.get_accuracy();
    ObCastCtx cast_ctx(&alloc, nullptr,
        CM_STRICT_MODE | CM_COLUMN_CONVERT | CM_STRICT_JSON | CM_NO_ZERO_DATE | CM_NO_ZERO_IN_DATE,
        column.get_collation_type(), &accuracy);
    input.set_varchar(raw);
    input.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    OZ (ObObjCaster::to_type(column.get_data_type(), cast_ctx, input, converted));
    OZ (obj_accuracy_check(cast_ctx, accuracy, column.get_collation_type(), converted, converted, accurate));
    if (OB_SUCC(ret) && cast_ctx.warning_ != OB_SUCCESS) { ret = cast_ctx.warning_; }
    if (OB_SUCC(ret) && OB_ISNULL(accurate)) { ret = OB_ERR_UNEXPECTED; }
    if (OB_SUCC(ret)) {
      // JSON and strings retain their validated original representation. Other
      // types use the converted value so publication cannot reinterpret input.
      if (ob_is_string_or_lob_type(column.get_data_type()) || column.get_data_type() == ObJsonType) {
        OZ (ai_worker_literal(literal, raw));
      } else {
        char *buffer = nullptr;
        int64_t length = 0;
        int64_t pos = 0;
        OZ (accurate->print_sql_literal(buffer, length, pos, alloc));
        OZ (literal.append(buffer, pos));
      }
    }
  }
  return ret;
}

int ai_worker_call(ObMySQLProxy &proxy, ObSchemaGetterGuard &guard,
                  const ObTableSchema &table, const ObColumnSchemaV2 &column,
                  const AiColumnDefinition &definition, const AiWorkItem &item,
                  ObIAllocator &alloc, ObSqlString &value)
{
  int ret = OB_SUCCESS;
  ObSqlString query;
  // Read only the original prompt expression. Type guidance is sent through
  // the provider's system-instruction channel, preserving the user's input.
  OZ (query.append_fmt("SELECT CAST((%.*s) AS CHAR) AS value FROM ",
      definition.prompt_expression_.length(), definition.prompt_expression_.ptr()));
  OZ (ai_worker_table(guard, table, query));
  OZ (query.append_fmt(" WHERE (%.*s) AND EXISTS (SELECT 1 FROM %s WHERE ",
      item.predicate_.length(), item.predicate_.ptr(), AI_TASKS));
  OZ (ai_worker_key(query, item, false));
  OZ (query.append(")"));
  ObString prompt;
  bool prompt_is_null = false;
  {
    alignas(16) ObMySQLProxy::MySQLResult result;
    sqlclient::ObMySQLResult *row = nullptr;
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(proxy.read(result, query.ptr(), nullptr, AI_CALL_TIMEOUT))) {
    } else if (OB_ISNULL(row = result.get_result())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(row->next())) {
    } else {
      ret = ai_worker_copy(*row, "value", alloc, prompt);
      if (ret == OB_ERR_NULL_VALUE) {
        ret = OB_SUCCESS;
        prompt_is_null = true;
      }
      if (OB_SUCC(ret)) {
        const int next_ret = row->next();
        if (next_ret != OB_ITER_END) { ret = next_ret == OB_SUCCESS ? OB_ERR_UNEXPECTED : next_ret; }
      }
    }
  }
  // Release the SQL result/snapshot before contacting an external service.
  if (OB_SUCC(ret) && prompt_is_null) {
    // Match AI_COMPLETE/AI_EMBED NULL propagation. READY is independent of
    // whether the successful result is SQL NULL.
    return value.append("NULL");
  }
  if (OB_SUCC(ret) && prompt.empty()) { ret = OB_INVALID_ARGUMENT; }
  ObAIFuncExprInfo *info = nullptr;
  share::ObAiModelEndpointInfo endpoint;
  query::ObIAiEndpointResolver *resolver = share::server_service<query::ObIAiEndpointResolver>();
  ObString raw;
  if (OB_SUCC(ret) && resolver == nullptr) { ret = OB_NOT_INIT; }
  OZ (ObAIFuncUtils::get_ai_func_info(alloc, definition.model_name_, guard, info));
  OZ (resolver->resolve_by_model_name(definition.model_name_, alloc, endpoint));
  if (OB_SUCC(ret) && info == nullptr) { ret = OB_ERR_UNEXPECTED; }
  if (OB_SUCC(ret)) {
    // Every retry is persisted in the catalog. Hidden transport retries would
    // exceed max_retries and make recovery/call accounting inconsistent.
    ObAIFuncModel model(alloc, *info, endpoint, 0);
    const int64_t saved_timeout = THIS_WORKER.get_timeout_ts();
    THIS_WORKER.set_timeout_ts(ObTimeUtility::current_time() + AI_CALL_TIMEOUT);
    if (column.get_data_type() == ObCollectionSQLType) {
      // Preserve an explicitly supplied AI_EMBED dimension argument. A column
      // dimension alone validates output and does not change the model input.
      const ParseNode *expression = nullptr;
      ObJsonObject *config = nullptr;
      OZ (sql::ObRawExprUtils::parse_expr_node_from_str(definition.expression_,
          sql::ObCharsets4Parser(), alloc, expression));
      if (OB_SUCC(ret) && (expression == nullptr || expression->num_child_ != 2
          || expression->children_[1] == nullptr)) { ret = OB_INVALID_DATA; }
      if (OB_SUCC(ret) && expression->children_[1]->num_child_ == 3) {
        const ParseNode *dimension = expression->children_[1]->children_[2];
        if (dimension == nullptr || dimension->type_ != T_INT
            || dimension->value_ != definition.vector_dimension_) {
          ret = OB_INVALID_DATA;
        } else {
          ObJsonInt *number = nullptr;
          OZ (ObAIFuncJsonUtils::get_json_object(alloc, config));
          OZ (ObAIFuncJsonUtils::get_json_int(alloc, dimension->value_, number));
          OZ (config->add("dimensions", number));
        }
      }
      OZ (model.call_dense_embedding(prompt, config, raw));
    } else {
      ret = model.call_completion(prompt, nullptr, raw, definition.type_instruction_);
    }
    THIS_WORKER.set_timeout_ts(saved_timeout);
  }
  if (OB_SUCC(ret) && raw.length() > AI_RESULT_LIMIT) { ret = OB_SIZE_OVERFLOW; }
  if (OB_SUCC(ret) && column.get_data_type() == ObCollectionSQLType) {
    // Reuse the string-to-vector assignment cast. VECTOR(n) is a column type,
    // not a supported target of the SQL CAST grammar in this engine version.
    ObSqlCollectionInfo collection(alloc);
    ObIArrayType *vector = nullptr;
    if (column.get_extended_type_info().count() != 1) { ret = OB_SCHEMA_ERROR; }
    if (OB_SUCC(ret)) {
      collection.set_name(column.get_extended_type_info().at(0));
      OZ (collection.parse_type_info());
    }
    if (OB_SUCC(ret) && (collection.collection_meta_ == nullptr
        || collection.collection_meta_->type_id_ != ObNestedType::OB_VECTOR_TYPE)) {
      ret = OB_SCHEMA_ERROR;
    }
    if (OB_SUCC(ret)) {
      OZ (ObArrayTypeObjFactory::construct(alloc, *collection.collection_meta_, vector));
      OZ (sql::ObArrayCastUtils::string_cast_vector(alloc, raw, vector, collection.collection_meta_, false));
      OZ (vector->check_validity(*collection.collection_meta_, *vector));
      OZ (vector->init());
      for (uint32_t i = 0; OB_SUCC(ret) && i < vector->size(); ++i) {
        ObObj element;
        OZ (vector->elem_at(i, element));
        if (OB_SUCC(ret) && ((element.get_type() == ObFloatType && !std::isfinite(element.get_float()))
            || (element.get_type() == ObDoubleType && !std::isfinite(element.get_double())))) {
          ret = OB_DATA_OUT_OF_RANGE;
        }
      }
    }
  }
  OZ (ai_worker_convert(raw, false, column, alloc, value));
  return ret;
}

const char *AI_ITEM_SELECT = "SELECT table_id, task_id, column_id, request_id, rowkey_predicate, "
    "definition_version, retry_count, state, CAST(COALESCE(result_value, '') AS CHAR) AS result_value FROM ";

int ai_worker_publish(ObMySQLProxy &proxy, ObSchemaGetterGuard &guard,
                      const ObTableSchema &table, const AiWorkItem &item)
{
  int ret = OB_SUCCESS;
  ObMySQLTransaction trans;
  ObArenaAllocator alloc("AiPublish");
  ObSEArray<AiWorkItem, 4> group;
  ObSqlString query;
  int64_t affected = 0;
  OZ (trans.start(&proxy));
  // Lock business row first, matching business DML's row -> task lock order.
  // This also prevents a concurrent DELETE/reinsert from reviving an old result.
  OZ (query.append("SELECT 1 FROM "));
  OZ (ai_worker_table(guard, table, query));
  OZ (query.append_fmt(" WHERE %.*s FOR UPDATE", item.predicate_.length(), item.predicate_.ptr()));
  if (OB_SUCC(ret)) {
    alignas(16) ObMySQLProxy::MySQLResult result;
    OZ (trans.read(result, query.ptr()));
    if (OB_SUCC(ret) && OB_ISNULL(result.get_result())) { ret = OB_ERR_UNEXPECTED; }
    if (OB_SUCC(ret)) {
      const int read_ret = result.get_result()->next();
      if (read_ret != OB_SUCCESS && read_ret != OB_ITER_END) { ret = read_ret; }
    }
  }
  query.reset();
  OZ (query.append_fmt("%s%s WHERE ", AI_ITEM_SELECT, AI_TASKS));
  OZ (ai_worker_key(query, item, true));
  OZ (query.append(" ORDER BY task_id FOR UPDATE"));
  OZ (ai_worker_read_items(trans, query, alloc, group));
  bool ready = !group.empty();
  for (int64_t i = 0; OB_SUCC(ret) && i < group.count(); ++i) {
    ready = ready && (group.at(i).state_ == 2 || group.at(i).state_ == 4);
  }
  if (OB_SUCC(ret) && ready) {
    query.reset();
    OZ (query.append("UPDATE "));
    OZ (ai_worker_table(guard, table, query));
    OZ (query.append(" SET "));
    for (int64_t i = 0; OB_SUCC(ret) && i < group.count(); ++i) {
      const AiWorkItem &part = group.at(i);
      const ObColumnSchemaV2 *column = table.get_column_schema(part.column_id_);
      AiColumnDefinition definition;
      if (OB_ISNULL(column) || !column->is_ai_column()
          || part.definition_version_ != column->get_schema_version()) {
        ret = OB_SCHEMA_EAGAIN;
      } else {
        OZ (column->get_ai_column_definition(definition));
        const ObColumnSchemaV2 *state = table.get_column_schema(definition.state_column_id_);
        const ObColumnSchemaV2 *request = table.get_column_schema(definition.request_column_id_);
        if (OB_SUCC(ret) && (OB_ISNULL(state) || OB_ISNULL(request))) { ret = OB_SCHEMA_ERROR; }
        if (OB_SUCC(ret)) {
          if (i > 0) { OZ (query.append(",")); }
          OZ (ai_worker_identifier(query, column->get_column_name_str()));
          OZ (query.append("="));
          OZ (query.append(part.state_ == 2 ? part.result_ : ObString::make_string("NULL")));
          OZ (query.append(","));
          OZ (ai_worker_identifier(query, state->get_column_name_str()));
          OZ (query.append_fmt("=%ld,", part.state_));
          OZ (ai_worker_identifier(query, request->get_column_name_str()));
          OZ (query.append("="));
          OZ (ai_worker_literal(query, part.request_id_));
        }
      }
    }
    OZ (query.append_fmt(" WHERE %.*s", item.predicate_.length(), item.predicate_.ptr()));
    OZ (trans.write(query.ptr(), affected));
    if (OB_SUCC(ret) && affected > 1) { ret = OB_ERR_UNEXPECTED; }
    query.reset();
    OZ (query.append_fmt("DELETE FROM %s WHERE ", AI_TASKS));
    OZ (ai_worker_key(query, item, true));
    OZ (trans.write(query.ptr(), affected));
    if (OB_SUCC(ret) && affected != group.count()) { ret = OB_ERR_UNEXPECTED; }
  }
  if (OB_SUCC(ret) && !ready && !group.empty()) {
    // Rotate a completed member while its siblings retry, so a large number
    // of incomplete groups cannot starve the eligible pending-task scan.
    query.reset();
    OZ (query.append_fmt("UPDATE %s SET eligible_ts=DATE_ADD(NOW(6),INTERVAL 1 SECOND) WHERE ", AI_TASKS));
    OZ (ai_worker_key(query, item, false));
    OZ (query.append(" AND state IN (2,4)"));
    OZ (trans.write(query.ptr(), affected));
  }
  if (trans.is_started()) {
    const int end_ret = trans.end(OB_SUCC(ret));
    if (OB_SUCC(ret)) { ret = end_ret; }
  }
  return ret;
}

int ai_worker_process(ObMySQLProxy &proxy, const AiWorkItem &item)
{
  int ret = OB_SUCCESS;
  ObSchemaGetterGuard guard;
  const ObTableSchema *table = nullptr;
  const ObColumnSchemaV2 *column = nullptr;
  AiColumnDefinition definition;
  ObSqlString query;
  int64_t affected = 0;
  OZ (GCTX.schema_service_->get_runtime_schema_guard(guard));
  OZ (guard.get_table_schema(item.table_id_, table));
  if (OB_SUCC(ret) && (OB_ISNULL(table)
      || OB_ISNULL(column = table->get_column_schema(item.column_id_))
      || !column->is_ai_column() || column->get_schema_version() != item.definition_version_)) {
    // Never use a task's old expression after DDL. A dropped table has no row to publish.
    LOG_INFO("discard obsolete AI column task", K(item.table_id_), K(item.column_id_),
             K(item.definition_version_), "current_version", column == nullptr ? -1 : column->get_schema_version(),
             "table_exists", table != nullptr, "column_exists", column != nullptr);
    OZ (query.append_fmt("DELETE FROM %s WHERE ", AI_TASKS));
    OZ (ai_worker_key(query, item, false));
    OZ (proxy.write(query.ptr(), affected));
  } else if (OB_SUCC(ret)) {
    OZ (column->get_ai_column_definition(definition));
    if (OB_SUCC(ret) && item.state_ != 2 && item.state_ != 4) {
      static std::atomic<uint64_t> sequence(0);
      ObSqlString lease;
      const int64_t now = ObTimeUtility::current_time();
      OZ (lease.append_fmt("%ld-%lu", now, ++sequence));
      OZ (query.append_fmt("UPDATE %s SET status='RUNNING',state=1,lease_id=", AI_TASKS));
      OZ (ai_worker_literal(query, lease.string()));
      OZ (query.append_fmt(",eligible_ts=DATE_ADD(NOW(6),INTERVAL %ld MICROSECOND) WHERE ", AI_LEASE_US));
      OZ (ai_worker_key(query, item, false));
      OZ (query.append(" AND state IN (0,1,3) AND eligible_ts<=NOW(6)"));
      OZ (proxy.write(query.ptr(), affected));
      if (OB_SUCC(ret) && affected == 1) {
        ObArenaAllocator alloc("AiCall");
        ObSqlString value;
        const int call_ret = ai_worker_call(proxy, guard, *table, *column, definition, item, alloc, value);
        const bool retry = call_ret != OB_SUCCESS && call_ret != OB_ITER_END
            && item.retry_count_ < definition.max_retries_;
        const int64_t state = call_ret == OB_SUCCESS ? 2 : retry ? 3 : 4;
        query.reset();
        OZ (query.append_fmt("UPDATE %s SET state=%ld,status='%s',retry_count=%ld,"
            "eligible_ts=DATE_ADD(NOW(6),INTERVAL 1 SECOND),last_error=", AI_TASKS,
            state, state == 2 ? "READY" : retry ? "RETRY" : "FAILED",
            item.retry_count_ + (retry ? 1 : 0)));
        ObSqlString error;
        if (call_ret != OB_SUCCESS) { OZ (error.append_fmt("error=%d", call_ret)); }
        OZ (ai_worker_literal(query, error.string()));
        OZ (query.append(",result_value="));
        OZ (ai_worker_literal(query, value.string()));
        OZ (query.append(" WHERE "));
        OZ (ai_worker_key(query, item, false));
        OZ (query.append(" AND state=1 AND lease_id="));
        OZ (ai_worker_literal(query, lease.string()));
        OZ (proxy.write(query.ptr(), affected));
        if (OB_SUCCESS != call_ret && OB_ITER_END != call_ret) {
          LOG_WARN("AI column model call or conversion failed", K(call_ret), K(item.table_id_),
                   K(item.column_id_), K(item.retry_count_));
        }
      }
    }
    if (OB_SUCC(ret)) {
      // Use a fresh guard after the potentially slow network call.
      ObSchemaGetterGuard publish_guard;
      const ObTableSchema *current = nullptr;
      OZ (GCTX.schema_service_->get_runtime_schema_guard(publish_guard));
      OZ (publish_guard.get_table_schema(item.table_id_, current));
      const ObColumnSchemaV2 *current_column = current == nullptr ? nullptr
          : current->get_column_schema(item.column_id_);
      if (OB_SUCC(ret) && current_column != nullptr
          && current_column->get_schema_version() == item.definition_version_) {
        OZ (ai_worker_publish(proxy, publish_guard, *current, item));
      }
    }
  }
  return ret;
}
} // namespace

int AiColumnWorker::start(ObMySQLProxy &proxy)
{
  int ret = OB_SUCCESS;
  if (timers_[0].inited()) {
    ret = OB_INIT_TWICE;
  } else {
    proxy_ = &proxy;
    stopped_.store(false);
    for (int64_t i = 0; OB_SUCC(ret) && i < RUNNERS; ++i) {
      OZ (timers_[i].init("AiColumn", ObMemAttr("AiColumn")));
      OZ (timers_[i].schedule(*this, 100 * 1000, true));
    }
    if (OB_FAIL(ret)) { destroy(); }
  }
  return ret;
}

void AiColumnWorker::stop()
{
  stopped_.store(true);
  for (int64_t i = 0; i < RUNNERS; ++i) {
    if (timers_[i].inited()) { timers_[i].stop(); }
  }
  for (int64_t i = 0; i < RUNNERS; ++i) {
    if (timers_[i].inited()) { timers_[i].wait(); }
  }
}

void AiColumnWorker::destroy()
{
  stop();
  for (int64_t i = 0; i < RUNNERS; ++i) {
    if (timers_[i].inited()) { timers_[i].destroy(); }
  }
  proxy_ = nullptr;
}

void AiColumnWorker::runTimerTask()
{
  if (stopped_.load() || GCTX.status_ != SS_SERVING || proxy_ == nullptr
      || share::server_role() != share::ObServerRole::PRIMARY_ROLE) { return; }
  int ret = OB_SUCCESS;
  ObArenaAllocator alloc("AiWorkScan");
  ObSEArray<AiWorkItem, 32> items;
  ObSqlString query;
  // Terminal tasks are scanned too: a crash after staging cannot lose publication.
  OZ (query.append_fmt("%s%s WHERE eligible_ts<=NOW(6) "
      "ORDER BY eligible_ts,table_id,task_id LIMIT 32", AI_ITEM_SELECT, AI_TASKS));
  OZ (ai_worker_read_items(*proxy_, query, alloc, items));
  for (int64_t i = 0; OB_SUCC(ret) && !stopped_.load() && i < items.count(); ++i) {
    const int item_ret = ai_worker_process(*proxy_, items.at(i));
    if (item_ret != OB_SUCCESS) {
      LOG_WARN("AI column work item failed", K(item_ret), K(items.at(i).table_id_), K(items.at(i).column_id_));
    }
  }
  if (OB_FAIL(ret) && REACH_TIME_INTERVAL(10L * 1000 * 1000)) {
    LOG_WARN("AI column task scan failed", K(ret));
  }
}
} // namespace observer
} // namespace oceanbase
