
//
// Created by Haoyu Huang on 2/20/19.
// Copyright (c) 2019 University of Southern California. All rights reserved.
//


#include "rdma_ctrl.hpp"
#include "nova_common.h"
#include "nova_config.h"
#include "nova_rdma_rc_store.h"
#include "cc/nova_cc_server.h"
#include "leveldb/db.h"
#include "leveldb/cache.h"
#include "leveldb/filter_policy.h"
#include "leveldb/comparator.h"
#include "leveldb/env.h"

#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <thread>
#include <assert.h>
#include <csignal>
#include <gflags/gflags.h>
#include <db/filename.h>
#include <util/env_posix.h>
#include <dc/nova_dc_server.h>

using namespace std;
using namespace rdmaio;
using namespace nova;

NovaConfig *NovaConfig::config;
NovaCCConfig *NovaCCConfig::cc_config;
NovaDCConfig *NovaDCConfig::dc_config;

DEFINE_string(db_path, "/tmp/nova", "level db path");

DEFINE_string(comp, "cc/mc/dc", "Component.");
DEFINE_string(cc_servers, "localhost:11211", "A list of cc servers");
DEFINE_string(dc_servers, "localhost:11211", "A list of dc servers");
DEFINE_int64(server_id, -1, "Server id.");

DEFINE_uint64(recordcount, 0, "Number of records.");

DEFINE_uint64(mem_pool_size_gb, 0, " Cache size in GB.");
DEFINE_uint64(use_fixed_value_size, 0, "Fixed value size.");

DEFINE_uint64(rdma_port, 0, "The port used by RDMA.");
DEFINE_uint64(rdma_max_msg_size, 0, "The maximum message size used by RDMA.");
DEFINE_uint64(rdma_max_num_sends, 0,
              "The maximum number of pending RDMA sends. This includes READ/WRITE/SEND. We also post the same number of RECV events. ");
DEFINE_uint64(rdma_doorbell_batch_size, 0, "The doorbell batch size.");
DEFINE_uint64(rdma_pq_batch_size, 0,
              "The number of pending requests a worker thread waits before polling RNIC.");
DEFINE_bool(enable_rdma, false, "Enable RDMA messaging.");
DEFINE_bool(enable_load_data, false, "Enable loading data.");

DEFINE_string(cc_config_path, "/tmp/uniform-3-32-10000000-frags.txt",
              "The path that stores fragment configuration.");
DEFINE_uint64(cc_num_conn_workers, 0, "Number of connection threads.");
DEFINE_uint32(cc_num_async_workers, 0, "Number of async worker threads.");
DEFINE_uint32(cc_num_compaction_workers, 0,
              "Number of compaction worker threads.");
DEFINE_uint64(cc_block_cache_mb, 0, "leveldb block cache size in mb");
DEFINE_uint64(cc_write_buffer_size_mb, 0, "write buffer size in mb");

DEFINE_string(dc_config_path, "/tmp/uniform-3-32-10000000-frags.txt",
              "The path that stores fragment configuration.");
DEFINE_bool(dc_write_sync, false, "fsync write");
DEFINE_string(dc_persist_log_records_mode, "", "local/rdma/nic");
DEFINE_uint32(dc_log_buf_size, 0, "log buffer size");
DEFINE_uint32(dc_workers, 0, "log buffer size");

namespace {
    class YCSBKeyComparator : public leveldb::Comparator {
    public:
        //   if a < b: negative result
        //   if a > b: positive result
        //   else: zero result
        int Compare(const leveldb::Slice &a, const leveldb::Slice &b) const {
            uint64_t ai = 0;
            str_to_int(a.data(), &ai, a.size());
            uint64_t bi = 0;
            str_to_int(b.data(), &bi, b.size());

            if (ai < bi) {
                return -1;
            } else if (ai > bi) {
                return 1;
            }
            return 0;
        }

        // Ignore the following methods for now:
        const char *Name() const { return "YCSBKeyComparator"; }

        void
        FindShortestSeparator(std::string *, const leveldb::Slice &) const {}

        void FindShortSuccessor(std::string *) const {}
    };
}

void start(NovaCCServer *server) {
    server->Start();
}

leveldb::DB *CreateDatabase(int db_index, leveldb::Cache *cache,
                            const std::vector<leveldb::EnvBGThread *> &bg_threads) {
    leveldb::EnvOptions env_option;
    env_option.sstable_mode = leveldb::NovaSSTableMode::SSTABLE_DISK;
    leveldb::PosixEnv *env = new leveldb::PosixEnv;
    env->set_env_option(env_option);
    leveldb::DB *db;
    leveldb::Options options;
    options.block_cache = cache;
    if (FLAGS_cc_write_buffer_size_mb > 0) {
        options.write_buffer_size = FLAGS_cc_write_buffer_size_mb * 1024 * 1024;
    }
    options.env = env;
    options.create_if_missing = true;
    options.compression = leveldb::kSnappyCompression;
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);
    options.bg_threads = bg_threads;
    options.enable_tracing = false;
    options.comparator = new YCSBKeyComparator();
    leveldb::Logger *log = nullptr;
    std::string db_path = DBName(NovaConfig::config->db_path,
                                 NovaCCConfig::cc_config->my_cc_server_id,
                                 db_index);
    mkdir(db_path.c_str(), 0777);

    RDMA_ASSERT(env->NewLogger(
            db_path + "/LOG-" + std::to_string(db_index), &log).ok());
    options.info_log = log;
    leveldb::Status status = leveldb::DB::Open(options, db_path, &db);
    RDMA_ASSERT(status.ok()) << "Open leveldb failed " << status.ToString();

    uint64_t index = 0;
    std::string logname = leveldb::LogFileName(db_path, 1111);
    ParseDBName(logname, &index);
    RDMA_ASSERT(index == db_index);
    return db;
}

void InitializeCC() {
    RdmaCtrl *rdma_ctrl = new RdmaCtrl(NovaConfig::config->my_server_id,
                                       NovaConfig::config->rdma_port);
    int port = NovaConfig::config->servers[NovaConfig::config->my_server_id].port;
    uint64_t nrdmatotal = nrdma_buf_cc();
    uint64_t ntotal = nrdmatotal;
    ntotal += NovaConfig::config->mem_pool_size_gb * 1024 * 1024 * 1024;
    RDMA_LOG(INFO) << "Allocated buffer size in bytes: " << ntotal;

    auto *buf = (char *) malloc(ntotal);
    memset(buf, 0, ntotal);
    NovaConfig::config->nova_buf = buf;
    NovaConfig::config->nnovabuf = ntotal;
    RDMA_ASSERT(buf != NULL) << "Not enough memory";

    leveldb::Cache *cache = nullptr;
    if (FLAGS_cc_block_cache_mb > 0) {
        cache = leveldb::NewLRUCache(
                FLAGS_cc_block_cache_mb * 1024 * 1024);
    }
    int ndbs = NovaConfig::ParseNumberOfDatabases(
            NovaCCConfig::cc_config->fragments,
            &NovaCCConfig::cc_config->db_fragment,
            NovaCCConfig::cc_config->my_cc_server_id);
    std::vector<leveldb::DB *> dbs;

    int bg_thread_id = 0;
    std::vector<leveldb::EnvBGThread *> bgs;
    for (int i = 0; i < FLAGS_cc_num_compaction_workers; i++) {
        bgs.push_back(new leveldb::PosixEnvBGThread);
    }
    for (int db_index = 0; db_index < ndbs; db_index++) {
        std::vector<leveldb::EnvBGThread *> bg_threads;
        bg_threads.push_back(bgs[bg_thread_id]);
        dbs.push_back(CreateDatabase(db_index, cache, bg_threads));
        bg_thread_id += 1;
        bg_thread_id %= bgs.size();
    }
    auto *mem_server = new NovaCCServer(rdma_ctrl, dbs, buf, port);
    mem_server->Start();
}

void InitializeDC() {
    RdmaCtrl *rdma_ctrl = new RdmaCtrl(NovaConfig::config->my_server_id,
                                       NovaConfig::config->rdma_port);
    int port = NovaConfig::config->servers[NovaConfig::config->my_server_id].port;
    uint64_t nrdmatotal = nrdma_buf_dc();
    uint64_t ntotal = nrdmatotal;
    ntotal += NovaConfig::config->mem_pool_size_gb * 1024 * 1024 * 1024;
    RDMA_LOG(INFO) << "Allocated buffer size in bytes: " << ntotal;

    auto *buf = (char *) malloc(ntotal);
    memset(buf, 0, ntotal);
    NovaConfig::config->nova_buf = buf;
    NovaConfig::config->nnovabuf = ntotal;
    RDMA_ASSERT(buf != NULL) << "Not enough memory";

    std::map<uint32_t, std::set<uint32_t >> dbs = NovaConfig::ReadDatabases(
            NovaCCConfig::cc_config->fragments);
    for (auto sid : dbs) {
        for (auto dbid : sid.second) {
            std::string db_path = DBName(NovaConfig::config->db_path,
                                         sid.first, dbid);
            mkdir(db_path.c_str(), 0777);
        }
    }
    nova::NovaDCServer *dc_server = new nova::NovaDCServer(rdma_ctrl, buf, dbs);
    dc_server->Start();
}

int main(int argc, char *argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    int i;
    const char **methods = event_get_supported_methods();
    printf("Starting Libevent %s.  Available methods are:\n",
           event_get_version());
    for (i = 0; methods[i] != NULL; ++i) {
        printf("    %s\n", methods[i]);
    }
    if (FLAGS_server_id == -1) {
        exit(0);
    }
    std::vector<gflags::CommandLineFlagInfo> flags;
    gflags::GetAllFlags(&flags);
    for (const auto &flag : flags) {
        printf("%s=%s\n", flag.name.c_str(),
               flag.current_value.c_str());
    }

    NovaConfig::config = new NovaConfig;
    NovaCCConfig::cc_config = new NovaCCConfig;
    NovaDCConfig::dc_config = new NovaDCConfig;

    NovaConfig::config->recordcount = FLAGS_recordcount;
    NovaConfig::config->mem_pool_size_gb = FLAGS_mem_pool_size_gb;
    NovaConfig::config->load_default_value_size = FLAGS_use_fixed_value_size;
    // RDMA
    NovaConfig::config->rdma_port = FLAGS_rdma_port;
    NovaConfig::config->max_msg_size = FLAGS_rdma_max_msg_size;
    NovaConfig::config->rdma_max_num_sends = FLAGS_rdma_max_num_sends;
    NovaConfig::config->rdma_doorbell_batch_size = FLAGS_rdma_doorbell_batch_size;
    NovaConfig::config->rdma_pq_batch_size = FLAGS_rdma_pq_batch_size;

    NovaConfig::config->db_path = FLAGS_db_path;

    NovaConfig::config->enable_rdma = FLAGS_enable_rdma;
    NovaConfig::config->enable_load_data = FLAGS_enable_load_data;

    NovaCCConfig::cc_config->cc_servers = convert_hosts(FLAGS_cc_servers);
    NovaDCConfig::dc_config->dc_servers = convert_hosts(FLAGS_dc_servers);
    NovaConfig::config->servers.insert(NovaConfig::config->servers.begin(),
                                       NovaCCConfig::cc_config->cc_servers.begin(),
                                       NovaCCConfig::cc_config->cc_servers.end());
    auto it = NovaConfig::config->servers.begin();
    std::advance(it, NovaConfig::config->servers.size());
    NovaConfig::config->servers.insert(it,
                                       NovaDCConfig::dc_config->dc_servers.begin(),
                                       NovaDCConfig::dc_config->dc_servers.end());
    if (FLAGS_comp == "cc") {
        NovaConfig::config->my_server_id = FLAGS_server_id;
    } else {
        NovaConfig::config->my_server_id =
                FLAGS_server_id + NovaCCConfig::cc_config->cc_servers.size();
    }

    NovaConfig::ReadFragments(FLAGS_cc_config_path,
                              &NovaCCConfig::cc_config->fragments);
    NovaCCConfig::cc_config->num_conn_workers = FLAGS_cc_num_conn_workers;
    NovaCCConfig::cc_config->num_async_workers = FLAGS_cc_num_async_workers;
    NovaCCConfig::cc_config->num_compaction_workers = FLAGS_cc_num_compaction_workers;


    NovaConfig::ReadFragments(FLAGS_dc_config_path,
                              &NovaDCConfig::dc_config->fragments);
    NovaConfig::config->log_buf_size = FLAGS_dc_log_buf_size;
    NovaDCConfig::dc_config->num_dc_workers = FLAGS_dc_workers;

    RDMA_ASSERT(FLAGS_cc_num_compaction_workers + FLAGS_cc_num_async_workers ==
                FLAGS_dc_workers);

    if (FLAGS_comp == "cc") {
        InitializeCC();
    } else {
        InitializeDC();
    }

//    int cores[] = {8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 26, 27, 28, 29, 30, 31};
//    for (int i = 0; i < threads.size(); i++) {
//        // Create a cpu_set_t object representing a set of CPUs. Clear it and mark
//        // only CPU i as set.
//        cpu_set_t cpuset;
//        CPU_ZERO(&cpuset);
//        CPU_SET(i, &cpuset);
//        int rc = pthread_setaffinity_np(threads[i].native_handle(),
//                                        sizeof(cpu_set_t), &cpuset);
//        RDMA_ASSERT(rc == 0) << rc;
//    }
    return 0;
}