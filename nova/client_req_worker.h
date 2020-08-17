
//
// Created by Haoyu Huang on 3/28/19.
// Copyright (c) 2019 University of Southern California. All rights reserved.
//

#ifndef RLIB_NOVA_MEM_STORE_H
#define RLIB_NOVA_MEM_STORE_H


#include <event.h>
#include <cstring>
#include <thread>
#include <atomic>
#include <chrono>
#include <unistd.h>

#include "nic_server.h"
#include "nova_common.h"
#include "nova_config.h"
#include "mc/nova_mem_manager.h"
#include "rocksdb/db.h"
#include "nova_async_worker.h"

namespace nova {

    class NovaMemServer;

    void event_handler(int fd, short which, void *arg);

    void rdma_timer_event_handler(int fd, short event, void *arg);

    struct Stats {
        uint64_t nreqs = 0;
        uint64_t nresponses = 0;
        uint64_t nreads = 0;
        uint64_t nreadsagain = 0;
        uint64_t nwrites = 0;
        uint64_t nwritesagain = 0;
        uint64_t service_time = 0;
        uint64_t read_service_time = 0;
        uint64_t write_service_time = 0;

        uint64_t ngets = 0;
        uint64_t nget_hits = 0;
        uint64_t nget_lc = 0;
        uint64_t nget_lc_hits = 0;

        uint64_t nget_rdma = 0;
        uint64_t nget_rdma_stale = 0;
        uint64_t nget_rdma_invalid = 0;

        uint64_t ngetindex_rdma = 0;
        uint64_t ngetindex_rdma_invalid = 0;
        uint64_t ngetindex_rdma_indirect = 0;

        uint64_t nputs = 0;
        uint64_t nput_lc = 0;

        uint64_t nranges = 0;

        uint64_t nreplicate_log_records = 0;

        uint64_t nremove_log_records = 0;

        uint64_t nreqs_to_poll_rdma = 0;

        Stats diff(const Stats &other) {
            Stats diff{};
            diff.nreqs = nreqs - other.nreqs;
            diff.nresponses = nresponses - other.nresponses;
            diff.nreads = nreads - other.nreads;
            diff.nreadsagain = nreadsagain - other.nreadsagain;
            diff.nwrites = nwrites - other.nwrites;
            diff.nwritesagain = nwritesagain - other.nwritesagain;
            diff.ngets = ngets - other.ngets;
            diff.nget_hits = nget_hits - other.nget_hits;
            diff.nget_lc = nget_lc - other.nget_lc;
            diff.nget_lc_hits = nget_lc_hits - other.nget_lc_hits;
            diff.nget_rdma = nget_rdma - other.nget_rdma;
            diff.nget_rdma_stale = nget_rdma_stale - other.nget_rdma_stale;
            diff.nget_rdma_invalid =
                    nget_rdma_invalid - other.nget_rdma_invalid;
            diff.ngetindex_rdma = ngetindex_rdma - other.ngetindex_rdma;
            diff.ngetindex_rdma_invalid =
                    ngetindex_rdma_invalid - other.ngetindex_rdma_invalid;
            diff.ngetindex_rdma_indirect =
                    ngetindex_rdma_indirect - other.ngetindex_rdma_indirect;
            diff.nputs = nputs - other.nputs;
            diff.nput_lc = nput_lc - other.nput_lc;
            diff.nranges = nranges - other.nranges;
            return diff;
        }
    };

    class NovaConnWorker {
    public:
        NovaConnWorker(int thread_id, NovaMemServer *server,
                       NovaAsyncCompleteQueue *async_cq)
                :
                thread_id_(thread_id), mem_server_(server),
                async_cq_(async_cq) {
            RDMA_LOG(INFO) << "memstore[" << thread_id << "]: "
                           << "create conn thread :" << thread_id;
            int fd[2];
            pipe(fd);
            async_cq_->read_fd = fd[0];
            async_cq_->write_fd = fd[1];

            request_buf = (char *) malloc(NovaConfig::config->max_msg_size);
            buf = (char *) malloc(NovaConfig::config->max_msg_size);
            RDMA_ASSERT(request_buf != NULL);
            RDMA_ASSERT(buf != NULL);

            memset(request_buf, 0, NovaConfig::config->max_msg_size);
            memset(buf, 0, NovaConfig::config->max_msg_size);
            req_ind = 0;
        }

        void Start();

        void set_dbs(const std::vector<rocksdb::DB *> &dbs) {
            dbs_ = dbs;
        }

        void AddTask(const NovaAsyncTask& task);

        timeval start{};
        timeval read_start{};
        timeval write_start{};
        int thread_id_ = 0;
        int listen_fd_ = -1;            /* listener descriptor      */
        int epoll_fd_ = -1;      /* used for all notification*/
        std::mutex mutex_;

        NovaMemServer *mem_server_ = nullptr;

        std::vector<rocksdb::DB *> dbs_;
        struct event_base *base = nullptr;
        int current_async_worker_id_ = 0;
        std::vector<NovaAsyncWorker*> async_workers_;
        NovaAsyncCompleteQueue *async_cq_;

        int nconns = 0;

        char *request_buf;
        uint32_t req_ind;
        char *buf; // buf used for responses.

        mutex conn_mu;
        vector<int> conn_queue;
        vector<Connection *> conns;
        Stats stats;
        Stats prev_stats;
    };
}

#endif //RLIB_NOVA_MEM_STORE_H
