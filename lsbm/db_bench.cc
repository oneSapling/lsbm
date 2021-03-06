// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <ctime>
#include <sys/types.h>
#include <unistd.h>
#include <math.h>

#include "generator.h"
#include "db_impl.h"
#include "leveldb/params.h"
#include "version_set.h"
#include "leveldb/cache.h"
#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/write_batch.h"
#include "port/port.h"
#include "util/crc32c.h"
#include "util/histogram.h"
#include "util/mutexlock.h"
#include "util/random.h"
#include "util/testutil.h"


using namespace std;
using namespace leveldb;
// Comma-separated list of operations to run in the specified order
//   Actual benchmarks:
//      fillseq       -- write N values in sequential key order in async mode
//      fillrandom    -- write N values in random key order in async mode
//      overwrite     -- overwrite N values in random key order in async mode
//      fillsync      -- write N/100 values in random key order in sync mode
//      fill100K      -- write N/1000 100K values in random order in async mode
//      deleteseq     -- delete N keys in sequential order
//      deleterandom  -- delete N keys in random order
//      readseq       -- read N times sequentially
//      readreverse   -- read N times in reverse order
//      readrandom    -- read N times in random order
//      readmissing   -- read N missing keys in random order
//      readhot       -- read N times in random order from 1% section of DB
//      seekrandom    -- N random seeks
//      crc32c        -- repeated crc32c of 4K of data
//      acquireload   -- load N*1000 times
//   Meta operations:
//      compact     -- Compact the entire DB
//      stats       -- Print DB stats
//      sstables    -- Print sstable info
//      heapprofile -- Dump a heap profile (if supported by this port)
static const char* FLAGS_benchmarks =
    "seperate"
	"mix"
    ;

// Number of read operations to do.  If negative, infinite.
static int FLAGS_random_reads = -1;

// Number of writes operations to do.  If negative, infinite.
static int FLAGS_writes = -1;

// Number of concurrent threads to run.
static int FLAGS_random_threads = 1;

// Size of each value
static int FLAGS_value_size = 1024;

// Arrange to generate values that shrink to this fraction of
// their original size after compression
//teng: set to 1 (original 0.5)
static double FLAGS_compression_ratio = 1;

// Print histogram of operation timings
//teng: default true
static bool FLAGS_histogram = true;

// Number of bytes to buffer in memtable before compacting
// (initialized to default value by "main")
//teng: set to 8MB by default
static int FLAGS_write_buffer_size = 8*1024*1024;

// Number of Megabytes to use as a cache of uncompressed data.
// Negative means use default settings.
static uint64_t FLAGS_block_cache_size = -1;

static uint64_t FLAGS_key_cache_size = 0;




// Maximum number of files to keep open at the same time (use default if == 64000)
static int FLAGS_open_files = 64000;

// Bloom filter bits per key.
// Negative means use default settings.
//teng: defualt 20
static int FLAGS_bloom_bits = 20;

// If true, do not destroy the existing database.  If you set this
// flag and also specify a benchmark that wants a fresh database, that
// benchmark will fail.
// teng: set to true
static bool FLAGS_use_existing_db = true;

// Use the db with the following name.
static const char* FLAGS_db = NULL;

/************* Extened Flags *****************/

//key range of requests in r/w benchmark

static int64_t FLAGS_write_span = -1;

static int64_t FLAGS_read_span = -1;


/*the noise percentage for rangehot workload*/
static int FLAGS_noise_percent = 5;

/*uniform zipfian latest*/
static char *FLAGS_write_ycsb_workload = NULL;
static char *FLAGS_read_ycsb_workload = NULL;

static int FLAGS_range_size = 10000;

static leveldb::port::Mutex range_query_mu_;
static int range_query_completed_ = 0;
static int last_range_query_ = 0;
static int last_range_count_ = 0;
static int range_total_ = 0;

static int64_t FLAGS_write_throughput = 0;
static int64_t FLAGS_read_throughput = 0;
static int64_t write_latency = 0;
static int64_t read_latency = 0;
static int64_t readwrite_latency = 0;

static int FLAGS_range_portion = 0;

static int FLAGS_batch_size = 10;

//end of teng's parameters

//below are the parameters used by teng for testing
long long llmax_ = 9223372036854775807ll;

static double FLAGS_countdown = -1;

static int FLAGS_random_seed = 301;

static int last_random_read = 0;

static int monitor_interval = -1; //microseconds
static bool first_monitor_interval = true;
static FILE* monitor_log = stdout;

static leveldb::Histogram intv_read_hist_;
static leveldb::Histogram intv_write_hist_;
static double intv_start_;
static leveldb::port::Mutex intv_mu_;
static leveldb::port::Mutex read_mu_;

static int hot_ratio = 1;

static int FLAGS_num = 0;
static int FLAGS_read_portion = 100;
static int FLAGS_throughput = 0;

static int readwrite_complete = 0;
static double FLAGS_zipfian_constant = 0.99;

static int rw_interval = 1;
static int last_rw = 0;

static int latency_gap = 50;

static int write_threads = 0;

static bool FLAGS_range_warmup = true;

/************* Extened Flags (END) *****************/
static bool printstats_ = true;


namespace leveldb {

namespace {

// Helper for quickly generating random data.
class RandomGenerator {
 private:
  std::string data_;
  int pos_;

 public:
  RandomGenerator() {
    // We use a limited amount of data over and over again and ensure
    // that it is larger than the compression window (32KB), and also
    // large enough to serve all typical value sizes we want to write.
    Random rnd(301);
    std::string piece;
    while (data_.size() < 1048576) {
      // Add a short fragment that is as compressible as specified
      // by FLAGS_compression_ratio.
      test::CompressibleString(&rnd, FLAGS_compression_ratio, 100, &piece);
      data_.append(piece);
    }
    pos_ = 0;
  }

  Slice Generate(size_t len) {
    if (pos_ + len > data_.size()) {
      pos_ = 0;
      assert(len < data_.size());
    }
    pos_ += len;
    return Slice(data_.data() + pos_ - len, len);
  }
};

static Slice TrimSpace(Slice s) {
  size_t start = 0;
  while (start < s.size() && isspace(s[start])) {
    start++;
  }
  size_t limit = s.size();
  while (limit > start && isspace(s[limit-1])) {
    limit--;
  }
  return Slice(s.data() + start, limit - start);
}

static void AppendWithSpace(std::string* str, Slice msg) {
  if (msg.empty()) return;
  if (!str->empty()) {
    str->push_back(' ');
  }
  str->append(msg.data(), msg.size());
}

class Stats {
 private:
  double start_;
  double finish_;
  double seconds_;
  int done_;
  int read_done_;
  int write_done_;
  int next_report_;
  int64_t bytes_;
  double last_op_finish_;
  Histogram hist_;
  Histogram read_hist_;
  Histogram write_hist_;
  std::string message_;
  double intv_end_;

 public:
	int tid_;
	int pid_;
  Stats() { Start(); }

  void Start() {
    next_report_ = 100;
    last_op_finish_ = start_;
    hist_.Clear();
    read_hist_.Clear();
    write_hist_.Clear();
    done_ = 0;
    read_done_ = 0;
    write_done_ = 0;
    bytes_ = 0;
    seconds_ = 0;
    start_ = Env::Default()->NowMicros();
    finish_ = start_;
    message_.clear();
  }

  void Merge(const Stats& other) {
    hist_.Merge(other.hist_);
    read_hist_.Merge(other.read_hist_);
    write_hist_.Merge(other.write_hist_);
    done_ += other.done_;
    read_done_ += other.read_done_;
    write_done_ += other.write_done_;
    bytes_ += other.bytes_;
    seconds_ += other.seconds_;
    if (other.start_ < start_) start_ = other.start_;
    if (other.finish_ > finish_) finish_ = other.finish_;

    // Just keep the messages from one thread
    if (message_.empty()) message_ = other.message_;
  }

  void Stop() {
    finish_ = Env::Default()->NowMicros();
    seconds_ = (finish_ - start_) * 1e-6;
  }

  void AddMessage(Slice msg) {
    AppendWithSpace(&message_, msg);
  }

  void FinishedReadOp() {
    if (monitor_interval != -1) {
      double now = Env::Default()->NowMicros();
      double micros = now - last_op_finish_;
      read_hist_.Add(micros);
      intv_read_hist_.AtomicAdd(micros);	
    }
    read_done_++;

    FinishedSingleOp(1);
  }

  void FinishedWriteOp() {
    if (monitor_interval != -1) {
      double now = Env::Default()->NowMicros();
      double micros = now - last_op_finish_;
      write_hist_.Add(micros);
      intv_write_hist_.AtomicAdd(micros); 
    }
    write_done_++;

    FinishedSingleOp(2);
  }

  void FinishedSingleOp(int rw) {
    if (FLAGS_histogram) {
      double now = Env::Default()->NowMicros();
      double micros = now - last_op_finish_;
      hist_.Add(micros);
     /* if (micros > 20000) {
        fprintf(stderr, "long op: %.1f micros%30s\r", micros, "");
        fflush(stderr);
      }*/
      last_op_finish_ = now;
    }

    done_++;
    if (done_ >= next_report_) {
      /*
      if      (next_report_ < 1000)   next_report_ += 100;
      else if (next_report_ < 5000)   next_report_ += 500;
      else if (next_report_ < 10000)  next_report_ += 1000;
      else if (next_report_ < 50000)  next_report_ += 5000;
      else if (next_report_ < 100000) next_report_ += 10000;
      else if (next_report_ < 500000) next_report_ += 50000;
      else                            next_report_ += 100000;*/
      next_report_ += 3000;
      fprintf(stderr, "... finished %d %s\r", done_, rw==1?"reads":"writes");
      fflush(stderr);
    }

    intv_end_ = Env::Default()->NowMicros();
    if (monitor_interval != -1 && intv_end_ - intv_start_ > monitor_interval) {
    	intv_mu_.Lock();
    	if (intv_end_ - intv_start_ > monitor_interval) {
    		if (first_monitor_interval) {
    			fprintf(monitor_log, "\nPID\tTID\tRL\tWL\tRD\tWD\tRT\tWT\n");
    			first_monitor_interval = false;
    		}
    		fprintf(monitor_log, "%d\t%d\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\n",
    				pid_, tid_,
					intv_read_hist_.Average(), intv_write_hist_.Average(), 
					intv_read_hist_.StandardDeviation(), intv_write_hist_.StandardDeviation(), 
					intv_read_hist_.Num() * 1000000 /(intv_end_ - intv_start_),
					intv_write_hist_.Num() * 1000000 /(intv_end_ - intv_start_));

    		intv_start_ = intv_end_;
    		intv_read_hist_.Clear();
    		intv_write_hist_.Clear();
    	}
    	intv_mu_.Unlock();
    }
  }

  void AddBytes(int64_t n) {
    bytes_ += n;
  }

  void Report(const Slice& name) {
    // Pretend at least one op was done in case we are running a benchmark
    // that does not call FinishedSingleOp().
    if (done_ < 1) done_ = 1;

    std::string extra;
    double elapsed = (finish_ - start_) * 1e-6;
    if (bytes_ > 0) {
      // Rate is computed on actual elapsed time, not the sum of per-thread
      // elapsed times.
      char rate[100];
      snprintf(rate, sizeof(rate), "%6.1f MB/s",
               (bytes_ / 1048576.0) / elapsed);
      extra = rate;
    }
    AppendWithSpace(&extra, message_);	// only involve one thread

    fprintf(stdout, "%-12s : %11.3f micros/op;\t%11.3f ops/s%s%s\n",
            name.ToString().c_str(),
            seconds_ * 1e6 / done_,
            done_ / elapsed,
            (extra.empty() ? "" : " "),
            extra.c_str());
    if (FLAGS_histogram) {
      fprintf(stdout, "Microseconds per op:\n%s\n", hist_.ToString().c_str());
    if (read_done_ > 0)
      fprintf(stdout, "Microseconds per ReadOp:\n%s\n", read_hist_.ToString().c_str());
    if (write_done_ > 0)
      fprintf(stdout, "Microseconds per WriteOp:\n%s\n", write_hist_.ToString().c_str());
    }
    fflush(stdout);
  }
};

// State shared by all concurrent executions of the same benchmark.
struct SharedState {
  port::Mutex mu;
  port::CondVar cv;
  int total;

  // Each thread goes through the following states:
  //    (1) initializing
  //    (2) waiting for others to be initialized
  //    (3) running
  //    (4) done

  int num_initialized;
  int num_done;
  bool start;

  SharedState() : cv(&mu){ }
};

// Per-thread state for concurrent executions of the same benchmark.
struct ThreadState {
  int tid;             // 0..n-1 when running in n threads
  Random *rand;         // Threads share the same seed
  Stats stats;
  SharedState* shared;

  ThreadState(int index, Random *r)
      : tid(index),
        rand(r) {
			stats.tid_ = index;
			stats.pid_ = getpid();
  }
};

}  // namespace

class Benchmark {
 private:
  Cache* cache_;
  Cache* key_cache_;
  const FilterPolicy* filter_policy_;
  DB* db_;
  int value_size_;
  int entries_per_batch_;
  WriteOptions write_options_;
  int random_reads_;
  int writes_;
  //int range_reads_;
  int heap_counter_;

  void PrintHeader() {
    const int kKeySize = 16;
    int num = FLAGS_random_reads>0?FLAGS_random_reads:0+FLAGS_writes>0?FLAGS_writes:0;
    PrintEnvironment();
    fprintf(stdout, "Keys:       %d bytes each\n", kKeySize);
    fprintf(stdout, "Values:     %d bytes each (%d bytes after compression)\n",
            FLAGS_value_size,
            static_cast<int>(FLAGS_value_size * FLAGS_compression_ratio + 0.5));
    fprintf(stdout, "Entries:    %d\n", num);
    fprintf(stdout, "RawSize:    %.1f MB (estimated)\n",
            ((static_cast<int64_t>(kKeySize + FLAGS_value_size) * num)
             / 1048576.0));
    fprintf(stdout, "FileSize:   %.1f MB (estimated)\n",
            (((kKeySize + FLAGS_value_size * FLAGS_compression_ratio) * num)
             / 1048576.0));
    PrintWarnings();
    fprintf(stdout, "------------------------------------------------\n");
  }

  void PrintWarnings() {
#if defined(__GNUC__) && !defined(__OPTIMIZE__)
    fprintf(stdout,
            "WARNING: Optimization is disabled: benchmarks unnecessarily slow\n"
            );
#endif
#ifndef NDEBUG
    fprintf(stdout,
            "WARNING: Assertions are enabled; benchmarks unnecessarily slow\n");
#endif

    // See if snappy is working by attempting to compress a compressible string
    const char text[] = "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy";
    std::string compressed;
    if (!port::Snappy_Compress(text, sizeof(text), &compressed)) {
      fprintf(stdout, "WARNING: Snappy compression is not enabled\n");
    } else if (compressed.size() >= sizeof(text)) {
      fprintf(stdout, "WARNING: Snappy compression is not effective\n");
    }
  }

  void PrintEnvironment() {
    fprintf(stderr, "LevelDB:    version %d.%d\n",
            kMajorVersion, kMinorVersion);

#if defined(__linux)
    time_t now = time(NULL);
    fprintf(stderr, "Date:       %s", ctime(&now));  // ctime() adds newline

    FILE* cpuinfo = fopen("/proc/cpuinfo", "r");
    if (cpuinfo != NULL) {
      char line[1000];
      int num_cpus = 0;
      std::string cpu_type;
      std::string cache_size;
      while (fgets(line, sizeof(line), cpuinfo) != NULL) {
        const char* sep = strchr(line, ':');
        if (sep == NULL) {
          continue;
        }
        Slice key = TrimSpace(Slice(line, sep - 1 - line));
        Slice val = TrimSpace(Slice(sep + 1));
        if (key == "model name") {
          ++num_cpus;
          cpu_type = val.ToString();
        } else if (key == "cache size") {
          cache_size = val.ToString();
        }
      }
      fclose(cpuinfo);
      fprintf(stderr, "CPU:        %d * %s\n", num_cpus, cpu_type.c_str());
      fprintf(stderr, "CPUCache:   %s\n", cache_size.c_str());
    }
#endif
  }

 public:
  Benchmark()
  : cache_(FLAGS_block_cache_size > 0 ? NewLRUCache(FLAGS_block_cache_size) : NULL),
	key_cache_(FLAGS_key_cache_size > 0 ? NewLRUCache(FLAGS_key_cache_size) : NULL),
    filter_policy_(FLAGS_bloom_bits >= 0
                   ? NewBloomFilterPolicy(FLAGS_bloom_bits)
                   : NULL),
    db_(NULL),
    value_size_(FLAGS_value_size),
    entries_per_batch_(FLAGS_batch_size),
    random_reads_(FLAGS_random_reads),
    writes_(FLAGS_writes),
    heap_counter_(0) {

    std::vector<std::string> files;
    Env::Default()->GetChildren(FLAGS_db, &files);
    for (size_t i = 0; i < files.size(); i++) {
      if (Slice(files[i]).starts_with("heap-")) {
        Env::Default()->DeleteFile(std::string(FLAGS_db) + "/" + files[i]);
      }
    }
    if (!FLAGS_use_existing_db) {
      DestroyDB(FLAGS_db, Options());
    }
  }

  ~Benchmark() {

    delete db_;
	if(cache_){
		delete cache_;
	}
	if(key_cache_){
		delete key_cache_;
	}
    delete filter_policy_;

  }

  void Run() {

    PrintHeader();
    Open();

    const char* benchmarks = FLAGS_benchmarks;
    while (benchmarks != NULL) {
      const char* sep = strchr(benchmarks, ',');
      Slice name;
      if (sep == NULL) {
        name = benchmarks;
        benchmarks = NULL;
      } else {
        name = Slice(benchmarks, sep - benchmarks);
        benchmarks = sep + 1;
      }

      // Reset parameters that may be overriddden below
      random_reads_ = FLAGS_random_reads;
      writes_ = FLAGS_writes;
      value_size_ = FLAGS_value_size;
      entries_per_batch_ = FLAGS_batch_size;
      write_options_ = WriteOptions();

      void (Benchmark::*method)(ThreadState*) = NULL;
      bool fresh_db = false;

      if (name == Slice("separate")) {
        method = &Benchmark::Random_Write;
        monitor_interval = 2000000;
      } else if(name == Slice("mix")) {
        method = &Benchmark::ReadWrite;
        monitor_interval = 2000000;
      } else {
        if (name != Slice()) {  // No error message for empty name
          fprintf(stderr, "unknown benchmark '%s'\n", name.ToString().c_str());
        }
      }

      if (fresh_db) {
        if (FLAGS_use_existing_db) {
          fprintf(stdout, "%-12s : skipped (--use_existing_db is true)\n",
                  name.ToString().c_str());
          method = NULL;
        } else {
          delete db_;
          db_ = NULL;
          DestroyDB(FLAGS_db, Options());
          Open();
        }
      }

      if (method != NULL) {
        RunBenchmark(name, method);
      }
    }
  }

 private:
  struct ThreadArg {
    Benchmark* bm;
    SharedState* shared;
    ThreadState* thread;
    void (Benchmark::*method)(ThreadState*);
  };

  static void ThreadBody(void* v) {
    ThreadArg* arg = reinterpret_cast<ThreadArg*>(v);
    SharedState* shared = arg->shared;
    ThreadState* thread = arg->thread;
    {
      MutexLock l(&shared->mu);
      shared->num_initialized++;
      if (shared->num_initialized >= shared->total) {
        shared->cv.SignalAll();
      }
      while (!shared->start) {
        shared->cv.Wait();
      }
    }

    thread->stats.Start();
    (arg->bm->*(arg->method))(thread);
    thread->stats.Stop();

    {
      MutexLock l(&shared->mu);
      shared->num_done++;
      if (shared->num_done >= shared->total) {
        shared->cv.SignalAll();
      }
    }
  }

  void RunBenchmark(Slice name, void (Benchmark::*method)(ThreadState*)) {

	/*read_latency is the overall latency, latency for each thread should longer*/

    int write_thread,random_read_thread,threads;

	if(method==&Benchmark::ReadWrite){//mix mode
    	write_thread = 0;
    	random_read_thread = 0;
    	threads = (FLAGS_num ==0 || FLAGS_throughput==0)?0:1;
    }else{//separate mode
    	write_thread = (FLAGS_writes==0||FLAGS_write_throughput==0)?0:1;
    	random_read_thread = (FLAGS_random_reads==0||FLAGS_read_throughput==0)?0:FLAGS_random_threads;
    	threads=0;
    	read_latency = read_latency*(random_read_thread);
    }
	write_threads = write_thread;
    if(random_read_thread==0&&threads==0){
       	runtime::need_warm_up = false;
    }

    int n = write_thread+random_read_thread+threads;
  	Random rand_(FLAGS_random_seed);
    SharedState shared;
    shared.total = n;
    shared.num_initialized = 0;
    shared.num_done = 0;
    shared.start = false;

	//Intialization for request latency monitoring
	intv_read_hist_.Clear();
	intv_write_hist_.Clear();
	intv_start_ = Env::Default()->NowMicros();

    ThreadArg* arg = new ThreadArg[n];
    int i=0;
    for (; i < write_thread; i++) {
      arg[i].bm = this;
      arg[i].method = &Benchmark::Random_Write;
      arg[i].shared = &shared;
      arg[i].thread = new ThreadState(i, &rand_);
      arg[i].thread->shared = &shared;
      Env::Default()->StartThread(ThreadBody, &arg[i]);
    }
    for (; i < write_thread+random_read_thread; i++) {
      arg[i].bm = this;
      arg[i].method = &Benchmark::Random_Read;
      arg[i].shared = &shared;
      arg[i].thread = new ThreadState(i, &rand_);
      arg[i].thread->shared = &shared;
      Env::Default()->StartThread(ThreadBody, &arg[i]);
    }
    for (; i < n; i++) {
      arg[i].bm = this;
      arg[i].method =&Benchmark::ReadWrite;
      arg[i].shared = &shared;
      arg[i].thread = new ThreadState(i, &rand_);
      arg[i].thread->shared = &shared;
      Env::Default()->StartThread(ThreadBody, &arg[i]);
    }
    shared.mu.Lock();
    while (shared.num_initialized < n) {
      shared.cv.Wait();
    }

    shared.start = true;
    shared.cv.SignalAll();
    while (shared.num_done < n) {

      shared.cv.Wait();
    }
    shared.mu.Unlock();

    for (int i = 1; i < n; i++) {
      arg[0].thread->stats.Merge(arg[i].thread->stats);
    }
    if(write_thread+random_read_thread+threads)
    arg[0].thread->stats.Report(name);

    for (int i = 0; i < n; i++) {
      delete arg[i].thread;
    }
    delete[] arg;

  }

  void Open() {

    assert(db_ == NULL);
    Options options;
    options.create_if_missing = true;//!FLAGS_use_existing_db;
    options.block_cache = cache_;
    options.key_cache_ = key_cache_;
    options.write_buffer_size = FLAGS_write_buffer_size;
    options.max_open_files = FLAGS_open_files;
    options.filter_policy = filter_policy_;
    options.compression = leveldb::kNoCompression;

    Status s = DB::Open(options, FLAGS_db, &db_);
    if (!s.ok()) {
      fprintf(stderr, "open error: %s\n", s.ToString().c_str());
      exit(1);
    }
  }

  void ConductWarmUp(){
//  	if(!cache_&&!key_cache_){
//  		runtime::warm_up_status = 2;
//  		return;
//  	}
  	ReadOptions options;

  	if(false||key_cache_){
  		std::string value;
  		char key[100];
  		//align k
  		uint64_t k = (runtime::read_from_/hot_ratio)*hot_ratio;

  		while(true){


  			if(k>=runtime::read_upto_||random_reads_==0){
  			  break;
  			}
  			k += hot_ratio;

  			snprintf(key, sizeof(key), "user%019ld",k);
  			db_->Get(options,key,&value);
  			double memused = 0,keyused = 0;
  			fprintf(stderr,"%10ld\%10ld ",k,runtime::read_upto_);
  			if(cache_){
  				memused = 100.0*(cache_->Percent());
  				fprintf(stderr,"block cache usage %f",memused);
  			}
  			if(key_cache_){
  				keyused = 100.0*(key_cache_->Percent());
  				fprintf(stderr,"key cache usage %f",keyused);
  			}
  			fprintf(stderr,"\n");
  			//if((cache_&&memused>=99)&&(key_cache_&&keyused>=99))
  			//{
  				//break;
  			//}

  		}


  	}else{
  	  uint64_t step = 100000;
  	  uint64_t key_from = runtime::read_from_;
  	  uint64_t key_to = key_from + step;
  	  uint64_t key_upto = runtime::read_upto_;
  	  char startch[100],endch[100];

  	  if(strcmp(FLAGS_read_ycsb_workload,"zipfian")==0){

  		key_upto = runtime::read_from_ + (FLAGS_block_cache_size/1024)/2;

  	  }


  	  while(key_from<key_upto){
  		  key_to = key_from+step;
  		  if(key_to>key_upto){
  			  key_to = key_upto;
  		  }

  		  snprintf(startch, sizeof(startch), "user%019ld", key_from);
  		  snprintf(endch, sizeof(endch), "user%019ld", key_to);

  		   //printf("%ld %ld\n",read_key_from, read_key_to);
  		   std::string startstr(startch);
  		   std::string endstr(endch);
  		   Slice start(startstr);
  		   Slice end(endstr);
  		   db_->RangeQuery(options,start,end);
  		  	fprintf(stderr,"%10ld\%10ld ",key_to,key_upto);
  			if(cache_){
  				fprintf(stderr,"block cache usage %f",100.0*(cache_->Percent()));
  			}
  			fprintf(stderr,"\n");
  			key_from += step;
  	  }

  	}

  	runtime::warm_up_status = 2;
  }


void WaitForWarmUp(){
	//printf("enter wait %d\n",type);
	while(leveldb::runtime::needWarmUp()&&!leveldb::runtime::doneWarmUp()){
	   Env::Default()->SleepForMicroseconds(100000);
	}
	//printf("leave wait %d\n",type);
}


void Random_Read000(ThreadState* thread) {
	generator::IntegerGenerator  *mygenerator = new generator::ZipfianGenerator(runtime::read_from_,(long int)runtime::read_upto_, FLAGS_zipfian_constant);

	int span = (runtime::read_upto_-runtime::read_from_);
	int gap = 1000;
	int arraycount = span/gap;
	int count[arraycount];
	for(int i=0;i<arraycount;i++){
		count[i]=0;
	}
	for(int i=0;i<span;i++){
		int key = mygenerator->nextInt();
		count[key/gap]++;
	}

	double cum = 0;
	for(int i=0;i<arraycount;i++){
		cum += (double)count[i]/span;
		printf("distribution|%d|%f|%f\n",i,(double)count[i]/span,cum);
	}
}
void Random_Read(ThreadState* thread) {

    ReadOptions randomoptions;
    ReadOptions rangeoptions;
    rangeoptions.range_query_ = true;
    rangeoptions.fill_cache = true;

    std::string value;
    Status s;
    bool isFound;
    time_t begin, now;
	struct timeval start, end;
	int64_t latency = 0;

    int found = 0;
    int done_random = 0;
    int done_range = 0;
    char key[100];


	generator::IntegerGenerator *mygenerator;
	generator::IntegerGenerator *noisegenerator = new generator::UniformIntegerGenerator(runtime::key_from_,runtime::key_upto_);
	if(FLAGS_read_ycsb_workload==NULL){
		mygenerator = new generator::CounterGenerator(runtime::read_from_,runtime::read_upto_);
	}
	else if(strcmp(FLAGS_read_ycsb_workload,"zipfian")==0){
	   mygenerator = new generator::ZipfianGenerator(runtime::read_from_,(long int)runtime::read_upto_, FLAGS_zipfian_constant);
	}
	else if(strcmp(FLAGS_read_ycsb_workload,"latest")==0){
	   generator::CounterGenerator base(runtime::read_upto_);
	   mygenerator = new generator::SkewedLatestGenerator(base);
	}
	else if(strcmp(FLAGS_read_ycsb_workload,"uniform")==0){
	   mygenerator = new generator::UniformIntegerGenerator(runtime::read_from_,runtime::read_upto_);
	   int repeattimes = random()*100000;
	   for(int i=0;i<repeattimes;i++){
		   mygenerator->nextInt();
	   }
	}
	else {
	   mygenerator = new generator::CounterGenerator(runtime::read_from_,runtime::read_upto_);
	}
/*
	if(cache_==NULL&&key_cache_==NULL){
		leveldb::runtime::need_warm_up = false;
	}
*/
    double noise_percent = (double)FLAGS_noise_percent/(1-(double)(runtime::read_upto_-runtime::read_from_)/(runtime::key_upto_-runtime::key_from_));

	bool startwarmup = false;
	if(runtime::needWarmUp()){
		read_mu_.Lock();
		startwarmup = runtime::notStartWarmUp();
		if(startwarmup){
			runtime::warm_up_status = 1;
		}
		read_mu_.Unlock();
		if(startwarmup){
			ConductWarmUp();
		}
		else{
			WaitForWarmUp();
		}
	}


	time(&begin);

    uint64_t k=0;
    int count = 0;
    int tmpcount = 0;
    char startch[100],endch[100];
    time_t start_intv = 0, now_intv = 0;

    gettimeofday(&start,NULL);
    while(true)
    {
       if(start_intv==0){
    	   time(&start_intv);
       }
       time(&now);
	   if (difftime(now, begin) > FLAGS_countdown || (runtime::num_reads_>=random_reads_&&random_reads_>=0)){
    		break;
	   }

       if(FLAGS_read_throughput==0){
    	   Env::Default()->SleepForMicroseconds(FLAGS_countdown*1000000);
    	   break;
       }
       if(random()*100>noise_percent){
           k = (mygenerator->nextInt()/hot_ratio)*hot_ratio;
       }else{
           k = noisegenerator->nextInt();
       }

       if(random()*100>FLAGS_range_portion){
    	  snprintf(key, sizeof(key), "user%019ld",k);
		  s = db_->Get(randomoptions, key, &value);
		  isFound = s.ok();
		  done_random++;
		  found += isFound;
		  thread->stats.FinishedReadOp();
		  read_mu_.Lock();
	      runtime::num_reads_++;
	      runtime::num_found_ += isFound;
		  read_mu_.Unlock();

       }else{//range query
		  long long read_key_from,read_key_to;
		  read_key_from = k;
		  read_key_to = read_key_from+FLAGS_range_size;
		  if(read_key_to>runtime::key_upto_){
			  read_key_to = runtime::key_upto_;
		  }

		   snprintf(startch, sizeof(startch), "user%019lld", read_key_from);
		   snprintf(endch, sizeof(endch), "user%019lld", read_key_to);

		   //printf("%ld %ld\n",read_key_from, read_key_to);
		   std::string startstr(startch);
		   std::string endstr(endch);
		   Slice start(startstr);
		   Slice end(endstr);
		 tmpcount = db_->RangeQuery(rangeoptions,start,end);

		 count += tmpcount;
		 done_range++;
		 read_mu_.Lock();
		 range_total_ += tmpcount;
		 range_query_completed_++;
		 read_mu_.Unlock();

       }

       if(thread->tid == write_threads){
    	   time(&now_intv);
    	   if(difftime(now_intv,start_intv)>=rw_interval){
    		 if(runtime::num_reads_-last_random_read!=0){
    			 fprintf(stdout,"readop: finishd %ld ops in %d sec, timepassed: %d\n",runtime::num_reads_-last_random_read,rw_interval,(int)difftime(now_intv,begin));
    	     }
    	     last_random_read = runtime::num_reads_;
    	     if(range_query_completed_-last_range_query_!=0){
    	    	 fprintf(stdout,"rangeop: finishd %d ops in %d sec, averagely %d found in each query, timepassed: %d\n",
					range_query_completed_-last_range_query_,
					rw_interval,
					(range_total_-last_range_count_)/(range_query_completed_-last_range_query_),
					(int)difftime(now,begin));
    	     }
    	     last_range_query_ = range_query_completed_;
    	     last_range_count_ = range_total_;
    	     start_intv = now_intv;
    	   }
       }
//       if(done%latency_gap==0){
//	      gettimeofday(&end,NULL);
//	      latency = (end.tv_sec-start.tv_sec)*1000000+(end.tv_usec-start.tv_usec);
//	      if(FLAGS_read_throughput>0&&latency_gap*read_latency>latency)
//	      {
//	       Env::Default()->SleepForMicroseconds(read_latency*latency_gap-latency);
//	      }
//	      gettimeofday(&start,NULL);
//       }


    }//end while




    time(&now);
    if(done_random>0){
		fprintf(stdout,"completes %d read ops (out of %ld) in %.3f seconds, %d found\n",
		  done_random, runtime::num_reads_, difftime(now, begin), found);
    }
    if(done_range>0){
        fprintf(stdout,"totally operated %d range queries (out of %d), and %d results (out of %d) are found\n"
            		,done_range,range_query_completed_,count,range_total_);
    }


    if(thread->tid == write_threads){
        char msg[100];
        snprintf(msg, sizeof(msg), "totally %ld of %ld found", runtime::num_found_, runtime::num_reads_);
        thread->stats.AddMessage(msg);
    }

    delete noisegenerator;
    delete mygenerator;

}


void Random_Read1(ThreadState* thread) {

    ReadOptions options;

    std::string value;
    Status s;
    bool isFound;
    time_t begin, now;
	struct timeval start, end;
	int64_t latency = 0;

    int found = 0;
    int done = 0;
    char key[100];


	generator::IntegerGenerator *mygenerator;
	generator::IntegerGenerator *noisegenerator = new generator::UniformIntegerGenerator(runtime::key_from_,runtime::key_upto_);
	if(FLAGS_read_ycsb_workload==NULL){
		mygenerator = new generator::CounterGenerator(runtime::read_from_,runtime::read_upto_);
	}
	else if(strcmp(FLAGS_read_ycsb_workload,"zipfian")==0){
	   mygenerator = new generator::ZipfianGenerator(runtime::read_from_,(long int)runtime::read_upto_, FLAGS_zipfian_constant);
	}
	else if(strcmp(FLAGS_read_ycsb_workload,"latest")==0){
	   generator::CounterGenerator base(runtime::read_upto_);
	   mygenerator = new generator::SkewedLatestGenerator(base);
	}
	else if(strcmp(FLAGS_read_ycsb_workload,"uniform")==0){
	   mygenerator = new generator::UniformIntegerGenerator(runtime::read_from_,runtime::read_upto_);
	}
	else {
	   mygenerator = new generator::CounterGenerator(runtime::read_from_,runtime::read_upto_);
	}
/*
	if(cache_==NULL&&key_cache_==NULL){
		leveldb::runtime::need_warm_up = false;
	}
*/
	bool startwarmup = false;
	if(runtime::needWarmUp()){
		read_mu_.Lock();
		startwarmup = runtime::notStartWarmUp();
		if(startwarmup){
			runtime::warm_up_status = 1;
		}
		read_mu_.Unlock();
		if(startwarmup){
			ConductWarmUp();
			//db_->RefineCompactionBuffer();
		}
		else{
			WaitForWarmUp();
		}
	}


	time(&begin);

    uint64_t k=0;
    time_t start_intv = 0, now_intv = 0;
    gettimeofday(&start,NULL);
    while(true)
    {
       if(start_intv==0){
    	   time(&start_intv);
       }
       time(&now);
	   if (difftime(now, begin) > FLAGS_countdown || (runtime::num_reads_>=random_reads_&&random_reads_>=0)){
    		break;
	   }

       if(FLAGS_read_throughput==0){
    	   Env::Default()->SleepForMicroseconds(FLAGS_countdown*1000000);
    	   break;
       }
       if(random()*100>FLAGS_noise_percent){
           k = (mygenerator->nextInt()/hot_ratio)*hot_ratio;
       }else{
           k = noisegenerator->nextInt();
       }
       snprintf(key, sizeof(key), "user%019ld",k);
       s = db_->Get(options, key, &value);
       isFound = s.ok();
       done++;
       if (isFound) {
         found++;
       }
       thread->stats.FinishedReadOp();

       read_mu_.Lock();
       runtime::num_reads_++;
       if(thread->tid == write_threads){
    	   time(&now_intv);
    	   if(difftime(now_intv,start_intv)>=rw_interval){
    	     fprintf(stdout,"readop: finishd %ld ops in %d sec, timepassed: %d\n",runtime::num_reads_-last_random_read,rw_interval,(int)difftime(now_intv,begin));
    	     last_random_read = runtime::num_reads_;
    	     start_intv = now_intv;
    	   }
       }
       read_mu_.Unlock();
       if(done%latency_gap==0){
	      gettimeofday(&end,NULL);
	      latency = (end.tv_sec-start.tv_sec)*1000000+(end.tv_usec-start.tv_usec);
	      if(FLAGS_read_throughput>0&&latency_gap*read_latency>latency)
	      {
	       Env::Default()->SleepForMicroseconds(read_latency*latency_gap-latency);
	      }
	      gettimeofday(&start,NULL);
       }


    }//end while

    char msg[100];
    snprintf(msg, sizeof(msg), "(%d of %d found in one read thread)", found, done);
    thread->stats.AddMessage(msg);

    time(&now);
    fprintf(stdout,"completes %d read ops (out of %ld) in %.3f seconds, %d found\n",
      done, runtime::num_reads_, difftime(now, begin), found);

}

double random()
{
	return (double)(rand()/(double)RAND_MAX);
}
//void Range_Read(ThreadState* thread) {
//    ReadOptions options;
//    options.fill_cache = false;
//    time_t begin, now;
//    time_t start_intv;
//    char key[100];
//    std::string value;
//
//
//    bool printstats = false;
//    bool startwarmup = false;
//    read_mu_.Lock();
//    if(printstats_){
//    	printstats_ = false;
//    	printstats = true;
//    }
//	read_mu_.Unlock();
//
//	if(runtime::needWarmUp()){
//		read_mu_.Lock();
//		startwarmup = runtime::notStartWarmUp();
//		if(startwarmup){
//			runtime::warm_up_status = 1;
//		}
//		read_mu_.Unlock();
//		if(startwarmup){
//			ConductWarmUp();
//		}
//		else{
//			WaitForWarmUp();
//		}
//	}
//	generator::IntegerGenerator *mygenerator;
//	generator::IntegerGenerator *noisegenerator = new generator::UniformIntegerGenerator(FLAGS_key_from,FLAGS_key_upto);
//	if(FLAGS_read_ycsb_workload==NULL){
//		mygenerator = new generator::CounterGenerator(runtime::read_from_,runtime::read_upto_);
//	}
//	else if(strcmp(FLAGS_read_ycsb_workload,"zipfian")==0){
//	   mygenerator = new generator::ZipfianGenerator(runtime::read_from_,(long int)runtime::read_upto_, FLAGS_zipfian_constant);
//	}
//	else if(strcmp(FLAGS_read_ycsb_workload,"latest")==0){
//	   generator::CounterGenerator base(runtime::read_upto_);
//	   mygenerator = new generator::SkewedLatestGenerator(base);
//	}
//	else if(strcmp(FLAGS_read_ycsb_workload,"uniform")==0){
//	   mygenerator = new generator::UniformIntegerGenerator(runtime::read_from_,runtime::read_upto_);
//	}
//	else {
//	   mygenerator = new generator::CounterGenerator(runtime::read_from_,runtime::read_upto_);
//	}
//
//    int done = 0;
//    int count = 0;
//    int tmpcount = 0;
//    char startch[100],endch[100];
//	time(&begin);
//	time(&start_intv);
//    while(true)
//    {
//      time(&now);
//	  if (difftime(now, begin) > FLAGS_countdown){
//    		break;
//	  }
//      if(range_query_completed_>=range_reads_&&range_reads_>=0){
//    	           break;
//      }
//
//		long long read_key_from,read_key_to;
//		if(100*random()<FLAGS_noise_percent){
//		  read_key_from = noisegenerator->nextInt();
//		  read_key_to = read_key_from+FLAGS_range_size;
//		}else{
//		  read_key_from = mygenerator->nextInt();
//		  read_key_to = read_key_from+FLAGS_range_size;
//		  if(read_key_to>runtime::read_upto_){
//			  read_key_to = runtime::read_upto_;
//		  }
//		}
//
//        snprintf(startch, sizeof(startch), "user%019lld", read_key_from);
//        snprintf(endch, sizeof(endch), "user%019lld", read_key_to);
//
//        std::string startstr(startch);
//        std::string endstr(endch);
//        Slice start(startstr);
//        Slice end(endstr);
//      tmpcount = db_->RangeQuery(options,start,end);
//
//      count += tmpcount;
//      done++;
//      range_query_mu_.Lock();
//      range_total_ += tmpcount;
//      range_query_completed_++;
//
//      if(printstats){
//		  time(&now);
//		  if(difftime(now,start_intv)>=rw_interval){
//				fprintf(stdout,"rangeop: finishd %d ops in %d sec, averagely %d found in each query, timepassed: %d\n",
//						range_query_completed_-last_range_query_,
//						rw_interval,
//						(range_total_-last_range_count_)/(range_query_completed_-last_range_query_),
//						(int)difftime(now,begin));
//				last_range_query_ = range_query_completed_;
//				last_range_count_ = range_total_;
//				start_intv = now;
//		  }
//      }
//
//      range_query_mu_.Unlock();
//    }//end while
//
//    fprintf(stdout,"\ntotally operated %d range queries (out of %d), and %d results (out of %d) are found\n"
//    		,done,range_query_completed_,count,range_total_);
//}
/*modification required for key*/

void Random_Write(ThreadState* thread) {

   RandomGenerator gen;
   WriteBatch batch;
   Status s;
   int64_t bytes = 0;

   time_t begin, now;
	struct timeval start, end;
	int64_t latency = 0;
   int bnum = 0;
   batch.Clear();

   if(writes_>0)
       fprintf(stderr, "RWRandom_Write will try to write %d ops\n", writes_);

   int done = 0;

   uint64_t key_span = runtime::write_upto_-runtime::write_from_;
   uint64_t *kset = new uint64_t[key_span];

   bool iscounter = true;
   if(strcmp(FLAGS_write_ycsb_workload,"uniform")==0){
	   for(int i=0;i<key_span;i++){
		   kset[i] = runtime::write_from_+i;
	   }
	   printf("initiating writer......\n");
	   generator::shuffle(kset,key_span);
	   iscounter = false;
   }else if(strcmp(FLAGS_write_ycsb_workload,"rcounter")==0){
	   for(int i=0;i<key_span;i++){
		   kset[i] = runtime::write_upto_-i;
	   }
   }else {
	   for(int i=0;i<key_span;i++){
		   kset[i] = runtime::write_from_+i;
	   }
   }

	WaitForWarmUp();

   time(&begin);

	uint64_t k;
	time_t start_intv = 0, now_intv = 0;
	gettimeofday(&start,NULL);
	int last_write = 0;
   while(true){
     char key[100];
     time(&now);
     if(start_intv==0){
   	  time(&start_intv);
     }
	  if (difftime(now, begin) >= FLAGS_countdown-1)
         break;
     if((done>=writes_&&writes_>=0)||(runtime::write_cursor>=key_span&&iscounter))
     {
   	  if (bnum != 0) {
   		  bnum = 0;
   	      s = db_->Write(write_options_, &batch);
   	      batch.Clear();
   	      if (!s.ok()) {
   	           fprintf(stderr, "put error: %s\n", s.ToString().c_str());
   	           exit(1);
   	      }
   	      thread->stats.AddBytes(bytes);
   	      bytes = 0;
   	  }
   	  if (difftime(now, begin) > FLAGS_countdown-1){
	            break;
   	  }
   	  else{
              fprintf(stderr,"I have finished %d write and now I need to sleep for %d seconds %ld\n",writes_, (int)(FLAGS_countdown-difftime(now,begin)),runtime::write_cursor);
              break;
   	  }
     }

     if(++runtime::write_cursor >= key_span){
		 runtime::write_cursor = 0;
	 }
     k = kset[runtime::write_cursor];

     snprintf(key, sizeof(key), "user%019ld", k);
     Slice keyslice(key);
     Slice valueslice = gen.Generate(value_size_);
     bytes += value_size_ + strlen(key);

     batch.Put(keyslice, valueslice);
     db_->UpdateKeyCache(keyslice,valueslice);

     bnum ++;
     done ++;

     if (bnum == entries_per_batch_) {
         bnum = 0;
         s = db_->Write(write_options_, &batch);
         batch.Clear();
         if (!s.ok()) {
           fprintf(stderr, "put error: %s\n", s.ToString().c_str());
           exit(1);
         }
         thread->stats.AddBytes(bytes);
         bytes = 0;
      }
     thread->stats.FinishedWriteOp();
	  runtime::num_writes_++;
	  time(&now_intv);
	  if(difftime(now_intv,start_intv)>=rw_interval){
	 	 fprintf(stdout,"writeop: finishd %ld ops out of %ld, timepassed: %d writecursor: %ld\n",runtime::num_writes_-last_write,runtime::num_writes_,(int)difftime(now_intv,begin),runtime::write_cursor);
	     //fprintf(stdout,"writeop: finishd %ld ops in %d sec, timepassed: %d %ld\n",runtime::num_writes_-last_write,rw_interval,(int)difftime(now_intv,begin),runtime::write_cursor);
	     last_write = runtime::num_writes_;
	     time(&start_intv);
	  }
     if(done%latency_gap==0){
	    gettimeofday(&end,NULL);
	    latency = (end.tv_sec-start.tv_sec)*1000000+(end.tv_usec-start.tv_usec);
	    if(write_latency*latency_gap>latency&&FLAGS_write_throughput>0)
	    {
	       Env::Default()->SleepForMicroseconds(write_latency*latency_gap-latency);
	    }
		gettimeofday(&start,NULL);

     }
   }

   delete[] kset;
   time(&now);
   fprintf(stdout,"completes %d write ops in %.3f seconds\n",done, difftime(now, begin));

}

  void Random_Write1(ThreadState* thread) {
	if(FLAGS_write_throughput==0)
	{
	  return;
	}

    RandomGenerator gen;
    WriteBatch batch;
    Status s;
    int64_t bytes = 0;
	
    time_t begin, now;
	struct timeval start, end;
	int64_t latency = 0;
    int bnum = 0;
    batch.Clear();

    if(writes_>0)
        fprintf(stderr, "RWRandom_Write will try to write %d ops\n", writes_);

    uint64_t done = 0;
	generator::IntegerGenerator *mygenerator;
	if(FLAGS_write_ycsb_workload==NULL){
	   mygenerator = new generator::CounterGenerator(runtime::write_from_,runtime::write_upto_);
    }
	else if(strcmp(FLAGS_write_ycsb_workload,"zipfian")==0){
	   mygenerator = new generator::ZipfianGenerator(runtime::write_from_,(long int )runtime::write_upto_,FLAGS_zipfian_constant);
	}
	else if(strcmp(FLAGS_write_ycsb_workload,"latest")==0){
	  generator::CounterGenerator base(runtime::write_upto_);
	  mygenerator = new generator::SkewedLatestGenerator(base);
	}
	else if(strcmp(FLAGS_write_ycsb_workload,"uniform")==0){
	  mygenerator = new generator::UniformIntegerGenerator(runtime::write_from_,runtime::write_upto_);
	}
	else{
	  mygenerator = new generator::CounterGenerator(runtime::write_from_,runtime::write_upto_);
	}

	WaitForWarmUp();

    time(&begin);

	int k;
	time_t start_intv = 0, now_intv = 0;
	gettimeofday(&start,NULL);
	int last_write = 0;
    while(true){
      char key[100];
      time(&now);
      if(start_intv==0){
    	  time(&start_intv);
      }
	  if (difftime(now, begin) >= FLAGS_countdown-1)
          break;
      if(done>=writes_&&writes_>=0)
      {
    	  if (bnum != 0) {
    		  bnum = 0;
    	      s = db_->Write(write_options_, &batch);
    	      batch.Clear();
    	      if (!s.ok()) {
    	           fprintf(stderr, "put error: %s\n", s.ToString().c_str());
    	           exit(1);
    	      }
    	      thread->stats.AddBytes(bytes);
    	      bytes = 0;
    	  }
    	  break;

      }

      k = mygenerator->nextInt();
      snprintf(key, sizeof(key), "user%019d", k);
      Slice keyslice(key);
      Slice valueslice = gen.Generate(value_size_);
      bytes += value_size_ + strlen(key);

      batch.Put(keyslice, valueslice);
      db_->UpdateKeyCache(keyslice,valueslice);

      bnum ++;
      done ++;

      if (bnum == entries_per_batch_) {
          bnum = 0;
          s = db_->Write(write_options_, &batch);
          batch.Clear();
          if (!s.ok()) {
            fprintf(stderr, "put error: %s\n", s.ToString().c_str());
            exit(1);
          }
          thread->stats.AddBytes(bytes);
          bytes = 0;
      }
      thread->stats.FinishedWriteOp();
	  runtime::num_writes_++;
	  time(&now_intv);
	  if(difftime(now_intv,start_intv)>=rw_interval){
	 	 fprintf(stdout,"writeop: finishd %d ops out of %d, timepassed: %d writecursor: %ld\n",readwrite_complete-last_rw,readwrite_complete,(int)difftime(now_intv,begin),runtime::write_cursor);

	     //fprintf(stdout,"writeop: finishd %ld ops in %d sec, timepassed: %d\n",runtime::num_writes_-last_write,rw_interval,(int)difftime(now_intv,begin));
	     last_write = runtime::num_writes_;
	     time(&start_intv);
	  }
      if(done%latency_gap==0){
	    gettimeofday(&end,NULL);
	    latency = (end.tv_sec-start.tv_sec)*1000000+(end.tv_usec-start.tv_usec);
	    if(write_latency*latency_gap>latency&&FLAGS_write_throughput>0)
	    {
	       Env::Default()->SleepForMicroseconds(write_latency*latency_gap-latency);
	    }
		gettimeofday(&start,NULL);

      }
    }

    time(&now);
    fprintf(stdout,"completes %ld write ops in %.3f seconds\n",done, difftime(now, begin));

}

  void ReadWrite(ThreadState* thread) {

	  if(FLAGS_throughput==0){
		  return;
	  }
   ReadOptions options;
   RandomGenerator gen;
   WriteBatch batch;
   std::string value;
   Status s;

   time_t begin, now;
   struct timeval start, end;
   int64_t latency = 0;

   int found = 0;
   int readdone = 0;
   int writedone = 0;
   char key[100];

  	generator::IntegerGenerator *readgenerator,*writegenerator;
	generator::IntegerGenerator *noisegenerator = new generator::UniformIntegerGenerator(runtime::key_from_,runtime::key_upto_);

  	if(FLAGS_read_ycsb_workload==NULL){
  		readgenerator = new generator::CounterGenerator(runtime::read_from_,runtime::read_upto_);
  	}
  	else if(strcmp(FLAGS_read_ycsb_workload,"zipfian")==0){
  		readgenerator = new generator::ZipfianGenerator(runtime::read_from_,(long int)runtime::read_upto_,FLAGS_zipfian_constant);
  	}
  	else if(strcmp(FLAGS_read_ycsb_workload,"latest")==0){
  	   generator::CounterGenerator base(runtime::read_upto_);
  	   readgenerator = new generator::SkewedLatestGenerator(base);
  	}
  	else if(strcmp(FLAGS_read_ycsb_workload,"uniform")==0){
  		readgenerator = new generator::UniformIntegerGenerator(runtime::read_from_,runtime::read_upto_);
  	}
  	else {
  		readgenerator = new generator::CounterGenerator(runtime::read_from_,runtime::read_upto_);
  	}

  	if(FLAGS_write_ycsb_workload==NULL){
  			writegenerator = new generator::CounterGenerator(runtime::write_from_,runtime::write_upto_);
  	}
  	else if(strcmp(FLAGS_write_ycsb_workload,"zipfian")==0){
  			writegenerator = new generator::ZipfianGenerator(runtime::write_from_,(long int )runtime::write_upto_,FLAGS_zipfian_constant);
  	}
  	else if(strcmp(FLAGS_write_ycsb_workload,"latest")==0){
  		generator::CounterGenerator base(runtime::write_upto_);
  	    writegenerator = new generator::SkewedLatestGenerator(base);
  	}
  	else if(strcmp(FLAGS_write_ycsb_workload,"uniform")==0){
  		writegenerator = new generator::UniformIntegerGenerator(runtime::write_from_,runtime::write_upto_);
  	}
  	else{
  		writegenerator = new generator::CounterGenerator(runtime::write_from_,runtime::write_upto_);
  	}

  	if(cache_==NULL&&key_cache_==NULL){
  		leveldb::runtime::need_warm_up = false;
  	}
  	bool startwarmup = false;
  	if(runtime::needWarmUp()){
  		read_mu_.Lock();
  		startwarmup = runtime::notStartWarmUp();
  		if(startwarmup){
  			runtime::warm_up_status = 1;
  		}
  		read_mu_.Unlock();
  		if(startwarmup){
  			ConductWarmUp();
  		}
  		else{
  			WaitForWarmUp();
  		}
  	}
  	time(&begin);

      int k=0;
      gettimeofday(&start,NULL);
      time_t start_intv = 0, now_intv = 0;

      while(true)
      {

        time(&now);
        if(start_intv==0){
        	time(&start_intv);
        }
  	    if (difftime(now, begin) > FLAGS_countdown || (readwrite_complete>=FLAGS_num&&FLAGS_num>=0)){
      		break;
  	    }
        if(random()*100<=FLAGS_read_portion){//read
         if(random()*100>FLAGS_noise_percent){
              k = (readgenerator->nextInt()/hot_ratio)*hot_ratio;
         }else{
              k = noisegenerator->nextInt();
         }
         snprintf(key, sizeof(key), "user%019d", k);
         s = db_->Get(options, key, &value);
         readdone++;
         if (s.ok()) {
           found++;
         }
         thread->stats.FinishedReadOp();
        }
        else{//write
    	   k = writegenerator->nextInt();
    	   snprintf(key, sizeof(key), "user%019d", k);
    	   batch.Put(key, gen.Generate(value_size_));
           writedone ++;
           s = db_->Write(write_options_, &batch);
    	   batch.Clear();
    	   if (!s.ok()) {
    	      fprintf(stderr, "put error: %s\n", s.ToString().c_str());
    	      exit(1);
    	   }
           thread->stats.FinishedWriteOp();
       }

       readwrite_complete++;
 	   time(&now_intv);
 	   if(difftime(now_intv,start_intv)>=rw_interval){
 		 fprintf(stdout,"writeop: finishd %d ops out of %d, timepassed: %d writecursor: %ld\n",readwrite_complete-last_rw,readwrite_complete,(int)difftime(now_intv,begin),runtime::write_cursor);
 	     //printf("rwop: finishd %d ops in %d sec, timepassed: %d\n",readwrite_complete-last_rw,rw_interval,(int)difftime(now_intv,begin));
 	     last_rw = readwrite_complete;
 	     time(&start_intv);
 	   }
       if(FLAGS_throughput>0&&readwrite_complete%latency_gap==0){
            gettimeofday(&end,NULL);
            latency = (end.tv_sec-start.tv_sec)*1000000+(end.tv_usec-start.tv_usec);
            if(latency_gap*readwrite_latency>latency)
         	{
         	  Env::Default()->SleepForMicroseconds(readwrite_latency*latency_gap-latency);
         	}
         	gettimeofday(&start,NULL);
       }

      }//end while

      char msg[100];
      snprintf(msg, sizeof(msg), "(%d of %d found in one read thread)", found, readdone);
      thread->stats.AddMessage(msg);

      time(&now);
      printf("completes %d read ops (out of %ld) in %.3f seconds, %d found\n",
        readdone, runtime::num_reads_, difftime(now, begin), found);
      printf("completes %d write ops in %.3f seconds\n",writedone, difftime(now, begin));

  }
/*
  void PrintStats(const char* key) {
    std::string stats;
    if (!db_->GetProperty(key, &stats)) {
      stats = "(failed)";
    }
    fprintf(stdout, "\n%s\n", stats.c_str());
  }

  static void WriteToFile(void* arg, const char* buf, int n) {
    reinterpret_cast<WritableFile*>(arg)->Append(Slice(buf, n));
  }*/

};

}  // namespace leveldb


int main(int argc, char** argv) {

  runtime::resetTimer();
  //FLAGS_write_buffer_size = leveldb::Options().write_buffer_size;
  //FLAGS_open_files = leveldb::Options().max_open_files;
  for (int i = 1; i < argc; i++) {
    double d;
    int n;
    int n1;
    int n2;
    int n3;
    long long ll;
    int64_t n64;
    char junk;
    if (leveldb::Slice(argv[i]).starts_with("--benchmarks=")) {
      FLAGS_benchmarks = argv[i] + 13;
    } else if (sscanf(argv[i], "--compression_ratio=%lf%c", &d, &junk) == 1) {
      FLAGS_compression_ratio = d;
    } else if (sscanf(argv[i], "--histogram=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_histogram = n;
    } else if (sscanf(argv[i], "--use_existing_db=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_use_existing_db = n;
    } else if (sscanf(argv[i], "--random_reads=%d%c", &n, &junk) == 1) {
      FLAGS_random_reads = n;
    } else if (sscanf(argv[i], "--writes=%d%c", &n, &junk) == 1) {
      FLAGS_writes = n;
    } else if (sscanf(argv[i], "--read_threads=%d%c", &n, &junk) == 1) {
      FLAGS_random_threads = n;
    } else if (sscanf(argv[i], "--value_size=%d%c", &n, &junk) == 1) {
      FLAGS_value_size = n;
    } else if (sscanf(argv[i], "--write_buffer_size=%d%c", &n, &junk) == 1) {
      FLAGS_write_buffer_size = n*1024*1024;
    } else if (sscanf(argv[i], "--block_cache_size=%d%c", &n, &junk) == 1) {
      FLAGS_block_cache_size = (uint64_t)n*1024*1024;
    } else if (sscanf(argv[i], "--key_cache_size=%d%c", &n, &junk) == 1) {
      FLAGS_key_cache_size = (uint64_t)n*1024*1024;
      leveldb::config::key_cache_size = FLAGS_key_cache_size;
    } else if (sscanf(argv[i], "--bloom_bits=%d%c", &n, &junk) == 1) {
      FLAGS_bloom_bits = n;
    } else if (sscanf(argv[i], "--bloom_bits_use=%d%c", &n, &junk) == 1) {
      leveldb::config::bloom_bits_use = n;
    } else if (sscanf(argv[i], "--cached_block_threshold=%d%c", &n, &junk) == 1) {
        leveldb::config::num_blocks_cached_threshold = n;
    } else if (sscanf(argv[i], "--open_files=%d%c", &n, &junk) == 1) {
      FLAGS_open_files = n;
    } else if (strncmp(argv[i], "--db=", 5) == 0) {
      FLAGS_db = argv[i] + 5;
      leveldb::config::db_path = FLAGS_db;
    } else if (sscanf(argv[i], "--key_from=%ld%c", &n64, &junk) == 1) {
      runtime::key_from_ = n64;
    } else if (sscanf(argv[i], "--key_upto=%ld%c", &n64, &junk) == 1) {
      runtime::key_upto_ = n64;
    }else if (sscanf(argv[i], "--read_key_from=%ld%c", &n64, &junk) == 1) {
      runtime::read_from_ = n64;
      for(int i=0;i<config::kNumLevels;i++){
    	  runtime::read_cursor_[i] = n64;
      }
      runtime::setReadCursor();
      runtime::setReadKeys();
    } else if (sscanf(argv[i], "--read_key_upto=%ld%c", &n64, &junk) == 1) {
      runtime::read_upto_ = n64;
      runtime::setReadKeys();
    } else if (sscanf(argv[i], "--write_key_from=%ld%c", &n64, &junk) == 1) {
      runtime::write_from_ = n64;
    } else if (sscanf(argv[i], "--write_key_upto=%ld%c", &n64, &junk) == 1) {
      runtime::write_upto_ = n64;
    } else if (sscanf(argv[i], "--file_size=%d%c", &n, &junk) == 1) {
      leveldb::config::kTargetFileSize = n * 1024*1024; // in MiB
    } else if (sscanf(argv[i], "--level0_size=%d%c", &n, &junk) == 1) {
      leveldb::config::kL0_size = n;
    } else if (sscanf(argv[i], "--countdown=%lf%c", &d, &junk) == 1) {
      FLAGS_countdown = d;
    } else if (sscanf(argv[i], "--random_seed=%lf%c", &d, &junk) == 1) {
      FLAGS_random_seed = d;
    } else if (sscanf(argv[i], "--run_compaction=%d%c", &n, &junk) == 1) {
      leveldb::config::run_compaction = n;
    } else if (sscanf(argv[i], "--print_version_info=%d%c", &n, &junk) == 1) {
      leveldb::runtime::print_version_info = n;
    } else if (sscanf(argv[i], "--print_compaction_buffer=%d%c", &n, &junk) == 1) {
          leveldb::runtime::print_compaction_buffer = n;
    } else if (sscanf(argv[i], "--print_dash=%d%c", &n, &junk) == 1) {
        leveldb::runtime::print_dash = n;
    } else if (sscanf(argv[i], "--pre_caching=%d%c", &n, &junk) == 1) {
        leveldb::runtime::pre_caching = n;
    } else if (sscanf(argv[i], "--warmup=%d%c", &n, &junk) == 1) {
        leveldb::runtime::need_warm_up = n;
        if(leveldb::runtime::needWarmUp()){
        	leveldb::runtime::warm_up_status = 0;
        }
        if(n>1){
        	FLAGS_range_warmup = false;
        }
    } else if (strncmp(argv[i], "--monitor_log=", 14) == 0) {
      monitor_log = fopen(argv[i] + 14, "w");
	} else if (strncmp(argv[i], "--write_workload=",17)==0) {
	  FLAGS_write_ycsb_workload = argv[i] + 17;
    } else if (strncmp(argv[i], "--read_workload=",16)==0) {
  	  FLAGS_read_ycsb_workload = argv[i] + 16;
    } else if (sscanf(argv[i], "--writespeed=%d%c", &n, &junk) == 1)  {
	  FLAGS_write_throughput = n;
	  if(FLAGS_write_throughput>0)
	  write_latency = 1000000/FLAGS_write_throughput;
    } else if (sscanf(argv[i], "--readspeed=%d%c", &n, &junk) == 1) {
	  FLAGS_read_throughput = n;
	  if(FLAGS_read_throughput>0)
	  read_latency = 1000000/FLAGS_read_throughput;
    } else if (sscanf(argv[i], "--range_size=%d%c", &n, &junk) == 1) {
        FLAGS_range_size = n;
    } else if (sscanf(argv[i], "--range_portion=%d%c", &n, &junk) == 1) {
        FLAGS_range_portion = n;
    } else if (sscanf(argv[i], "--hot_ratio=%d%c", &n, &junk) == 1) {
    	if(n!=0){
    		hot_ratio = 100/n;
    	}
    } else if (sscanf(argv[i], "--noise_percent=%d%c", &n, &junk) == 1) {
    	FLAGS_noise_percent = n;
    } else if (sscanf(argv[i], "--num=%d%c", &n, &junk) == 1) {
        FLAGS_num = n;
    } else if (sscanf(argv[i], "--throughput=%d%c", &n, &junk) == 1) {
    	FLAGS_throughput = n;
    	if(FLAGS_throughput>0)
    		  readwrite_latency = 1000000/FLAGS_throughput;
    } else if (sscanf(argv[i], "--read_portion=%d%c", &n, &junk) == 1) {
    	FLAGS_read_portion = n;
    } else if (sscanf(argv[i], "--hitratio_interval=%d%c", &n, &junk) == 1) {
    	leveldb::runtime::hitratio_interval = n;
    }  else if (sscanf(argv[i], "--rw_interval=%d%c", &n, &junk) == 1) {
    	rw_interval = n;
    }  else if (sscanf(argv[i], "--zipfian_constant=%lf%c", &d, &junk) == 1) {
        FLAGS_zipfian_constant = d;
    }  else if (sscanf(argv[i], "--max_print_level=%d%c", &n, &junk) == 1) {
        leveldb::runtime::max_print_level = n;
    }	else if (sscanf(argv[i], "--hot_file_threshold=%d%c", &n, &junk) == 1) {
        leveldb::config::hot_file_threshold = n;
    }	else if (sscanf(argv[i], "--compaction_buffer_length=%d%c", &n,&junk) == 1) {
        leveldb::runtime::compaction_buffer_length[n%10] = n/10;
    }  else if (sscanf(argv[i], "--compaction_buffer_use_length=%d%c", &n,&junk) == 1) {
        leveldb::runtime::compaction_buffer_use_length[n%10] = n/10;
    }
    else if (sscanf(argv[i], "--compaction_buffer_management_interval=%d%c", &n,&junk) == 1) {
        leveldb::runtime::compaction_buffer_management_interval[1] = n;
        leveldb::runtime::compaction_buffer_management_interval[2] = n*10;
        leveldb::runtime::compaction_buffer_management_interval[3] = n*10*10;
    }
    else if (sscanf(argv[i], "--preload_metadata=%d%c", &n, &junk) == 1) {
        leveldb::config::preload_metadata = n;
    }  else if (sscanf(argv[i], "--buffered_merge=%d%c", &n, &junk) == 1) {
        leveldb::config::buffered_merge = n;
    }  else if (sscanf(argv[i], "--data_merged_each_round=%d%c", &n, &junk) == 1) {
        leveldb::config::data_merged_each_round = n;
    }  else if (sscanf(argv[i], "--compaction_min_score=%lf%c", &d, &junk) == 1) {
    	if(d<1){
    		leveldb::runtime::compaction_min_score = d;
        }
    } else {
    	printf("%s\n",argv[i]);
      fprintf(stderr, "Invalid flag '%s'\n", argv[i]);
      exit(1);
    }
  }

  FLAGS_read_span = runtime::read_upto_ - runtime::read_from_;
  FLAGS_write_span = runtime::write_upto_ - runtime::write_from_;
  fprintf(stderr, "Range: %ld(w) %ld(r)\n", FLAGS_write_span, FLAGS_read_span);

  // Choose a location for the test database if none given with --db=<path>
  if (FLAGS_db == NULL) {
      printf("please specify database path!\n");
      exit(0);
  }
  leveldb::Benchmark benchmark;
  benchmark.Run();

  return 0;
}
