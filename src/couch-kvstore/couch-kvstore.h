/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2015 Couchbase, Inc
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

#ifndef SRC_COUCH_KVSTORE_COUCH_KVSTORE_H_
#define SRC_COUCH_KVSTORE_COUCH_KVSTORE_H_ 1

#include "config.h"
#include "libcouchstore/couch_db.h"
#include <relaxed_atomic.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "configuration.h"
#include "couch-kvstore/couch-fs-stats.h"
#include "couch-kvstore/couch-kvstore-metadata.h"
#include <platform/histogram.h>
#include <platform/strerror.h>
#include "logger.h"
#include "item.h"
#include "kvstore.h"
#include "atomicqueue.h"

#define COUCHSTORE_NO_OPTIONS 0

class EventuallyPersistentEngine;

/**
 * Class representing a document to be persisted in couchstore.
 */
class CouchRequest : public IORequest
{
public:
    /**
     * Constructor
     *
     * @param it Item instance to be persisted
     * @param rev vbucket database revision number
     * @param cb persistence callback
     * @param del flag indicating if it is an item deletion or not
     * @param persistDocNamespace true if we should store the key's namespace
     */
    CouchRequest(const Item& it,
                 uint64_t rev,
                 MutationRequestCallback& cb,
                 bool del,
                 bool persistDocNamespace);

    virtual ~CouchRequest() {}

    /**
     * Get the vbucket id of a document to be persisted
     *
     * @return vbucket id of a document
     */
    uint16_t getVBucketId(void) {
        return vbucketId;
    }

    /**
     * Get the revision number of the vbucket database file
     * where the document is persisted
     *
     * @return revision number of the corresponding vbucket database file
     */
    uint64_t getRevNum(void) {
        return fileRevNum;
    }

    /**
     * Get the couchstore Doc instance of a document to be persisted
     *
     * @return pointer to the couchstore Doc instance of a document
     *         or nullptr if the its a deleted item and doesn't have
     *         a value.
     */
    void *getDbDoc(void) {
        if (deleteItem && value.get() == nullptr) {
            return nullptr;
        }
        return &dbDoc;
    }

    /**
     * Get the couchstore DocInfo instance of a document to be persisted
     *
     * @return pointer to the couchstore DocInfo instance of a document
     */
    DocInfo *getDbDocInfo(void) {
        return &dbDocInfo;
    }

    /**
     * Get the length of a document body to be persisted
     *
     * @return length of a document body
     */
    size_t getNBytes() {
        return dbDocInfo.rev_meta.size + dbDocInfo.size;
    }

    /**
     * Return true if the document to be persisted is for DELETE
     *
     * @return true if the document to be persisted is for DELETE
     */
    bool isDelete() {
        return deleteItem;
    };

    /**
     * Get the key of a document to be persisted
     *
     * @return key of a document to be persisted
     */
    const StoredDocKey& getKey(void) const {
        return key;
    }

protected:
    static couchstore_content_meta_flags getContentMeta(const Item& it);

    value_t value;

    MetaData meta;
    uint64_t fileRevNum;
    Doc dbDoc;
    DocInfo dbDocInfo;
};

/**
 * KVStore with couchstore as the underlying storage system
 */
class CouchKVStore : public KVStore
{
public:
    /**
     * Constructor - creates a read/write CouchKVStore
     *
     * @param config    Configuration information
     */
    CouchKVStore(KVStoreConfig& config);

    /**
     * Alternate constructor for injecting base FileOps
     *
     * @param config    Configuration information
     * @param ops       Couchstore FileOps implementation to be used
     */
    CouchKVStore(KVStoreConfig& config, FileOpsInterface& ops);

    /**
     * Copy constructor
     *
     * @param from the source kvstore instance
     */
    CouchKVStore(const CouchKVStore &from);

    /**
     * Deconstructor
     */
    ~CouchKVStore();

    /**
     * A read only CouchKVStore can only be created by a RW store. They should
     * be created in pairs as they share some data.
     *
     * @return a unique_ptr holding a RO 'sibling' to this object.
     */
    std::unique_ptr<CouchKVStore> makeReadOnlyStore();

    void initialize();

    /**
     * Reset vbucket to a clean state.
     */
    void reset(uint16_t vbucketId) override;

    /**
     * Begin a transaction (if not already in one).
     *
     * @return true if the transaction is started successfully
     */
    bool begin(void) override {
        if (isReadOnly()) {
            throw std::logic_error("CouchKVStore::begin: Not valid on a "
                    "read-only object.");
        }
        intransaction = true;
        return intransaction;
    }

    /**
     * Commit a transaction (unless not currently in one).
     *
     * @param collectionsManifest a pointer to an Item which is a SystemEvent
     *        that contains a collections manifest to be written in the commit.
     *        Can be nullptr if the commit has no manifest to write.
     * @return true if the commit is completed successfully.
     */
    bool commit(const Item* collectionsManifest) override;

    /**
     * Rollback a transaction (unless not currently in one).
     */
    void rollback(void) override {
        if (isReadOnly()) {
            throw std::logic_error("CouchKVStore::rollback: Not valid on a "
                    "read-only object.");
        }
        if (intransaction) {
            intransaction = false;
        }
    }

    /**
     * Query the properties of the underlying storage.
     *
     * @return properties of the underlying storage system
     */
    StorageProperties getStorageProperties(void) override;

    /**
     * Insert or update a given document.
     *
     * @param itm instance representing the document to be inserted or updated
     * @param cb callback instance for SET
     */
    void set(const Item &itm, Callback<mutation_result> &cb) override;

    /**
     * Retrieve the document with a given key from the underlying storage system.
     *
     * @param key the key of a document to be retrieved
     * @param vb vbucket id of a document
     * @param cb callback instance for GET
     * @param fetchDelete True if we want to retrieve a deleted item if it not
     *        purged yet.
     */
    void get(const DocKey& key, uint16_t vb, Callback<GetValue> &cb,
             bool fetchDelete = false) override;

    void getWithHeader(void *dbHandle, const DocKey& key,
                       uint16_t vb, Callback<GetValue> &cb,
                       bool fetchDelete = false) override;

    /**
     * Retrieve the multiple documents from the underlying storage system at once.
     *
     * @param vb vbucket id of a document
     * @param itms list of items whose documents are going to be retrieved
     */
    void getMulti(uint16_t vb, vb_bgfetch_queue_t &itms) override;

    /**
     * Get the number of vbuckets in a single database file
     *
     * returns - the number of vbuckets in a database file
     */
    uint16_t getNumVbsPerFile(void) override {
        return 1;
    }

    /**
     * Delete a given document from the underlying storage system.
     *
     * @param itm instance representing the document to be deleted
     * @param cb callback instance for DELETE
     */
    void del(const Item &itm, Callback<int> &cb) override;

    /**
     * Delete a given vbucket database instance from underlying storage
     *
     * @param vbucket vbucket id
     * @param fileRev the revision of the file to delete
     */
    void delVBucket(uint16_t vbucket, uint64_t fileRev) override;

    /**
     * Retrieve the list of persisted vbucket states
     *
     * @return vbucket state vector instance where key is vbucket id and
     * value is vbucket state
     */
   std::vector<vbucket_state *>  listPersistedVbuckets(void) override;

    /**
     * Retrieve ths list of persisted engine stats
     *
     * @param stats map instance where the persisted engine stats will be added
     */
    void getPersistedStats(std::map<std::string, std::string> &stats) override;

    /**
     * Persist a snapshot of the vbucket states in the underlying storage system.
     *
     * @param vbucketId - vbucket id
     * @param vbstate   - vbucket state
     * @param options   - options used for persisting the state to disk
     * @return true if the snapshot is done successfully
     */
    bool snapshotVBucket(uint16_t vbucketId,
                         const vbucket_state &vbstate,
                         VBStatePersist options) override;

     /**
     * Compact a database file in the underlying storage system.
     *
     * @param ctx - compaction context that holds the identifier of the
                    underlying database file, options and callbacks
                    that need to invoked.
     * @return true if successful
     */
    bool compactDB(compaction_ctx *ctx) override;

    /**
     * Return the database file id from the compaction request
     * @param compact_req request structure for compaction
     *
     * return database file id
     */
    uint16_t getDBFileId(const protocol_binary_request_compact_db& req) override {
        return ntohs(req.message.header.request.vbucket);
    }

    vbucket_state *getVBucketState(uint16_t vbid) override;

    /**
     * Get the number of deleted items that are persisted to a vbucket file
     *
     * @param vbid The vbucket if of the file to get the number of deletes for
     */
    size_t getNumPersistedDeletes(uint16_t vbid) override;

    /**
     * Get the vbucket pertaining stats from a vbucket database file
     *
     * @param vbid The vbucket of the file to get the number of docs for
     */
    DBFileInfo getDbFileInfo(uint16_t vbid) override;

    /**
     * Get the file statistics for the underlying KV store
     *
     * return cumulative file size and space usage for the KV store
     */
    DBFileInfo getAggrDbFileInfo() override;

    /**
     * Get the number of non-deleted items from a vbucket database file
     *
     * @param vbid The vbucket of the file to get the number of docs for
     * @param min_seq The sequence number to start the count from
     * @param max_seq The sequence number to stop the count at
     */
    size_t getNumItems(uint16_t vbid, uint64_t min_seq, uint64_t max_seq) override;

    /**
     * This method will return the total number of items in the vbucket. Unlike
     * the getNumItems function that returns items within a specified range of
     * sequence numbers, this will return all the items in the vbucket.
     *
     * vbid - vbucket id
     */
    size_t getItemCount(uint16_t vbid) override;

    /**
     * Do a rollback to the specified seqNo on the particular vbucket
     *
     * @param vbid The vbucket of the file that's to be rolled back
     * @param rollbackSeqno The sequence number upto which the engine needs
     * to be rolled back
     * @param cb getvalue callback
     */
    RollbackResult rollback(uint16_t vbid, uint64_t rollbackSeqno,
                            std::shared_ptr<RollbackCB> cb) override;

    /**
     * Perform pending tasks after persisting dirty items
     */
    void pendingTasks() override;

    bool getStat(const char* name, size_t& value) override;

    static int recordDbDump(Db *db, DocInfo *docinfo, void *ctx);
    static int recordDbStat(Db *db, DocInfo *docinfo, void *ctx);
    static int getMultiCb(Db *db, DocInfo *docinfo, void *ctx);
    ENGINE_ERROR_CODE readVBState(Db *db, uint16_t vbId);

    couchstore_error_t fetchDoc(Db *db, DocInfo *docinfo,
                                GetValue &docValue, uint16_t vbId,
                                bool metaOnly);
    ENGINE_ERROR_CODE couchErr2EngineErr(couchstore_error_t errCode);

    uint64_t getLastPersistedSeqno(uint16_t vbid);

    /**
     * Get all_docs API, to return the list of all keys in the store
     */
    ENGINE_ERROR_CODE getAllKeys(uint16_t vbid, const DocKey start_key,
                                 uint32_t count,
                                 std::shared_ptr<Callback<const DocKey&>> cb) override;

    ScanContext* initScanContext(std::shared_ptr<Callback<GetValue> > cb,
                                 std::shared_ptr<Callback<CacheLookup> > cl,
                                 uint16_t vbid, uint64_t startSeqno,
                                 DocumentFilter options,
                                 ValueFilter valOptions) override;

    scan_error_t scan(ScanContext* sctx) override;

    void destroyScanContext(ScanContext* ctx) override;

    /**
     * This method peforms a full update (open/write/commit) of the
     * _local/manifest document for the specified vbucket.
     *
     * @param vbid The VBucket id
     * @param manifestValue An Item whose value is the source of the document
     *        data.
     * @returns true if the update was successful. Note logging on all failures
     *
     */
    bool persistCollectionsManifestItem(uint16_t vbid,
                                        const Item& manifestItem) override;

    std::string getCollectionsManifest(uint16_t vbid) override;

    /**
     * Increment the revision number of the vbucket.
     * @param vbid ID of the vbucket to change.
     */
    void incrementRevision(uint16_t vbid) override;

    /**
     * Prepare for delete of the vbucket file, this just removes the in-memory
     * stats for the vbucket and returns the current file revision (which is
     * the revision that must later be unlinked).
     *
     * @param vbid ID of the vbucket being deleted
     * @return the revision ID to delete (via ::delVBucket)
     */
    uint64_t prepareToDelete(uint16_t vbid) override;

protected:
    /*
     * Returns the DbInfo for the given vbucket database.
     */
    DbInfo getDbInfo(uint16_t vbid);

    bool setVBucketState(uint16_t vbucketId,
                         const vbucket_state& vbstate,
                         VBStatePersist options);

    template <typename T>
    void addStat(const std::string &prefix, const char *nm, T &val,
                 ADD_STAT add_stat, const void *c);

    void operator=(const CouchKVStore &from);

    void close();
    bool commit2couchstore(const Item* collectionsManifest);

    uint64_t checkNewRevNum(std::string &dbname, bool newFile = false);
    void populateFileNameMap(std::vector<std::string> &filenames,
                             std::vector<uint16_t> *vbids);
    void remVBucketFromDbFileMap(uint16_t vbucketId);
    void updateDbFileMap(uint16_t vbucketId, uint64_t newFileRev);
    couchstore_error_t openDB(uint16_t vbucketId,
                              uint64_t fileRev,
                              Db** db,
                              couchstore_open_flags options,
                              FileOpsInterface* ops = nullptr);

    /**
     * save the Documents held in docs to the file associated with vbid/rev
     *
     * @param vbid the vbucket file to open/write/commit
     * @param rev the revision of the vbucket file to open/write/commit
     * @param docs vector of Doc* to be written (can be empty)
     * @param docsinfo vector of DocInfo* to be written (non const due to
     *        couchstore API). Entry n corresponds to entry n of docs.
     * @param kvctx a stats context object to update
     * @param collectionsManifest a pointer to an item which contains the
     *        manifest update data (can be nullptr)
     *
     * @returns COUCHSTORE_SUCCESS or a failure code (failure paths log)
     */
    couchstore_error_t saveDocs(uint16_t vbid,
                                uint64_t rev,
                                const std::vector<Doc*>& docs,
                                std::vector<DocInfo*>& docinfos,
                                kvstats_ctx& kvctx,
                                const Item* collectionsManifest);

    void commitCallback(std::vector<CouchRequest *> &committedReqs,
                        kvstats_ctx &kvctx,
                        couchstore_error_t errCode);
    couchstore_error_t saveVBState(Db *db, const vbucket_state &vbState);

    /**
     * Save the collections manifest to the _local/collections_manifest document
     *
     * The document written is a JSON representation of the VBucket's
     * collection manifest and is generated by
     * an Item crafted by the Collections::VB::Manifest class.
     *
     * @param db Handle to the open data store.
     * @param item A SystemEvent which can generate the JSON data to write.
     */
    couchstore_error_t saveCollectionsManifest(Db& db,
                                               const Item& collectionsManifest);

    /**
     * Read the collections manifest from the _local/collections_manifest
     * document.
     *
     * @return std:string with the manifest JSON data or an empty string if no
     *         manifest is present.
     */
    std::string readCollectionsManifest(Db& db);

    void setDocsCommitted(uint16_t docs);
    void closeDatabaseHandle(Db *db);

    /**
     * Unlink selected couch file, which will be removed by the OS,
     * once all its references close.
     */
    void unlinkCouchFile(uint16_t vbucket, uint64_t fRev);

    /**
     * Remove compact file
     *
     * @param dbname
     * @param vbucket id
     * @param current db rev number
     */
    void removeCompactFile(const std::string &dbname, uint16_t vbid,
                           uint64_t currentRev);

    void removeCompactFile(const std::string &filename);

    const std::string dbname;

    /**
     * Per-vbucket file revision atomic to ensure writer threads see increments.
     * This is a reference of the real vector which there should be one per
     * RW/RO pair.
     */
    std::vector<std::atomic<uint64_t>>& dbFileRevMap;

    /**
     * The RW couch-kvstore owns the fileRevMap and passes a reference to it's
     * RO sibling.
     */
    std::vector<std::atomic<uint64_t>> fileRevMap;

    uint16_t numDbFiles;
    std::vector<CouchRequest *> pendingReqsQ;
    bool intransaction;

    /**
     * FileOpsInterface implementation for couchstore which tracks
     * all bytes read/written by couchstore *except* compaction.
     *
     * Backed by this->st.fsStats
     */
    std::unique_ptr<FileOpsInterface> statCollectingFileOps;

    /**
     * FileOpsInterface implementation for couchstore which tracks
     * all bytes read/written by couchstore just for compaction
     *
     * Backed by this->st.fsStatsCompaction
     */
    std::unique_ptr<FileOpsInterface> statCollectingFileOpsCompaction;

    /* deleted docs in each file, indexed by vBucket. RelaxedAtomic
       to allow stats access witout lock */
    std::vector<Couchbase::RelaxedAtomic<size_t>> cachedDeleteCount;
    std::vector<Couchbase::RelaxedAtomic<uint64_t>> cachedFileSize;
    std::vector<Couchbase::RelaxedAtomic<uint64_t>> cachedSpaceUsed;
    /* pending file deletions */
    AtomicQueue<std::string> pendingFileDeletions;

    std::atomic<size_t> scanCounter; //atomic counter for generating scan id
    std::map<size_t, Db*> scans; //map holding active scans
    std::mutex scanLock; //lock guarding the scan map

    Logger& logger;

    /**
     * Base fileops implementation to be wrapped by stat collecting fileops
     */
    FileOpsInterface& base_ops;

private:
    /**
     * Construct the store, this constructor does the object initialisation and
     * is used by the public read/write constructors and the private read-only
     * constructor.
     *
     * @param config configuration data for the store
     * @param ops the file ops to use
     * @param readOnly true if the store can only do read functionality
     * @param dbFileRevMap a reference to the map (which should be data owned by
     *        the RW store).
     * @param fileRevMapSize how big to make the fileRevMap. E.g. when the
     *        read-only constructor is called, it doesn't need to resize the map
     *        as it will use a reference to the RW store's map, so 0 would be
     *        passed.
     */
    CouchKVStore(KVStoreConfig& config,
                 FileOpsInterface& ops,
                 bool readOnly,
                 std::vector<std::atomic<uint64_t>>& dbFileRevMap,
                 size_t fileRevMapSize);

    /**
     * Construct a read-only store - private as should be called via
     * CouchKVStore::makeReadOnlyStore
     *
     * @param config configuration data for the store
     * @param dbFileRevMap a reference to the map (which should be data owned by
     *        the RW store).
     */
    CouchKVStore(KVStoreConfig& config,
                 std::vector<std::atomic<uint64_t>>& dbFileRevMap);

    class DbHolder {
    public:
        DbHolder(CouchKVStore* kvs) : kvstore(kvs), db(nullptr) {}

        DbHolder(const DbHolder&) = delete;
        DbHolder(const DbHolder&&) = delete;
        DbHolder operator=(const DbHolder&) = delete;


        Db** getDbAddress() {
            return &db;
        }

        Db* getDb() {
            return db;
        }

        Db* releaseDb() {
            auto* result = db;
            db = nullptr;
            return result;
        }

        ~DbHolder() {
            if (db) {
                kvstore->closeDatabaseHandle(db);
            }
        }
        CouchKVStore* kvstore;
        Db* db;
    };

    /**
     * RAII holder for a couchstore LocalDoc object
     */
    class LocalDocHolder {
    public:
        LocalDocHolder() : localDoc(nullptr) {
        }

        ~LocalDocHolder() {
            if (localDoc) {
                couchstore_free_local_document(localDoc);
            }
        }

        LocalDocHolder(const DbHolder&) = delete;
        LocalDocHolder(const DbHolder&&) = delete;
        LocalDocHolder operator=(const DbHolder&) = delete;

        LocalDoc** getLocalDocAddress() {
            return &localDoc;
        }

        LocalDoc* getLocalDoc() {
            return localDoc;
        }

    private:
        LocalDoc* localDoc;
    };
};

#endif  // SRC_COUCH_KVSTORE_COUCH_KVSTORE_H_
