// Copyright 2022 PingCAP, Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <Common/DNSCache.h>
#include <Flash/Disaggregated/S3LockClient.h>
#include <Flash/Mpp/MPPHandler.h>
#include <Flash/Mpp/MPPTaskManager.h>
#include <Flash/Mpp/MinTSOScheduler.h>
#include <Interpreters/Context.h>
#include <Server/RaftConfigParser.h>
#include <Storages/DeltaMerge/Remote/DisaggSnapshotManager.h>
#include <Storages/S3/S3Common.h>
#include <Storages/S3/S3GCManager.h>
#include <Storages/Transaction/BackgroundService.h>
#include <Storages/Transaction/KVStore.h>
#include <Storages/Transaction/RegionExecutionResult.h>
#include <Storages/Transaction/RegionRangeKeys.h>
#include <Storages/Transaction/TMTContext.h>
#include <TiDB/Etcd/Client.h>
#include <TiDB/OwnerInfo.h>
#include <TiDB/OwnerManager.h>
#include <TiDB/Schema/SchemaSyncer.h>
#include <TiDB/Schema/TiDBSchemaSyncer.h>
#include <common/logger_useful.h>
#include <pingcap/pd/MockPDClient.h>

#include <memory>

namespace DB
{
// default batch-read-index timeout is 10_000ms.
extern const uint64_t DEFAULT_BATCH_READ_INDEX_TIMEOUT_MS = 10 * 1000;
// default wait-index timeout is 5 * 60_000ms.
extern const uint64_t DEFAULT_WAIT_INDEX_TIMEOUT_MS = 5 * 60 * 1000;

const int64_t DEFAULT_WAIT_REGION_READY_TIMEOUT_SEC = 20 * 60;

const int64_t DEFAULT_READ_INDEX_WORKER_TICK_MS = 10;

static SchemaSyncerPtr createSchemaSyncer(bool exist_pd_addr, bool for_unit_test, const KVClusterPtr & cluster, bool disaggregated_compute_mode)
{
    // Doesn't need SchemaSyncer for tiflash_compute mode.
    if (disaggregated_compute_mode)
        return nullptr;
    if (exist_pd_addr)
    {
        // product env
        // Get DBInfo/TableInfo from TiKV, and create table with names `t_${table_id}`
        return std::static_pointer_cast<SchemaSyncer>(
            std::make_shared<TiDBSchemaSyncer</*mock_getter*/ false, /*mock_mapper*/ false>>(cluster));
    }
    else if (!for_unit_test)
    {
        // mock test
        // Get DBInfo/TableInfo from MockTiDB, and create table with its display names
        return std::static_pointer_cast<SchemaSyncer>(
            std::make_shared<TiDBSchemaSyncer</*mock_getter*/ true, /*mock_mapper*/ true>>(cluster));
    }
    // unit test.
    // Get DBInfo/TableInfo from MockTiDB, but create table with names `t_${table_id}`
    return std::static_pointer_cast<SchemaSyncer>(
        std::make_shared<TiDBSchemaSyncer</*mock_getter*/ true, /*mock_mapper*/ false>>(cluster));
}

TMTContext::TMTContext(Context & context_, const TiFlashRaftConfig & raft_config, const pingcap::ClusterConfig & cluster_config)
    : context(context_)
    , kvstore(context_.isDisaggregatedComputeMode() && context_.useAutoScaler() ? nullptr : std::make_shared<KVStore>(context))
    , region_table(context)
    , background_service(nullptr)
    , gc_manager(context)
    , cluster(raft_config.pd_addrs.empty() ? std::make_shared<pingcap::kv::Cluster>()
                                           : std::make_shared<pingcap::kv::Cluster>(raft_config.pd_addrs, cluster_config))
    , ignore_databases(raft_config.ignore_databases)
    , schema_syncer(createSchemaSyncer(!raft_config.pd_addrs.empty(), raft_config.for_unit_test, cluster, context_.isDisaggregatedComputeMode()))
    , mpp_task_manager(std::make_shared<MPPTaskManager>(
          std::make_unique<MinTSOScheduler>(
              context.getSettingsRef().task_scheduler_thread_soft_limit,
              context.getSettingsRef().task_scheduler_thread_hard_limit,
              context.getSettingsRef().task_scheduler_active_set_soft_limit)))
    , engine(raft_config.engine)
    , batch_read_index_timeout_ms(DEFAULT_BATCH_READ_INDEX_TIMEOUT_MS)
    , wait_index_timeout_ms(DEFAULT_WAIT_INDEX_TIMEOUT_MS)
    , read_index_worker_tick_ms(DEFAULT_READ_INDEX_WORKER_TICK_MS)
    , wait_region_ready_timeout_sec(DEFAULT_WAIT_REGION_READY_TIMEOUT_SEC)
{
    if (!raft_config.pd_addrs.empty() && S3::ClientFactory::instance().isEnabled() && !context.isDisaggregatedComputeMode())
    {
        etcd_client = Etcd::Client::create(cluster->pd_client, cluster_config);
        s3gc_owner = OwnerManager::createS3GCOwner(context, /*id*/ raft_config.flash_server_addr, etcd_client);
        s3gc_owner->campaignOwner(); // start campaign
        s3lock_client = std::make_shared<S3::S3LockClient>(cluster.get(), s3gc_owner);

        S3::S3GCConfig gc_config;
        gc_config.temp_path = context.getTemporaryPath() + "/s3_temp"; // TODO: unify the suffix for it?
        s3gc_manager = std::make_unique<S3::S3GCManagerService>(context, cluster->pd_client, s3gc_owner, s3lock_client, gc_config);

        snapshot_manager = std::make_unique<DM::Remote::DisaggSnapshotManager>(context);
    }
}

TMTContext::~TMTContext() = default;

void TMTContext::updateSecurityConfig(const TiFlashRaftConfig & raft_config, const pingcap::ClusterConfig & cluster_config)
{
    if (!raft_config.pd_addrs.empty())
    {
        // update the client config including pd_client
        cluster->update(raft_config.pd_addrs, cluster_config);
        // update the etcd_client after pd_client get updated
        etcd_client->update(cluster_config);
    }
}

void TMTContext::restore(PathPool & path_pool, const TiFlashRaftProxyHelper * proxy_helper)
{
    // For tiflash_compute mode, kvstore should be nullptr, no need to restore region_table.
    if (context.isDisaggregatedComputeMode() && context.useAutoScaler())
        return;

    kvstore->restore(path_pool, proxy_helper);
    region_table.restore();
    store_status = StoreStatus::Ready;

    if (proxy_helper != nullptr)
    {
        // Only create when running with Raft threads
        background_service = std::make_unique<BackgroundService>(*this);
    }
}

void TMTContext::shutdown()
{
    if (s3gc_owner)
    {
        // stop the campaign loop, so the S3LockService will
        // let client retry
        s3gc_owner->cancel();
        s3gc_owner = nullptr;
    }

    if (s3gc_manager)
    {
        s3gc_manager->shutdown();
        s3gc_manager = nullptr;
    }

    if (s3lock_client)
    {
        s3lock_client = nullptr;
    }

    if (background_service)
    {
        background_service->shutdown();
        background_service = nullptr;
    }
}

KVStorePtr & TMTContext::getKVStore()
{
    return kvstore;
}

const KVStorePtr & TMTContext::getKVStore() const
{
    return kvstore;
}

ManagedStorages & TMTContext::getStorages()
{
    return storages;
}

const ManagedStorages & TMTContext::getStorages() const
{
    return storages;
}

RegionTable & TMTContext::getRegionTable()
{
    return region_table;
}

const RegionTable & TMTContext::getRegionTable() const
{
    return region_table;
}

BackgroundService & TMTContext::getBackgroundService()
{
    return *background_service;
}

const BackgroundService & TMTContext::getBackgroundService() const
{
    return *background_service;
}

GCManager & TMTContext::getGCManager()
{
    return gc_manager;
}

Context & TMTContext::getContext()
{
    return context;
}

const Context & TMTContext::getContext() const
{
    return context;
}

bool TMTContext::isInitialized() const
{
    return getStoreStatus() != StoreStatus::Idle;
}

void TMTContext::setStatusRunning()
{
    store_status = StoreStatus::Running;
}

TMTContext::StoreStatus TMTContext::getStoreStatus(std::memory_order memory_order) const
{
    return store_status.load(memory_order);
}

SchemaSyncerPtr TMTContext::getSchemaSyncer() const
{
    std::lock_guard lock(mutex);
    return schema_syncer;
}

pingcap::pd::ClientPtr TMTContext::getPDClient() const
{
    return cluster->pd_client;
}

const OwnerManagerPtr & TMTContext::getS3GCOwnerManager() const
{
    return s3gc_owner;
}

DM::Remote::DisaggSnapshotManager * TMTContext::getDisaggSnapshotManager() const
{
    return snapshot_manager.get();
}

MPPTaskManagerPtr TMTContext::getMPPTaskManager()
{
    return mpp_task_manager;
}

const std::unordered_set<std::string> & TMTContext::getIgnoreDatabases() const
{
    return ignore_databases;
}

void TMTContext::reloadConfig(const Poco::Util::AbstractConfiguration & config)
{
    if (context.isDisaggregatedComputeMode() && context.useAutoScaler())
        return;

    static constexpr const char * COMPACT_LOG_MIN_PERIOD = "flash.compact_log_min_period";
    static constexpr const char * COMPACT_LOG_MIN_ROWS = "flash.compact_log_min_rows";
    static constexpr const char * COMPACT_LOG_MIN_BYTES = "flash.compact_log_min_bytes";
    static constexpr const char * BATCH_READ_INDEX_TIMEOUT_MS = "flash.batch_read_index_timeout_ms";
    static constexpr const char * WAIT_INDEX_TIMEOUT_MS = "flash.wait_index_timeout_ms";
    static constexpr const char * WAIT_REGION_READY_TIMEOUT_SEC = "flash.wait_region_ready_timeout_sec";
    static constexpr const char * READ_INDEX_WORKER_TICK_MS = "flash.read_index_worker_tick_ms";

    // default config about compact-log: period 120s, rows 40k, bytes 32MB.
    getKVStore()->setRegionCompactLogConfig(std::max(config.getUInt64(COMPACT_LOG_MIN_PERIOD, 120), 1),
                                            std::max(config.getUInt64(COMPACT_LOG_MIN_ROWS, 40 * 1024), 1),
                                            std::max(config.getUInt64(COMPACT_LOG_MIN_BYTES, 32 * 1024 * 1024), 1));
    {
        batch_read_index_timeout_ms = config.getUInt64(BATCH_READ_INDEX_TIMEOUT_MS, DEFAULT_BATCH_READ_INDEX_TIMEOUT_MS);
        wait_index_timeout_ms = config.getUInt64(WAIT_INDEX_TIMEOUT_MS, DEFAULT_WAIT_INDEX_TIMEOUT_MS);
        wait_region_ready_timeout_sec = ({
            int64_t t = config.getInt64(WAIT_REGION_READY_TIMEOUT_SEC, /*20min*/ DEFAULT_WAIT_REGION_READY_TIMEOUT_SEC);
            t = t >= 0 ? t : std::numeric_limits<int64_t>::max(); // set -1 to wait infinitely
            t;
        });
        read_index_worker_tick_ms = config.getUInt64(READ_INDEX_WORKER_TICK_MS, DEFAULT_READ_INDEX_WORKER_TICK_MS);
    }
    {
        LOG_INFO(
            Logger::get(),
            "read-index timeout: {}ms; wait-index timeout: {}ms; wait-region-ready timeout: {}s; read-index-worker-tick: {}ms",
            batchReadIndexTimeout(),
            waitIndexTimeout(),
            waitRegionReadyTimeout(),
            readIndexWorkerTick());
    }
}

bool TMTContext::checkShuttingDown(std::memory_order memory_order) const
{
    return getStoreStatus(memory_order) >= StoreStatus::Stopping;
}
bool TMTContext::checkTerminated(std::memory_order memory_order) const
{
    return getStoreStatus(memory_order) == StoreStatus::Terminated;
}
bool TMTContext::checkRunning(std::memory_order memory_order) const
{
    return getStoreStatus(memory_order) == StoreStatus::Running;
}

void TMTContext::setStatusStopping()
{
    store_status = StoreStatus::Stopping;
    // notify all region to stop learner read.
    kvstore->traverseRegions([](const RegionID, const RegionPtr & region) { region->notifyApplied(); });
}

void TMTContext::setStatusTerminated()
{
    store_status = StoreStatus::Terminated;
}

UInt64 TMTContext::batchReadIndexTimeout() const
{
    return batch_read_index_timeout_ms.load(std::memory_order_relaxed);
}
UInt64 TMTContext::waitIndexTimeout() const
{
    return wait_index_timeout_ms.load(std::memory_order_relaxed);
}
Int64 TMTContext::waitRegionReadyTimeout() const
{
    return wait_region_ready_timeout_sec.load(std::memory_order_relaxed);
}
uint64_t TMTContext::readIndexWorkerTick() const
{
    return read_index_worker_tick_ms.load(std::memory_order_relaxed);
}

const std::string & IntoStoreStatusName(TMTContext::StoreStatus status)
{
    static const std::string StoreStatusName[] = {
        "Idle",
        "Ready",
        "Running",
        "Stopping",
        "Terminated",
    };
    static const std::string Unknown = "Unknown";
    auto idx = static_cast<uint8_t>(status);
    return idx > static_cast<uint8_t>(TMTContext::StoreStatus::_MIN) && idx < static_cast<uint8_t>(TMTContext::StoreStatus::_MAX)
        ? StoreStatusName[idx - 1]
        : Unknown;
}

} // namespace DB
