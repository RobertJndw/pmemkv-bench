// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE-BSD file. See the AUTHORS file for names of contributors.

// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
// This source code is licensed under the Apache 2.0 License
// (found in the LICENSE file in the root directory).

// SPDX-License-Identifier: Apache-2.0
/* Copyright 2017-2021, Intel Corporation */

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <inttypes.h>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

#include "csv.h"
#include "histogram.h"
#include "leveldb/env.h"
#include "libpmemkv.hpp"
#include "mutexlock.h"
#include "port/port_posix.h"
#include "random.h"
#include "testutil.h"

static const std::string USAGE =
	"pmemkv_bench\n"
	"--engine=<name>            (storage engine name, default: cmap)\n"
	"--db=<location>            (path to persistent pool, default: /dev/shm/pmemkv)\n"
	"                           (note: file on DAX filesystem, DAX device, or poolset file)\n"
	"--db_size_in_gb=<integer>  (size of persistent pool to create in GB, default: 0)\n"
	"                           (note: for existing poolset or device DAX configs use 0 or leave default value)\n"
	"                           (note: when pool path is non-existing, value should be > 0)\n"
	"--histogram=<0|1>          (show histograms when reporting latencies)\n"
	"--num=<integer>            (number of keys to place in database, default: 1000000)\n"
	"--reads=<integer>          (number of read operations, default: 1000000)\n"
	"--miss_rate=<integer>      (percentage of read which should result in a miss. "
	"The default value 10 means 10% of read operations will result in a miss\n"
	"--threads=<integer>        (number of concurrent threads, default: 1)\n"
	"--key_size=<integer>       (size of keys in bytes, default: 8)\n"
	"--value_size=<integer>     (size of values in bytes, default: 100)\n"
	"--readwritepercent=<integer> (Ratio of reads to reads/writes (expressed "
	"as percentage) for the ReadRandomWriteRandom workload. The default value "
	"90 means 90% operations out of all reads and writes operations are reads. "
	"In other words, 9 gets for every 1 put.) type: int32 default: 90\n"
	"--tx_size=<integer>        (number of elements to insert in a single tx, there will be"
	"num/tx_size transactions per thread in total, the last tx might be smaller, default: 10)\n"
	"--disjoint=<0|1>           (specifies whether each thread works on disjoint set of keys. "
	"0 means that all threads read/write to the db using any key between 0 and `num`, so that "
	"number of ops is `threads` * `num`. 1 means that each thread performs reads/writes using "
	"only [`thread_id` * `num` / `threads`, (`thread_id` + 1) * `num` / `threads`) subset of keys, "
	"so that total number of ops is `num`. The default value is 0.)\n"
	"--benchmarks=<name>,       (comma-separated list of benchmarks to run)\n"
	"    fillseq                (load N values in sequential key order)\n"
	"    fillrandom             (load N values in random key order)\n"
	"    readseq                (read N values in sequential key order)\n"
	"    readrandom             (read N values in random key order)\n"
	"    readmissing            (read N missing values in random key order)\n"
	"    deleteseq              (delete N values in sequential key order)\n"
	"    deleterandom           (delete N values in random key order)\n"
	"    readwhilewriting       (1 writer, N threads doing random reads)\n"
	"    readrandomwriterandom  (N threads doing random-read, random-write)\n"
	"    txfillrandom           (load N values in random key order transactionally)\n";

/* Number of key/values to place in database */
static int FLAGS_num = 1000000;

static bool FLAGS_disjoint = false;

/* Number of read operations to do. If negative, do FLAGS_num reads. */
static int FLAGS_reads = -1;

/* Percentage of miss operation during reads, only applies if entries < reads */
static int FLAGS_miss_rate = 10;

/* Number of concurrent threads to run. */
static int FLAGS_threads = 1;

static int FLAGS_key_size = 8;

/* Size of each value */
static int FLAGS_value_size = 100;

/* Print histogram of operation timings */
static bool FLAGS_histogram = false;

/* Use the db with the following name. */
static const char *FLAGS_db = "/dev/shm/pmemkv";

/* Use following size when opening the database. */
static int FLAGS_db_size_in_gb = 0;

static const double FLAGS_compression_ratio = 1.0;

static const int FLAGS_ops_between_duration_checks = 1000;

static const int FLAGS_duration = 0;

static int FLAGS_readwritepercent = 90;

static int FLAGS_tx_size = 10;

using namespace leveldb;

leveldb::Env *g_env = NULL;

#if defined(__linux)

static Slice TrimSpace(Slice s)
{
	size_t start = 0;
	while (start < s.size() && isspace(s[start])) {
		start++;
	}
	size_t limit = s.size();
	while (limit > start && isspace(s[limit - 1])) {
		limit--;
	}
	return Slice(s.data() + start, limit - start);
}

#endif

using kv_pointer = std::unique_ptr<pmem::kv::db, std::function<void(pmem::kv::db *)>>;

/* Helper for quickly generating random data. */
class RandomGenerator {
private:
	std::string data_;
	unsigned int pos_;

public:
	RandomGenerator()
	{
		/* We use a limited amount of data over and over again and ensure
		 * that it is larger than the compression window (32KB), and also
		 * large enough to serve all typical value sizes we want to write. */
		Random rnd(301);
		std::string piece;
		while (data_.size() < (unsigned)std::max(1048576, FLAGS_value_size)) {
			/* Add a short fragment that is as compressible as specified
			 * by FLAGS_compression_ratio. */
			test::CompressibleString(&rnd, FLAGS_compression_ratio, 100, &piece);
			data_.append(piece);
		}
		pos_ = 0;
	}

	Slice Generate(unsigned int len)
	{
		assert(len <= data_.size());
		if (pos_ + len > data_.size()) {
			pos_ = 0;
		}
		pos_ += len;
		return Slice(data_.data() + pos_ - len, len);
	}

	Slice GenerateWithTTL(unsigned int len)
	{
		assert(len <= data_.size());
		if (pos_ + len > data_.size()) {
			pos_ = 0;
		}
		pos_ += len;
		return Slice(data_.data() + pos_ - len, len);
	}
};

static void AppendWithSpace(std::string *str, Slice msg)
{
	if (msg.empty())
		return;
	if (!str->empty()) {
		str->push_back(' ');
	}
	str->append(msg.data(), msg.size());
}

enum OperationType : unsigned char {
	kRead = 0,
	kWrite,
	kDelete,
	kSeek,
	kMerge,
	kUpdate,
};

class BenchmarkLogger {
private:
	struct hist {
		int id;
		std::string name;
		std::string histogram;
	};
	int id = 0;
	std::vector<hist> histograms;
	CSV<int> csv = CSV<int>("sequence_id");

public:
	void insert(std::string name, Histogram histogram)
	{
		histograms.push_back({id, name, histogram.ToString()});
		std::vector<double> percentiles = {50, 75, 90, 99.9, 99.99};
		for (double &percentile : percentiles) {
			csv.insert(id, "Percentile P" + std::to_string(percentile) + " [micros/op]",
				   histogram.Percentile(percentile));
		}
		csv.insert(id, "Median [micros/op]", histogram.Median());
	}

	template <typename T>
	void insert(std::string column, T data)
	{
		csv.insert(id, column, data);
	}

	void insert(std::string column, std::time_t time)
	{
		std::ostringstream time_stream;
		time_stream << std::put_time(std::localtime(&time), "%D %T");
		insert(column, time_stream.str());
	}

	void print_histogram()
	{
		std::cout << "------------------------------------------------" << std::endl;
		for (auto &histogram : histograms) {
			std::cout << "benchmark: " << histogram.id << ", " << histogram.name << std::endl
				  << histogram.histogram << std::endl;
		}
	}

	void print()
	{
		csv.print();
	}

	void next_benchmark()
	{
		id++;
	}
};

class Stats {
private:
	double start_;
	double finish_;
	double seconds_;
	int done_;
	int next_report_;
	int64_t bytes_;
	double last_op_finish_;
	Histogram hist_;
	std::string message_;
	bool exclude_from_merge_;

public:
	Stats()
	{
		Start();
	}

	void Start()
	{
		next_report_ = 100;
		last_op_finish_ = start_;
		hist_.Clear();
		done_ = 0;
		bytes_ = 0;
		seconds_ = 0;
		start_ = g_env->NowMicros();
		finish_ = start_;
		message_.clear();
		/* When set, stats from this thread won't be merged with others */
		exclude_from_merge_ = false;
	}

	void Merge(const Stats &other)
	{
		if (other.exclude_from_merge_)
			return;

		hist_.Merge(other.hist_);
		done_ += other.done_;
		bytes_ += other.bytes_;
		seconds_ += other.seconds_;
		if (other.start_ < start_)
			start_ = other.start_;
		if (other.finish_ > finish_)
			finish_ = other.finish_;

		/* Just keep the messages from one thread */
		if (message_.empty())
			message_ = other.message_;
	}

	void Stop()
	{
		finish_ = g_env->NowMicros();
		seconds_ = (finish_ - start_) * 1e-6;
	}

	void AddMessage(Slice msg)
	{
		AppendWithSpace(&message_, msg);
	}

	void SetExcludeFromMerge()
	{
		exclude_from_merge_ = true;
	}

	void FinishedSingleOp()
	{
		double now = g_env->NowMicros();
		double micros = now - last_op_finish_;
		hist_.Add(micros);
		last_op_finish_ = now;

		done_++;
		if (done_ >= next_report_) {
			if (next_report_ < 1000)
				next_report_ += 100;
			else if (next_report_ < 5000)
				next_report_ += 500;
			else if (next_report_ < 10000)
				next_report_ += 1000;
			else if (next_report_ < 50000)
				next_report_ += 5000;
			else if (next_report_ < 100000)
				next_report_ += 10000;
			else if (next_report_ < 500000)
				next_report_ += 50000;
			else
				next_report_ += 100000;
		}
	}

	void AddBytes(int64_t n)
	{
		bytes_ += n;
	}

	float get_micros_per_op()
	{
		/* Pretend at least one op was done in case we are running a benchmark
		 * that does not call FinishedSingleOp(). */
		if (done_ < 1)
			done_ = 1;
		return seconds_ * 1e6 / done_;
	}

	float get_ops_per_sec()
	{
		/* Pretend at least one op was done in case we are running a benchmark
		 * that does not call FinishedSingleOp(). */
		if (done_ < 1)
			done_ = 1;
		double elapsed = (finish_ - start_) * 1e-6;

		return done_ / elapsed;
	}

	float get_throughput()
	{
		/* Rate and ops/sec is computed on actual elapsed time, not the sum of per-thread
		 * elapsed times. */
		double elapsed = (finish_ - start_) * 1e-6;
		return (bytes_ / 1048576.0) / elapsed;
	}

	std::string get_extra_data()
	{
		return message_;
	}

	Histogram &get_histogram()
	{
		return hist_;
	}
};

/* State shared by all concurrent executions of the same benchmark. */
struct SharedState {
	port::Mutex mu;
	port::CondVar cv;
	int total;

	/* Each thread goes through the following states:
	 * (1) initializing
	 * (2) waiting for others to be initialized
	 * (3) running
	 * (4) done
	 */

	int num_initialized;
	int num_done;
	bool start;

	SharedState() : cv(&mu)
	{
	}
};

/* Per-thread state for concurrent executions of the same benchmark. */
struct ThreadState {
	int tid;     /* 0..n-1 when running in n threads */
	Random rand; /* Has different seeds for different threads */
	Stats stats;
	SharedState *shared;

	ThreadState(int index) : tid(index), rand(1000 + index)
	{
	}
};

class Duration {
	typedef std::chrono::high_resolution_clock::time_point time_point;

public:
	Duration(uint64_t max_seconds, int64_t max_ops, int64_t ops_per_stage = 0)
	{
		max_seconds_ = max_seconds;
		max_ops_ = max_ops;
		ops_per_stage_ = (ops_per_stage > 0) ? ops_per_stage : max_ops;
		ops_ = 0;
		start_at_ = std::chrono::high_resolution_clock::now();
	}

	int64_t GetStage()
	{
		return std::min(ops_, max_ops_ - 1) / ops_per_stage_;
	}

	bool Done(int64_t increment)
	{
		if (increment <= 0)
			increment = 1; /* avoid Done(0) and infinite loops */
		ops_ += increment;

		if (max_seconds_) {
			/* Recheck every appx 1000 ops (exact iff increment is factor of 1000) */
			auto granularity = FLAGS_ops_between_duration_checks;
			if ((ops_ / granularity) != ((ops_ - increment) / granularity)) {
				time_point now = std::chrono::high_resolution_clock::now();
				return std::chrono::duration_cast<std::chrono::milliseconds>(now - start_at_)
					       .count() >= max_seconds_;
			} else {
				return false;
			}
		} else {
			return ops_ > max_ops_;
		}
	}

private:
	uint64_t max_seconds_;
	int64_t max_ops_;
	int64_t ops_per_stage_;
	int64_t ops_;
	time_point start_at_;
};

class Benchmark {
private:
	pmem::kv::db *kv_;
	int num_;
	int tx_size_;
	int value_size_;
	int key_size_;
	int reads_;
	int64_t readwrites_;
	BenchmarkLogger &logger;
	Slice name;
	int n;
	const char *engine;

	void (Benchmark::*method)(ThreadState *) = NULL;

	void PrintHeader()
	{
		PrintEnvironment();
		logger.insert("Path", FLAGS_db);
		logger.insert("Engine", engine);
		logger.insert("Keys [bytes each]", FLAGS_key_size);
		logger.insert("Values [bytes each]", FLAGS_value_size);
		logger.insert("Entries", num_);
		logger.insert("RawSize [MB (estimated)]",
			      ((static_cast<int64_t>(FLAGS_key_size + FLAGS_value_size) * num_) / 1048576.0));
		PrintWarnings();
	}

	void PrintWarnings()
	{
#if defined(__GNUC__) && !defined(__OPTIMIZE__)
		fprintf(stdout, "WARNING: Optimization is disabled: benchmarks unnecessarily slow\n");
#endif
#ifndef NDEBUG
		fprintf(stdout, "WARNING: Assertions are enabled; benchmarks unnecessarily slow\n");
#endif
	}

	void PrintEnvironment()
	{
#if defined(__linux)
		auto now = std::time(NULL);
		logger.insert("Date", now);

		FILE *cpuinfo = fopen("/proc/cpuinfo", "r");
		if (cpuinfo != NULL) {
			char line[1000];
			int num_cpus = 0;
			std::string cpu_type;
			std::string cache_size;
			while (fgets(line, sizeof(line), cpuinfo) != NULL) {
				const char *sep = strchr(line, ':');
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
			logger.insert("CPU", std::to_string(num_cpus));
			logger.insert("CPU model", cpu_type);
			logger.insert("CPUCache", cache_size);
		}
#endif
	}

public:
	Benchmark(Slice name, kv_pointer &kv, int num_threads, const char *engine, BenchmarkLogger &logger)
	    : kv_(kv.get()), num_(FLAGS_num), tx_size_(FLAGS_tx_size), value_size_(FLAGS_value_size),
	      key_size_(FLAGS_key_size), reads_(FLAGS_reads < 0 ? FLAGS_num : FLAGS_reads),
	      readwrites_(FLAGS_reads < 0 ? FLAGS_num : FLAGS_reads), logger(logger), n(num_threads),
	      name(name), engine(engine)
	{
		fprintf(stderr, "Running %s\n", name.ToString().c_str());

		if (name == Slice("fillseq")) {
			method = &Benchmark::WriteSeq;
		} else if (name == Slice("fillrandom")) {
			method = &Benchmark::WriteRandom;
		} else if (name == Slice("txfillrandom")) {
			method = &Benchmark::TxFillRandom;
		} else if (name == Slice("readseq")) {
			method = &Benchmark::ReadSeq;
		} else if (name == Slice("readrandom")) {
			method = &Benchmark::ReadRandom;
		} else if (name == Slice("readmissing")) {
			method = &Benchmark::ReadMissing;
		} else if (name == Slice("deleteseq")) {
			method = &Benchmark::DeleteSeq;
		} else if (name == Slice("deleterandom")) {
			method = &Benchmark::DeleteRandom;
		} else if (name == Slice("readwhilewriting")) {
			++num_threads;
			method = &Benchmark::ReadWhileWriting;
		} else if (name == Slice("readrandomwriterandom")) {
			method = &Benchmark::ReadRandomWriteRandom;
		} else {
			throw std::runtime_error("unknown benchmark: " + name.ToString());
		}
		logger.next_benchmark();
		logger.insert("Benchmark", name.ToString());
		PrintHeader();

		if (!kv_) {
			Create();
			kv.reset(kv_);
		}
	}

	Slice AllocateKey(std::unique_ptr<const char[]> &key_guard)
	{
		const char *tmp = new char[key_size_];
		key_guard.reset(tmp);
		return Slice(key_guard.get(), key_size_);
	}

	/**
	 * Create key with binary value of v and filled up with '0',
	 * up to key_size (if needed).
	 */
	void GenerateKeyFromInt(uint64_t v, Slice *key, bool missing = false)
	{
		char *start = const_cast<char *>(key->data());
		char *pos = start;
		int bytes_to_fill = std::min(key_size_, 8);
		if (missing) {
			int64_t v1 = -v;
			memcpy(pos, static_cast<void *>(&v1), bytes_to_fill);
		} else
			memcpy(pos, static_cast<void *>(&v), bytes_to_fill);
		pos += bytes_to_fill;
		if (key_size_ > pos - start) {
			memset(pos, '0', key_size_ - (pos - start));
		}
	}

	void Run()
	{
		SharedState shared;
		shared.total = n;
		shared.num_initialized = 0;
		shared.num_done = 0;
		shared.start = false;

		ThreadArg *arg = new ThreadArg[n];
		for (int i = 0; i < n; i++) {
			arg[i].bm = this;
			arg[i].method = method;
			arg[i].shared = &shared;
			arg[i].thread = new ThreadState(i);
			arg[i].thread->shared = &shared;
			g_env->StartThread(ThreadBody, &arg[i]);
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
		auto thread_stats = arg[0].thread->stats;
		logger.insert("micros/op (avarage)", thread_stats.get_micros_per_op());
		logger.insert("ops/sec", thread_stats.get_ops_per_sec());
		logger.insert("throughput [MB/s]", thread_stats.get_throughput());
		logger.insert("extra_data", thread_stats.get_extra_data());
		logger.insert(name.ToString(), thread_stats.get_histogram());
		for (int i = 0; i < n; i++) {
			delete arg[i].thread;
		}
		delete[] arg;
	}

private:
	struct ThreadArg {
		Benchmark *bm;
		SharedState *shared;
		ThreadState *thread;

		void (Benchmark::*method)(ThreadState *);
	};

	struct DbInserter {
		DbInserter(pmem::kv::db *db) : db(db)
		{
		}

		pmem::kv::status put(pmem::kv::string_view key, pmem::kv::string_view value)
		{
			return db->put(key, value);
		}

		pmem::kv::status commit()
		{
			return pmem::kv::status::OK;
		}

	private:
		pmem::kv::db *db;
	};

	struct TxInserter {
		TxInserter(pmem::kv::db *db) : tx(db->tx_begin().get_value())
		{
		}

		pmem::kv::status put(pmem::kv::string_view key, pmem::kv::string_view value)
		{
			return tx.put(key, value);
		}

		pmem::kv::status commit()
		{
			return tx.commit();
		}

	private:
		pmem::kv::tx tx;
	};

	static void ThreadBody(void *v)
	{
		ThreadArg *arg = reinterpret_cast<ThreadArg *>(v);
		SharedState *shared = arg->shared;
		ThreadState *thread = arg->thread;
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

	/* Throw exception for failed put (with proper message) */
	void throw_put_error(int i, leveldb::Slice key, pmem::kv::status s)
	{
		std::string prnt_key = key.ToString();
		std::ostringstream err_msg;
		err_msg << "Put error for " << std::to_string(i) << "-th key: ";

		/* key is a binary data, print also non-printable chars */
		for (int c = 0; c < prnt_key.size(); c++) {
			if (!isprint(prnt_key[c])) {
				err_msg << "'0x" << std::hex << int(prnt_key[c]) << "'";
			} else {
				err_msg << "'" << prnt_key[c] << "'";
			}
		}

		err_msg << " (pmemkv status: " << std::to_string(int(s)) << ", error: '"
			<< pmem::kv::errormsg() << "')";
		throw std::runtime_error(err_msg.str());
	}

	void Create()
	{
		assert(kv_ == nullptr);
		auto start = g_env->NowMicros();
		auto size = 1024ULL * 1024ULL * 1024ULL * FLAGS_db_size_in_gb;
		pmem::kv::config cfg;

		auto cfg_s = cfg.put_string("path", FLAGS_db);
		if (cfg_s != pmem::kv::status::OK)
			throw std::runtime_error("putting 'path' to config failed");

		cfg_s = cfg.put_create_if_missing(true);
		if (cfg_s != pmem::kv::status::OK)
			throw std::runtime_error("putting 'create_if_missing' to config failed");

		cfg_s = cfg.put_uint64("size", size);
		if (cfg_s != pmem::kv::status::OK)
			throw std::runtime_error("putting 'size' to config failed");

		kv_ = new pmem::kv::db;
		auto s = kv_->open(engine, std::move(cfg));

		if (s != pmem::kv::status::OK) {
			throw std::runtime_error("Cannot start engine '" + std::string(engine) +
						 "' for path '" + FLAGS_db + "' with " +
						 std::to_string(FLAGS_db_size_in_gb) +
						 " GB capacity.\nError '" + pmem::kv::errormsg() + "'");
		}
		logger.insert("Open [millis/op]", ((g_env->NowMicros() - start) * 1e-3));
	}

	template <typename Inserter = DbInserter>
	void DoWrite(ThreadState *thread, bool seq)
	{
		if (num_ != FLAGS_num) {
			char msg[100];
			snprintf(msg, sizeof(msg), "(%d ops)", num_);
			thread->stats.AddMessage(msg);
		}
		std::unique_ptr<const char[]> key_guard;
		Slice key = AllocateKey(key_guard);

		auto num = FLAGS_disjoint ? num_ / FLAGS_threads : num_;
		auto start = FLAGS_disjoint ? thread->tid * num : 0;
		auto end = FLAGS_disjoint ? (thread->tid + 1) * num : num_;

		pmem::kv::status s;
		int64_t bytes = 0;
		auto batch_size = std::is_same<Inserter, TxInserter>::value ? tx_size_ : 1;
		for (int n = start; n < end; n += batch_size) {
			Inserter inserter(kv_);

			for (int i = n; i < n + batch_size; i++) {
				const int k = seq ? i : (thread->rand.Next() % num) + start;
				GenerateKeyFromInt(k, &key);
				std::string value = std::string();
				value.append(value_size_, 'X');
				s = inserter.put(key.ToString(), value);
				bytes += value_size_ + key.size();
				if (s != pmem::kv::status::OK) {
					throw_put_error(i, key, s);
				}
			}
			s = inserter.commit();
			thread->stats.FinishedSingleOp();
			if (s != pmem::kv::status::OK) {
				throw std::runtime_error("Commit failed at batch " +
							 std::to_string(n / batch_size) + "\nError '" +
							 pmem::kv::errormsg() + "'");
			}
		}
		thread->stats.AddBytes(bytes);
	}

	void WriteSeq(ThreadState *thread)
	{
		DoWrite<DbInserter>(thread, true);
	}

	void WriteRandom(ThreadState *thread)
	{
		DoWrite<DbInserter>(thread, false);
	}

	void DoRead(ThreadState *thread, bool seq, bool missing)
	{
		pmem::kv::status s;
		int64_t bytes = 0;
		int found = 0;
		std::unique_ptr<const char[]> key_guard;
		Slice key = AllocateKey(key_guard);

		auto num = FLAGS_disjoint ? reads_ / FLAGS_threads : reads_;
		auto start = FLAGS_disjoint ? thread->tid * num : 0;
		auto end = FLAGS_disjoint ? (thread->tid + 1) * num : reads_;

		int counter_misses = (int)num * ((float) FLAGS_miss_rate / 100.0);
		int counter_hits = num - counter_misses;
		Random rnd(time(NULL));

		for (int i = start; i < end; i++) {
			int k;
			if(counter_misses != 0 && (counter_hits == 0 || rnd.Uniform(100) < FLAGS_miss_rate)){
				k = i + num_;
				counter_misses--;
			} else {
				k = seq ? (i % num_) : (thread->rand.Next() % num_) + start;
				counter_hits--;
			}
			GenerateKeyFromInt(k, &key, missing);
			std::string value;
			if (kv_->get(key.ToString(), &value) == pmem::kv::status::OK)
				found++;
			thread->stats.FinishedSingleOp();
			bytes += value.length() + key.size();
		}
		thread->stats.AddBytes(bytes);
		char msg[100];
		if (found)
			snprintf(msg, sizeof(msg), "(%d of %d found by one thread)", found, reads_);
		else
			snprintf(msg, sizeof(msg), "(%d of %d found by one thread) WARNING! FOUND NOTHING!",
				 found, reads_);
		thread->stats.AddMessage(msg);
	}

	void ReadSeq(ThreadState *thread)
	{
		DoRead(thread, true, false);
	}

	void ReadRandom(ThreadState *thread)
	{
		DoRead(thread, false, false);
	}

	void ReadMissing(ThreadState *thread)
	{
		DoRead(thread, false, true);
	}

	void DoDelete(ThreadState *thread, bool seq)
	{
		std::unique_ptr<const char[]> key_guard;
		Slice key = AllocateKey(key_guard);
		for (int i = 0; i < num_; i++) {
			const int k = seq ? i : (thread->rand.Next() % FLAGS_num);
			GenerateKeyFromInt(k, &key);
			kv_->remove(key.ToString());
			thread->stats.FinishedSingleOp();
		}
	}

	void DeleteSeq(ThreadState *thread)
	{
		DoDelete(thread, true);
	}

	void DeleteRandom(ThreadState *thread)
	{
		DoDelete(thread, false);
	}

	void BGWriter(ThreadState *thread, enum OperationType write_merge)
	{
		/* Special thread that keeps writing until other threads are done. */
		RandomGenerator gen;
		int64_t bytes = 0;

		/* Don't merge stats from this thread with the readers. */
		thread->stats.SetExcludeFromMerge();

		std::unique_ptr<const char[]> key_guard;
		Slice key = AllocateKey(key_guard);
		uint32_t written = 0;
		bool hint_printed = false;

		while (true) {
			{
				MutexLock l(&thread->shared->mu);

				if (thread->shared->num_done + 1 >= thread->shared->num_initialized) {
					/* Finish the write immediately */
					break;
				}
			}

			GenerateKeyFromInt(thread->rand.Next() % FLAGS_num, &key);
			pmem::kv::status s;

			if (write_merge == kWrite) {
				s = kv_->put(key.ToString(), gen.Generate(value_size_).ToString());
				if (s != pmem::kv::status::OK) {
					throw_put_error(written, key, s);
				}
			} else {
				throw std::runtime_error("Merge operation not supported");
			}
			written++;
			bytes += key.size() + value_size_;
		}
		thread->stats.AddBytes(bytes);
	}

	void ReadWhileWriting(ThreadState *thread)
	{
		if (thread->tid > 0) {
			ReadRandom(thread);
		} else {
			BGWriter(thread, kWrite);
		}
	}

	void ReadRandomWriteRandom(ThreadState *thread)
	{
		RandomGenerator gen;
		std::string value;
		int64_t found = 0;
		int get_weight = 0;
		int put_weight = 0;
		int64_t reads_done = 0;
		int64_t writes_done = 0;
		int64_t bytes = 0;
		Duration duration(FLAGS_duration, readwrites_);

		std::unique_ptr<const char[]> key_guard;
		Slice key = AllocateKey(key_guard);

		/* the number of iterations is the larger of read_ or write_ */
		while (!duration.Done(1)) {
			GenerateKeyFromInt(thread->rand.Next() % FLAGS_num, &key);
			if (get_weight == 0 && put_weight == 0) {
				/* one batch completed, reinitialize for next batch */
				get_weight = FLAGS_readwritepercent;
				put_weight = 100 - get_weight;
			}
			if (get_weight > 0) {
				value.clear();
				pmem::kv::status s = kv_->get(key.ToString(), &value);
				if (s == pmem::kv::status::OK) {
					found++;
				} else if (s != pmem::kv::status::NOT_FOUND) {
					fprintf(stderr, "Get error for key '%s' (error: '%s')\n",
						key.ToString().c_str(), pmem::kv::errormsg().c_str());
				}

				bytes += value.length() + key.size();
				get_weight--;
				reads_done++;
				thread->stats.FinishedSingleOp();
			} else if (put_weight > 0) {
				/* then do all the corresponding number of puts
				 * for all the gets we have done earlier */
				pmem::kv::status s =
					kv_->put(key.ToString(), gen.Generate(value_size_).ToString());
				if (s != pmem::kv::status::OK) {
					throw_put_error(writes_done, key, s);
				}
				bytes += key.size() + value_size_;
				put_weight--;
				writes_done++;
				thread->stats.FinishedSingleOp();
			}
		}
		thread->stats.AddBytes(bytes);
		char msg[100];
		snprintf(msg, sizeof(msg),
			 "(reads:%" PRIu64 " writes:%" PRIu64 " total:%" PRIu64 " found:%" PRIu64 ")",
			 reads_done, writes_done, readwrites_, found);
		thread->stats.AddMessage(msg);
	}

	void TxFillRandom(ThreadState *thread)
	{
		DoWrite<TxInserter>(thread, false);
	}
};

int main(int argc, char **argv)
{
	/* Default list of comma-separated operations to run */
	static const char *FLAGS_benchmarks =
		"fillseq,fillrandom,readseq,readrandom,readmissing,deleteseq,deleterandom,readwhilewriting,readrandomwriterandom";
	/* Default engine name */
	static const char *FLAGS_engine = "cmap";

	/* Print usage statement if necessary */
	if (argc != 1) {
		if ((strcmp(argv[1], "?") == 0) || (strcmp(argv[1], "-?") == 0) ||
		    (strcmp(argv[1], "h") == 0) || (strcmp(argv[1], "-h") == 0) ||
		    (strcmp(argv[1], "-help") == 0) || (strcmp(argv[1], "--help") == 0)) {
			fprintf(stderr, "%s", USAGE.c_str());
			exit(1);
		}
	}

	/* Parse command-line arguments */
	for (int i = 1; i < argc; i++) {
		int n;
		char junk;
		if (leveldb::Slice(argv[i]).starts_with("--benchmarks=")) {
			FLAGS_benchmarks = argv[i] + strlen("--benchmarks=");
		} else if (strncmp(argv[i], "--engine=", 9) == 0) {
			FLAGS_engine = argv[i] + 9;
		} else if (sscanf(argv[i], "--histogram=%d%c", &n, &junk) == 1 && (n == 0 || n == 1)) {
			FLAGS_histogram = n;
		} else if (sscanf(argv[i], "--num=%d%c", &n, &junk) == 1) {
			FLAGS_num = n;
		} else if (sscanf(argv[i], "--reads=%d%c", &n, &junk) == 1) {
			FLAGS_reads = n;
		} else if (sscanf(argv[i], "--miss_rate=%d%c", &n, &junk) == 1) {
			FLAGS_miss_rate = n;
		} else if (sscanf(argv[i], "--threads=%d%c", &n, &junk) == 1) {
			FLAGS_threads = n;
		} else if (sscanf(argv[i], "--key_size=%d%c", &n, &junk) == 1) {
			FLAGS_key_size = n;
		} else if (sscanf(argv[i], "--value_size=%d%c", &n, &junk) == 1) {
			FLAGS_value_size = n;
		} else if (sscanf(argv[i], "--readwritepercent=%d%c", &n, &junk) == 1) {
			FLAGS_readwritepercent = n;
		} else if (strncmp(argv[i], "--db=", 5) == 0) {
			FLAGS_db = argv[i] + 5;
		} else if (sscanf(argv[i], "--db_size_in_gb=%d%c", &n, &junk) == 1) {
			FLAGS_db_size_in_gb = n;
		} else if (sscanf(argv[i], "--tx_size=%d%c", &n, &junk) == 1) {
			FLAGS_tx_size = n;
		} else if (sscanf(argv[i], "--disjoint=%d%c", &n, &junk) == 1 && (n == 0 || n == 1)) {
			FLAGS_disjoint = n;
		} else {
			fprintf(stderr, "Invalid flag '%s'\n", argv[i]);
			exit(1);
		}
	}

	/* Run benchmark against default environment */
	g_env = leveldb::Env::Default();

	BenchmarkLogger logger = BenchmarkLogger();
	int return_value = 0;
	auto kv = kv_pointer(nullptr, [](pmem::kv::db *kv) {
		kv->close();
		delete kv;
	});
	const char *benchmarks = FLAGS_benchmarks;
	while (benchmarks != NULL) {
		const char *sep = strchr(benchmarks, ',');
		Slice name;
		if (sep == NULL) {
			name = benchmarks;
			benchmarks = NULL;
		} else {
			name = Slice(benchmarks, sep - benchmarks);
			benchmarks = sep + 1;
		}
		try {
			auto benchmark = Benchmark(name, kv, FLAGS_threads, FLAGS_engine, logger);
			benchmark.Run();
		} catch (std::exception &e) {
			std::cerr << e.what() << std::endl;
			return_value = 1;
			break;
		}
	}
	logger.print();
	if (FLAGS_histogram) {
		logger.print_histogram();
	}
	return return_value;
}
