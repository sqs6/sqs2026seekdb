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

#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/dict/ob_ft_dict_hub.h"

#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "storage/fts/dict/ob_ft_cache_container.h"
#include "storage/fts/dict/ob_ft_dict_def.h"
#include "storage/fts/dict/ob_ft_range_dict.h"
namespace oceanbase
{
namespace storage
{
int ObFTDictHub::init(ObIAllocator *alloc)
{
  int ret = OB_SUCCESS;
  static constexpr int K_MAX_DICT_BUCKET = 128; // for now, only built-in dicts.
  alloc_ = alloc;
  if (OB_FAIL(dict_map_.create(K_MAX_DICT_BUCKET, "dict_map"))) {
  } else if (OB_FAIL(rw_dict_lock_.init(K_MAX_DICT_BUCKET))) {
  } else {
    is_inited_ = true;
  }
  return ret;
};

int ObFTDictHub::destroy()
{
  int ret = OB_SUCCESS;
  for (int i = 0; i < MAX_DICT_TYPE_NUM; ++i) {
    if (OB_NOT_NULL(shared_dicts_[i])) {
      destroy_shared_dict(shared_dicts_[i]);
      shared_dicts_[i] = nullptr;
    }
  }
  is_inited_ = false;
  return ret;
}

void ObFTDictHub::destroy_shared_dict(ObFTSharedDict *shared)
{
  if (OB_ISNULL(shared) || OB_ISNULL(alloc_)) {
    return;
  }
  if (OB_NOT_NULL(shared->dict_)) {
    shared->dict_->~ObFTRangeDict();
    alloc_->free(shared->dict_);
    shared->dict_ = nullptr;
  }
  if (OB_NOT_NULL(shared->container_)) {
    OB_DELETEx(ObFTCacheRangeContainer, alloc_, shared->container_);
    shared->container_ = nullptr;
  }
  OB_DELETEx(ObFTSharedDict, alloc_, shared);
}

int ObFTDictHub::build_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  ObBucketHashWLockGuard guard(rw_dict_lock_, key.hash());
  ret = build_cache_locked(desc, container);
  return ret;
}

int ObFTDictHub::build_cache_locked(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  ObFTDictInfo info;
  container.reset();

  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    // try if valid with no recursive lock
    if (OB_FAIL(get_dict_info(key, info))) {
      if (OB_HASH_NOT_EXIST == ret) {
        // dict not exist, make new one, by caller
        ret = OB_ENTRY_NOT_EXIST;
      } else {
      }
    } else if (OB_FAIL(ObFTRangeDict::try_load_cache(desc, info.range_count_, container))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
      } else {
      }
    }

    if (OB_FAIL(ret)) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        if (OB_FAIL(ObFTRangeDict::build_cache_from_ik_dict(desc, container))) {
        } else if (FALSE_IT(info.range_count_ = container.get_handles().size())) {
        } else if (OB_FAIL(put_dict_info(key, info))) {
        }
      }
    }
  }
  return ret;
}

int ObFTDictHub::build_shared_dict(const ObFTDictDesc &desc, ObFTSharedDict *&shared)
{
  int ret = OB_SUCCESS;
  shared = nullptr;
  ObFTCacheRangeContainer *container = nullptr;
  ObFTRangeDict *range_dict = nullptr;
  ObFTSharedDict *entry = nullptr;

  if (OB_ISNULL(alloc_)) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(container = OB_NEWx(ObFTCacheRangeContainer, alloc_, *alloc_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(build_cache_locked(desc, *container))) {
    OB_DELETEx(ObFTCacheRangeContainer, alloc_, container);
    container = nullptr;
  } else if (OB_ISNULL(range_dict = OB_NEWx(ObFTRangeDict, alloc_, *alloc_, container, desc))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    OB_DELETEx(ObFTCacheRangeContainer, alloc_, container);
    container = nullptr;
  } else if (OB_FAIL(range_dict->init())) {
    range_dict->~ObFTRangeDict();
    alloc_->free(range_dict);
    OB_DELETEx(ObFTCacheRangeContainer, alloc_, container);
    container = nullptr;
  } else if (OB_ISNULL(entry = OB_NEWx(ObFTSharedDict, alloc_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    range_dict->~ObFTRangeDict();
    alloc_->free(range_dict);
    OB_DELETEx(ObFTCacheRangeContainer, alloc_, container);
    container = nullptr;
  } else {
    entry->container_ = container;
    entry->dict_ = range_dict;
    shared = entry;
  }
  return ret;
}

int ObFTDictHub::get_shared_dict(const ObFTDictDesc &desc, ObIFTDict *&dict)
{
  int ret = OB_SUCCESS;
  dict = nullptr;
  const uint32_t type_idx = static_cast<uint32_t>(desc.type_);
  if (!is_inited_ || OB_ISNULL(alloc_)) {
    ret = OB_NOT_INIT;
  } else if (type_idx >= static_cast<uint32_t>(MAX_DICT_TYPE_NUM)) {
    ret = OB_NOT_SUPPORTED;
  } else {
    ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
    // 构建只发生一次，这里持写锁串行化，避免重复构建。
    ObBucketHashWLockGuard guard(rw_dict_lock_, key.hash());
    ObFTSharedDict *shared = shared_dicts_[type_idx];
    if (OB_ISNULL(shared)) {
      if (OB_FAIL(build_shared_dict(desc, shared))) {
      } else {
        shared_dicts_[type_idx] = shared;
      }
    }
    if (OB_SUCC(ret) && OB_NOT_NULL(shared)) {
      dict = shared->dict_;
    }
  }
  return ret;
}

int ObFTDictHub::load_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  ObFTDictInfo info;
  container.reset();
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    {
      ObBucketHashRLockGuard guard(rw_dict_lock_, key.hash());
      if (OB_FAIL(get_dict_info(key, info))) {
        if (OB_HASH_NOT_EXIST == ret) {
          // dict not exist, make new one, by caller
          ret = OB_ENTRY_NOT_EXIST;
        } else {
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObFTRangeDict::try_load_cache(desc, info.range_count_, container))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        // dict not exist, make new one, by caller
      } else {
      }
    }
  }

  return ret;
}


int ObFTDictHub::get_dict_info(const ObFTDictInfoKey &key, ObFTDictInfo &info)
{
  int ret = OB_SUCCESS;

  if (OB_FAIL(dict_map_.get_refactored(key, info))) {
    if (OB_HASH_NOT_EXIST != ret) {
    }
  }

  return ret;
}

int ObFTDictHub::put_dict_info(const ObFTDictInfoKey &key, const ObFTDictInfo &info)
{
  int ret = OB_SUCCESS;
  const int cover_exist_flag = 1;
  if (OB_FAIL(dict_map_.set_refactored(key, info, cover_exist_flag))) {
  }

  return ret;
}
} //  namespace storage
} //  namespace oceanbase
