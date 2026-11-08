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

#define USING_LOG_PREFIX STORAGE

#include "ob_text_daat_iter.h"

namespace oceanbase
{
namespace storage
{
int ObTextDaaTIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRDaaTIterImpl::init(*param.base_param_, *param.dim_iters_,
                                            *param.allocator_, *param.relevance_collector_))) {
  } else if (param.base_param_->need_project_relevance()
      && OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  } else {
    mode_flag_ = param.mode_flag_;
    function_lookup_mode_ = param.function_lookup_mode_;
    count_only_ = param.count_only_ && !function_lookup_mode_
        && mode_flag_ == sql::ObMatchAgainstMode::NATURAL_LANGUAGE_MODE
        && !param.base_param_->need_project_relevance()
        && !param.base_param_->need_filter() && !param.base_param_->need_pushdown_topk();
  }
  return ret;
}

int ObTextDaaTIter::cache_result(int64_t &count, const ObDatum &id_datum, const double relevance)
{
  if (!count_only_) {
    return ObSRDaaTIterImpl::cache_result(count, id_datum, relevance);
  }
  const common::ObLimitParam &limit = *iter_param_->limit_param_;
  ++input_row_cnt_;
  if (!limit.is_valid() || input_row_cnt_ > limit.offset_) {
    ++count;
    ++output_row_cnt_;
  }
  return limit.is_valid() && output_row_cnt_ >= limit.limit_ ? OB_ITER_END : OB_SUCCESS;
}

int ObTextDaaTIter::project_results(const int64_t count)
{
  return count_only_ ? OB_SUCCESS : ObSRDaaTIterImpl::project_results(count);
}

void ObTextDaaTIter::reset()
{
  bm25_param_estimator_.reset();
  // this class can know the type of dim_iters_, but ObSRDaaTIterImpl maybe not
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRDaaTIterImpl::reset();
}

void ObTextDaaTIter::reuse(const bool switch_tablet)
{
  if ((OB_NOT_NULL(dim_iters_) && 0 == dim_iters_->count())) {
    // do nothing
  } else {
    bm25_param_estimator_.reuse(switch_tablet);
  }
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRDaaTIterImpl::reuse(switch_tablet);
}

int ObTextDaaTIter::pre_process()
{
  int ret = OB_SUCCESS;
  if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (iter_param_->need_project_relevance()) {
    if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
    }
  }
  return ret;
}

int ObTextDaaTIter::do_one_merge_round(int64_t &count)
{
  // For a few membership-only posting lists, a linear selection avoids the
  // heap push/pop/rebuild operations and score reads for every posting. Keep
  // the collector so minimum_should_match and duplicate query terms retain
  // their existing semantics. Ranked, Boolean and larger queries are unchanged.
  if (mode_flag_ != sql::NATURAL_LANGUAGE_MODE || function_lookup_mode_
      || iter_param_->need_project_relevance() || iter_param_->need_filter()
      || iter_param_->need_pushdown_topk() || dim_iters_->count() > 4) {
    return ObSRDaaTIterImpl::do_one_merge_round(count);
  }

  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < next_round_cnt_; ++i) {
    const int64_t idx = next_round_iter_idxes_[i];
    ObISRDaaTDimIter *iter = dim_iters_->at(idx);
    if (OB_ISNULL(iter)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(iter->get_next_row())) {
      if (OB_ITER_END == ret) {
        iter_domain_ids_[idx] = nullptr;
        ret = OB_SUCCESS;
      }
    } else if (OB_FAIL(iter->get_curr_id(iter_domain_ids_[idx]))) {
    } else if (OB_ISNULL(iter_domain_ids_[idx])) {
      ret = OB_ERR_UNEXPECTED;
    }
  }

  ObSRMergeItem minimum;
  next_round_cnt_ = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
    if (nullptr != iter_domain_ids_[i]) {
      ObSRMergeItem item;
      item.iter_idx_ = i;
      int64_t cmp = -1;
      if (minimum.iter_idx_ >= 0 && OB_FAIL(merge_cmp_.cmp(item, minimum, cmp))) {
      } else {
        if (cmp < 0) {
          minimum = item;
          next_round_cnt_ = 0;
        }
        if (cmp <= 0) {
          next_round_iter_idxes_[next_round_cnt_++] = i;
        }
      }
    }
  }

  double relevance = 0.0;
  bool accepted = false;
  if (OB_FAIL(ret)) {
  } else if (minimum.iter_idx_ < 0) {
    ret = OB_ITER_END;
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < next_round_cnt_; ++i) {
      if (OB_FAIL(relevance_collector_->collect_one_dim(next_round_iter_idxes_[i], 1.0))) {
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(relevance_collector_->get_result(relevance, accepted))) {
    } else if (OB_SUCC(ret) && accepted
        && OB_FAIL(cache_result(count, *iter_domain_ids_[minimum.iter_idx_], relevance))) {
    }
  }
  return ret;
}

ObTextBMWIter::ObTextBMWIter()
  : ObSRBMWIterImpl(),
    bm25_param_estimator_() {}

void ObTextBMWIter::reuse(const bool switch_tablet)
{
  bm25_param_estimator_.reuse(switch_tablet);
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRBMWIterImpl::reuse(switch_tablet);
}

void ObTextBMWIter::reset()
{
  bm25_param_estimator_.reset();
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRBMWIterImpl::reset();
}

int ObTextBMWIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRBMWIterImpl::init(*param.base_param_, *param.dim_iters_,
                                           *param.allocator_, *param.relevance_collector_))) {
  } else if (OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  }
  return ret;
}

int ObTextBMWIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
  }

  if (FAILEDx(ObSRBMWIterImpl::get_next_rows(capacity, count))) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    }
  }
  return ret;
}

int ObTextBMWIter::init_before_wand_process()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!bm25_param_estimator_.is_estimated())) {
    ret = OB_ERR_UNEXPECTED;
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
    ObTextRetrievalBlockMaxIter *block_max_iter = static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i));
    if (OB_FAIL(block_max_iter->init_block_max_iter(
        bm25_param_estimator_.get_total_doc_cnt(), bm25_param_estimator_.get_avg_doc_token_cnt()))) {
    }
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase
