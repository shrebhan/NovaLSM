
//
// Created by Haoyu Huang on 1/11/20.
// Copyright (c) 2020 University of Southern California. All rights reserved.
//

#ifndef LEVELDB_CC_CLIENT_H
#define LEVELDB_CC_CLIENT_H

#include <cstdint>
#include <vector>
#include <infiniband/verbs.h>
#include <semaphore.h>
#include <unordered_map>

#include "subrange.h"
#include "db_types.h"

namespace leveldb {
    struct CCBlockHandle {
        uint64_t offset;
        uint64_t size;
    };

    enum WriteResult {
        REPLICATE_LOG_RECORD_NONE = 0,
        WAIT_FOR_ALLOC = 1,
        ALLOC_SUCCESS = 2,
        WAIT_FOR_WRITE = 3,
        WRITE_SUCESS = 4,
    };

    struct WriteState {
        WriteResult result;
        int rdma_wr_id;
    };

    struct CompactionRequest {
        std::string dbname;
        uint64_t smallest_snapshot;
        std::vector<FileMetaData *> inputs[2];
        std::vector<FileMetaData *> guides;
        std::vector<SubRange> subranges;
        uint32_t source_level = 0;
        uint32_t target_level = 0;

        std::vector<FileMetaData *> outputs;

        uint32_t EncodeRequest(char *buf);

        void DecodeRequest(char *buf, uint32_t buf_size);

        void FreeMemoryLTC();

        void FreeMemoryStoC();
    };

    std::string write_result_str(WriteResult wr);

    enum CCRequestType : char {
        CC_RTABLE_READ_BLOCKS = 'a',
        CC_READ_BLOCKS = 'b',
        CC_READ_SSTABLE = 'c',
        CC_FLUSH_SSTABLE = 'd',
        CC_WRITE_REPLICATE_SSTABLE = 'e',
        CC_FLUSH_SSTABLE_BUF = 'f',
        CC_FLUSH_SSTABLE_SUCC = 'g',
        CC_ALLOCATE_SSTABLE_BUFFER = 'h',
        CC_ALLOCATE_SSTABLE_BUFFER_SUCC = 'i',
        CC_RELEASE_SSTABLE_BUFFER = 'j',
        CC_ALLOCATE_LOG_BUFFER = 'k',
        CC_ALLOCATE_LOG_BUFFER_SUCC = 'l',
        CC_DELETE_LOG_FILE = 'm',
        CC_DELETE_LOG_FILE_SUCC = 'n',
        CC_DELETE_TABLES = 'o',
        CC_READ_IN_MEMORY_LOG_FILE = 'p',
        CC_RTABLE_WRITE_SSTABLE = 'q',
        CC_RTABLE_WRITE_SSTABLE_RESPONSE = 'r',
        CC_RTABLE_PERSIST_RESPONSE = 't',
        CC_DC_READ_STATS = 'u',
        CC_DC_READ_STATS_RESPONSE = 's',
        CC_REPLICATE_LOG_RECORDS = 'v',
        CC_QUERY_LOG_FILES = 'w',
        CC_QUERY_LOG_FILES_RESPONSE = 'x',
        CC_FILENAME_RTABLEID = 'y',
        CC_FILENAME_RTABLEID_RESPONSE = 'z',
        CC_COMPACTION = 'C',
        CC_COMPACTION_RESPONSE = 'R'
    };

    struct CCRequestContext {
        CCRequestType req_type;
        uint32_t remote_server_id = 0;
        std::string dbname;
        uint64_t file_number = 0;
        char *backing_mem = nullptr;
        uint32_t size = 0;
        bool done = false;

        uint64_t wr_id = 0;
        uint32_t rtable_id = 0;
        std::vector<RTableHandle> rtable_handles;

        uint64_t dc_queue_depth = 0;
        uint64_t dc_pending_read_bytes = 0;
        uint64_t dc_pending_write_bytes = 0;

        // log records.
        char *log_record_mem = nullptr;
        uint64_t log_record_size = 0;
        std::string log_file_name;
        uint64_t thread_id = 0;
        uint32_t db_id = 0;
        uint32_t memtable_id = 0;
        WriteState *replicate_log_record_states = nullptr;
        std::unordered_map<std::string, uint64_t> *logfile_offset = nullptr;
        // compaction request.
        CompactionRequest *compaction = nullptr;
    };

    struct CCResponse {
        uint32_t rtable_id = 0;
        std::vector<RTableHandle> rtable_handles;

        uint64_t dc_queue_depth;
        uint64_t dc_pending_read_bytes;
        uint64_t dc_pending_write_bytes;
    };

    enum RDMAAsyncRequestType : char {
        RDMA_ASYNC_REQ_READ = 'a',
        RDMA_ASYNC_REQ_LOG_RECORD = 'b',
        RDMA_ASYNC_REQ_CLOSE_LOG = 'c',
        RDMA_ASYNC_REQ_WRITE_DATA_BLOCKS = 'd',
        RDMA_ASYNC_REQ_DELETE_TABLES = 'e',
        RDMA_ASYNC_READ_DC_STATS = 'f',
        RDMA_ASYNC_REQ_QUERY_LOG_FILES = 'g',
        RDMA_ASYNC_READ_LOG_FILE = 'h',
        RDMA_ASYNC_FILENAME_RTABLE_MAPPING = 'i',
        RDMA_ASYNC_COMPACTION = 'j',
    };

    struct LevelDBLogRecord {
        Slice key;
        Slice value;
        uint64_t sequence_number = 0;
    };

    struct RDMAAsyncClientRequestTask {
        RDMAAsyncRequestType type;
        sem_t *sem = nullptr;

        char *rdma_log_record_backing_mem = nullptr;
        uint64_t remote_dc_offset = 0;

        RTableHandle rtable_handle = {};
        uint64_t offset = 0;
        uint32_t size = 0;
        char *result = nullptr;
        std::string filename;

        std::string log_file_name;
        uint64_t thread_id = 0;
        uint32_t dbid = 0;
        uint32_t memtable_id = 0;
        std::vector<LevelDBLogRecord> log_records;

        int server_id = -1;
        std::vector<SSTableRTablePair> rtable_ids;

        char *write_buf = nullptr;
        std::string dbname;
        uint64_t file_number = 0;
        uint32_t write_size = 0;
        bool is_meta_blocks = false;

        CompactionRequest *compaction_request = nullptr;

        WriteState *replicate_log_record_states = nullptr;
        std::unordered_map<std::string, uint64_t> *logfile_offset = nullptr;
        std::unordered_map<std::string, uint32_t> fn_rtableid;
        CCResponse *response = nullptr;
    };

    class LEVELDB_EXPORT CCClient {
    public:
        virtual uint32_t InitiateCompaction(uint32_t remote_server_id,
                                            CompactionRequest *compaction_request) = 0;

        virtual uint32_t
        InitiateFileNameRTableMapping(uint32_t stoc_id,
                                      const std::unordered_map<std::string, uint32_t> &fn_rtableid) = 0;

        virtual uint32_t
        InitiateRTableReadDataBlock(const RTableHandle &rtable_handle,
                                    uint64_t offset,
                                    uint32_t size,
                                    char *result,
                                    uint32_t result_size,
                                    std::string filename) = 0;

        virtual uint32_t InitiateQueryLogFile(
                uint32_t storage_server_id, uint32_t server_id,
                uint32_t dbid,
                std::unordered_map<std::string, uint64_t> *logfile_offset) = 0;

        virtual uint32_t
        InitiateRTableWriteDataBlocks(uint32_t server_id, uint32_t thread_id,
                                      uint32_t *rtable_id,
                                      char *buf,
                                      const std::string &dbname,
                                      uint64_t file_number, uint32_t size,
                                      bool is_meta_blocks) = 0;

        virtual uint32_t
        InitiateDeleteTables(uint32_t server_id,
                             const std::vector<SSTableRTablePair> &rtable_ids) = 0;

        virtual uint32_t
        InitiateReadInMemoryLogFile(char *local_buf, uint32_t remote_server_id,
                                    uint64_t remote_offset, uint64_t size) = 0;

        virtual uint32_t
        InitiateReplicateLogRecords(const std::string &log_file_name,
                                    uint64_t thread_id,
                                    uint32_t db_id,
                                    uint32_t memtable_id,
                                    char *rdma_backing_mem,
                                    const std::vector<LevelDBLogRecord> &log_records,
                                    WriteState *replicate_log_record_states) = 0;


        virtual uint32_t
        InitiateCloseLogFile(const std::string &log_file_name,
                             uint32_t dbid) = 0;

        virtual uint32_t InitiateReadDCStats(uint32_t server_id) = 0;

        virtual bool OnRecv(ibv_wc_opcode type, uint64_t wr_id,
                            int remote_server_id, char *buf,
                            uint32_t imm_data, bool* generate_a_new_request) = 0;

        virtual bool
        IsDone(uint32_t req_id, CCResponse *response, uint64_t *timeout) = 0;
    };
}


#endif //LEVELDB_CC_CLIENT_H
