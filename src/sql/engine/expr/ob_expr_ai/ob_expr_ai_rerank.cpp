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

#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_ai_rerank.h"
#include <algorithm>
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include "lib/utility/utility.h"
#include "common/json_type/ob_json_common.h"
 
using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{

static int get_rerank_score(const ObJsonNode *node, double &score)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(node)) {
    ret = OB_INVALID_DATA;
  } else if (node->json_type() == ObJsonNodeType::J_DOUBLE) {
    score = static_cast<const ObJsonDouble *>(node)->value();
  } else if (node->json_type() == ObJsonNodeType::J_INT) {
    score = static_cast<const ObJsonInt *>(node)->value();
  } else if (node->json_type() == ObJsonNodeType::J_UINT) {
    score = static_cast<const ObJsonUint *>(node)->value();
  } else {
    ret = OB_INVALID_DATA;
  }
  return ret;
}

static int get_rerank_index(const ObJsonNode *node, int64_t &index)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(node)) {
    ret = OB_INVALID_DATA;
  } else if (node->json_type() == ObJsonNodeType::J_INT) {
    index = static_cast<const ObJsonInt *>(node)->value();
  } else if (node->json_type() == ObJsonNodeType::J_UINT &&
             static_cast<const ObJsonUint *>(node)->value() <= INT64_MAX) {
    index = static_cast<int64_t>(static_cast<const ObJsonUint *>(node)->value());
  } else {
    ret = OB_INVALID_DATA;
  }
  return ret;
}

ObExprAIRerank::ObExprAIRerank(common::ObIAllocator &alloc)
: ObFuncExprOperator(alloc, 
                    T_FUN_SYS_AI_RERANK, 
                    N_AI_RERANK, 
                    MORE_THAN_ZERO,
                    NOT_VALID_FOR_GENERATED_COL, 
                    NOT_ROW_DIMENSION) 
{
}

ObExprAIRerank::~ObExprAIRerank() 
{
}

int ObExprAIRerank::calc_result_typeN(ObExprResType &type,
                                    ObExprResType *types_stack,
                                    int64_t param_num,
                                    common::ObExprTypeCtx &type_ctx) const {
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num < 3 || param_num > 4)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    if (!ob_is_string_tc(types_stack[MODEL_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model key must be string type");
    } else if (!ob_is_string_tc(types_stack[QUERY_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, query must be string type");
    } else {
      types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      types_stack[QUERY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    }
    ObObjType in_type = types_stack[DOCUMENTS_IDX].get_type();
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types_stack, DOCUMENTS_IDX, N_AI_RERANK))) {
    } else if (ob_is_string_type(in_type) && types_stack[DOCUMENTS_IDX].get_collation_type() != CS_TYPE_BINARY) {
      if (types_stack[DOCUMENTS_IDX].get_charset_type() != CHARSET_UTF8MB4) {
        types_stack[DOCUMENTS_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (param_num == 4) {
      ObObjType doc_key_type = types_stack[DOC_KEY_IDX].get_type();
      if (doc_key_type == ObNullType) {
      } else if (doc_key_type == ObJsonType) {
      } else if (ob_is_string_type(doc_key_type)) {
        types_stack[DOC_KEY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      } else {  
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, doc key must be string type");
      }
    }

    if (OB_SUCC(ret)) {
      type.set_json();
      type.set_length((ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType]).get_length());
    }
  }
  return ret;
}

int ObExprAIRerank::eval_ai_rerank(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_query = nullptr;
  ObDatum *arg_documents = nullptr;
  ObDatum *arg_doc_key = nullptr;
  if (OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_query, arg_documents, arg_doc_key))) {
  } else if (arg_model_id->is_null() || arg_query->is_null() || arg_documents->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model id or query or documents is null");
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_RERANK));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString query = arg_query->get_string();
    ObArray<ObString> header_array;
    ObJsonArray *document_array = nullptr;
    ObJsonArray *result_array = nullptr;
    ObIJsonBase *j_base = nullptr;
    int64_t batch_size = 20; // max batch size for rerank
    bool is_null_result = false;
    if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, temp_allocator, DOCUMENTS_IDX,
          j_base, is_null_result))) {
    } else if (OB_ISNULL(j_base)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (j_base->json_type() != ObJsonNodeType::J_ARRAY) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is not array");
    } else if (OB_ISNULL(document_array = static_cast<ObJsonArray *>(j_base))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (document_array->element_count() == 0) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is empty");
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < document_array->element_count(); i++) {
        ObJsonNode *doc_node = document_array->get_value(i);
        if (OB_ISNULL(doc_node)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_NOT_NULL(arg_doc_key) && !arg_doc_key->is_null() && doc_node->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not object");
        } else if ((OB_ISNULL(arg_doc_key) || arg_doc_key->is_null()) && doc_node->json_type() != ObJsonNodeType::J_STRING) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not string");
        }
      }
    }
    
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } 

    if (OB_FAIL(ret)) {
    } else if (OB_NOT_NULL(arg_doc_key) && !arg_doc_key->is_null()) {
      ObString doc_key = arg_doc_key->get_string();
      if (OB_FAIL(eval_ai_rerank_with_doc_key(expr, ctx, temp_allocator, model_id,
                  query, document_array, doc_key, *info, *endpoint_info, res))) {
      }
    } else {
      if (OB_ISNULL(info)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(ObAIFuncUtils::check_info_type_rerank(info))) {
      } else if (OB_FAIL(ObAIFuncUtils::get_header(temp_allocator, *info, *endpoint_info, header_array))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(temp_allocator, result_array))) {
      }

      if (OB_SUCC(ret)) {
        ObString score_key(SCORE_KEY);
        int64_t end_idx = 0;
        ObJsonArray *compact_array = nullptr;
        ObJsonArray *batch_result_array = nullptr;
        ObJsonArray *batch_document_array = nullptr;
        for (int64_t i = 0; OB_SUCC(ret) && i < document_array->element_count(); i += batch_size) {
          end_idx = i + batch_size;
          if (end_idx > document_array->element_count()) {
            end_idx = document_array->element_count();
          }
          if (OB_FAIL(construct_batch_document_array(temp_allocator, document_array, i, end_idx, batch_document_array))) {
          } else if (OB_FAIL(inner_eval_ai_rerank(temp_allocator, *info, *endpoint_info, header_array, query, batch_document_array, batch_result_array))) {
          } else if (OB_FAIL(batch_result_add_base(temp_allocator, batch_result_array, i))) {
          } else if (OB_FAIL(compact_json_array_by_key(temp_allocator, result_array, batch_result_array, score_key, compact_array))) {
          } else {
            result_array = compact_array;
          }
        }
      }
      if (OB_SUCC(ret)) {
        ObString raw_str;
        if (OB_FAIL(result_array->get_raw_binary(raw_str, &temp_allocator))) {
        } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, raw_str))) {
        }
      }
    }
  }
  return ret;
}

int ObExprAIRerank::eval_ai_rerank_with_doc_key(const ObExpr &expr, ObEvalCtx &ctx,
                                                ObIAllocator &allocator, ObString &model_id,
                                                ObString &query, ObJsonArray *document_array,
                                                ObString &doc_key, const ObAIFuncExprInfo &info,
                                                const share::ObAiModelEndpointInfo &endpoint_info, ObDatum &res)
{
  int ret = OB_SUCCESS;
  ObJsonArray *docs = nullptr;
  ObJsonArray *scores = nullptr;
  ObJsonArray *sorted_docs = nullptr;
  ObArray<ObString> headers;
  if (OB_FAIL(get_doc_array_from_documents_array_with_key(allocator, document_array, doc_key, docs))) {
  } else if (OB_FAIL(ObAIFuncUtils::check_info_type_rerank(&info))) {
  } else if (OB_FAIL(ObAIFuncUtils::get_header(allocator, info, endpoint_info, headers))) {
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, scores))) {
  } else {
    ObString score_key(SCORE_KEY);
    const int64_t batch_size = 20;
    for (int64_t start = 0; OB_SUCC(ret) && start < docs->element_count(); start += batch_size) {
      const int64_t end = std::min<int64_t>(start + batch_size, docs->element_count());
      ObJsonArray *batch_docs = nullptr;
      ObJsonArray *batch_scores = nullptr;
      ObJsonArray *merged = nullptr;
      if (OB_FAIL(construct_batch_document_array(allocator, docs, start, end, batch_docs))) {
      } else if (OB_FAIL(inner_eval_ai_rerank(allocator, info, endpoint_info,
                       headers, query, batch_docs, batch_scores))) {
      } else if (OB_FAIL(batch_result_add_base(allocator, batch_scores, start))) {
      } else if (OB_FAIL(compact_json_array_by_key(allocator, scores,
                       batch_scores, score_key, merged))) {
      } else {
        scores = merged;
      }
    }
    if (OB_SUCC(ret)) {
      ret = sort_document_array_by_model_result(allocator, document_array,
                                                scores, sorted_docs);
    }
  }
  if (OB_SUCC(ret)) {
    ObString raw;
    if (OB_FAIL(sorted_docs->get_raw_binary(raw, &allocator))) {
    } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, raw))) {
    }
  }
  return ret;
}

int ObExprAIRerank::get_doc_array_from_documents_array_with_key(ObIAllocator &allocator, ObJsonArray *document_object_array, ObString &doc_key, ObJsonArray *&doc_array)
{
  INIT_SUCC(ret);
  if (OB_ISNULL(document_object_array)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (doc_key.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonArray *doc_array_obj = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, doc_array_obj))) {
    } else {
      int64_t count = document_object_array->element_count();
      ObJsonNode *doc_node = nullptr;
      ObJsonNode *doc_value_node = nullptr;
      ObJsonObject *doc_obj = nullptr;
      for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
        doc_node = document_object_array->get_value(i);
        if (OB_ISNULL(doc_node)) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, document_array element is null");
        } else if (doc_node->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not object");
        } else {
          doc_obj = static_cast<ObJsonObject *>(doc_node);
          doc_value_node = doc_obj->get_value(doc_key);
          if (OB_ISNULL(doc_value_node)) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is null");
          } else if (doc_value_node->json_type() != ObJsonNodeType::J_STRING) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is not string");
          } else if (OB_FAIL(doc_array_obj->append(doc_value_node))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      doc_array = doc_array_obj;
    }
  }
  return ret;
}

int ObExprAIRerank::sort_document_array_by_model_result(ObIAllocator &allocator,
                                                          ObJsonArray *documents, ObJsonArray *scores,
                                                          ObJsonArray *&sorted_document_array)
{
  int ret = OB_SUCCESS;
  ObJsonArray *out = nullptr;
  if (OB_ISNULL(documents) || OB_ISNULL(scores) ||
      documents->element_count() != scores->element_count()) {
    ret = OB_INVALID_DATA;
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, out))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < scores->element_count(); ++i) {
      ObJsonNode *node = scores->get_value(i);
      if (OB_ISNULL(node) || node->json_type() != ObJsonNodeType::J_OBJECT) {
        ret = OB_INVALID_DATA;
      } else {
        ObJsonObject *score_obj = static_cast<ObJsonObject *>(node);
        ObJsonNode *idx_node = score_obj->get_value(INDEX_KEY);
        ObJsonNode *score_node = score_obj->get_value(SCORE_KEY);
        double score = 0;
        int64_t idx = -1;
        if (OB_FAIL(get_rerank_index(idx_node, idx)) ||
            OB_FAIL(get_rerank_score(score_node, score))) {
          ret = OB_INVALID_DATA;
        } else {
          ObJsonNode *orig = idx >= 0 && idx < documents->element_count()
                                 ? documents->get_value(idx) : nullptr;
          if (OB_ISNULL(orig) || orig->json_type() != ObJsonNodeType::J_OBJECT) {
            ret = OB_INVALID_DATA;
          } else {
            ObJsonObject *copy = static_cast<ObJsonObject *>(orig->clone(&allocator, true));
            if (OB_ISNULL(copy)) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
            } else if (OB_FAIL(copy->add(MODEL_SCORE_KEY, score_node))) {
            } else if (OB_FAIL(out->append(copy))) {
            }
          }
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    sorted_document_array = out;
  }
  return ret;
}

int ObExprAIRerank::construct_batch_document_array(ObIAllocator &allocator, ObJsonArray *document_array, int64_t start_idx, int64_t end_idx, ObJsonArray *&batch_document_array)
{
  INIT_SUCC(ret);
  ObJsonArray *result_array = nullptr;
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, result_array))) {
  } else {
    for (int64_t i = start_idx; i < end_idx; i++) {
      if (OB_FAIL(result_array->append(document_array->get_value(i)))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    batch_document_array = result_array;
  }
  return ret;
}

int ObExprAIRerank::inner_eval_ai_rerank(ObIAllocator &allocator,
                                          const ObAIFuncExprInfo &info,
                                          const share::ObAiModelEndpointInfo &endpoint_info,
                                          ObArray<ObString> &headers, ObString &query,
                                          ObJsonArray *documents, ObJsonArray *&result_array)
{
  int ret = OB_SUCCESS;
  ObJsonObject *body = nullptr;
  ObJsonObject *http_response = nullptr;
  ObIJsonBase *response = nullptr;
  ObAIFuncClient client;
  if (OB_FAIL(ObAIFuncUtils::get_rerank_body(allocator, info, endpoint_info,
                                            query, documents, nullptr, body))) {
  } else if (OB_FAIL(client.send_post(allocator, endpoint_info.get_url(), headers, body, http_response))) {
  } else if (OB_FAIL(ObAIFuncUtils::parse_rerank_output(allocator, endpoint_info, http_response, response))) {
  } else if (OB_ISNULL(response) || response->json_type() != ObJsonNodeType::J_ARRAY) {
    ret = OB_INVALID_DATA;
  } else {
    ObJsonArray *scores = static_cast<ObJsonArray *>(response);
    int64_t count = documents->element_count();
    ObArray<bool> seen;
    if (scores->element_count() != count) {
      ret = OB_INVALID_DATA;
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
      if (OB_FAIL(seen.push_back(false))) {
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
      ObJsonNode *node = scores->get_value(i);
      if (OB_ISNULL(node) || node->json_type() != ObJsonNodeType::J_OBJECT) {
        ret = OB_INVALID_DATA;
      } else {
        ObJsonObject *obj = static_cast<ObJsonObject *>(node);
        ObJsonNode *idx_node = obj->get_value(INDEX_KEY);
        double score = 0;
        int64_t idx = -1;
        if (OB_FAIL(get_rerank_index(idx_node, idx)) ||
            OB_FAIL(get_rerank_score(obj->get_value(SCORE_KEY), score))) {
          ret = OB_INVALID_DATA;
        } else {
          if (idx < 0 || idx >= count || seen.at(idx)) {
            ret = OB_INVALID_DATA;
          } else {
            seen.at(idx) = true;
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      result_array = scores;
    }
  }
  return ret;
}

int ObExprAIRerank::batch_result_add_base(ObIAllocator &allocator,
                                            ObJsonArray *array, int64_t start_idx)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(array)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < array->element_count(); ++i) {
      ObJsonNode *node = array->get_value(i);
      if (OB_ISNULL(node) || node->json_type() != ObJsonNodeType::J_OBJECT) {
        ret = OB_INVALID_DATA;
      } else {
        ObJsonObject *obj = static_cast<ObJsonObject *>(node);
        ObJsonNode *index_node = obj->get_value(INDEX_KEY);
        int64_t idx = -1;
        if (OB_FAIL(get_rerank_index(index_node, idx)) || idx < 0 || idx > INT64_MAX - start_idx) {
          ret = OB_INVALID_DATA;
        } else if (index_node->json_type() == ObJsonNodeType::J_INT) {
          static_cast<ObJsonInt *>(index_node)->set_value(idx + start_idx);
        } else {
          ObJsonInt *global_index = nullptr;
          if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, idx + start_idx, global_index))) {
          } else if (OB_FAIL(obj->add(INDEX_KEY, global_index))) {
          }
        }
      }
    }
  }
  return ret;
}

int ObExprAIRerank::compact_json_array_by_key(ObIAllocator &allocator,
                                               ObJsonArray *array1, ObJsonArray *array2,
                                               ObString &score_key, ObJsonArray *&compact_array)
{
  int ret = OB_SUCCESS;
  ObArray<ObJsonObject *> ordered;
  if (OB_ISNULL(array1) || OB_ISNULL(array2)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    for (int a = 0; OB_SUCC(ret) && a < 2; ++a) {
      ObJsonArray *array = a == 0 ? array1 : array2;
      for (int64_t i = 0; OB_SUCC(ret) && i < array->element_count(); ++i) {
        ObJsonNode *node = array->get_value(i);
        if (OB_ISNULL(node) || node->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_DATA;
        } else {
          ObJsonObject *obj = static_cast<ObJsonObject *>(node);
          double score = 0;
          if (OB_FAIL(get_rerank_score(obj->get_value(score_key), score))) {
          } else if (OB_FAIL(ordered.push_back(obj))) {
          }
        }
      }
    }
    // Stable descending order even if the provider's batch is unsorted.
    for (int64_t i = 1; OB_SUCC(ret) && i < ordered.count(); ++i) {
      ObJsonObject *item = ordered.at(i);
      double score = 0;
      if (OB_FAIL(get_rerank_score(item->get_value(score_key), score))) {
      } else {
        int64_t j = i;
        while (j > 0 && OB_SUCC(ret)) {
          double prev_score = 0;
          if (OB_FAIL(get_rerank_score(ordered.at(j - 1)->get_value(score_key), prev_score))) {
          } else {
            int64_t prev_idx = -1;
            int64_t curr_idx = -1;
            if (OB_FAIL(get_rerank_index(ordered.at(j - 1)->get_value(INDEX_KEY), prev_idx)) ||
                OB_FAIL(get_rerank_index(item->get_value(INDEX_KEY), curr_idx))) {
            } else if (prev_score > score || (prev_score == score && prev_idx <= curr_idx)) {
              break;
            } else {
              ordered.at(j) = ordered.at(j - 1);
              --j;
            }
          }
        }
        if (OB_SUCC(ret)) {
          ordered.at(j) = item;
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    ObJsonArray *out = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, out))) {
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < ordered.count(); ++i) {
        if (OB_FAIL(out->append(ordered.at(i)))) {
        }
      }
      if (OB_SUCC(ret)) {
        compact_array = out;
      }
    }
  }
  return ret;
}

int ObExprAIRerank::construct_config_json(ObIAllocator &allocator, int64_t top_n, int64_t return_doc, ObJsonObject *&config_json)
{
  INIT_SUCC(ret);
  ObJsonObject *config_obj = nullptr;
  ObJsonInt *top_n_obj = nullptr;
  ObJsonBoolean *return_doc_obj = nullptr;
  bool return_doc_value = true;
  if (return_doc <= 0) {
    return_doc_value = false;
  }
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config_obj))) {
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_boolean(allocator, return_doc_value, return_doc_obj))) {
  } else if (OB_FAIL(config_obj->add("return_documents", return_doc_obj))) {
  } else if (top_n > 0) {
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, top_n, top_n_obj))) {
    } else if (OB_FAIL(config_obj->add("top_n", top_n_obj))) {
    }
  }
  if (OB_SUCC(ret)) {
    config_json = config_obj;
  }
  return ret;
}

int ObExprAIRerank::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                        const ObRawExpr &raw_expr,
                        ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   bool got_data = false;
  //   ObObj const_data;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else {
  //     ObAIFuncExprInfo *info = OB_NEWx(ObAIFuncExprInfo, allocator, *allocator, T_FUN_SYS_AI_RERANK);
  //     if (OB_ISNULL(info)) {
  //       ret = OB_ALLOCATE_MEMORY_FAILED;
  //       LOG_WARN("failed to allocate memory for ai rerank info", K(ret));
  //     } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //       LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //     } else {
  //       rt_expr.extra_info_ = info;
  //     }
  //   }
  // }
  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIRerank::eval_ai_rerank;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
