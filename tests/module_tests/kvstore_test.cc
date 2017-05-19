/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2016 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "config.h"

#include <platform/dirutils.h>

#include "callbacks.h"
#include "couch-kvstore/couch-kvstore.h"
#include "kvstore.h"
#include "src/internal.h"
#include "tests/module_tests/test_helpers.h"
#include "tests/test_fileops.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <kvstore.h>
#include <unordered_map>
#include <vector>

class WriteCallback : public Callback<mutation_result> {
public:
    WriteCallback() {}

    void callback(mutation_result &result) {

    }

};

class StatsCallback : public Callback<kvstats_ctx> {
public:
    StatsCallback() {}

    void callback(kvstats_ctx &result) {

    }

};

class KVStoreTestCacheCallback : public Callback<CacheLookup> {
public:
    KVStoreTestCacheCallback(int64_t s, int64_t e, uint16_t vbid) :
        start(s), end(e), vb(vbid) { }

    void callback(CacheLookup &lookup) {
        EXPECT_EQ(vb, lookup.getVBucketId());
        EXPECT_LE(start, lookup.getBySeqno());
        EXPECT_LE(lookup.getBySeqno(), end);
    }

private:
    int64_t start;
    int64_t end;
    uint16_t vb;
};

class GetCallback : public Callback<GetValue> {
public:
    GetCallback(ENGINE_ERROR_CODE _expectedErrorCode = ENGINE_SUCCESS) :
        expectCompressed(false),
        expectedErrorCode(_expectedErrorCode) { }

    GetCallback(bool expect_compressed,
                ENGINE_ERROR_CODE _expectedErrorCode = ENGINE_SUCCESS) :
        expectCompressed(expect_compressed),
        expectedErrorCode(_expectedErrorCode) { }

    void callback(GetValue &result) {
        EXPECT_EQ(expectedErrorCode, result.getStatus());
        if (result.getStatus() == ENGINE_SUCCESS) {
            if (expectCompressed) {
                EXPECT_EQ(PROTOCOL_BINARY_DATATYPE_SNAPPY,
                          result.getValue()->getDataType());
                result.getValue()->decompressValue();
            }

            EXPECT_EQ(0,
                      strncmp("value",
                              result.getValue()->getData(),
                              result.getValue()->getNBytes()));
            delete result.getValue();
        }
    }

private:
    bool expectCompressed;
    ENGINE_ERROR_CODE expectedErrorCode;
};

class BloomFilterCallback : public Callback<std::string&, bool&> {
public:
    BloomFilterCallback() {}
    void callback(std::string& ra, bool& rb) override {}
};

class ExpiryCallback : public Callback<std::string&, uint64_t&> {
public:
    ExpiryCallback() {}
    void callback(std::string& ra, uint64_t& rb) override {}
};

/**
 * Utility template for generating callbacks for various
 * KVStore functions from a lambda/std::function
 */
template <typename... RV>
class CustomCallback : public Callback<RV...> {
public:
    CustomCallback(std::function<void(RV...)> _cb)
        : cb(_cb) {}
    CustomCallback()
        : cb([](RV... val){}) {}

    void callback(RV&...result) {
        cb(result...);
    }

protected:
    std::function<void(RV...)> cb;
};

/**
 * Callback that can be given a lambda to use, specifically
 * for the Rollback callback
 */
class CustomRBCallback : public RollbackCB {
public:
    CustomRBCallback(std::function<void(GetValue)> _cb)
        : cb(_cb) {}
    CustomRBCallback()
        : cb([](GetValue val){}) {}

    void callback(GetValue &result) {
        cb(result);
        delete result.getValue();
    }

protected:
    std::function<void(GetValue)> cb;
};

// Initializes a KVStore
static void initialize_kv_store(KVStore* kvstore) {
    std::string failoverLog("");
    // simulate the setVbState by incrementing the rev
    kvstore->incrementRevision(0);
    vbucket_state state(vbucket_state_active, 0, 0, 0, 0, 0, 0, 0, failoverLog);
    // simulate the setVbState by incrementing the rev
    kvstore->incrementRevision(0);
    kvstore->snapshotVBucket(0, state,
                            VBStatePersist::VBSTATE_PERSIST_WITHOUT_COMMIT);
}

// Creates and initializes a KVStore with the given config
static std::unique_ptr<KVStore> setup_kv_store(KVStoreConfig& config) {
    auto kvstore = KVStoreFactory::create(config);
    initialize_kv_store(kvstore.rw.get());
    return std::move(kvstore.rw);
}

/* Test callback for stats handling.
 * 'cookie' is a std::unordered_map<std::string, std::string) which stats
 * are accumulated in.
 */
static void add_stat_callback(const char *key, const uint16_t klen,
                              const char *val, const uint32_t vlen,
                              const void *cookie) {
    auto* map = reinterpret_cast<std::map<std::string, std::string>*>(
            const_cast<void*>(cookie));
    ASSERT_NE(nullptr, map);
    map->insert(std::make_pair(std::string(key, klen),
                               std::string(val, vlen)));
}

/**
 * Test fixture for KVStore tests. Inherited by both ForestDB and
 * Couchstore test fixtures.
 **/
class KVStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        data_dir = std::string(info->test_case_name()) + "_" + info->name() +
            ".db";
    }

    void TearDown() override {
        cb::io::rmrf(data_dir);
    }

    std::string data_dir;
};

/// Test fixture for tests which run on both Couchstore and ForestDB.
class CouchAndForestTest : public KVStoreTest,
                           public ::testing::WithParamInterface<std::string> {
};

/// Test fixture for tests which run only on Couchstore.
class CouchKVStoreTest : public KVStoreTest {
};

/* Test basic set / get of a document */
TEST_P(CouchAndForestTest, BasicTest) {
    KVStoreConfig config(
            1024, 4, data_dir, GetParam(), 0, false /*persistnamespace*/);
    auto kvstore = setup_kv_store(config);

    kvstore->begin();
    StoredDocKey key = makeStoredDocKey("key");
    Item item(key, 0, 0, "value", 5);
    WriteCallback wc;
    kvstore->set(item, wc);

    EXPECT_TRUE(kvstore->commit(nullptr /*no collections manifest*/));

    GetCallback gc;
    kvstore->get(key, 0, gc);
}

TEST_F(CouchKVStoreTest, CompressedTest) {
    KVStoreConfig config(
            1024, 4, data_dir, "couchdb", 0, false /*persistnamespace*/);
    auto kvstore = setup_kv_store(config);

    kvstore->begin();

    uint8_t datatype = PROTOCOL_BINARY_RAW_BYTES;
    WriteCallback wc;
    for (int i = 1; i <= 5; i++) {
        std::string key("key" + std::to_string(i));
        Item item(makeStoredDocKey(key),
                  0, 0, "value", 5, &datatype, 1, 0, i);
        kvstore->set(item, wc);
    }

    StatsCallback sc;
    kvstore->commit(nullptr /*no collections manifest*/);

    std::shared_ptr<Callback<GetValue> > cb(new GetCallback(true/*expectcompressed*/));
    std::shared_ptr<Callback<CacheLookup> > cl(new KVStoreTestCacheCallback(1, 5, 0));
    ScanContext* scanCtx;
    scanCtx = kvstore->initScanContext(cb, cl, 0, 1,
                                       DocumentFilter::ALL_ITEMS,
                                       ValueFilter::VALUES_COMPRESSED);

    ASSERT_NE(nullptr, scanCtx);
    EXPECT_EQ(scan_success, kvstore->scan(scanCtx));
    kvstore->destroyScanContext(scanCtx);
}

// Verify the stats returned from operations are accurate.
TEST_F(CouchKVStoreTest, StatsTest) {
    KVStoreConfig config(
            1024, 4, data_dir, "couchdb", 0, false /*persistnamespace*/);
    auto kvstore = setup_kv_store(config);

    // Perform a transaction with a single mutation (set) in it.
    kvstore->begin();
    const std::string key{"key"};
    const std::string value{"value"};
    Item item(makeStoredDocKey(key), 0, 0, value.c_str(), value.size());
    WriteCallback wc;
    kvstore->set(item, wc);

    StatsCallback sc;
    EXPECT_TRUE(kvstore->commit(nullptr /*no collections manifest*/));
    // Check statistics are correct.
    std::map<std::string, std::string> stats;
    kvstore->addStats(add_stat_callback, &stats);
    EXPECT_EQ("1", stats["rw_0:io_num_write"]);
    const size_t io_write_bytes = stoul(stats["rw_0:io_write_bytes"]);
    EXPECT_EQ(key.size() + value.size() +
              MetaData::getMetaDataSize(MetaData::Version::V1),
              io_write_bytes);

    // Hard to determine exactly how many bytes should have been written, but
    // expect non-zero, and least as many as the actual documents.
    const size_t io_total_write_bytes = stoul(stats["rw_0:io_total_write_bytes"]);
    EXPECT_GT(io_total_write_bytes, 0);
    EXPECT_GE(io_total_write_bytes, io_write_bytes);
}

// Verify the compaction stats returned from operations are accurate.
TEST_F(CouchKVStoreTest, CompactStatsTest) {
    KVStoreConfig config(
            1, 4, data_dir, "couchdb", 0, false /*persistnamespace*/);
    auto kvstore = setup_kv_store(config);

    // Perform a transaction with a single mutation (set) in it.
    kvstore->begin();
    const std::string key{"key"};
    const std::string value{"value"};
    Item item(makeStoredDocKey(key), 0, 0, value.c_str(), value.size());
    WriteCallback wc;
    kvstore->set(item, wc);

    EXPECT_TRUE(kvstore->commit(nullptr /*no collections manifest*/));

    std::shared_ptr<Callback<std::string&, bool&> >
        filter(new BloomFilterCallback());
    std::shared_ptr<Callback<std::string&, uint64_t&> >
        expiry(new ExpiryCallback());

    compaction_ctx cctx;
    cctx.purge_before_seq = 0;
    cctx.purge_before_ts = 0;
    cctx.curr_time = 0;
    cctx.drop_deletes = 0;
    cctx.db_file_id = 0;

    EXPECT_TRUE(kvstore->compactDB(&cctx));
    // Check statistics are correct.
    std::map<std::string, std::string> stats;
    kvstore->addStats(add_stat_callback, &stats);
    EXPECT_EQ("1", stats["rw_0:io_num_write"]);
    const size_t io_write_bytes = stoul(stats["rw_0:io_write_bytes"]);

    // Hard to determine exactly how many bytes should have been written, but
    // expect non-zero, and at least twice as many as the actual documents for
    // the total and once as many for compaction alone.
    const size_t io_total_write_bytes = stoul(stats["rw_0:io_total_write_bytes"]);
    const size_t io_compaction_write_bytes = stoul(stats["rw_0:io_compaction_write_bytes"]);
    EXPECT_GT(io_total_write_bytes, 0);
    EXPECT_GT(io_compaction_write_bytes, 0);
    EXPECT_GT(io_total_write_bytes, io_compaction_write_bytes);
    EXPECT_GE(io_total_write_bytes, io_write_bytes * 2);
    EXPECT_GE(io_compaction_write_bytes, io_write_bytes);
}

// Regression test for MB-17517 - ensure that if a couchstore file has a max
// CAS of -1, it is detected and reset to zero when file is loaded.
TEST_F(CouchKVStoreTest, MB_17517MaxCasOfMinus1) {
    KVStoreConfig config(
            1024, 4, data_dir, "couchdb", 0, false /*persistnamespace*/);
    auto kvstore = KVStoreFactory::create(config);
    ASSERT_NE(nullptr, kvstore.rw);

    // Activate vBucket.
    std::string failoverLog("[]");
    vbucket_state state(vbucket_state_active, /*ckid*/0, /*maxDelSeqNum*/0,
                        /*highSeqno*/0, /*purgeSeqno*/0, /*lastSnapStart*/0,
                        /*lastSnapEnd*/0, /*maxCas*/-1, failoverLog);
    EXPECT_TRUE(kvstore.rw->snapshotVBucket(
            /*vbid*/ 0, state, VBStatePersist::VBSTATE_PERSIST_WITHOUT_COMMIT));
    EXPECT_EQ(~0ull, kvstore.rw->listPersistedVbuckets()[0]->maxCas);

    // Close the file, then re-open.
    kvstore = KVStoreFactory::create(config);
    EXPECT_NE(nullptr, kvstore.rw);

    // Check that our max CAS was repaired on startup.
    EXPECT_EQ(0u, kvstore.rw->listPersistedVbuckets()[0]->maxCas);
}

// Regression test for MB-19430 - ensure that an attempt to get the
// item count from a file which doesn't exist yet propagates the
// error so the caller can detect (and retry as necessary).
TEST_F(CouchKVStoreTest, MB_18580_ENOENT) {
    KVStoreConfig config(
            1024, 4, data_dir, "couchdb", 0, false /*persistnamespace*/);
    // Create a read-only kvstore (which disables item count caching), then
    // attempt to get the count from a non-existent vbucket.
    auto kvstore = KVStoreFactory::create(config);
    ASSERT_NE(nullptr, kvstore.ro);

    // Expect to get a system_error (ENOENT)
    EXPECT_THROW(kvstore.ro->getDbFileInfo(0), std::system_error);
}

/**
 * The CouchKVStoreErrorInjectionTest cases utilise GoogleMock to inject
 * errors into couchstore as if they come from the filesystem in order
 * to observe how CouchKVStore handles the error and logs it.
 *
 * The GoogleMock framework allows expectations to be set on how an object
 * will be called and how it will respond. Generally we will set a Couchstore
 * FileOps instance to return an error code on the 'nth' call as follows:
 *
 *      EXPECT_CALL(ops, open(_, _, _, _)).Times(AnyNumber());
 *      EXPECT_CALL(ops, open(_, _, _, _))
 *          .WillOnce(Return(COUCHSTORE_ERROR_OPEN_FILE)).RetiresOnSaturation();
 *      EXPECT_CALL(ops, open(_, _, _, _)).Times(n).RetiresOnSaturation();
 *
 * We will additionally set an expectation on the LoggerMock regarding how it
 * will be called by CouchKVStore. In this instance we have set an expectation
 * that the logger will be called with a logging level greater than or equal
 * to info, and the log message will contain the error string that corresponds
 * to `COUCHSTORE_ERROR_OPEN_FILE`.
 *
 *      EXPECT_CALL(logger, mlog(_, _)).Times(AnyNumber());
 *      EXPECT_CALL(logger, mlog(Ge(EXTENSION_LOG_WARNING),
 *                               VCE(COUCHSTORE_ERROR_OPEN_FILE))
 *      ).Times(1).RetiresOnSaturation();
 */

using namespace testing;

/**
 * The MockLogger is used to verify that the logger is called with certain
 * parameters / messages.
 *
 * The MockLogger is slightly misleading in that it mocks a function that
 * is not on the API of the logger, instead mocking a function that is
 * called with the preformatted log message.
 */
class MockLogger : public Logger {
public:
    MockLogger() {
        ON_CALL(*this, mlog(_, _)).WillByDefault(Invoke([](EXTENSION_LOG_LEVEL sev,
                                                           const std::string& msg){
        }));
    }

    void vlog(EXTENSION_LOG_LEVEL severity, const char* fmt, va_list va) const override {
        mlog(severity, vatos(fmt, va));
    }

    MOCK_CONST_METHOD2(mlog, void(EXTENSION_LOG_LEVEL severity,
                                  const std::string& message));

private:
    /**
     * Convert fmt cstring and a variadic arguments list to a string
     */
    static std::string vatos(const char* fmt, va_list va) {
        std::vector<char> buffer;
        va_list cpy;

        // Calculate Size
        va_copy(cpy, va);
        buffer.resize(vsnprintf(nullptr, 0, fmt, cpy) + 1);
        va_end(cpy);

        // Write to vector and return as string
        vsnprintf(buffer.data(), buffer.size(), fmt, va);
        return std::string(buffer.data());
    }

};

/**
 * VCE: Verify Couchstore Error
 *
 * This is a GoogleMock matcher which will match against a string
 * which has the corresponding message for the passed couchstore
 * error code in it. e.g.
 *
 *     VCE(COUCHSTORE_ERROR_WRITE)
 *
 * will match against a string which contains 'error writing to file'.
 */
MATCHER_P(VCE, value, "is string of %(value)") {
    return arg.find(couchstore_strerror(value)) != std::string::npos;
}

/**
 * CouchKVStoreErrorInjectionTest is used for tests which verify
 * log messages from error injection in couchstore.
 */
class CouchKVStoreErrorInjectionTest : public ::testing::Test {
public:
    CouchKVStoreErrorInjectionTest()
        : data_dir("CouchKVStoreErrorInjectionTest.db"),
          ops(create_default_file_ops()),
          config(KVStoreConfig(1024,
                               4,
                               data_dir,
                               "couchdb",
                               0,
                               false /*persistnamespace*/)
                         .setLogger(logger)
                         .setBuffered(false)) {
        cb::io::rmrf(data_dir.c_str());
        kvstore.reset(new CouchKVStore(config, ops));
        initialize_kv_store(kvstore.get());
    }
    ~CouchKVStoreErrorInjectionTest() {
        cb::io::rmrf(data_dir.c_str());
    }

protected:
    void generate_items(size_t count) {
        for(unsigned i(0); i < count; i++) {
            std::string key("key" + std::to_string(i));
            items.push_back(Item(makeStoredDocKey(key), 0, 0, "value", 5,
                                 nullptr, 0, 0, i + 1));
        }
    }

    void populate_items(size_t count) {
        generate_items(count);
        CustomCallback<mutation_result> set_callback;
        kvstore->begin();
        for(const auto& item: items) {
            kvstore->set(item, set_callback);
        }
        kvstore->commit(nullptr /*no collections manifest*/);
    }

    vb_bgfetch_queue_t make_bgfetch_queue() {
        vb_bgfetch_queue_t itms;
        for(const auto& item: items) {
            vb_bgfetch_item_ctx_t ctx;
            ctx.isMetaOnly = false;
            itms[item.getKey()] = std::move(ctx);
        }
        return itms;
    }


    const std::string data_dir;

    ::testing::NiceMock<MockOps> ops;
    ::testing::NiceMock<MockLogger> logger;

    KVStoreConfig config;
    std::unique_ptr<CouchKVStore> kvstore;
    std::vector<Item> items;
};


/**
 * Injects error during CouchKVStore::openDB_retry/couchstore_open_db_ex
 */
TEST_F(CouchKVStoreErrorInjectionTest, openDB_retry_open_db_ex) {
    generate_items(1);
    CustomCallback<mutation_result> set_callback;

    kvstore->begin();
    kvstore->set(items.front(), set_callback);
    {
        /* Establish Logger expectation */
        EXPECT_CALL(logger, mlog(_, _)).Times(AnyNumber());
        EXPECT_CALL(logger, mlog(Ge(EXTENSION_LOG_NOTICE),
                                 VCE(COUCHSTORE_ERROR_OPEN_FILE))
        ).Times(1).RetiresOnSaturation();

        /* Establish FileOps expectation */
        EXPECT_CALL(ops, open(_, _, _, _)).Times(AnyNumber());
        EXPECT_CALL(ops, open(_, _, _, _))
            .WillOnce(Return(COUCHSTORE_ERROR_OPEN_FILE)).RetiresOnSaturation();

        kvstore->commit(nullptr /*no collections manifest*/);
    }
}

/**
 * Injects error during CouchKVStore::openDB/couchstore_open_db_ex
 */
TEST_F(CouchKVStoreErrorInjectionTest, openDB_open_db_ex) {
    generate_items(1);
    CustomCallback<mutation_result> set_callback;

    kvstore->begin();
    kvstore->set(items.front(), set_callback);
    {
        /* Establish Logger expectation */
        EXPECT_CALL(logger, mlog(_, _)).Times(AnyNumber());
        EXPECT_CALL(logger, mlog(Ge(EXTENSION_LOG_WARNING),
                                 VCE(COUCHSTORE_ERROR_OPEN_FILE))
                   ).Times(1).RetiresOnSaturation();

        /* Establish FileOps expectation */
        EXPECT_CALL(ops, open(_, _, _, _))
            .WillRepeatedly(Return(COUCHSTORE_ERROR_OPEN_FILE)).RetiresOnSaturation();

        kvstore->commit(nullptr /*no collections manifest*/);
    }
}

/**
 * Injects error during CouchKVStore::commit/couchstore_save_documents
 */
TEST_F(CouchKVStoreErrorInjectionTest, commit_save_documents) {
    generate_items(1);
    CustomCallback<mutation_result> set_callback;

    kvstore->begin();
    kvstore->set(items.front(), set_callback);
    {
        /* Establish Logger expectation */
        EXPECT_CALL(logger, mlog(_, _)).Times(AnyNumber());
        EXPECT_CALL(logger, mlog(Ge(EXTENSION_LOG_WARNING),
                                 VCE(COUCHSTORE_ERROR_WRITE))
                   ).Times(1).RetiresOnSaturation();

        /* Establish FileOps expectation */
        EXPECT_CALL(ops, pwrite(_, _, _, _, _))
            .WillOnce(Return(COUCHSTORE_ERROR_WRITE)).RetiresOnSaturation();

        kvstore->commit(nullptr /*no collections manifest*/);
    }

}

/**
 * Injects error during CouchKVStore::commit/couchstore_save_local_document
 */
TEST_F(CouchKVStoreErrorInjectionTest, commit_save_local_document) {
    generate_items(1);
    CustomCallback<mutation_result> set_callback;

    kvstore->begin();
    kvstore->set(items.front(), set_callback);
    {
        /* Establish Logger expectation */
        EXPECT_CALL(logger, mlog(_, _)).Times(AnyNumber());
        EXPECT_CALL(logger, mlog(Ge(EXTENSION_LOG_WARNING),
                                 VCE(COUCHSTORE_ERROR_WRITE))
                   ).Times(1).RetiresOnSaturation();

        /* Establish FileOps expectation */
        EXPECT_CALL(ops, pwrite(_, _, _, _, _))
            .WillOnce(Return(COUCHSTORE_ERROR_WRITE)).RetiresOnSaturation();
        EXPECT_CALL(ops, pwrite(_, _, _, _, _)).Times(6).RetiresOnSaturation();

        kvstore->commit(nullptr /*no collections manifest*/);
    }

}

/**
 * Injects error during CouchKVStore::commit/couchstore_commit
 */
TEST_F(CouchKVStoreErrorInjectionTest, commit_commit) {
    generate_items(1);
    CustomCallback<mutation_result> set_callback;

    kvstore->begin();
    kvstore->set(items.front(), set_callback);
    {
        /* Establish Logger expectation */
        EXPECT_CALL(logger, mlog(_, _)).Times(AnyNumber());
        EXPECT_CALL(logger, mlog(Ge(EXTENSION_LOG_WARNING),
                                 VCE(COUCHSTORE_ERROR_WRITE))
                   ).Times(1).RetiresOnSaturation();

        /* Establish FileOps expectation */
        EXPECT_CALL(ops, pwrite(_, _, _, _, _))
            .WillOnce(Return(COUCHSTORE_ERROR_WRITE)).RetiresOnSaturation();
        EXPECT_CALL(ops, pwrite(_, _, _, _, _)).Times(8).RetiresOnSaturation();

        kvstore->commit(nullptr /*no collections manifest*/);
    }
}

/**
 * Injects error during CouchKVStore::get/couchstore_docinfo_by_id
 */
TEST_F(CouchKVStoreErrorInjectionTest, get_docinfo_by_id) {
    populate_items(1);
    CustomCallback<GetValue> get_callback;
    {
        /* Establish Logger expectation */
        EXPECT_CALL(logger, mlog(_, _)).Times(AnyNumber());
        EXPECT_CALL(logger, mlog(Ge(EXTENSION_LOG_WARNING),
                                 VCE(COUCHSTORE_ERROR_READ))
                   ).Times(1).RetiresOnSaturation();

        /* Establish FileOps expectation */
        EXPECT_CALL(ops, pread(_, _, _, _, _))
            .WillOnce(Return(COUCHSTORE_ERROR_READ)).RetiresOnSaturation();
        EXPECT_CALL(ops, pread(_, _, _, _, _)).Times(3).RetiresOnSaturation();
        kvstore->get(items.front().getKey(), 0, get_callback);

    }
}

/**
 * Injects error during CouchKVStore::get/couchstore_open_doc_with_docinfo
 */
TEST_F(CouchKVStoreErrorInjectionTest, get_open_doc_with_docinfo) {
    populate_items(1);
    CustomCallback<GetValue> get_callback;
    {
        /* Establish Logger expectation */
        EXPECT_CALL(logger, mlog(_, _)).Times(AnyNumber());
        EXPECT_CALL(logger, mlog(Ge(EXTENSION_LOG_WARNING),
                                 VCE(COUCHSTORE_ERROR_READ))
                   ).Times(1).RetiresOnSaturation();

        /* Establish FileOps expectation */
        EXPECT_CALL(ops, pread(_, _, _, _, _))
            .WillOnce(Return(COUCHSTORE_ERROR_READ)).RetiresOnSaturation();
        EXPECT_CALL(ops, pread(_, _, _, _, _)).Times(5).RetiresOnSaturation();
        kvstore->get(items.front().getKey(), 0, get_callback);

    }
}

/**
 * Injects error during CouchKVStore::getMulti/couchstore_docinfos_by_id
 */
TEST_F(CouchKVStoreErrorInjectionTest, getMulti_docinfos_by_id) {
    populate_items(1);
    vb_bgfetch_queue_t itms(make_bgfetch_queue());
    {

        /* Establish Logger expectation */
        EXPECT_CALL(logger, mlog(_, _)).Times(AnyNumber());
        EXPECT_CALL(logger, mlog(Ge(EXTENSION_LOG_WARNING),
                                 VCE(COUCHSTORE_ERROR_READ))
                   ).Times(1).RetiresOnSaturation();

        /* Establish FileOps expectation */
        EXPECT_CALL(ops, pread(_, _, _, _, _))
            .WillOnce(Return(COUCHSTORE_ERROR_READ)).RetiresOnSaturation();
        EXPECT_CALL(ops, pread(_, _, _, _, _)).Times(3).RetiresOnSaturation();
        kvstore->getMulti(0, itms);

    }
}


/**
 * Injects error during CouchKVStore::getMulti/couchstore_open_doc_with_docinfo
 */
TEST_F(CouchKVStoreErrorInjectionTest, getMulti_open_doc_with_docinfo) {
    populate_items(1);
    vb_bgfetch_queue_t itms(make_bgfetch_queue());
    {
        /* Check preconditions */
        ASSERT_EQ(0, kvstore->getKVStoreStat().numGetFailure);

        /* Establish FileOps expectation */
        EXPECT_CALL(ops, pread(_, _, _, _, _))
            .WillOnce(Return(COUCHSTORE_ERROR_READ)).RetiresOnSaturation();
        EXPECT_CALL(ops, pread(_, _, _, _, _)).Times(5).RetiresOnSaturation();
        kvstore->getMulti(0, itms);

        EXPECT_EQ(1, kvstore->getKVStoreStat().numGetFailure);
    }
}

/**
 * Injects error during CouchKVStore::compactDB/couchstore_compact_db_ex
 */
TEST_F(CouchKVStoreErrorInjectionTest, compactDB_compact_db_ex) {
    populate_items(1);

    compaction_ctx cctx;
    cctx.purge_before_seq = 0;
    cctx.purge_before_ts = 0;
    cctx.curr_time = 0;
    cctx.drop_deletes = 0;
    cctx.db_file_id = 0;

    {
        /* Establish Logger expectation */
        EXPECT_CALL(logger, mlog(_, _)).Times(AnyNumber());
        EXPECT_CALL(logger, mlog(Ge(EXTENSION_LOG_WARNING),
                                 VCE(COUCHSTORE_ERROR_OPEN_FILE))
                   ).Times(1).RetiresOnSaturation();

        /* Establish FileOps expectation */
        EXPECT_CALL(ops, open(_, _, _, _))
            .WillOnce(Return(COUCHSTORE_ERROR_OPEN_FILE)).RetiresOnSaturation();
        EXPECT_CALL(ops, open(_, _, _, _)).Times(1).RetiresOnSaturation();
        kvstore->compactDB(&cctx);
    }
}

/**
 * Injects error during CouchKVStore::getNumItems/couchstore_changes_count
 */
TEST_F(CouchKVStoreErrorInjectionTest, getNumItems_changes_count) {
    populate_items(1);
    {
        /* Establish FileOps expectation */
        EXPECT_CALL(ops, pread(_, _, _, _, _))
            .WillOnce(Return(COUCHSTORE_ERROR_READ)).RetiresOnSaturation();
        EXPECT_CALL(ops, pread(_, _, _, _, _)).Times(3).RetiresOnSaturation();
        try {
            kvstore->getNumItems(0, 0, 100000);
            EXPECT_TRUE(false) << "kvstore->getNumItems(0, 0, 100000); should "
                                  "have thrown a runtime_error";
        } catch (const std::runtime_error& e) {
            EXPECT_THAT(std::string(e.what()), VCE(COUCHSTORE_ERROR_READ));
        }

    }
}

/**
 * Injects error during CouchKVStore::reset/couchstore_commit
 */
TEST_F(CouchKVStoreErrorInjectionTest, reset_commit) {
    populate_items(1);
    {
        /* Establish Logger expectation */
        EXPECT_CALL(logger, mlog(_, _)).Times(AnyNumber());
        EXPECT_CALL(logger, mlog(Ge(EXTENSION_LOG_WARNING),
                                 VCE(COUCHSTORE_ERROR_READ))
                   ).Times(1).RetiresOnSaturation();

        /* Establish FileOps expectation */
        EXPECT_CALL(ops, sync(_, _))
            .WillOnce(Return(COUCHSTORE_ERROR_READ)).RetiresOnSaturation();

        kvstore->reset(0);
    }
}

/**
 * Injects error during CouchKVStore::initScanContext/couchstore_changes_count
 */
TEST_F(CouchKVStoreErrorInjectionTest, initScanContext_changes_count) {
    populate_items(1);
    auto cb(std::make_shared<CustomCallback<GetValue>>());
    auto cl(std::make_shared<CustomCallback<CacheLookup>>());
    {
        /* Establish FileOps expectation */
        EXPECT_CALL(ops, pread(_, _, _, _, _))
            .WillOnce(Return(COUCHSTORE_ERROR_READ)).RetiresOnSaturation();
        EXPECT_CALL(ops, pread(_, _, _, _, _)).Times(3).RetiresOnSaturation();

        ScanContext* scanCtx = nullptr;
        scanCtx = kvstore->initScanContext(cb, cl, 0, 0,
                                           DocumentFilter::ALL_ITEMS,
                                           ValueFilter::VALUES_DECOMPRESSED);
        EXPECT_EQ(nullptr, scanCtx)
                << "kvstore->initScanContext(cb, cl, 0, 0, "
                   "DocumentFilter::ALL_ITEMS, "
                   "ValueFilter::VALUES_DECOMPRESSED); should "
                   "have returned NULL";

        kvstore->destroyScanContext(scanCtx);
    }
}

/**
 * Injects error during CouchKVStore::scan/couchstore_changes_since
 */
TEST_F(CouchKVStoreErrorInjectionTest, scan_changes_since) {
    populate_items(1);
    auto cb(std::make_shared<CustomCallback<GetValue>>());
    auto cl(std::make_shared<CustomCallback<CacheLookup>>());
    auto scan_context = kvstore->initScanContext(cb, cl, 0, 0,
                                              DocumentFilter::ALL_ITEMS,
                                              ValueFilter::VALUES_DECOMPRESSED);
    {
        /* Establish Logger expectation */
        EXPECT_CALL(logger, mlog(_, _)).Times(AnyNumber());
        EXPECT_CALL(logger, mlog(Ge(EXTENSION_LOG_WARNING),
                                 VCE(COUCHSTORE_ERROR_READ))
                   ).Times(1).RetiresOnSaturation();

        /* Establish FileOps expectation */
        EXPECT_CALL(ops, pread(_, _, _, _, _))
            .WillOnce(Return(COUCHSTORE_ERROR_READ)).RetiresOnSaturation();

        kvstore->scan(scan_context);
    }

    kvstore->destroyScanContext(scan_context);
}

/**
 * Injects error during CouchKVStore::recordDbDump/couchstore_open_doc_with_docinfo
 */
TEST_F(CouchKVStoreErrorInjectionTest, recordDbDump_open_doc_with_docinfo) {
    populate_items(1);
    auto cb(std::make_shared<CustomCallback<GetValue>>());
    auto cl(std::make_shared<CustomCallback<CacheLookup>>());
    auto scan_context = kvstore->initScanContext(cb, cl, 0, 0,
                                                 DocumentFilter::ALL_ITEMS,
                                                 ValueFilter::VALUES_DECOMPRESSED);
    {
        /* Establish Logger expectation */
        EXPECT_CALL(logger, mlog(_, _)).Times(AnyNumber());
        EXPECT_CALL(logger, mlog(Ge(EXTENSION_LOG_WARNING),
                                 VCE(COUCHSTORE_ERROR_READ))
                   ).Times(1).RetiresOnSaturation();

        /* Establish FileOps expectation */
        EXPECT_CALL(ops, pread(_, _, _, _, _))
            .WillOnce(Return(COUCHSTORE_ERROR_READ)).RetiresOnSaturation();
        EXPECT_CALL(ops, pread(_, _, _, _, _)).Times(2).RetiresOnSaturation();

        kvstore->scan(scan_context);
    }

    kvstore->destroyScanContext(scan_context);
}

/**
 * Injects error during CouchKVStore::rollback/couchstore_changes_count/1
 */
TEST_F(CouchKVStoreErrorInjectionTest, rollback_changes_count1) {
    generate_items(6);
    CustomCallback<mutation_result> set_callback;

    for(const auto item: items) {
        kvstore->begin();
        kvstore->set(item, set_callback);
        kvstore->commit(nullptr /*no collections manifest*/);
    }

    auto rcb(std::make_shared<CustomRBCallback>());
    {
        /* Establish Logger expectation */
        EXPECT_CALL(logger, mlog(_, _)).Times(AnyNumber());
        EXPECT_CALL(logger, mlog(Ge(EXTENSION_LOG_WARNING),
                                 VCE(COUCHSTORE_ERROR_READ))
                   ).Times(1).RetiresOnSaturation();

        /* Establish FileOps expectation */
        EXPECT_CALL(ops, pread(_, _, _, _, _))
            .WillOnce(Return(COUCHSTORE_ERROR_READ)).RetiresOnSaturation();
        EXPECT_CALL(ops, pread(_, _, _, _, _)).Times(3).RetiresOnSaturation();

        kvstore->rollback(0, 5, rcb);
    }
}

/**
 * Injects error during CouchKVStore::rollback/couchstore_rewind_header
 */
TEST_F(CouchKVStoreErrorInjectionTest, rollback_rewind_header) {
    generate_items(6);
    CustomCallback<mutation_result> set_callback;

    for(const auto item: items) {
        kvstore->begin();
        kvstore->set(item, set_callback);
        kvstore->commit(nullptr /*no collections manifest*/);
    }

    auto rcb(std::make_shared<CustomRBCallback>());
    {
        /* Establish Logger expectation */
        EXPECT_CALL(logger, mlog(_, _)).Times(AnyNumber());
        EXPECT_CALL(logger, mlog(Ge(EXTENSION_LOG_WARNING),
                                 VCE(COUCHSTORE_ERROR_DB_NO_LONGER_VALID))
                   ).Times(1).RetiresOnSaturation();

        /* Establish FileOps expectation */
        EXPECT_CALL(ops, pread(_, _, _, _, _))
            /* Doing an ALLOC_FAIL as Couchstore will just
             * keep rolling back otherwise */
            .WillOnce(Return(COUCHSTORE_ERROR_ALLOC_FAIL)).RetiresOnSaturation();
        EXPECT_CALL(ops, pread(_, _, _, _, _)).Times(9).RetiresOnSaturation();

        kvstore->rollback(0, 5, rcb);
    }
}

/**
 * Injects error during CouchKVStore::rollback/couchstore_changes_count/2
 */
TEST_F(CouchKVStoreErrorInjectionTest, rollback_changes_count2) {
    generate_items(6);
    CustomCallback<mutation_result> set_callback;

    for(const auto item: items) {
        kvstore->begin();
        kvstore->set(item, set_callback);
        kvstore->commit(nullptr /*no collections manifest*/);
    }

    auto rcb(std::make_shared<CustomRBCallback>());
    {
        /* Establish Logger expectation */
        EXPECT_CALL(logger, mlog(_, _)).Times(AnyNumber());
        EXPECT_CALL(logger, mlog(Ge(EXTENSION_LOG_WARNING),
                                 VCE(COUCHSTORE_ERROR_READ))
                   ).Times(1).RetiresOnSaturation();

        /* Establish FileOps expectation */
        EXPECT_CALL(ops, pread(_, _, _, _, _))
            .WillOnce(Return(COUCHSTORE_ERROR_READ)).RetiresOnSaturation();
        EXPECT_CALL(ops, pread(_, _, _, _, _)).Times(11).RetiresOnSaturation();

        kvstore->rollback(0, 5, rcb);
    }
}

/**
 * Injects error during CouchKVStore::readVBState/couchstore_open_local_document
 */
TEST_F(CouchKVStoreErrorInjectionTest, readVBState_open_local_document) {
    generate_items(6);
    CustomCallback<mutation_result> set_callback;

    for(const auto item: items) {
        kvstore->begin();
        kvstore->set(item, set_callback);
        kvstore->commit(nullptr /*no collections manifest*/);
    }

    auto rcb(std::make_shared<CustomRBCallback>());
    {
        /* Establish Logger expectation */
        EXPECT_CALL(logger, mlog(_, _)).Times(AnyNumber());
        EXPECT_CALL(logger, mlog(Ge(EXTENSION_LOG_WARNING),
                                 VCE(COUCHSTORE_ERROR_READ))
                   ).Times(1).RetiresOnSaturation();

        /* Establish FileOps expectation */
        EXPECT_CALL(ops, pread(_, _, _, _, _))
            .WillOnce(Return(COUCHSTORE_ERROR_READ)).RetiresOnSaturation();
        EXPECT_CALL(ops, pread(_, _, _, _, _)).Times(20).RetiresOnSaturation();

        kvstore->rollback(0, 5, rcb);
    }
}

/**
 * Injects error during CouchKVStore::getAllKeys/couchstore_all_docs
 */
TEST_F(CouchKVStoreErrorInjectionTest, getAllKeys_all_docs) {
    populate_items(1);

    auto adcb(std::make_shared<CustomCallback<const DocKey&>>());
    StoredDocKey start = makeStoredDocKey("");
    {
        /* Establish Logger expectation */
        EXPECT_CALL(logger, mlog(_, _)).Times(AnyNumber());
        EXPECT_CALL(logger, mlog(Ge(EXTENSION_LOG_WARNING),
                                 VCE(COUCHSTORE_ERROR_READ))
                   ).Times(1).RetiresOnSaturation();

        /* Establish FileOps expectation */
        EXPECT_CALL(ops, pread(_, _, _, _, _))
            .WillOnce(Return(COUCHSTORE_ERROR_READ)).RetiresOnSaturation();
        EXPECT_CALL(ops, pread(_, _, _, _, _)).Times(3).RetiresOnSaturation();


        kvstore->getAllKeys(0, start, 1, adcb);
    }
}

/**
 * Injects error during CouchKVStore::closeDB/couchstore_close_file
 */
TEST_F(CouchKVStoreErrorInjectionTest, closeDB_close_file) {
    {
        /* Establish Logger expectation */
        EXPECT_CALL(logger, mlog(_, _)).Times(AnyNumber());
        EXPECT_CALL(logger, mlog(Ge(EXTENSION_LOG_WARNING),
                                 VCE(COUCHSTORE_ERROR_FILE_CLOSE))
                   ).Times(1).RetiresOnSaturation();

        /* Establish FileOps expectation */
        EXPECT_CALL(ops, close(_, _)).Times(AnyNumber());
        EXPECT_CALL(ops, close(_, _))
                .WillOnce(DoAll(IgnoreResult(Invoke(ops.get_wrapped(),
                                                    &FileOpsInterface::close)),
                                Return(COUCHSTORE_ERROR_FILE_CLOSE)))
                .RetiresOnSaturation();

        populate_items(1);
    }
}

class MockCouchRequest : public CouchRequest {
public:
    class MetaData {
    public:
        MetaData()
            : cas(0),
              expiry(0),
              flags(0),
              ext1(0),
              ext2(0),
              legacyDeleted(0) {
        }

        uint64_t cas;
        uint32_t expiry;
        uint32_t flags;
        uint8_t ext1;
        uint8_t ext2;
        uint8_t legacyDeleted; // allow testing via 19byte meta document

        static const size_t sizeofV0 = 16;
        static const size_t sizeofV1 = 18;
        static const size_t sizeofV2 = 19;
    };

    MockCouchRequest(const Item& it,
                     uint64_t rev,
                     MutationRequestCallback& cb,
                     bool del)
        : CouchRequest(it, rev, cb, del, false /*persist namespace*/) {
    }

    ~MockCouchRequest() {}

    // Update what will be written as 'metadata'
    void writeMetaData(MetaData& meta, size_t size) {
        std::memcpy(dbDocInfo.rev_meta.buf, &meta, size);
        dbDocInfo.rev_meta.size = size;
    }
};

class MockCouchKVStore : public CouchKVStore {
public:
    MockCouchKVStore(KVStoreConfig& config) : CouchKVStore(config) {
    }

    // Mocks original code but returns the IORequest for fuzzing
    MockCouchRequest* setAndReturnRequest(const Item &itm, Callback<mutation_result> &cb) {
        if (isReadOnly()) {
            throw std::logic_error("MockCouchKVStore::set: Not valid on a read-only "
                            "object.");
        }
        if (!intransaction) {
            throw std::invalid_argument("MockCouchKVStore::set: intransaction must be "
                            "true to perform a set operation.");
        }

        bool deleteItem = false;
        MutationRequestCallback requestcb;
        uint64_t fileRev = dbFileRevMap[itm.getVBucketId()];

        // each req will be de-allocated after commit
        requestcb.setCb = &cb;
        MockCouchRequest *req = new MockCouchRequest(itm, fileRev, requestcb, deleteItem);
        pendingReqsQ.push_back(req);
        return req;
    }
};

//
// Explicitly test couchstore (not valid for ForestDB)
// Intended to ensure we can read and write couchstore files and
// parse metadata we store in them.
//
class CouchstoreTest : public ::testing::Test {
public:
    CouchstoreTest()
        : data_dir("CouchstoreTest.db"),
          vbid(0),
          config(KVStoreConfig(1024,
                               4,
                               data_dir,
                               "couchdb",
                               0,
                               false /*persistnamespace*/)
                         .setBuffered(false)) {
        cb::io::rmrf(data_dir.c_str());
        kvstore.reset(new MockCouchKVStore(config));
        StatsCallback sc;
        std::string failoverLog("");
        // simulate a setVBState - increment the rev and then persist the
        // state
        kvstore->incrementRevision(0);
        vbucket_state state(vbucket_state_active, 0, 0, 0, 0, 0, 0, 0,
                            failoverLog);
        // simulate a setVBState - increment the dbFile revision
        kvstore->incrementRevision(0);
        kvstore->snapshotVBucket(0, state,
                                 VBStatePersist::VBSTATE_PERSIST_WITHOUT_COMMIT);
    }

    ~CouchstoreTest() {
        cb::io::rmrf(data_dir.c_str());
    }

protected:
    std::string data_dir;
    std::unique_ptr<MockCouchKVStore> kvstore;
    uint16_t vbid;
    KVStoreConfig config;
};

template<class T>
class MockedGetCallback : public Callback<T> {
    public:
        MockedGetCallback() {}

        ~MockedGetCallback() {
            delete savedValue.getValue();
        }

        void callback(GetValue& value){
            status(value.getStatus());
            if (value.getStatus() == ENGINE_SUCCESS) {
                EXPECT_CALL(*this, value("value"));
                cas(value.getValue()->getCas());
                expTime(value.getValue()->getExptime());
                flags(value.getValue()->getFlags());
                datatype(protocol_binary_datatype_t(value.getValue()->getDataType()));
                this->value(std::string(value.getValue()->getData(),
                                        value.getValue()->getNBytes()));
                savedValue = value;
            }
        }

        Item* getValue() {
            return savedValue.getValue();
        }

        /*
         * Define a number of mock methods that will be invoked by the
         * callback method. Functions can then setup expectations of the
         * value of each method e.g. expect cas to be -1
         */
        MOCK_METHOD1_T(status, void(ENGINE_ERROR_CODE));
        MOCK_METHOD1_T(cas, void(uint64_t));
        MOCK_METHOD1_T(expTime, void(uint32_t));
        MOCK_METHOD1_T(flags, void(uint32_t));
        MOCK_METHOD1_T(datatype, void(protocol_binary_datatype_t));
        MOCK_METHOD1_T(value, void(std::string));
    private:
        GetValue savedValue;
};

/*
 * The overall aim of these tests is to create an Item, write it to disk
 * then read it back from disk and look at various fields which are
 * built from the couchstore rev_meta feature.
 *
 * Validation of the Item read from disk is performed by the GetCallback.
 * A number of validators can be called upon which compare the disk Item
 * against an expected Item.
 *
 * The MockCouchKVStore exposes some of the internals of the class so we
 * can inject custom metadata by using ::setAndReturnRequest instead of ::set
 *
 */
TEST_F(CouchstoreTest, noMeta) {
    StoredDocKey key = makeStoredDocKey("key");
    Item item(key, 0, 0, "value", 5);
    WriteCallback wc;
    kvstore->begin();
    auto request = kvstore->setAndReturnRequest(item, wc);

    // Now directly mess with the metadata of the value which will be written
    MockCouchRequest::MetaData meta;
    request->writeMetaData(meta, 0); // no meta!

    kvstore->commit(nullptr /*no collections manifest*/);

    GetCallback gc(ENGINE_TMPFAIL);
    kvstore->get(key, 0, gc);
}

TEST_F(CouchstoreTest, shortMeta) {
    StoredDocKey key = makeStoredDocKey("key");
    Item item(key, 0, 0, "value", 5);
    WriteCallback wc;
    kvstore->begin();
    auto request = kvstore->setAndReturnRequest(item, wc);

    // Now directly mess with the metadata of the value which will be written
    MockCouchRequest::MetaData meta;
    request->writeMetaData(meta, 4); // not enough meta!
    kvstore->commit(nullptr /*no collections manifest*/);

    GetCallback gc(ENGINE_TMPFAIL);
    kvstore->get(key, 0, gc);
}

TEST_F(CouchstoreTest, testV0MetaThings) {
    StoredDocKey key = makeStoredDocKey("key");
    // Baseline test, just writes meta things and reads them
    // via standard interfaces
    // Ensure CAS, exptime and flags are set to something.
    Item item(key,
              0x01020304/*flags*/, 0xaa00bb11/*expiry*/,
              "value", 5,
              nullptr, 0,
              0xf00fcafe11225566ull);

    WriteCallback wc;
    kvstore->begin();
    kvstore->set(item, wc);
    kvstore->commit(nullptr /*no collections manifest*/);

    MockedGetCallback<GetValue> gc;
    EXPECT_CALL(gc, status(ENGINE_SUCCESS));
    EXPECT_CALL(gc, cas(0xf00fcafe11225566ull));
    EXPECT_CALL(gc, expTime(0xaa00bb11));
    EXPECT_CALL(gc, flags(0x01020304));
    EXPECT_CALL(gc, datatype(PROTOCOL_BINARY_RAW_BYTES));
    kvstore->get(key, 0, gc);
}

TEST_F(CouchstoreTest, testV1MetaThings) {
    // Baseline test, just writes meta things and reads them
    // via standard interfaces
    // Ensure CAS, exptime and flags are set to something.
    uint8_t datatype = PROTOCOL_BINARY_DATATYPE_JSON; //lies, but non-zero
    StoredDocKey key = makeStoredDocKey("key");
    Item item(key,
              0x01020304/*flags*/, 0xaa00bb11,/*expiry*/
              "value", 5,
              &datatype, 1, /*ext_meta is v1 extension*/
              0xf00fcafe11225566ull);
    EXPECT_NE(0, datatype); // make sure we writing non-zero
    WriteCallback wc;
    kvstore->begin();
    kvstore->set(item, wc);
    kvstore->commit(nullptr /*no collections manifest*/);

    MockedGetCallback<GetValue> gc;
    EXPECT_CALL(gc, status(ENGINE_SUCCESS));
    EXPECT_CALL(gc, cas(0xf00fcafe11225566ull));
    EXPECT_CALL(gc, expTime(0xaa00bb11));
    EXPECT_CALL(gc, flags(0x01020304));
    EXPECT_CALL(gc, datatype(PROTOCOL_BINARY_DATATYPE_JSON));

    kvstore->get(key, 0, gc);
}

TEST_F(CouchstoreTest, fuzzV0) {
    StoredDocKey key = makeStoredDocKey("key");
    Item item(key, 0, 0, "value", 5);
    WriteCallback wc;
    kvstore->begin();
    auto request = kvstore->setAndReturnRequest(item, wc);

    // Now directly mess with the metadata of the value which will be written
    MockCouchRequest::MetaData meta;
    meta.cas = 0xf00fcafe11225566ull;
    meta.expiry = 0xaa00bb11;
    meta.flags = 0x01020304;
    request->writeMetaData(meta, MockCouchRequest::MetaData::sizeofV0);
    kvstore->commit(nullptr /*no collections manifest*/);

    // CAS is byteswapped when read back
    MockedGetCallback<GetValue> gc;
    EXPECT_CALL(gc, status(ENGINE_SUCCESS));
    EXPECT_CALL(gc, cas(htonll(0xf00fcafe11225566ull)));
    EXPECT_CALL(gc, expTime(htonl(0xaa00bb11)));
    EXPECT_CALL(gc, flags(0x01020304));
    EXPECT_CALL(gc, datatype(PROTOCOL_BINARY_RAW_BYTES));
    kvstore->get(key, 0, gc);
}

TEST_F(CouchstoreTest, fuzzV1) {
    StoredDocKey key = makeStoredDocKey("key");
    Item item(key, 0, 0, "value", 5);
    WriteCallback wc;
    kvstore->begin();
    auto request = kvstore->setAndReturnRequest(item, wc);

    // Now directly mess with the metadata of the value which will be written
    MockCouchRequest::MetaData meta;
    meta.cas = 0xf00fcafe11225566ull;
    meta.expiry = 0xaa00bb11;
    meta.flags = 0x01020304;
    meta.ext1 = 2;
    meta.ext2 = 33;
    request->writeMetaData(meta, MockCouchRequest::MetaData::sizeofV1);
    kvstore->commit(nullptr /*no collections manifest*/);
    MockedGetCallback<GetValue> gc;
    uint8_t expectedDataType = 33;
    EXPECT_CALL(gc, status(ENGINE_SUCCESS));
    EXPECT_CALL(gc, cas(htonll(0xf00fcafe11225566ull)));
    EXPECT_CALL(gc, expTime(htonl(0xaa00bb11)));
    EXPECT_CALL(gc, flags(0x01020304));
    EXPECT_CALL(gc, datatype(protocol_binary_datatype_t(expectedDataType)));
    kvstore->get(key, 0, gc);
}

TEST_F(CouchstoreTest, testV0WriteReadWriteRead) {
    // Ensure CAS, exptime and flags are set to something.
    uint8_t datatype = PROTOCOL_BINARY_DATATYPE_JSON; //lies, but non-zero
    StoredDocKey key = makeStoredDocKey("key");
    Item item(key,
              0x01020304/*flags*/, 0xaa00bb11,/*expiry*/
              "value", 5,
              &datatype, 1, /*ext_meta is v1 extension*/
              0xf00fcafe11225566ull);

    EXPECT_NE(0, datatype); // make sure we writing non-zero values

    // Write an item with forced (valid) V0 meta
    MockCouchRequest::MetaData meta;
    meta.cas = 0xf00fcafe11225566ull;
    meta.expiry = 0xaa00bb11;
    meta.flags = 0x01020304;

    WriteCallback wc;
    kvstore->begin();
    auto request = kvstore->setAndReturnRequest(item, wc);

    // Force the meta to be V0
    request->writeMetaData(meta, MockCouchRequest::MetaData::sizeofV0);

    // Commit it
    kvstore->commit(nullptr /*no collections manifest*/);

    // Read back, are V1 fields sane?
    MockedGetCallback<GetValue> gc;
    EXPECT_CALL(gc, status(ENGINE_SUCCESS));
    EXPECT_CALL(gc, cas(htonll(0xf00fcafe11225566ull)));
    EXPECT_CALL(gc, expTime(htonl(0xaa00bb11)));
    EXPECT_CALL(gc, flags(0x01020304));
    EXPECT_CALL(gc, datatype(protocol_binary_datatype_t(meta.ext2)));
    kvstore->get(key, 0, gc);

    // Write back the item we read (this will write out V1 meta)
    kvstore->begin();
    kvstore->set(*gc.getValue(), wc);
    kvstore->commit(nullptr /*no collections manifest*/);

    // Read back, is conf_res_mode sane?
    MockedGetCallback<GetValue> gc2;
    EXPECT_CALL(gc2, status(ENGINE_SUCCESS));
    EXPECT_CALL(gc2, cas(htonll(0xf00fcafe11225566ull)));
    EXPECT_CALL(gc2, expTime(htonl(0xaa00bb11)));
    EXPECT_CALL(gc2, flags(0x01020304));
    EXPECT_CALL(gc2, datatype(protocol_binary_datatype_t(meta.ext2)));
    kvstore->get(key, 0, gc2);
}

TEST_F(CouchstoreTest, testV2WriteRead) {
    // Ensure CAS, exptime and flags are set to something.
    uint8_t datatype = PROTOCOL_BINARY_DATATYPE_JSON; //lies, but non-zero
    StoredDocKey key = makeStoredDocKey("key");
    Item item(key,
              0x01020304/*flags*/, 0xaa00bb11,/*expiry*/
              "value", 5,
              &datatype, 1, /*ext_meta is v1 extension*/
              0xf00fcafe11225566ull);

    EXPECT_NE(0, datatype); // make sure we writing non-zero values

    // Write an item with forced (valid) V2 meta
    // In 4.6 we removed the extra conflict resolution byte, so be sure we
    // operate correctly if a document has V2 meta.
    MockCouchRequest::MetaData meta;
    meta.cas = 0xf00fcafe11225566ull;
    meta.expiry = 0xaa00bb11;
    meta.flags = 0x01020304;
    meta.ext1 = FLEX_META_CODE;
    meta.ext2 = datatype;
    meta.legacyDeleted = 0x01;

    WriteCallback wc;
    kvstore->begin();
    auto request = kvstore->setAndReturnRequest(item, wc);

    // Force the meta to be V2 (19 bytes)
    request->writeMetaData(meta, MockCouchRequest::MetaData::sizeofV2);

    // Commit it
    kvstore->commit(nullptr /*no collections manifest*/);

    // Read back successful, the extra byte will of been dropped.
    MockedGetCallback<GetValue> gc;
    EXPECT_CALL(gc, status(ENGINE_SUCCESS));
    EXPECT_CALL(gc, cas(htonll(0xf00fcafe11225566ull)));
    EXPECT_CALL(gc, expTime(htonl(0xaa00bb11)));
    EXPECT_CALL(gc, flags(0x01020304));
    EXPECT_CALL(gc, datatype(protocol_binary_datatype_t(meta.ext2)));
    kvstore->get(key, 0, gc);
}

class CouchKVStoreMetaData : public ::testing::Test {
};

TEST_F(CouchKVStoreMetaData, basic) {
    // Lock down the size assumptions.
    EXPECT_EQ(16, MetaData::getMetaDataSize(MetaData::Version::V0));
    EXPECT_EQ(16 + 2, MetaData::getMetaDataSize(MetaData::Version::V1));
    EXPECT_EQ(16 + 2 + 1, MetaData::getMetaDataSize(MetaData::Version::V2));
}

TEST_F(CouchKVStoreMetaData, overlay) {
    std::vector<char> data(16);
    sized_buf meta;
    meta.buf = data.data();
    meta.size = data.size();
    auto metadata = MetaDataFactory::createMetaData(meta);
    EXPECT_EQ(MetaData::Version::V0, metadata->getVersionInitialisedFrom());

    data.resize(16 + 2);
    meta.buf = data.data();
    meta.size = data.size();
    metadata = MetaDataFactory::createMetaData(meta);
    EXPECT_EQ(MetaData::Version::V1, metadata->getVersionInitialisedFrom());

    // Even with a 19 byte (v2) meta, the expectation is we become V1
    data.resize(16 + 2 + 1);
    meta.buf = data.data();
    meta.size = data.size();
    metadata = MetaDataFactory::createMetaData(meta);
    EXPECT_EQ(MetaData::Version::V1, metadata->getVersionInitialisedFrom());

    // Buffers too large and small
    data.resize(16 + 2 + 1 + 1);
    meta.buf = data.data();
    meta.size = data.size();
    EXPECT_THROW(MetaDataFactory::createMetaData(meta), std::logic_error);

    data.resize(15);
    meta.buf = data.data();
    meta.size = data.size();
    EXPECT_THROW(MetaDataFactory::createMetaData(meta), std::logic_error);
}

TEST_F(CouchKVStoreMetaData, overlayExpands1) {
    std::vector<char> data(16);
    sized_buf meta;
    sized_buf out;
    meta.buf = data.data();
    meta.size = data.size();

    // V0 in yet V1 "moved out"
    auto metadata = MetaDataFactory::createMetaData(meta);
    EXPECT_EQ(MetaData::Version::V0, metadata->getVersionInitialisedFrom());
    out.size = MetaData::getMetaDataSize(MetaData::Version::V1);
    out.buf = new char[out.size];
    metadata->copyToBuf(out);
    EXPECT_EQ(out.size, MetaData::getMetaDataSize(MetaData::Version::V1));

    // We created a copy of the metadata so we must cleanup
    delete [] out.buf;
}

TEST_F(CouchKVStoreMetaData, overlayExpands2) {
    std::vector<char> data(16 + 2);
    sized_buf meta;
    sized_buf out;
    meta.buf = data.data();
    meta.size = data.size();

    // V1 in V1 "moved out"
    auto metadata = MetaDataFactory::createMetaData(meta);
    EXPECT_EQ(MetaData::Version::V1, metadata->getVersionInitialisedFrom());
    out.size = MetaData::getMetaDataSize(MetaData::Version::V1);
    out.buf = new char[out.size];
    metadata->copyToBuf(out);
    EXPECT_EQ(out.size, MetaData::getMetaDataSize(MetaData::Version::V1));

    // We created a copy of the metadata so we must cleanup
    delete [] out.buf;
}

TEST_F(CouchKVStoreMetaData, writeToOverlay) {
    std::vector<char> data(16);
    sized_buf meta;
    sized_buf out;
    meta.buf = data.data();
    meta.size = data.size();

    // Test that we can initialise from V0 but still set
    // all fields of all versions
    auto metadata = MetaDataFactory::createMetaData(meta);
    EXPECT_EQ(MetaData::Version::V0, metadata->getVersionInitialisedFrom());

    uint64_t cas = 0xf00f00ull;
    uint32_t exp = 0xcafe1234;
    uint32_t flags = 0xc0115511;
    metadata->setCas(cas);
    metadata->setExptime(exp);
    metadata->setFlags(flags);
    metadata->setDataType(PROTOCOL_BINARY_DATATYPE_JSON);

    // Check they all read back
    EXPECT_EQ(cas, metadata->getCas());
    EXPECT_EQ(exp, metadata->getExptime());
    EXPECT_EQ(flags, metadata->getFlags());
    EXPECT_EQ(FLEX_META_CODE, metadata->getFlexCode());
    EXPECT_EQ(PROTOCOL_BINARY_DATATYPE_JSON, metadata->getDataType());

    // Now we move the metadata out, this will give back a V1 structure
    out.size = MetaData::getMetaDataSize(MetaData::Version::V1);
    out.buf = new char[out.size];
    metadata->copyToBuf(out);
    metadata = MetaDataFactory::createMetaData(out);
    EXPECT_EQ(MetaData::Version::V1, metadata->getVersionInitialisedFrom()); // Is it V1?

    // All the written fields should be the same
    // Check they all read back
    EXPECT_EQ(cas, metadata->getCas());
    EXPECT_EQ(exp, metadata->getExptime());
    EXPECT_EQ(flags, metadata->getFlags());
    EXPECT_EQ(FLEX_META_CODE, metadata->getFlexCode());
    EXPECT_EQ(PROTOCOL_BINARY_DATATYPE_JSON, metadata->getDataType());
    EXPECT_EQ(out.size, MetaData::getMetaDataSize(MetaData::Version::V1));

    // We moved the metadata so we must cleanup
    delete [] out.buf;
}

//
// Test that assignment operates as expected (we use this in edit_docinfo_hook)
//
TEST_F(CouchKVStoreMetaData, assignment) {
    std::vector<char> data(16);
    sized_buf meta;
    meta.buf = data.data();
    meta.size = data.size();
    auto metadata = MetaDataFactory::createMetaData(meta);
    uint64_t cas = 0xf00f00ull;
    uint32_t exp = 0xcafe1234;
    uint32_t flags = 0xc0115511;
    metadata->setCas(cas);
    metadata->setExptime(exp);
    metadata->setFlags(flags);
    metadata->setDataType( PROTOCOL_BINARY_DATATYPE_JSON);

    // Create a second metadata to write into
    auto copy = MetaDataFactory::createMetaData();

    // Copy overlaid into managed
    *copy = *metadata;

    // Test that the copy doesn't write to metadata
    copy->setExptime(100);
    EXPECT_EQ(exp, metadata->getExptime());

    EXPECT_EQ(cas, copy->getCas());
    EXPECT_EQ(100, copy->getExptime());
    EXPECT_EQ(flags, copy->getFlags());
    EXPECT_EQ(FLEX_META_CODE, copy->getFlexCode());
    EXPECT_EQ(PROTOCOL_BINARY_DATATYPE_JSON, copy->getDataType());

    // And a final assignment
    auto copy2 = MetaDataFactory::createMetaData();
    *copy2 = *copy;

    // test that copy2 doesn't update copy
    copy2->setCas(99);
    EXPECT_NE(99, copy->getCas());

    // Yet copy2 did
    EXPECT_EQ(99, copy2->getCas());
    EXPECT_EQ(100, copy2->getExptime());
    EXPECT_EQ(flags, copy2->getFlags());
    EXPECT_EQ(FLEX_META_CODE, copy2->getFlexCode());
    EXPECT_EQ(PROTOCOL_BINARY_DATATYPE_JSON, copy2->getDataType());
}

#ifdef EP_USE_FORESTDB
// Test cases which run on both Couchstore and ForestDB
INSTANTIATE_TEST_CASE_P(CouchstoreAndForestDB,
                        CouchAndForestTest,
                        ::testing::Values("couchdb", "forestdb"),
                        [] (const ::testing::TestParamInfo<std::string>& info) {
                            return info.param;
                        });
#else
INSTANTIATE_TEST_CASE_P(CouchstoreAndForestDB,
                        CouchAndForestTest,
                        ::testing::Values("couchdb"),
                        [] (const ::testing::TestParamInfo<std::string>& info) {
                            return info.param;
                        });
#endif
