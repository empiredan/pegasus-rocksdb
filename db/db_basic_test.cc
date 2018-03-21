//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
#include "db/db_test_util.h"
#include "port/stack_trace.h"
#include "rocksdb/perf_context.h"
#if !defined(ROCKSDB_LITE)
#include "util/sync_point.h"
#endif

namespace rocksdb {

class DBBasicTest : public DBTestBase {
 public:
  DBBasicTest() : DBTestBase("/db_basic_test") {}
};

TEST_F(DBBasicTest, OpenWhenOpen) {
  Options options = CurrentOptions();
  options.env = env_;
  rocksdb::DB* db2 = nullptr;
  rocksdb::Status s = DB::Open(options, dbname_, &db2);

  ASSERT_EQ(Status::Code::kIOError, s.code());
  ASSERT_EQ(Status::SubCode::kNone, s.subcode());
  ASSERT_TRUE(strstr(s.getState(), "lock ") != nullptr);

  delete db2;
}

#ifndef ROCKSDB_LITE
TEST_F(DBBasicTest, ReadOnlyDB) {
  ASSERT_OK(Put("foo", "v1"));
  ASSERT_OK(Put("bar", "v2"));
  ASSERT_OK(Put("foo", "v3"));
  Close();

  auto options = CurrentOptions();
  assert(options.env == env_);
  ASSERT_OK(ReadOnlyReopen(options));
  ASSERT_EQ("v3", Get("foo"));
  ASSERT_EQ("v2", Get("bar"));
  Iterator* iter = db_->NewIterator(ReadOptions());
  int count = 0;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    ASSERT_OK(iter->status());
    ++count;
  }
  ASSERT_EQ(count, 2);
  delete iter;
  Close();

  // Reopen and flush memtable.
  Reopen(options);
  Flush();
  Close();
  // Now check keys in read only mode.
  ASSERT_OK(ReadOnlyReopen(options));
  ASSERT_EQ("v3", Get("foo"));
  ASSERT_EQ("v2", Get("bar"));
  ASSERT_TRUE(db_->SyncWAL().IsNotSupported());
}

TEST_F(DBBasicTest, CompactedDB) {
  const uint64_t kFileSize = 1 << 20;
  Options options = CurrentOptions();
  options.disable_auto_compactions = true;
  options.write_buffer_size = kFileSize;
  options.target_file_size_base = kFileSize;
  options.max_bytes_for_level_base = 1 << 30;
  options.compression = kNoCompression;
  Reopen(options);
  // 1 L0 file, use CompactedDB if max_open_files = -1
  ASSERT_OK(Put("aaa", DummyString(kFileSize / 2, '1')));
  Flush();
  Close();
  ASSERT_OK(ReadOnlyReopen(options));
  Status s = Put("new", "value");
  ASSERT_EQ(s.ToString(),
            "Not implemented: Not supported operation in read only mode.");
  ASSERT_EQ(DummyString(kFileSize / 2, '1'), Get("aaa"));
  Close();
  options.max_open_files = -1;
  ASSERT_OK(ReadOnlyReopen(options));
  s = Put("new", "value");
  ASSERT_EQ(s.ToString(),
            "Not implemented: Not supported in compacted db mode.");
  ASSERT_EQ(DummyString(kFileSize / 2, '1'), Get("aaa"));
  Close();
  Reopen(options);
  // Add more L0 files
  ASSERT_OK(Put("bbb", DummyString(kFileSize / 2, '2')));
  Flush();
  ASSERT_OK(Put("aaa", DummyString(kFileSize / 2, 'a')));
  Flush();
  ASSERT_OK(Put("bbb", DummyString(kFileSize / 2, 'b')));
  ASSERT_OK(Put("eee", DummyString(kFileSize / 2, 'e')));
  Flush();
  Close();

  ASSERT_OK(ReadOnlyReopen(options));
  // Fallback to read-only DB
  s = Put("new", "value");
  ASSERT_EQ(s.ToString(),
            "Not implemented: Not supported operation in read only mode.");
  Close();

  // Full compaction
  Reopen(options);
  // Add more keys
  ASSERT_OK(Put("fff", DummyString(kFileSize / 2, 'f')));
  ASSERT_OK(Put("hhh", DummyString(kFileSize / 2, 'h')));
  ASSERT_OK(Put("iii", DummyString(kFileSize / 2, 'i')));
  ASSERT_OK(Put("jjj", DummyString(kFileSize / 2, 'j')));
  db_->CompactRange(CompactRangeOptions(), nullptr, nullptr);
  ASSERT_EQ(3, NumTableFilesAtLevel(1));
  Close();

  // CompactedDB
  ASSERT_OK(ReadOnlyReopen(options));
  s = Put("new", "value");
  ASSERT_EQ(s.ToString(),
            "Not implemented: Not supported in compacted db mode.");
  ASSERT_EQ("NOT_FOUND", Get("abc"));
  ASSERT_EQ(DummyString(kFileSize / 2, 'a'), Get("aaa"));
  ASSERT_EQ(DummyString(kFileSize / 2, 'b'), Get("bbb"));
  ASSERT_EQ("NOT_FOUND", Get("ccc"));
  ASSERT_EQ(DummyString(kFileSize / 2, 'e'), Get("eee"));
  ASSERT_EQ(DummyString(kFileSize / 2, 'f'), Get("fff"));
  ASSERT_EQ("NOT_FOUND", Get("ggg"));
  ASSERT_EQ(DummyString(kFileSize / 2, 'h'), Get("hhh"));
  ASSERT_EQ(DummyString(kFileSize / 2, 'i'), Get("iii"));
  ASSERT_EQ(DummyString(kFileSize / 2, 'j'), Get("jjj"));
  ASSERT_EQ("NOT_FOUND", Get("kkk"));

  // MultiGet
  std::vector<std::string> values;
  std::vector<Status> status_list = dbfull()->MultiGet(
      ReadOptions(),
      std::vector<Slice>({Slice("aaa"), Slice("ccc"), Slice("eee"),
                          Slice("ggg"), Slice("iii"), Slice("kkk")}),
      &values);
  ASSERT_EQ(status_list.size(), static_cast<uint64_t>(6));
  ASSERT_EQ(values.size(), static_cast<uint64_t>(6));
  ASSERT_OK(status_list[0]);
  ASSERT_EQ(DummyString(kFileSize / 2, 'a'), values[0]);
  ASSERT_TRUE(status_list[1].IsNotFound());
  ASSERT_OK(status_list[2]);
  ASSERT_EQ(DummyString(kFileSize / 2, 'e'), values[2]);
  ASSERT_TRUE(status_list[3].IsNotFound());
  ASSERT_OK(status_list[4]);
  ASSERT_EQ(DummyString(kFileSize / 2, 'i'), values[4]);
  ASSERT_TRUE(status_list[5].IsNotFound());

  Reopen(options);
  // Add a key
  ASSERT_OK(Put("fff", DummyString(kFileSize / 2, 'f')));
  Close();
  ASSERT_OK(ReadOnlyReopen(options));
  s = Put("new", "value");
  ASSERT_EQ(s.ToString(),
            "Not implemented: Not supported operation in read only mode.");
}

TEST_F(DBBasicTest, LevelLimitReopen) {
  Options options = CurrentOptions();

  const std::string value(1024 * 1024, ' ');
  int i = 0;
  while (NumTableFilesAtLevel(2, 0) == 0) {
    ASSERT_OK(Put(Key(i++), value));
    dbfull()->TEST_WaitForFlushMemTable();
    dbfull()->TEST_WaitForCompact();
  }

  options.num_levels = 1;
  options.max_bytes_for_level_multiplier_additional.resize(1, 1);
  Status s = TryReopenWithColumnFamilies({"default", "pikachu"}, options);
  ASSERT_EQ(s.IsInvalidArgument(), true);
  ASSERT_EQ(s.ToString(),
            "Invalid argument: db has more levels than options.num_levels");

  options.num_levels = 10;
  options.max_bytes_for_level_multiplier_additional.resize(10, 1);
  ASSERT_OK(TryReopenWithColumnFamilies({"default"}, options));
}
#endif  // ROCKSDB_LITE

TEST_F(DBBasicTest, PutDeleteGet) {
  do {
    ASSERT_OK(Put("foo", "v1"));
    ASSERT_EQ("v1", Get("foo"));
    ASSERT_OK(Put("foo", "v2"));
    ASSERT_EQ("v2", Get("foo"));
    ASSERT_OK(Delete("foo"));
    ASSERT_EQ("NOT_FOUND", Get("foo"));
  } while (ChangeOptions(kSkipPipelinedWrite));
}

TEST_F(DBBasicTest, PutSingleDeleteGet) {
  do {
    //CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
    ASSERT_OK(Put("foo", "v1"));
    ASSERT_EQ("v1", Get("foo"));
    ASSERT_OK(Put("foo2", "v2"));
    ASSERT_EQ("v2", Get("foo2"));
    ASSERT_OK(SingleDelete("foo"));
    ASSERT_EQ("NOT_FOUND", Get("foo"));
    // Skip HashCuckooRep as it does not support single delete. FIFO and
    // universal compaction do not apply to the test case. Skip MergePut
    // because single delete does not get removed when it encounters a merge.
  } while (ChangeOptions(kSkipHashCuckoo | kSkipFIFOCompaction |
                         kSkipUniversalCompaction | kSkipMergePut |
                         kSkipPipelinedWrite));
}

TEST_F(DBBasicTest, EmptyFlush) {
  // It is possible to produce empty flushes when using single deletes. Tests
  // whether empty flushes cause issues.
  do {
    Random rnd(301);

    Options options = CurrentOptions();
    options.disable_auto_compactions = true;

    Put("a", Slice());
    SingleDelete("a");
    ASSERT_OK(Flush(0));

    ASSERT_EQ("[ ]", AllEntriesFor("a", 0));
    // Skip HashCuckooRep as it does not support single delete. FIFO and
    // universal compaction do not apply to the test case. Skip MergePut
    // because merges cannot be combined with single deletions.
  } while (ChangeOptions(kSkipHashCuckoo | kSkipFIFOCompaction |
                         kSkipUniversalCompaction | kSkipMergePut| kSkipPipelinedWrite));
}

TEST_F(DBBasicTest, GetFromVersions) {
  do {
    //CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
    ASSERT_OK(Put("foo", "v1"));
    ASSERT_OK(Flush(0));
    ASSERT_EQ("v1", Get("foo"));
//    ASSERT_EQ("NOT_FOUND", Get(0, "foo"));
  } while (ChangeOptions(kSkipPipelinedWrite));
}

#ifndef ROCKSDB_LITE
TEST_F(DBBasicTest, GetSnapshot) {
  anon::OptionsOverride options_override;
  options_override.skip_policy = kSkipNoSnapshot;
  do {
//    CreateAndReopenWithCF({"pikachu"}, CurrentOptions(options_override));
    // Try with both a short key and a long key
    for (int i = 0; i < 2; i++) {
      std::string key = (i == 0) ? std::string("foo") : std::string(200, 'x');
      ASSERT_OK(Put(key, "v1"));
      const Snapshot* s1 = db_->GetSnapshot();
      if (option_config_ == kHashCuckoo) {
        // Unsupported case.
        ASSERT_TRUE(s1 == nullptr);
        break;
      }
      ASSERT_OK(Put(key, "v2"));
      ASSERT_EQ("v2", Get(key));
      ASSERT_EQ("v1", Get(key, s1));
      ASSERT_OK(Flush(0));
      ASSERT_EQ("v2", Get(key));
      ASSERT_EQ("v1", Get(key, s1));
      db_->ReleaseSnapshot(s1);
    }
  } while (ChangeOptions(kSkipPipelinedWrite));
}
#endif  // ROCKSDB_LITE

TEST_F(DBBasicTest, CheckLock) {
  do {
    DB* localdb;
    Options options = CurrentOptions();
    ASSERT_OK(TryReopen(options));

    // second open should fail
    ASSERT_TRUE(!(DB::Open(options, dbname_, &localdb)).ok());
  } while (ChangeCompactOptions());
}

TEST_F(DBBasicTest, FlushMultipleMemtable) {
  do {
    Options options = CurrentOptions();
    WriteOptions writeOpt = WriteOptions();
    writeOpt.disableWAL = true;
    options.max_write_buffer_number = 4;
    options.min_write_buffer_number_to_merge = 3;
    options.max_write_buffer_number_to_maintain = -1;
    //CreateAndReopenWithCF({"pikachu"}, options);
    ASSERT_OK(dbfull()->Put(writeOpt,"foo", "v1"));
    ASSERT_OK(Flush(0));
    ASSERT_OK(dbfull()->Put(writeOpt,"bar", "v1"));

    ASSERT_EQ("v1", Get("foo"));
    ASSERT_EQ("v1", Get("bar"));
    ASSERT_OK(Flush(0));
  } while (ChangeCompactOptions());
}

TEST_F(DBBasicTest, DISABLED_FlushEmptyColumnFamily) {
  // Block flush thread and disable compaction thread
  env_->SetBackgroundThreads(1, Env::HIGH);
  env_->SetBackgroundThreads(1, Env::LOW);
  test::SleepingBackgroundTask sleeping_task_low;
  env_->Schedule(&test::SleepingBackgroundTask::DoSleepTask, &sleeping_task_low,
                 Env::Priority::LOW);
  test::SleepingBackgroundTask sleeping_task_high;
  env_->Schedule(&test::SleepingBackgroundTask::DoSleepTask,
                 &sleeping_task_high, Env::Priority::HIGH);

  Options options = CurrentOptions();
  // disable compaction
  options.disable_auto_compactions = true;
  WriteOptions writeOpt = WriteOptions();
  writeOpt.disableWAL = true;
  options.max_write_buffer_number = 2;
  options.min_write_buffer_number_to_merge = 1;
  options.max_write_buffer_number_to_maintain = 1;
  CreateAndReopenWithCF({"pikachu"}, options);

  auto cfh = dbfull()->DefaultColumnFamily();
  auto cfd = reinterpret_cast<ColumnFamilyHandleImpl*>(cfh)->cfd();
  ASSERT_EQ(0, cfd->imm()->NumNotFlushed());

  // Compaction can still go through even if no thread can flush the
  // mem table.
  ASSERT_OK(Flush(0));
  ASSERT_OK(Flush(1));
  ASSERT_EQ(0, cfd->imm()->NumNotFlushed());

  // Insert can go through
  ASSERT_OK(dbfull()->Put(writeOpt, handles_[0], "foo", "v1"));
  ASSERT_OK(dbfull()->Put(writeOpt, handles_[1], "bar", "v1"));

  ASSERT_EQ("v1", Get(0, "foo"));
  ASSERT_EQ("v1", Get(1, "bar"));

  // Compaction without waiting will go through even if no thread can
  // flush the mem table, which will switch the mem table.
  FlushOptions flush_options;
  flush_options.wait = false;
  ASSERT_OK(Flush(0, flush_options));
  ASSERT_OK(Flush(1, flush_options));
  ASSERT_EQ(1, cfd->imm()->NumNotFlushed());

  // Compaction without waiting will go through even if no thread can
  // flush the mem table, and no new compaction will be started.
  ASSERT_OK(Flush(0, flush_options));
  ASSERT_OK(Flush(1, flush_options));
  ASSERT_EQ(1, cfd->imm()->NumNotFlushed());

  // Insert can go through
  ASSERT_OK(dbfull()->Put(writeOpt, handles_[0], "k1", "v1"));
  ASSERT_OK(dbfull()->Put(writeOpt, handles_[1], "k2", "v2"));

  ASSERT_EQ("v1", Get(0, "k1"));
  ASSERT_EQ("v2", Get(1, "k2"));

  sleeping_task_high.WakeUp();
  sleeping_task_high.WaitUntilDone();

  // Flush can still go through.
  ASSERT_OK(Flush(0));
  ASSERT_OK(Flush(1));
  ASSERT_EQ(0, cfd->imm()->NumNotFlushed());

  sleeping_task_low.WakeUp();
  sleeping_task_low.WaitUntilDone();
}

TEST_F(DBBasicTest, FLUSH) {
  do {
//    CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
    WriteOptions writeOpt = WriteOptions();
    writeOpt.disableWAL = true;
    SetPerfLevel(kEnableTime);
    ASSERT_OK(dbfull()->Put(writeOpt,"foo", "v1"));
    // this will now also flush the last 2 writes
    ASSERT_OK(Flush(0));
    ASSERT_OK(dbfull()->Put(writeOpt,"bar", "v1"));

    get_perf_context()->Reset();
    Get("foo");
    ASSERT_TRUE((int)get_perf_context()->get_from_output_files_time > 0);
    ASSERT_EQ(2, (int)get_perf_context()->get_read_bytes);

    ReopenWithColumnFamilies({"default"}, CurrentOptions());
    ASSERT_EQ("v1", Get("foo"));
    ASSERT_EQ("v1", Get("bar"));

    writeOpt.disableWAL = true;
    ASSERT_OK(dbfull()->Put(writeOpt, "bar", "v2"));
    ASSERT_OK(dbfull()->Put(writeOpt, "foo", "v2"));
    ASSERT_OK(Flush(0));

    ReopenWithColumnFamilies({"default"}, CurrentOptions());
    ASSERT_EQ("v2", Get("bar"));
    get_perf_context()->Reset();
    ASSERT_EQ("v2", Get("foo"));
    ASSERT_TRUE((int)get_perf_context()->get_from_output_files_time > 0);

    writeOpt.disableWAL = false;
    ASSERT_OK(dbfull()->Put(writeOpt, "bar", "v3"));
    ASSERT_OK(dbfull()->Put(writeOpt, "foo", "v3"));
    ASSERT_OK(Flush(0));

    ReopenWithColumnFamilies({"default"}, CurrentOptions());
    // 'foo' should be there because its put
    // has WAL enabled.
    ASSERT_EQ("v3", Get("foo"));
    ASSERT_EQ("v3", Get("bar"));

    SetPerfLevel(kDisable);
  } while (ChangeCompactOptions());
}

TEST_F(DBBasicTest, ManifestRollOver) {
  do {
    Options options;
    options.max_manifest_file_size = 10;  // 10 bytes
    options = CurrentOptions(options);
    {
      ASSERT_OK(Put("manifest_key1", std::string(1000, '1')));
      ASSERT_OK(Put("manifest_key2", std::string(1000, '2')));
      ASSERT_OK(Put("manifest_key3", std::string(1000, '3')));
      uint64_t manifest_before_flush = dbfull()->TEST_Current_Manifest_FileNo();
      ASSERT_OK(Flush(0));  // This should trigger LogAndApply.
      uint64_t manifest_after_flush = dbfull()->TEST_Current_Manifest_FileNo();
      ASSERT_GT(manifest_after_flush, manifest_before_flush);
      ReopenWithColumnFamilies({"default"}, options);
      ASSERT_GT(dbfull()->TEST_Current_Manifest_FileNo(), manifest_after_flush);
      // check if a new manifest file got inserted or not.
      ASSERT_EQ(std::string(1000, '1'), Get("manifest_key1"));
      ASSERT_EQ(std::string(1000, '2'), Get("manifest_key2"));
      ASSERT_EQ(std::string(1000, '3'), Get("manifest_key3"));
    }
  } while (ChangeCompactOptions());
}

TEST_F(DBBasicTest, IdentityAcrossRestarts) {
  do {
    std::string id1;
    ASSERT_OK(db_->GetDbIdentity(id1));

    Options options = CurrentOptions();
    Reopen(options);
    std::string id2;
    ASSERT_OK(db_->GetDbIdentity(id2));
    // id1 should match id2 because identity was not regenerated
    ASSERT_EQ(id1.compare(id2), 0);

    std::string idfilename = IdentityFileName(dbname_);
    ASSERT_OK(env_->DeleteFile(idfilename));
    Reopen(options);
    std::string id3;
    ASSERT_OK(db_->GetDbIdentity(id3));
    // id1 should NOT match id3 because identity was regenerated
    ASSERT_NE(id1.compare(id3), 0);
  } while (ChangeCompactOptions());
}

#ifndef ROCKSDB_LITE
TEST_F(DBBasicTest, Snapshot) {
  anon::OptionsOverride options_override;
  options_override.skip_policy = kSkipNoSnapshot;
  do {
    //CreateAndReopenWithCF({"pikachu"}, CurrentOptions(options_override));
    Put(/*0, */"foo", "0v1");
    //Put(1, "foo", "1v1");

    const Snapshot* s1 = db_->GetSnapshot();
    ASSERT_EQ(1U, GetNumSnapshots());
    uint64_t time_snap1 = GetTimeOldestSnapshots();
    ASSERT_GT(time_snap1, 0U);
    Put(/*0, */"foo", "0v2");
    //Put(1, "foo", "1v2");

    env_->addon_time_.fetch_add(1);

    const Snapshot* s2 = db_->GetSnapshot();
    ASSERT_EQ(2U, GetNumSnapshots());
    ASSERT_EQ(time_snap1, GetTimeOldestSnapshots());
    Put(/*0, */"foo", "0v3");
    //Put(1, "foo", "1v3");

    {
      ManagedSnapshot s3(db_);
      ASSERT_EQ(3U, GetNumSnapshots());
      ASSERT_EQ(time_snap1, GetTimeOldestSnapshots());

      Put(/*0, */"foo", "0v4");
      //Put(1, "foo", "1v4");
      ASSERT_EQ("0v1", Get(/*0, */"foo", s1));
      //ASSERT_EQ("1v1", Get(1, "foo", s1));
      ASSERT_EQ("0v2", Get(/*0, */"foo", s2));
      //ASSERT_EQ("1v2", Get(1, "foo", s2));
      ASSERT_EQ("0v3", Get(/*0, */"foo", s3.snapshot()));
      //ASSERT_EQ("1v3", Get(1, "foo", s3.snapshot()));
      ASSERT_EQ("0v4", Get(/*0, */"foo"));
      //ASSERT_EQ("1v4", Get(1, "foo"));
    }

    ASSERT_EQ(2U, GetNumSnapshots());
    ASSERT_EQ(time_snap1, GetTimeOldestSnapshots());
    ASSERT_EQ("0v1", Get(/*0, */"foo", s1));
    //ASSERT_EQ("1v1", Get(1, "foo", s1));
    ASSERT_EQ("0v2", Get(/*0, */"foo", s2));
    //ASSERT_EQ("1v2", Get(1, "foo", s2));
    ASSERT_EQ("0v4", Get(/*0, */"foo"));
    //ASSERT_EQ("1v4", Get(1, "foo"));

    db_->ReleaseSnapshot(s1);
    ASSERT_EQ("0v2", Get(/*0, */"foo", s2));
    //ASSERT_EQ("1v2", Get(1, "foo", s2));
    ASSERT_EQ("0v4", Get(/*0, */"foo"));
    //ASSERT_EQ("1v4", Get(1, "foo"));
    ASSERT_EQ(1U, GetNumSnapshots());
    ASSERT_LT(time_snap1, GetTimeOldestSnapshots());

    db_->ReleaseSnapshot(s2);
    ASSERT_EQ(0U, GetNumSnapshots());
    ASSERT_EQ("0v4", Get(/*0, */"foo"));
    //ASSERT_EQ("1v4", Get(1, "foo"));
  } while (ChangeOptions(kSkipHashCuckoo | kSkipPipelinedWrite));
}

#endif  // ROCKSDB_LITE

TEST_F(DBBasicTest, CompactBetweenSnapshots) {
  anon::OptionsOverride options_override;
  options_override.skip_policy = kSkipNoSnapshot;
  do {
    Options options = CurrentOptions(options_override);
    options.disable_auto_compactions = true;
    Reopen(options);
    Random rnd(301);
    FillLevels("a", "z", 0);

    Put("foo", "first");
    const Snapshot* snapshot1 = db_->GetSnapshot();
    Put("foo", "second");
    Put("foo", "third");
    Put("foo", "fourth");
    const Snapshot* snapshot2 = db_->GetSnapshot();
    Put("foo", "fifth");
    Put("foo", "sixth");

    // All entries (including duplicates) exist
    // before any compaction or flush is triggered.
    ASSERT_EQ(AllEntriesFor("foo", 0),
              "[ sixth, fifth, fourth, third, second, first ]");
    ASSERT_EQ("sixth", Get("foo"));
    ASSERT_EQ("fourth", Get("foo", snapshot2));
    ASSERT_EQ("first", Get("foo", snapshot1));

    // After a flush, "second", "third" and "fifth" should
    // be removed
    ASSERT_OK(Flush(0));
    ASSERT_EQ(AllEntriesFor("foo"), "[ sixth, fourth, first ]");

    // after we release the snapshot1, only two values left
    db_->ReleaseSnapshot(snapshot1);
    FillLevels("a", "z", 0);
    dbfull()->CompactRange(CompactRangeOptions(), nullptr,
                           nullptr);

    // We have only one valid snapshot snapshot2. Since snapshot1 is
    // not valid anymore, "first" should be removed by a compaction.
    ASSERT_EQ("sixth", Get("foo"));
    ASSERT_EQ("fourth", Get("foo", snapshot2));
    ASSERT_EQ(AllEntriesFor("foo", 0), "[ sixth, fourth ]");

    // after we release the snapshot2, only one value should be left
    db_->ReleaseSnapshot(snapshot2);
    FillLevels("a", "z", 0);
    dbfull()->CompactRange(CompactRangeOptions(), nullptr,
                           nullptr);
    ASSERT_EQ("sixth", Get("foo"));
    ASSERT_EQ(AllEntriesFor("foo", 0), "[ sixth ]");
    // skip HashCuckooRep as it does not support snapshot
  } while (ChangeOptions(kSkipHashCuckoo | kSkipFIFOCompaction
                         | kSkipPipelinedWrite));
}

TEST_F(DBBasicTest, DBOpen_Options) {
  Options options = CurrentOptions();
  std::string dbname = test::TmpDir(env_) + "/db_options_test";
  ASSERT_OK(DestroyDB(dbname, options));

  // Does not exist, and create_if_missing == false: error
  DB* db = nullptr;
  options.create_if_missing = false;
  Status s = DB::Open(options, dbname, &db);
  ASSERT_TRUE(strstr(s.ToString().c_str(), "does not exist") != nullptr);
  ASSERT_TRUE(db == nullptr);

  // Does not exist, and create_if_missing == true: OK
  options.create_if_missing = true;
  s = DB::Open(options, dbname, &db);
  ASSERT_OK(s);
  ASSERT_TRUE(db != nullptr);

  delete db;
  db = nullptr;

  // Does exist, and error_if_exists == true: error
  options.create_if_missing = false;
  options.error_if_exists = true;
  s = DB::Open(options, dbname, &db);
  ASSERT_TRUE(strstr(s.ToString().c_str(), "exists") != nullptr);
  ASSERT_TRUE(db == nullptr);

  // Does exist, and error_if_exists == false: OK
  options.create_if_missing = true;
  options.error_if_exists = false;
  s = DB::Open(options, dbname, &db);
  ASSERT_OK(s);
  ASSERT_TRUE(db != nullptr);

  delete db;
  db = nullptr;
}

TEST_F(DBBasicTest, CompactOnFlush) {
  anon::OptionsOverride options_override;
  options_override.skip_policy = kSkipNoSnapshot;
  do {
    Options options = CurrentOptions(options_override);
    options.disable_auto_compactions = true;
    //CreateAndReopenWithCF({"pikachu"}, options);

    Put("foo", "v1");
    ASSERT_OK(Flush(0));
    ASSERT_EQ(AllEntriesFor("foo", 0), "[ v1 ]");

    // Write two new keys
    Put("a", "begin");
    Put("z", "end");
    Flush(0);

    // Case1: Delete followed by a put
    Delete("foo");
    Put("foo", "v2");
    ASSERT_EQ(AllEntriesFor("foo", 0), "[ v2, DEL, v1 ]");

    // After the current memtable is flushed, the DEL should
    // have been removed
    ASSERT_OK(Flush(0));
    ASSERT_EQ(AllEntriesFor("foo", 0), "[ v2, v1 ]");

    Put("foo", "v2");        // add data to memtable to ensure compact will be executed
    dbfull()->CompactRange(CompactRangeOptions(), nullptr,
                           nullptr);
    ASSERT_EQ(AllEntriesFor("foo", 0), "[ v2 ]");

    // Case 2: Delete followed by another delete
    Delete("foo");
    Delete("foo");
    ASSERT_EQ(AllEntriesFor("foo"), "[ DEL, DEL, v2 ]");
    ASSERT_OK(Flush(0));
    ASSERT_EQ(AllEntriesFor("foo", 0), "[ DEL, v2 ]");
    Delete("foo");
    dbfull()->CompactRange(CompactRangeOptions(), nullptr,
                           nullptr);
    ASSERT_EQ(AllEntriesFor("foo", 0), "[ ]");

    // Case 3: Put followed by a delete
    Put("foo", "v3");
    Delete("foo");
    ASSERT_EQ(AllEntriesFor("foo", 0), "[ DEL, v3 ]");
    ASSERT_OK(Flush(0));
    ASSERT_EQ(AllEntriesFor("foo", 0), "[ DEL ]");
    Delete("foo");
    dbfull()->CompactRange(CompactRangeOptions(), nullptr,
                           nullptr);
    ASSERT_EQ(AllEntriesFor("foo", 0), "[ ]");

    // Case 4: Put followed by another Put
    Put("foo", "v4");
    Put("foo", "v5");
    ASSERT_EQ(AllEntriesFor("foo", 0), "[ v5, v4 ]");
    ASSERT_OK(Flush(0));
    ASSERT_EQ(AllEntriesFor("foo", 0), "[ v5 ]");
    Put("foo", "v5");
    dbfull()->CompactRange(CompactRangeOptions(), nullptr,
                           nullptr);
    ASSERT_EQ(AllEntriesFor("foo", 0), "[ v5 ]");

    // clear database
    Delete("foo");
    dbfull()->CompactRange(CompactRangeOptions(), nullptr,
                           nullptr);
    ASSERT_EQ(AllEntriesFor("foo", 0), "[ ]");

    // Case 5: Put followed by snapshot followed by another Put
    // Both puts should remain.
    Put("foo", "v6");
    const Snapshot* snapshot = db_->GetSnapshot();
    Put("foo", "v7");
    ASSERT_OK(Flush(0));
    ASSERT_EQ(AllEntriesFor("foo", 0), "[ v7, v6 ]");
    db_->ReleaseSnapshot(snapshot);

    // clear database
    Delete("foo");
    dbfull()->CompactRange(CompactRangeOptions(), nullptr,
                           nullptr);
    ASSERT_EQ(AllEntriesFor("foo", 0), "[ ]");

    // Case 5: snapshot followed by a put followed by another Put
    // Only the last put should remain.
    const Snapshot* snapshot1 = db_->GetSnapshot();
    Put("foo", "v8");
    Put("foo", "v9");
    ASSERT_OK(Flush(0));
    ASSERT_EQ(AllEntriesFor("foo", 0), "[ v9 ]");
    db_->ReleaseSnapshot(snapshot1);
  } while (ChangeCompactOptions());
}

TEST_F(DBBasicTest, DISABLED_FlushOneColumnFamily) {
  Options options = CurrentOptions();
  CreateAndReopenWithCF({"pikachu", "ilya", "muromec", "dobrynia", "nikitich",
                         "alyosha", "popovich"},
                        options);

  ASSERT_OK(Put(0, "Default", "Default"));
  ASSERT_OK(Put(1, "pikachu", "pikachu"));
  ASSERT_OK(Put(2, "ilya", "ilya"));
  ASSERT_OK(Put(3, "muromec", "muromec"));
  ASSERT_OK(Put(4, "dobrynia", "dobrynia"));
  ASSERT_OK(Put(5, "nikitich", "nikitich"));
  ASSERT_OK(Put(6, "alyosha", "alyosha"));
  ASSERT_OK(Put(7, "popovich", "popovich"));

  for (int i = 0; i < 8; ++i) {
    Flush(i);
    auto tables = ListTableFiles(env_, dbname_);
    ASSERT_EQ(tables.size(), i + 1U);
  }
}

TEST_F(DBBasicTest, MultiGetSimple) {
  do {
    SetPerfLevel(kEnableCount);
    ASSERT_OK(Put("k1", "v1"));
    ASSERT_OK(Put("k2", "v2"));
    ASSERT_OK(Put("k3", "v3"));
    ASSERT_OK(Put("k4", "v4"));
    ASSERT_OK(Delete("k4"));
    ASSERT_OK(Put("k5", "v5"));
    ASSERT_OK(Delete("no_key"));

    std::vector<Slice> keys({"k1", "k2", "k3", "k4", "k5", "no_key"});

    std::vector<std::string> values(20, "Temporary data to be overwritten");

    get_perf_context()->Reset();
    std::vector<Status> s = db_->MultiGet(ReadOptions(), keys, &values);
    ASSERT_EQ(values.size(), keys.size());
    ASSERT_EQ(values[0], "v1");
    ASSERT_EQ(values[1], "v2");
    ASSERT_EQ(values[2], "v3");
    ASSERT_EQ(values[4], "v5");
    // four kv pairs * two bytes per value
    ASSERT_EQ(8, (int)get_perf_context()->multiget_read_bytes);

    ASSERT_OK(s[0]);
    ASSERT_OK(s[1]);
    ASSERT_OK(s[2]);
    ASSERT_TRUE(s[3].IsNotFound());
    ASSERT_OK(s[4]);
    ASSERT_TRUE(s[5].IsNotFound());
    SetPerfLevel(kDisable);
  } while (ChangeCompactOptions());
}

TEST_F(DBBasicTest, MultiGetEmpty) {
  do {
    CreateAndReopenWithCF({"pikachu"}, CurrentOptions());
    // Empty Key Set
    std::vector<Slice> keys;
    std::vector<std::string> values;
    std::vector<ColumnFamilyHandle*> cfs;
    std::vector<Status> s = db_->MultiGet(ReadOptions(), cfs, keys, &values);
    ASSERT_EQ(s.size(), 0U);

    // Empty Database, Empty Key Set
    Options options = CurrentOptions();
    options.create_if_missing = true;
    DestroyAndReopen(options);
    CreateAndReopenWithCF({"pikachu"}, options);
    s = db_->MultiGet(ReadOptions(), cfs, keys, &values);
    ASSERT_EQ(s.size(), 0U);

    // Empty Database, Search for Keys
    keys.resize(2);
    keys[0] = "a";
    keys[1] = "b";
    cfs.push_back(handles_[0]);
    cfs.push_back(handles_[1]);
    s = db_->MultiGet(ReadOptions(), cfs, keys, &values);
    ASSERT_EQ(static_cast<int>(s.size()), 2);
    ASSERT_TRUE(s[0].IsNotFound() && s[1].IsNotFound());
  } while (ChangeCompactOptions());
}

TEST_F(DBBasicTest, ChecksumTest) {
  BlockBasedTableOptions table_options;
  Options options = CurrentOptions();
  // change when new checksum type added
  int max_checksum = static_cast<int>(kxxHash);
  const int kNumPerFile = 2;

  // generate one table with each type of checksum
  for (int i = 0; i <= max_checksum; ++i) {
    table_options.checksum = static_cast<ChecksumType>(i);
    options.table_factory.reset(NewBlockBasedTableFactory(table_options));
    Reopen(options);
    for (int j = 0; j < kNumPerFile; ++j) {
      ASSERT_OK(Put(Key(i * kNumPerFile + j), Key(i * kNumPerFile + j)));
    }
    ASSERT_OK(Flush());
  }

  // verify data with each type of checksum
  for (int i = 0; i <= kxxHash; ++i) {
    table_options.checksum = static_cast<ChecksumType>(i);
    options.table_factory.reset(NewBlockBasedTableFactory(table_options));
    Reopen(options);
    for (int j = 0; j < (max_checksum + 1) * kNumPerFile; ++j) {
      ASSERT_EQ(Key(j), Get(Key(j)));
    }
  }
}

// On Windows you can have either memory mapped file or a file
// with unbuffered access. So this asserts and does not make
// sense to run
#ifndef OS_WIN
TEST_F(DBBasicTest, MmapAndBufferOptions) {
  if (!IsMemoryMappedAccessSupported()) {
    return;
  }
  Options options = CurrentOptions();

  options.use_direct_reads = true;
  options.allow_mmap_reads = true;
  ASSERT_NOK(TryReopen(options));

  // All other combinations are acceptable
  options.use_direct_reads = false;
  ASSERT_OK(TryReopen(options));

  if (IsDirectIOSupported()) {
    options.use_direct_reads = true;
    options.allow_mmap_reads = false;
    ASSERT_OK(TryReopen(options));
  }

  options.use_direct_reads = false;
  ASSERT_OK(TryReopen(options));
}
#endif

}  // namespace rocksdb

int main(int argc, char** argv) {
  rocksdb::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
