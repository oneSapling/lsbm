/*
 * dlsm_param.cc


 *
 *  Created on: Mar 17, 2015
 *      Author: teng
 */

#include "dlsm_param.h"
#include "dbformat.h"
namespace leveldb{

namespace config{

const char *db_path;
int dbmode;

//teng: target file size, default 8 MB
int kTargetFileSize = 8 * 1048576;

//teng: level 0 size, default 100 M
int kL0_size = 100;

//teng: run compaction
bool run_compaction = true;

//teng: bloom filter in use
int bloom_bits_use = -1;

//teng: end level for dlsm mode
int dlsm_end_level = 6;

//teng: key-value cache
int key_cache_size = 0;
}

namespace runtime{

double compaction_min_score = 1;
bool two_phase_compaction = true;
int warm_up_status = 0;
bool need_warm_up = false;
bool print_version_info = false;
bool print_lazy_version_info = false;
int hitratio_interval = 100;
int max_print_level = leveldb::config::LogicalLevelnum-1;
int level0_max_score = config::kL0_SlowdownWritesTrigger/config::kL0_CompactionTrigger;
bool pre_caching = false;
}


}




