//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#ifndef ROCKSDB_LITE

#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "db/db_impl/db_impl.h"
#include "port/port.h"
#include "rocksdb/compaction_filter.h"
#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "test_util/sync_point.h"
#include "test_util/testharness.h"
#include "util/cast_util.h"
#include "util/string_util.h"

namespace ROCKSDB_NAMESPACE {

class CompactFilesTest : public testing::Test {
 public:
  CompactFilesTest() {
    env_ = Env::Default();
    db_name_ = test::PerThreadDBPath("compact_files_test");
  }

  std::string db_name_;
  Env* env_;
};

// A class which remembers the name of each flushed file.
class FlushedFileCollector : public EventListener {
 public:
  FlushedFileCollector() {}
  ~FlushedFileCollector() override {}

  void OnFlushCompleted(DB* /*db*/, const FlushJobInfo& info) override {
    std::lock_guard<std::mutex> lock(mutex_);
    flushed_files_.push_back(info.file_path);
  }

  std::vector<std::string> GetFlushedFiles() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    for (auto fname : flushed_files_) {
      result.push_back(fname);
    }
    return result;
  }
  void ClearFlushedFiles() {
    std::lock_guard<std::mutex> lock(mutex_);
    flushed_files_.clear();
  }

 private:
  std::vector<std::string> flushed_files_;
  std::mutex mutex_;
};

class TestFilterFactory : public CompactionFilterFactory {
 public:
  std::shared_ptr<CompactionFilter::Context> context_;
  std::shared_ptr<int> compaction_count_;

  TestFilterFactory(std::shared_ptr<CompactionFilter::Context> context,
                    std::shared_ptr<int> compaction_count) {
    this->context_ = context;
    this->compaction_count_ = compaction_count;
  }

  ~TestFilterFactory() {}

  const char* Name() const { return "TestFilterFactory"; }

  std::unique_ptr<CompactionFilter> CreateCompactionFilter(
      const CompactionFilter::Context& context) {
    context_->start_key = context.start_key;
    context_->end_key = context.end_key;
    context_->is_end_key_inclusive = context.is_end_key_inclusive;
    context_->file_numbers.clear();
    context_->table_properties.clear();
    for (size_t i = 0; i < context.file_numbers.size(); ++i) {
      context_->file_numbers.push_back(context.file_numbers[i]);
      context_->table_properties.push_back(context.table_properties[i]);
    }
    *compaction_count_.get() += 1;
    return nullptr;
  }
};

TEST_F(CompactFilesTest, FilterContext) {
  Options options;
  // to trigger compaction more easily
  const int kWriteBufferSize = 10000;
  const int kLevel0Trigger = 10;
  options.create_if_missing = true;
  options.compaction_style = kCompactionStyleLevel;
  // Small slowdown and stop trigger for experimental purpose.
  options.level0_slowdown_writes_trigger = 20;
  options.level0_stop_writes_trigger = 20;
  options.write_buffer_size = kWriteBufferSize;
  options.level0_file_num_compaction_trigger = kLevel0Trigger;
  options.compression = kNoCompression;

  std::shared_ptr<CompactionFilter::Context> expected_context(
      new CompactionFilter::Context);
  std::shared_ptr<int> compaction_count(new int(0));
  CompactionFilterFactory* factory =
      new TestFilterFactory(expected_context, compaction_count);
  options.compaction_filter_factory =
      std::shared_ptr<CompactionFilterFactory>(factory);

  DB* db = nullptr;
  DestroyDB(db_name_, options);
  Status s = DB::Open(options, db_name_, &db);
  assert(s.ok());
  assert(db);

  // `Flush` is different from `Compaction`.
  db->Put(WriteOptions(), ToString(1), "");
  db->Put(WriteOptions(), ToString(51), "");
  db->Flush(FlushOptions());
  db->Put(WriteOptions(), ToString(50), "");
  db->Put(WriteOptions(), ToString(99), "");
  db->Flush(FlushOptions());
  ASSERT_EQ(*compaction_count.get(), 0);

  db->CompactRange(CompactRangeOptions(), nullptr, nullptr);
  usleep(10000);  // Wait for compaction start.
  ASSERT_EQ(expected_context->start_key, Slice("1"));
  ASSERT_EQ(expected_context->is_end_key_inclusive, 1);
  ASSERT_EQ(expected_context->file_numbers[0], 11);
  ASSERT_EQ(*compaction_count.get(), 1);

  delete (db);
}

TEST_F(CompactFilesTest, L0ConflictsFiles) {
  Options options;
  // to trigger compaction more easily
  const int kWriteBufferSize = 10000;
  const int kLevel0Trigger = 2;
  options.create_if_missing = true;
  options.compaction_style = kCompactionStyleLevel;
  // Small slowdown and stop trigger for experimental purpose.
  options.level0_slowdown_writes_trigger = 20;
  options.level0_stop_writes_trigger = 20;
  options.level0_stop_writes_trigger = 20;
  options.write_buffer_size = kWriteBufferSize;
  options.level0_file_num_compaction_trigger = kLevel0Trigger;
  options.compression = kNoCompression;

  DB* db = nullptr;
  DestroyDB(db_name_, options);
  Status s = DB::Open(options, db_name_, &db);
  assert(s.ok());
  assert(db);

  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->LoadDependency({
      {"CompactFilesImpl:0", "BackgroundCallCompaction:0"},
      {"BackgroundCallCompaction:1", "CompactFilesImpl:1"},
  });
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->EnableProcessing();

  // create couple files
  // Background compaction starts and waits in BackgroundCallCompaction:0
  for (int i = 0; i < kLevel0Trigger * 4; ++i) {
    ASSERT_OK(db->Put(WriteOptions(), ToString(i), ""));
    ASSERT_OK(db->Put(WriteOptions(), ToString(100 - i), ""));
    ASSERT_OK(db->Flush(FlushOptions()));
  }

  ROCKSDB_NAMESPACE::ColumnFamilyMetaData meta;
  db->GetColumnFamilyMetaData(&meta);
  std::string file1;
  for (auto& file : meta.levels[0].files) {
    ASSERT_EQ(0, meta.levels[0].level);
    if (file1 == "") {
      file1 = file.db_path + "/" + file.name;
    } else {
      std::string file2 = file.db_path + "/" + file.name;
      // Another thread starts a compact files and creates an L0 compaction
      // The background compaction then notices that there is an L0 compaction
      // already in progress and doesn't do an L0 compaction
      // Once the background compaction finishes, the compact files finishes
      ASSERT_OK(db->CompactFiles(ROCKSDB_NAMESPACE::CompactionOptions(),
                                 {file1, file2}, 0));
      break;
    }
  }
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->DisableProcessing();
  delete db;
}

TEST_F(CompactFilesTest, MultipleLevel) {
  Options options;
  options.create_if_missing = true;
  options.level_compaction_dynamic_level_bytes = true;
  options.num_levels = 6;
  // Add listener
  FlushedFileCollector* collector = new FlushedFileCollector();
  options.listeners.emplace_back(collector);

  DB* db = nullptr;
  DestroyDB(db_name_, options);
  Status s = DB::Open(options, db_name_, &db);
  ASSERT_OK(s);
  ASSERT_NE(db, nullptr);

  // create couple files in L0, L3, L4 and L5
  for (int i = 5; i > 2; --i) {
    collector->ClearFlushedFiles();
    ASSERT_OK(db->Put(WriteOptions(), ToString(i), ""));
    ASSERT_OK(db->Flush(FlushOptions()));
    auto l0_files = collector->GetFlushedFiles();
    ASSERT_OK(db->CompactFiles(CompactionOptions(), l0_files, i));

    std::string prop;
    ASSERT_TRUE(
        db->GetProperty("rocksdb.num-files-at-level" + ToString(i), &prop));
    ASSERT_EQ("1", prop);
  }
  ASSERT_OK(db->Put(WriteOptions(), ToString(0), ""));
  ASSERT_OK(db->Flush(FlushOptions()));

  ColumnFamilyMetaData meta;
  db->GetColumnFamilyMetaData(&meta);
  // Compact files except the file in L3
  std::vector<std::string> files;
  for (int i = 0; i < 6; ++i) {
    if (i == 3) continue;
    for (auto& file : meta.levels[i].files) {
      files.push_back(file.db_path + "/" + file.name);
    }
  }

  SyncPoint::GetInstance()->LoadDependency({
      {"CompactionJob::Run():Start", "CompactFilesTest.MultipleLevel:0"},
      {"CompactFilesTest.MultipleLevel:1", "CompactFilesImpl:3"},
  });
  SyncPoint::GetInstance()->EnableProcessing();

  std::thread thread([&] {
    TEST_SYNC_POINT("CompactFilesTest.MultipleLevel:0");
    ASSERT_OK(db->Put(WriteOptions(), "bar", "v2"));
    ASSERT_OK(db->Put(WriteOptions(), "foo", "v2"));
    ASSERT_OK(db->Flush(FlushOptions()));
    TEST_SYNC_POINT("CompactFilesTest.MultipleLevel:1");
  });

  // Compaction cannot move up the data to higher level
  // here we have input file from level 5, so the output level has to be >= 5
  for (int invalid_output_level = 0; invalid_output_level < 5;
       invalid_output_level++) {
    s = db->CompactFiles(CompactionOptions(), files, invalid_output_level);
    std::cout << s.ToString() << std::endl;
    ASSERT_TRUE(s.IsInvalidArgument());
  }

  ASSERT_OK(db->CompactFiles(CompactionOptions(), files, 5));
  SyncPoint::GetInstance()->DisableProcessing();
  thread.join();

  delete db;
}

TEST_F(CompactFilesTest, ObsoleteFiles) {
  Options options;
  // to trigger compaction more easily
  const int kWriteBufferSize = 65536;
  options.create_if_missing = true;
  // Disable RocksDB background compaction.
  options.compaction_style = kCompactionStyleNone;
  options.level0_slowdown_writes_trigger = (1 << 30);
  options.level0_stop_writes_trigger = (1 << 30);
  options.write_buffer_size = kWriteBufferSize;
  options.max_write_buffer_number = 2;
  options.compression = kNoCompression;

  // Add listener
  FlushedFileCollector* collector = new FlushedFileCollector();
  options.listeners.emplace_back(collector);

  DB* db = nullptr;
  DestroyDB(db_name_, options);
  Status s = DB::Open(options, db_name_, &db);
  ASSERT_OK(s);
  ASSERT_NE(db, nullptr);

  // create couple files
  for (int i = 1000; i < 2000; ++i) {
    ASSERT_OK(db->Put(WriteOptions(), ToString(i),
                      std::string(kWriteBufferSize / 10, 'a' + (i % 26))));
  }

  auto l0_files = collector->GetFlushedFiles();
  ASSERT_OK(db->CompactFiles(CompactionOptions(), l0_files, 1));
  ASSERT_OK(static_cast_with_check<DBImpl>(db)->TEST_WaitForCompact());

  // verify all compaction input files are deleted
  for (auto fname : l0_files) {
    ASSERT_EQ(Status::NotFound(), env_->FileExists(fname));
  }
  delete db;
}

TEST_F(CompactFilesTest, NotCutOutputOnLevel0) {
  Options options;
  options.create_if_missing = true;
  // Disable RocksDB background compaction.
  options.compaction_style = kCompactionStyleNone;
  options.level0_slowdown_writes_trigger = 1000;
  options.level0_stop_writes_trigger = 1000;
  options.write_buffer_size = 65536;
  options.max_write_buffer_number = 2;
  options.compression = kNoCompression;
  options.max_compaction_bytes = 5000;

  // Add listener
  FlushedFileCollector* collector = new FlushedFileCollector();
  options.listeners.emplace_back(collector);

  DB* db = nullptr;
  DestroyDB(db_name_, options);
  Status s = DB::Open(options, db_name_, &db);
  assert(s.ok());
  assert(db);

  // create couple files
  for (int i = 0; i < 500; ++i) {
    ASSERT_OK(db->Put(WriteOptions(), ToString(i),
                      std::string(1000, 'a' + (i % 26))));
  }
  ASSERT_OK(static_cast_with_check<DBImpl>(db)->TEST_WaitForFlushMemTable());
  auto l0_files_1 = collector->GetFlushedFiles();
  collector->ClearFlushedFiles();
  for (int i = 0; i < 500; ++i) {
    ASSERT_OK(db->Put(WriteOptions(), ToString(i),
                      std::string(1000, 'a' + (i % 26))));
  }
  ASSERT_OK(static_cast_with_check<DBImpl>(db)->TEST_WaitForFlushMemTable());
  auto l0_files_2 = collector->GetFlushedFiles();
  ASSERT_OK(db->CompactFiles(CompactionOptions(), l0_files_1, 0));
  ASSERT_OK(db->CompactFiles(CompactionOptions(), l0_files_2, 0));
  // no assertion failure
  delete db;
}

TEST_F(CompactFilesTest, CapturingPendingFiles) {
  Options options;
  options.create_if_missing = true;
  // Disable RocksDB background compaction.
  options.compaction_style = kCompactionStyleNone;
  // Always do full scans for obsolete files (needed to reproduce the issue).
  options.delete_obsolete_files_period_micros = 0;

  // Add listener.
  FlushedFileCollector* collector = new FlushedFileCollector();
  options.listeners.emplace_back(collector);

  DB* db = nullptr;
  DestroyDB(db_name_, options);
  Status s = DB::Open(options, db_name_, &db);
  ASSERT_OK(s);
  assert(db);

  // Create 5 files.
  for (int i = 0; i < 5; ++i) {
    ASSERT_OK(db->Put(WriteOptions(), "key" + ToString(i), "value"));
    ASSERT_OK(db->Flush(FlushOptions()));
  }

  auto l0_files = collector->GetFlushedFiles();
  EXPECT_EQ(5, l0_files.size());

  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->LoadDependency({
      {"CompactFilesImpl:2", "CompactFilesTest.CapturingPendingFiles:0"},
      {"CompactFilesTest.CapturingPendingFiles:1", "CompactFilesImpl:3"},
  });
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->EnableProcessing();

  // Start compacting files.
  ROCKSDB_NAMESPACE::port::Thread compaction_thread(
      [&] { EXPECT_OK(db->CompactFiles(CompactionOptions(), l0_files, 1)); });

  // In the meantime flush another file.
  TEST_SYNC_POINT("CompactFilesTest.CapturingPendingFiles:0");
  ASSERT_OK(db->Put(WriteOptions(), "key5", "value"));
  ASSERT_OK(db->Flush(FlushOptions()));
  TEST_SYNC_POINT("CompactFilesTest.CapturingPendingFiles:1");

  compaction_thread.join();

  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->DisableProcessing();

  delete db;

  // Make sure we can reopen the DB.
  s = DB::Open(options, db_name_, &db);
  ASSERT_OK(s);
  assert(db);
  delete db;
}

TEST_F(CompactFilesTest, CompactionFilterWithGetSv) {
  class FilterWithGet : public CompactionFilter {
   public:
    bool Filter(int /*level*/, const Slice& /*key*/, const Slice& /*value*/,
                std::string* /*new_value*/,
                bool* /*value_changed*/) const override {
      if (db_ == nullptr) {
        return true;
      }
      std::string res;
      db_->Get(ReadOptions(), "", &res);
      return true;
    }

    void SetDB(DB* db) { db_ = db; }

    const char* Name() const override { return "FilterWithGet"; }

   private:
    DB* db_;
  };

  std::shared_ptr<FilterWithGet> cf(new FilterWithGet());

  Options options;
  options.create_if_missing = true;
  options.compaction_filter = cf.get();

  DB* db = nullptr;
  DestroyDB(db_name_, options);
  Status s = DB::Open(options, db_name_, &db);
  ASSERT_OK(s);

  cf->SetDB(db);

  // Write one L0 file
  ASSERT_OK(db->Put(WriteOptions(), "K1", "V1"));
  ASSERT_OK(db->Flush(FlushOptions()));

  // Compact all L0 files using CompactFiles
  ROCKSDB_NAMESPACE::ColumnFamilyMetaData meta;
  db->GetColumnFamilyMetaData(&meta);
  for (auto& file : meta.levels[0].files) {
    std::string fname = file.db_path + "/" + file.name;
    ASSERT_OK(
        db->CompactFiles(ROCKSDB_NAMESPACE::CompactionOptions(), {fname}, 0));
  }

  delete db;
}

TEST_F(CompactFilesTest, SentinelCompressionType) {
  if (!Zlib_Supported()) {
    fprintf(stderr, "zlib compression not supported, skip this test\n");
    return;
  }
  if (!Snappy_Supported()) {
    fprintf(stderr, "snappy compression not supported, skip this test\n");
    return;
  }
  // Check that passing `CompressionType::kDisableCompressionOption` to
  // `CompactFiles` causes it to use the column family compression options.
  for (auto compaction_style : {CompactionStyle::kCompactionStyleLevel,
                                CompactionStyle::kCompactionStyleUniversal,
                                CompactionStyle::kCompactionStyleNone}) {
    DestroyDB(db_name_, Options());
    Options options;
    options.compaction_style = compaction_style;
    // L0: Snappy, L1: ZSTD, L2: Snappy
    options.compression_per_level = {CompressionType::kSnappyCompression,
                                     CompressionType::kZlibCompression,
                                     CompressionType::kSnappyCompression};
    options.create_if_missing = true;
    FlushedFileCollector* collector = new FlushedFileCollector();
    options.listeners.emplace_back(collector);
    DB* db = nullptr;
    ASSERT_OK(DB::Open(options, db_name_, &db));

    ASSERT_OK(db->Put(WriteOptions(), "key", "val"));
    ASSERT_OK(db->Flush(FlushOptions()));

    auto l0_files = collector->GetFlushedFiles();
    ASSERT_EQ(1, l0_files.size());

    // L0->L1 compaction, so output should be ZSTD-compressed
    CompactionOptions compaction_opts;
    compaction_opts.compression = CompressionType::kDisableCompressionOption;
    ASSERT_OK(db->CompactFiles(compaction_opts, l0_files, 1));

    ROCKSDB_NAMESPACE::TablePropertiesCollection all_tables_props;
    ASSERT_OK(db->GetPropertiesOfAllTables(&all_tables_props));
    for (const auto& name_and_table_props : all_tables_props) {
      ASSERT_EQ(CompressionTypeToString(CompressionType::kZlibCompression),
                name_and_table_props.second->compression_name);
    }
    delete db;
  }
}

TEST_F(CompactFilesTest, GetCompactionJobInfo) {
  Options options;
  options.create_if_missing = true;
  // Disable RocksDB background compaction.
  options.compaction_style = kCompactionStyleNone;
  options.level0_slowdown_writes_trigger = 1000;
  options.level0_stop_writes_trigger = 1000;
  options.write_buffer_size = 65536;
  options.max_write_buffer_number = 2;
  options.compression = kNoCompression;
  options.max_compaction_bytes = 5000;

  // Add listener
  FlushedFileCollector* collector = new FlushedFileCollector();
  options.listeners.emplace_back(collector);

  DB* db = nullptr;
  DestroyDB(db_name_, options);
  Status s = DB::Open(options, db_name_, &db);
  ASSERT_OK(s);
  assert(db);

  // create couple files
  for (int i = 0; i < 500; ++i) {
    ASSERT_OK(db->Put(WriteOptions(), ToString(i),
                      std::string(1000, 'a' + (i % 26))));
  }
  ASSERT_OK(static_cast_with_check<DBImpl>(db)->TEST_WaitForFlushMemTable());
  auto l0_files_1 = collector->GetFlushedFiles();
  CompactionOptions co;
  co.compression = CompressionType::kLZ4Compression;
  CompactionJobInfo compaction_job_info{};
  ASSERT_OK(
      db->CompactFiles(co, l0_files_1, 0, -1, nullptr, &compaction_job_info));
  ASSERT_EQ(compaction_job_info.base_input_level, 0);
  ASSERT_EQ(compaction_job_info.cf_id, db->DefaultColumnFamily()->GetID());
  ASSERT_EQ(compaction_job_info.cf_name, db->DefaultColumnFamily()->GetName());
  ASSERT_EQ(compaction_job_info.compaction_reason,
            CompactionReason::kManualCompaction);
  ASSERT_EQ(compaction_job_info.compression, CompressionType::kLZ4Compression);
  ASSERT_EQ(compaction_job_info.output_level, 0);
  ASSERT_OK(compaction_job_info.status);
  // no assertion failure
  delete db;
}

TEST_F(CompactFilesTest, IsWriteStalled) {
  class SlowFilter : public CompactionFilter {
   public:
    SlowFilter(std::atomic<bool>* would_block) { would_block_ = would_block; }

    bool Filter(int /*level*/, const Slice& /*key*/, const Slice& /*value*/,
                std::string* /*new_value*/,
                bool* /*value_changed*/) const override {
      while (would_block_->load(std::memory_order_relaxed)) {
        usleep(10000);
      }
      return false;
    }

    const char* Name() const override { return "SlowFilter"; }

   private:
    std::atomic<bool>* would_block_;
  };

  Options options;
  options.create_if_missing = true;
  options.delayed_write_rate = 1;

  ColumnFamilyOptions cf_options;
  cf_options.level0_slowdown_writes_trigger = 12;
  cf_options.level0_stop_writes_trigger = 15;
  cf_options.write_buffer_size = 1024 * 1024;

  std::atomic<bool> compaction_would_block;
  compaction_would_block.store(true, std::memory_order_relaxed);
  cf_options.compaction_filter = new SlowFilter(&compaction_would_block);

  std::vector<ColumnFamilyDescriptor> cfds;
  cfds.push_back(ColumnFamilyDescriptor("default", cf_options));

  DB* db = nullptr;
  std::vector<ColumnFamilyHandle*> handles;
  DestroyDB(db_name_, options);

  Status s = DB::Open(options, db_name_, cfds, &handles, &db);
  assert(s.ok());
  assert(db);

  int flushed_l0_files = 0;
  for (; flushed_l0_files < 100;) {
    WriteBatch wb;
    for (int j = 0; j < 100; ++j) {
      char key[16];
      bzero(key, 16);
      snprintf(key, sizeof(key), "foo%.2d", j);

      ASSERT_OK(wb.Put(handles[0], key, "bar"));
    }

    WriteOptions wopts;
    wopts.no_slowdown = true;
    s = db->Write(wopts, &wb);
    if (s.ok()) {
      FlushOptions fopts;
      fopts.allow_write_stall = true;
      ASSERT_OK(db->Flush(fopts));
      ++flushed_l0_files;
    } else {
      ASSERT_EQ(s.code(), Status::Code::kIncomplete);
      break;
    }
  }

  // The write loop must be terminated by write stall.
  ASSERT_EQ(flushed_l0_files, 12);
  uint64_t stalled = false;
  db->GetIntProperty(handles[0], "rocksdb.is-write-stalled", &stalled);
  ASSERT_TRUE(stalled);

  compaction_would_block.store(false, std::memory_order_relaxed);
  for (size_t i = 0; i < handles.size(); ++i) {
    delete handles[i];
  }
  delete (db);
}

}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

#else
#include <stdio.h>

int main(int /*argc*/, char** /*argv*/) {
  fprintf(stderr,
          "SKIPPED as DBImpl::CompactFiles is not supported in ROCKSDB_LITE\n");
  return 0;
}

#endif  // !ROCKSDB_LITE
