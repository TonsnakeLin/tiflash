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

#include <Common/CPUAffinityManager.h>
#include <Common/Stopwatch.h>
#include <Common/ThreadMetricUtil.h>
#include <Common/TiFlashMetrics.h>
#include <Common/VariantOp.h>
#include <Common/getNumberOfCPUCores.h>
#include <Common/setThreadName.h>
#include <Debug/MockStorage.h>
#include <Flash/BatchCoprocessorHandler.h>
#include <Flash/Disaggregated/DisaggregatedTask.h>
#include <Flash/Disaggregated/S3LockService.h>
#include <Flash/EstablishCall.h>
#include <Flash/FlashService.h>
#include <Flash/Management/ManualCompact.h>
#include <Flash/Mpp/MPPHandler.h>
#include <Flash/Mpp/MPPTaskManager.h>
#include <Flash/Mpp/MppVersion.h>
#include <Flash/Mpp/Utils.h>
#include <Flash/ServiceUtils.h>
#include <Interpreters/Context.h>
#include <Server/IServer.h>
#include <Storages/IManageableStorage.h>
#include <Storages/S3/S3Common.h>
#include <Storages/Transaction/TMTContext.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/support/status_code_enum.h>

#include <ext/scope_guard.h>

namespace DB
{
namespace ErrorCodes
{
extern const int NOT_IMPLEMENTED;
extern const int UNKNOWN_EXCEPTION;
} // namespace ErrorCodes

#define CATCH_FLASHSERVICE_EXCEPTION                                                                                                        \
    catch (Exception & e)                                                                                                                   \
    {                                                                                                                                       \
        LOG_ERROR(log, "DB Exception: {}", e.message());                                                                                    \
        return std::make_tuple(std::make_shared<Context>(*context), grpc::Status(tiflashErrorCodeToGrpcStatusCode(e.code()), e.message())); \
    }                                                                                                                                       \
    catch (const std::exception & e)                                                                                                        \
    {                                                                                                                                       \
        LOG_ERROR(log, "std exception: {}", e.what());                                                                                      \
        return std::make_tuple(std::make_shared<Context>(*context), grpc::Status(grpc::StatusCode::INTERNAL, e.what()));                    \
    }                                                                                                                                       \
    catch (...)                                                                                                                             \
    {                                                                                                                                       \
        LOG_ERROR(log, "other exception");                                                                                                  \
        return std::make_tuple(std::make_shared<Context>(*context), grpc::Status(grpc::StatusCode::INTERNAL, "other exception"));           \
    }

constexpr char tls_err_msg[] = "common name check is failed";

FlashService::FlashService() = default;

void FlashService::init(Context & context_)
{
    context = &context_;
    log = &Poco::Logger::get("FlashService");
    manual_compact_manager = std::make_unique<Management::ManualCompactManager>(
        context->getGlobalContext(),
        context->getGlobalContext().getSettingsRef());

    // Only when the s3 storage is enabled on write node, provide the lock service interfaces
    if (!context->isDisaggregatedComputeMode() && S3::ClientFactory::instance().isEnabled())
        s3_lock_service = std::make_unique<S3::S3LockService>(*context);

    auto settings = context->getSettingsRef();
    enable_local_tunnel = settings.enable_local_tunnel;
    enable_async_grpc_client = settings.enable_async_grpc_client;
    const size_t default_size = getNumberOfLogicalCPUCores();

    auto cop_pool_size = static_cast<size_t>(settings.cop_pool_size);
    cop_pool_size = cop_pool_size ? cop_pool_size : default_size;
    LOG_INFO(log, "Use a thread pool with {} threads to handle cop requests.", cop_pool_size);
    cop_pool = std::make_unique<ThreadPool>(cop_pool_size, [] { setThreadName("cop-pool"); });

    auto batch_cop_pool_size = static_cast<size_t>(settings.batch_cop_pool_size);
    batch_cop_pool_size = batch_cop_pool_size ? batch_cop_pool_size : default_size;
    LOG_INFO(log, "Use a thread pool with {} threads to handle batch cop requests.", batch_cop_pool_size);
    batch_cop_pool = std::make_unique<ThreadPool>(batch_cop_pool_size, [] { setThreadName("batch-cop-pool"); });
}

FlashService::~FlashService() = default;

// Use executeInThreadPool to submit job to thread pool which return grpc::Status.
grpc::Status executeInThreadPool(ThreadPool & pool, std::function<grpc::Status()> job)
{
    std::packaged_task<grpc::Status()> task(job);
    std::future<grpc::Status> future = task.get_future();
    pool.schedule([&task] { task(); });
    return future.get();
}

String getClientMetaVarWithDefault(const grpc::ServerContext * grpc_context, const String & name, const String & default_val)
{
    if (auto it = grpc_context->client_metadata().find(name); it != grpc_context->client_metadata().end())
        return String(it->second.data(), it->second.size());

    return default_val;
}

grpc::Status FlashService::Coprocessor(
    grpc::ServerContext * grpc_context,
    const coprocessor::Request * request,
    coprocessor::Response * response)
{
    CPUAffinityManager::getInstance().bindSelfGrpcThread();
    LOG_DEBUG(log, "Handling coprocessor request: {}", request->DebugString());

    auto check_result = checkGrpcContext(grpc_context);
    if (!check_result.ok())
        return check_result;

    bool is_remote_read = getClientMetaVarWithDefault(grpc_context, "is_remote_read", "") == "true";
    GET_METRIC(tiflash_coprocessor_request_count, type_cop).Increment();
    GET_METRIC(tiflash_coprocessor_handling_request_count, type_cop).Increment();
    if (is_remote_read)
    {
        GET_METRIC(tiflash_coprocessor_request_count, type_remote_read).Increment();
        GET_METRIC(tiflash_coprocessor_handling_request_count, type_remote_read).Increment();
    }
    Stopwatch watch;
    SCOPE_EXIT({
        GET_METRIC(tiflash_coprocessor_handling_request_count, type_cop).Decrement();
        GET_METRIC(tiflash_coprocessor_request_duration_seconds, type_cop).Observe(watch.elapsedSeconds());
        GET_METRIC(tiflash_coprocessor_response_bytes, type_cop).Increment(response->ByteSizeLong());
        if (is_remote_read)
            GET_METRIC(tiflash_coprocessor_handling_request_count, type_remote_read).Decrement();
    });

    context->setMockStorage(mock_storage);

    const auto & settings = context->getSettingsRef();
    auto handle_limit = settings.cop_pool_handle_limit != 0 ? settings.cop_pool_handle_limit.get() : 10 * cop_pool->size();
    auto max_queued_duration_seconds = std::min(settings.cop_pool_max_queued_seconds, 20);

    if (handle_limit > 0)
    {
        // We use this atomic variable metrics from the prometheus-cpp library to mark the number of queued queries.
        // TODO: Use grpc asynchronous server and a more fully-featured thread pool.
        if (auto current = GET_METRIC(tiflash_coprocessor_handling_request_count, type_cop).Value(); current > handle_limit)
        {
            response->mutable_region_error()->mutable_server_is_busy()->set_reason(fmt::format("tiflash cop pool queued too much, current = {}, limit = {}", current, handle_limit));
            return grpc::Status::OK;
        }
    }


    grpc::Status ret = executeInThreadPool(*cop_pool, [&] {
        if (max_queued_duration_seconds > 0)
        {
            if (auto current = watch.elapsedSeconds(); current > max_queued_duration_seconds)
            {
                response->mutable_region_error()->mutable_server_is_busy()->set_reason(fmt::format("this task queued in tiflash cop pool too long, current = {}, limit = {}", current, max_queued_duration_seconds));
                return grpc::Status::OK;
            }
        }
        auto [db_context, status] = createDBContext(grpc_context);
        if (!status.ok())
        {
            return status;
        }
        if (is_remote_read)
            GET_METRIC(tiflash_coprocessor_handling_request_count, type_remote_read_executing).Increment();
        SCOPE_EXIT({
            if (is_remote_read)
                GET_METRIC(tiflash_coprocessor_handling_request_count, type_remote_read_executing).Decrement();
        });
        CoprocessorContext cop_context(*db_context, request->context(), *grpc_context);
        CoprocessorHandler cop_handler(cop_context, request, response);
        return cop_handler.execute();
    });

    LOG_DEBUG(log, "Handle coprocessor request done: {}, {}", ret.error_code(), ret.error_message());
    return ret;
}

grpc::Status FlashService::BatchCoprocessor(grpc::ServerContext * grpc_context, const coprocessor::BatchRequest * request, grpc::ServerWriter<coprocessor::BatchResponse> * writer)
{
    CPUAffinityManager::getInstance().bindSelfGrpcThread();
    LOG_DEBUG(log, "Handling coprocessor request: {}", request->DebugString());

    auto check_result = checkGrpcContext(grpc_context);
    if (!check_result.ok())
        return check_result;

    GET_METRIC(tiflash_coprocessor_request_count, type_batch).Increment();
    GET_METRIC(tiflash_coprocessor_handling_request_count, type_batch).Increment();
    Stopwatch watch;
    SCOPE_EXIT({
        GET_METRIC(tiflash_coprocessor_handling_request_count, type_batch).Decrement();
        GET_METRIC(tiflash_coprocessor_request_duration_seconds, type_batch).Observe(watch.elapsedSeconds());
        // TODO: update the value of metric tiflash_coprocessor_response_bytes.
    });

    grpc::Status ret = executeInThreadPool(*batch_cop_pool, [&] {
        auto [db_context, status] = createDBContext(grpc_context);
        if (!status.ok())
        {
            return status;
        }
        CoprocessorContext cop_context(*db_context, request->context(), *grpc_context);
        BatchCoprocessorHandler cop_handler(cop_context, request, writer);
        return cop_handler.execute();
    });

    LOG_DEBUG(log, "Handle coprocessor request done: {}, {}", ret.error_code(), ret.error_message());
    return ret;
}

grpc::Status FlashService::DispatchMPPTask(
    grpc::ServerContext * grpc_context,
    const mpp::DispatchTaskRequest * request,
    mpp::DispatchTaskResponse * response)
{
    CPUAffinityManager::getInstance().bindSelfGrpcThread();
    LOG_DEBUG(log, "Handling mpp dispatch request: {}", request->DebugString());
    auto check_result = checkGrpcContext(grpc_context);
    if (!check_result.ok())
        return check_result;

    // DO NOT register mpp task and return grpc error
    if (auto mpp_version = request->meta().mpp_version(); !DB::CheckMppVersion(mpp_version))
    {
        auto && err_msg = fmt::format("Failed to handling mpp dispatch request, reason=`{}`", DB::GenMppVersionErrorMessage(mpp_version));
        LOG_WARNING(log, err_msg);
        return grpc::Status(grpc::StatusCode::CANCELLED, std::move(err_msg));
    }

    GET_METRIC(tiflash_coprocessor_request_count, type_dispatch_mpp_task).Increment();
    GET_METRIC(tiflash_coprocessor_handling_request_count, type_dispatch_mpp_task).Increment();
    GET_METRIC(tiflash_thread_count, type_active_threads_of_dispatch_mpp).Increment();
    GET_METRIC(tiflash_thread_count, type_total_threads_of_raw).Increment();
    if (!tryToResetMaxThreadsMetrics())
    {
        GET_METRIC(tiflash_thread_count, type_max_threads_of_dispatch_mpp).Set(std::max(GET_METRIC(tiflash_thread_count, type_max_threads_of_dispatch_mpp).Value(), GET_METRIC(tiflash_thread_count, type_active_threads_of_dispatch_mpp).Value()));
        GET_METRIC(tiflash_thread_count, type_max_threads_of_raw).Set(std::max(GET_METRIC(tiflash_thread_count, type_max_threads_of_raw).Value(), GET_METRIC(tiflash_thread_count, type_total_threads_of_raw).Value()));
    }

    Stopwatch watch;
    SCOPE_EXIT({
        GET_METRIC(tiflash_thread_count, type_total_threads_of_raw).Decrement();
        GET_METRIC(tiflash_thread_count, type_active_threads_of_dispatch_mpp).Decrement();
        GET_METRIC(tiflash_coprocessor_handling_request_count, type_dispatch_mpp_task).Decrement();
        GET_METRIC(tiflash_coprocessor_request_duration_seconds, type_dispatch_mpp_task).Observe(watch.elapsedSeconds());
        GET_METRIC(tiflash_coprocessor_response_bytes, type_dispatch_mpp_task).Increment(response->ByteSizeLong());
    });

    auto [db_context, status] = createDBContext(grpc_context);
    if (!status.ok())
    {
        return status;
    }
    db_context->setMockStorage(mock_storage);
    db_context->setMockMPPServerInfo(mpp_test_info);

    MPPHandler mpp_handler(*request);
    return mpp_handler.execute(db_context, response);
}

grpc::Status FlashService::IsAlive(grpc::ServerContext * grpc_context [[maybe_unused]],
                                   const mpp::IsAliveRequest * request [[maybe_unused]],
                                   mpp::IsAliveResponse * response [[maybe_unused]])
{
    CPUAffinityManager::getInstance().bindSelfGrpcThread();
    auto check_result = checkGrpcContext(grpc_context);
    if (!check_result.ok())
        return check_result;

    auto & tmt_context = context->getTMTContext();
    response->set_available(tmt_context.checkRunning());
    response->set_mpp_version(DB::GetMppVersion());
    return grpc::Status::OK;
}

static grpc::Status CheckMppVersionForEstablishMPPConnection(const mpp::EstablishMPPConnectionRequest * request)
{
    const auto & sender_mpp_version = request->sender_meta().mpp_version();
    const auto & receiver_mpp_version = request->receiver_meta().mpp_version();

    std::string && err_reason{};

    if (!DB::CheckMppVersion(sender_mpp_version))
    {
        err_reason += fmt::format("sender failed: {}; ", DB::GenMppVersionErrorMessage(sender_mpp_version));
    }
    if (!DB::CheckMppVersion(receiver_mpp_version))
    {
        err_reason += fmt::format("receiver failed: {}; ", DB::GenMppVersionErrorMessage(receiver_mpp_version));
    }

    if (!err_reason.empty())
    {
        auto && err_msg = fmt::format("Failed to establish MPP connection, reason=`{}`", err_reason);
        return grpc::Status(grpc::StatusCode::INTERNAL, std::move(err_msg));
    }
    return grpc::Status::OK;
}

grpc::Status AsyncFlashService::establishMPPConnectionAsync(grpc::ServerContext * grpc_context,
                                                            const mpp::EstablishMPPConnectionRequest * request,
                                                            EstablishCallData * call_data)
{
    CPUAffinityManager::getInstance().bindSelfGrpcThread();
    // Establish a pipe for data transferring. The pipes have registered by the task in advance.
    // We need to find it out and bind the grpc stream with it.
    LOG_DEBUG(log, "Handling establish mpp connection request: {}", request->DebugString());

    auto check_result = checkGrpcContext(grpc_context);
    if (!check_result.ok())
        return check_result;

    if (auto res = CheckMppVersionForEstablishMPPConnection(request); !res.ok())
    {
        LOG_WARNING(log, res.error_message());
        return res;
    }

    GET_METRIC(tiflash_coprocessor_request_count, type_mpp_establish_conn).Increment();
    GET_METRIC(tiflash_coprocessor_handling_request_count, type_mpp_establish_conn).Increment();

    call_data->startEstablishConnection();
    call_data->tryConnectTunnel();
    return grpc::Status::OK;
}

grpc::Status FlashService::EstablishMPPConnection(grpc::ServerContext * grpc_context, const mpp::EstablishMPPConnectionRequest * request, grpc::ServerWriter<mpp::MPPDataPacket> * sync_writer)
{
    CPUAffinityManager::getInstance().bindSelfGrpcThread();
    // Establish a pipe for data transferring. The pipes have registered by the task in advance.
    // We need to find it out and bind the grpc stream with it.
    LOG_DEBUG(log, "Handling establish mpp connection request: {}", request->DebugString());

    auto check_result = checkGrpcContext(grpc_context);
    if (!check_result.ok())
        return check_result;

    if (auto res = CheckMppVersionForEstablishMPPConnection(request); !res.ok())
    {
        LOG_WARNING(log, res.error_message());
        return res;
    }

    GET_METRIC(tiflash_coprocessor_request_count, type_mpp_establish_conn).Increment();
    GET_METRIC(tiflash_coprocessor_handling_request_count, type_mpp_establish_conn).Increment();
    GET_METRIC(tiflash_thread_count, type_active_threads_of_establish_mpp).Increment();
    GET_METRIC(tiflash_thread_count, type_total_threads_of_raw).Increment();
    if (!tryToResetMaxThreadsMetrics())
    {
        GET_METRIC(tiflash_thread_count, type_max_threads_of_establish_mpp).Set(std::max(GET_METRIC(tiflash_thread_count, type_max_threads_of_establish_mpp).Value(), GET_METRIC(tiflash_thread_count, type_active_threads_of_establish_mpp).Value()));
        GET_METRIC(tiflash_thread_count, type_max_threads_of_raw).Set(std::max(GET_METRIC(tiflash_thread_count, type_max_threads_of_raw).Value(), GET_METRIC(tiflash_thread_count, type_total_threads_of_raw).Value()));
    }
    Stopwatch watch;
    SCOPE_EXIT({
        GET_METRIC(tiflash_thread_count, type_total_threads_of_raw).Decrement();
        GET_METRIC(tiflash_thread_count, type_active_threads_of_establish_mpp).Decrement();
        GET_METRIC(tiflash_coprocessor_handling_request_count, type_mpp_establish_conn).Decrement();
        GET_METRIC(tiflash_coprocessor_request_duration_seconds, type_mpp_establish_conn).Observe(watch.elapsedSeconds());
        // TODO: update the value of metric tiflash_coprocessor_response_bytes.
    });

    auto & tmt_context = context->getTMTContext();
    auto task_manager = tmt_context.getMPPTaskManager();
    std::chrono::seconds timeout(10);
    auto [tunnel, err_msg] = task_manager->findTunnelWithTimeout(request, timeout);
    if (tunnel == nullptr)
    {
        if (!sync_writer->Write(getPacketWithError(err_msg)))
        {
            LOG_DEBUG(log, "Write error message failed for unknown reason.");
            return grpc::Status(grpc::StatusCode::UNKNOWN, "Write error message failed for unknown reason.");
        }
    }
    else
    {
        Stopwatch stopwatch;
        SyncPacketWriter writer(sync_writer);
        tunnel->connectSync(&writer);
        tunnel->waitForFinish();
        LOG_INFO(tunnel->getLogger(), "connection for {} cost {} ms.", tunnel->id(), stopwatch.elapsedMilliseconds());
    }
    return grpc::Status::OK;
}

grpc::Status FlashService::CancelMPPTask(
    grpc::ServerContext * grpc_context,
    const mpp::CancelTaskRequest * request,
    mpp::CancelTaskResponse * response)
{
    CPUAffinityManager::getInstance().bindSelfGrpcThread();
    // CancelMPPTask cancels the query of the task.
    LOG_DEBUG(log, "cancel mpp task request: {}", request->DebugString());

    auto check_result = checkGrpcContext(grpc_context);
    if (!check_result.ok())
        return check_result;

    if (auto mpp_version = request->meta().mpp_version(); !DB::CheckMppVersion(mpp_version))
    {
        auto && err_msg = fmt::format("Failed to cancel mpp task, reason=`{}`", DB::GenMppVersionErrorMessage(mpp_version));
        LOG_WARNING(log, err_msg);
        return grpc::Status(grpc::StatusCode::INTERNAL, std::move(err_msg));
    }

    GET_METRIC(tiflash_coprocessor_request_count, type_cancel_mpp_task).Increment();
    GET_METRIC(tiflash_coprocessor_handling_request_count, type_cancel_mpp_task).Increment();
    Stopwatch watch;
    SCOPE_EXIT({
        GET_METRIC(tiflash_coprocessor_handling_request_count, type_cancel_mpp_task).Decrement();
        GET_METRIC(tiflash_coprocessor_request_duration_seconds, type_cancel_mpp_task).Observe(watch.elapsedSeconds());
        GET_METRIC(tiflash_coprocessor_response_bytes, type_cancel_mpp_task).Increment(response->ByteSizeLong());
    });

    auto & tmt_context = context->getTMTContext();
    auto task_manager = tmt_context.getMPPTaskManager();
    task_manager->abortMPPQuery(MPPQueryId(request->meta()), "Receive cancel request from TiDB", AbortType::ONCANCELLATION);
    return grpc::Status::OK;
}

std::tuple<ContextPtr, grpc::Status> FlashService::createDBContextForTest() const
{
    try
    {
        /// Create DB context.
        auto tmp_context = std::make_shared<Context>(*context);
        tmp_context->setGlobalContext(*context);

        String query_id;
        tmp_context->setCurrentQueryId(query_id);
        ClientInfo & client_info = tmp_context->getClientInfo();
        client_info.query_kind = ClientInfo::QueryKind::INITIAL_QUERY;
        client_info.interface = ClientInfo::Interface::GRPC;

        String max_threads;
        tmp_context->setSetting("enable_async_server", is_async ? "true" : "false");
        tmp_context->setSetting("enable_local_tunnel", enable_local_tunnel ? "true" : "false");
        tmp_context->setSetting("enable_async_grpc_client", enable_async_grpc_client ? "true" : "false");
        return std::make_tuple(tmp_context, grpc::Status::OK);
    }
    CATCH_FLASHSERVICE_EXCEPTION
}

::grpc::Status FlashService::cancelMPPTaskForTest(const ::mpp::CancelTaskRequest * request, ::mpp::CancelTaskResponse * response)
{
    CPUAffinityManager::getInstance().bindSelfGrpcThread();
    // CancelMPPTask cancels the query of the task.
    LOG_DEBUG(log, "cancel mpp task request: {}", request->DebugString());
    auto [context, status] = createDBContextForTest();
    if (!status.ok())
    {
        auto err = std::make_unique<mpp::Error>();
        err->set_mpp_version(DB::GetMppVersion());
        err->set_msg("error status");
        response->set_allocated_error(err.release());
        return status;
    }
    auto & tmt_context = context->getTMTContext();
    auto task_manager = tmt_context.getMPPTaskManager();
    task_manager->abortMPPQuery(MPPQueryId(request->meta()), "Receive cancel request from GTest", AbortType::ONCANCELLATION);
    return grpc::Status::OK;
}

grpc::Status FlashService::checkGrpcContext(const grpc::ServerContext * grpc_context) const
{
    // For coprocessor/mpp test, we don't care about security config.
    if (likely(!context->isMPPTest() && !context->isCopTest()))
    {
        if (!context->getSecurityConfig()->checkGrpcContext(grpc_context))
        {
            return grpc::Status(grpc::PERMISSION_DENIED, tls_err_msg);
        }
    }
    std::string peer = grpc_context->peer();
    Int64 pos = peer.find(':');
    if (pos == -1)
    {
        return grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "Invalid peer address: " + peer);
    }
    return grpc::Status::OK;
}

std::tuple<ContextPtr, grpc::Status> FlashService::createDBContext(const grpc::ServerContext * grpc_context) const
{
    try
    {
        /// Create DB context.
        auto tmp_context = std::make_shared<Context>(*context);
        tmp_context->setGlobalContext(*context);

        /// Set a bunch of client information.
        std::string user = getClientMetaVarWithDefault(grpc_context, "user", "default");
        std::string password = getClientMetaVarWithDefault(grpc_context, "password", "");
        std::string quota_key = getClientMetaVarWithDefault(grpc_context, "quota_key", "");
        std::string peer = grpc_context->peer();
        Int64 pos = peer.find(':');
        std::string client_ip = peer.substr(pos + 1);
        Poco::Net::SocketAddress client_address(client_ip);

        // For MPP or Cop test, we don't care about security config.
        if (likely(!context->isTest()))
            tmp_context->setUser(user, password, client_address, quota_key);

        String query_id = getClientMetaVarWithDefault(grpc_context, "query_id", "");
        tmp_context->setCurrentQueryId(query_id);

        ClientInfo & client_info = tmp_context->getClientInfo();
        client_info.query_kind = ClientInfo::QueryKind::INITIAL_QUERY;
        client_info.interface = ClientInfo::Interface::GRPC;

        /// Set DAG parameters.
        std::string dag_records_per_chunk_str = getClientMetaVarWithDefault(grpc_context, "dag_records_per_chunk", "");
        if (!dag_records_per_chunk_str.empty())
        {
            tmp_context->setSetting("dag_records_per_chunk", dag_records_per_chunk_str);
        }

        String max_threads = getClientMetaVarWithDefault(grpc_context, "tidb_max_tiflash_threads", "");
        if (!max_threads.empty())
        {
            tmp_context->setSetting("max_threads", max_threads);
            LOG_INFO(log, "set context setting max_threads to {}", max_threads);
        }

        tmp_context->setSetting("enable_async_server", is_async ? "true" : "false");
        tmp_context->setSetting("enable_local_tunnel", enable_local_tunnel ? "true" : "false");
        tmp_context->setSetting("enable_async_grpc_client", enable_async_grpc_client ? "true" : "false");
        return std::make_tuple(tmp_context, grpc::Status::OK);
    }
    CATCH_FLASHSERVICE_EXCEPTION
}

grpc::Status FlashService::Compact(grpc::ServerContext * grpc_context, const kvrpcpb::CompactRequest * request, kvrpcpb::CompactResponse * response)
{
    CPUAffinityManager::getInstance().bindSelfGrpcThread();
    auto check_result = checkGrpcContext(grpc_context);
    if (!check_result.ok())
        return check_result;

    return manual_compact_manager->handleRequest(request, response);
}

grpc::Status FlashService::tryAddLock(grpc::ServerContext * grpc_context, const disaggregated::TryAddLockRequest * request, disaggregated::TryAddLockResponse * response)
{
    if (!s3_lock_service)
    {
        return grpc::Status(::grpc::StatusCode::INTERNAL,
                            fmt::format(
                                "can not handle tryAddLock, s3enabled={} compute_node={}",
                                S3::ClientFactory::instance().isEnabled(),
                                context->isDisaggregatedComputeMode()));
    }

    CPUAffinityManager::getInstance().bindSelfGrpcThread();
    auto check_result = checkGrpcContext(grpc_context);
    if (!check_result.ok())
        return check_result;

    return s3_lock_service->tryAddLock(request, response);
}

grpc::Status FlashService::tryMarkDelete(grpc::ServerContext * grpc_context, const disaggregated::TryMarkDeleteRequest * request, disaggregated::TryMarkDeleteResponse * response)
{
    if (!s3_lock_service)
    {
        return grpc::Status(::grpc::StatusCode::INTERNAL,
                            fmt::format(
                                "can not handle tryMarkDelete, s3enabled={} compute_node={}",
                                S3::ClientFactory::instance().isEnabled(),
                                context->isDisaggregatedComputeMode()));
    }

    CPUAffinityManager::getInstance().bindSelfGrpcThread();
    auto check_result = checkGrpcContext(grpc_context);
    if (!check_result.ok())
        return check_result;

    return s3_lock_service->tryMarkDelete(request, response);
}

grpc::Status FlashService::EstablishDisaggTask(grpc::ServerContext * grpc_context, const disaggregated::EstablishDisaggTaskRequest * request, disaggregated::EstablishDisaggTaskResponse * response)
{
    CPUAffinityManager::getInstance().bindSelfGrpcThread();
    LOG_DEBUG(log, "Handling EstablishDisaggTask request: {}", request->ShortDebugString());
    if (auto check_result = checkGrpcContext(grpc_context); !check_result.ok())
        return check_result;
    // TODO metrics
    auto [db_context, status] = createDBContext(grpc_context);
    if (!status.ok())
        return status;
    db_context->setMockStorage(mock_storage);
    db_context->setMockMPPServerInfo(mpp_test_info);

    RUNTIME_CHECK(context->isDisaggregatedStorageMode());

    const auto & meta = request->meta();
    DM::DisaggTaskId task_id(meta);
    auto task = std::make_shared<DisaggregatedTask>(db_context, task_id);
    SCOPE_EXIT({
        current_memory_tracker = nullptr;
    });

    grpc::Status ret_status = grpc::Status::OK;
    auto record_error = [&](grpc::StatusCode err_code, int flash_err_code, const String & err_msg) {
        auto * err = response->mutable_error();
        err->set_code(flash_err_code);
        err->set_msg(err_msg);
        ret_status = grpc::Status(err_code, err_msg);
    };

    try
    {
        task->prepare(request);
        task->execute(response);
    }
    catch (Exception & e)
    {
        LOG_ERROR(task->log, "DB Exception: {}\n{}", e.displayText(), e.getStackTrace().toString());
        record_error(grpc::StatusCode::INTERNAL, e.code(), e.message());
    }
    catch (const pingcap::Exception & e)
    {
        LOG_ERROR(log, "KV Client Exception: {}", e.message());
        record_error(grpc::StatusCode::INTERNAL, ErrorCodes::UNKNOWN_EXCEPTION, e.message());
    }
    catch (std::exception & e)
    {
        LOG_ERROR(task->log, "std exception: {}", e.what());
        record_error(grpc::StatusCode::INTERNAL, ErrorCodes::UNKNOWN_EXCEPTION, e.what());
    }
    catch (...)
    {
        LOG_ERROR(task->log, "other exception");
        record_error(grpc::StatusCode::INTERNAL, ErrorCodes::UNKNOWN_EXCEPTION, "other exception");
    }

    if (auto * dag_ctx = db_context->getDAGContext(); dag_ctx)
    {
        // There may be region errors. Add information about which region to retry.
        for (const auto & region : dag_ctx->retry_regions)
        {
            auto * retry_region = response->add_retry_regions();
            retry_region->set_id(region.region_id);
            retry_region->mutable_region_epoch()->set_conf_ver(region.region_conf_version);
            retry_region->mutable_region_epoch()->set_version(region.region_version);
        }
    }

    LOG_DEBUG(log, "Handle EstablishDisaggTask request done, resp_err={}", response->error().ShortDebugString());
    return ret_status;
}

grpc::Status FlashService::FetchDisaggPages(
    grpc::ServerContext * grpc_context,
    const disaggregated::FetchDisaggPagesRequest * request,
    grpc::ServerWriter<disaggregated::PagesPacket> * sync_writer)
{
    CPUAffinityManager::getInstance().bindSelfGrpcThread();
    LOG_DEBUG(log, "Handling FetchDisaggPages request: {}", request->ShortDebugString());
    if (auto check_result = checkGrpcContext(grpc_context); !check_result.ok())
        return check_result;

    disaggregated::PagesPacket err_response;
    auto record_error = [&](grpc::StatusCode err_code, const String & err_msg) {
        auto * err = err_response.mutable_error();
        err->set_code(ErrorCodes::UNKNOWN_EXCEPTION);
        err->set_msg(err_msg);
        sync_writer->Write(err_response);

        return grpc::Status(err_code, err_msg);
    };

    const DM::DisaggTaskId task_id(request->snapshot_id());
    LOG_DEBUG(log, "Fetching pages, task_id={}", task_id);
    try
    {
        PageIdU64s read_ids;
        read_ids.reserve(request->page_ids_size());
        for (auto page_id : request->page_ids())
            read_ids.emplace_back(page_id);

#if 0
        auto tunnel = PageTunnel::build(
            *context,
            task_id,
            request->table_id(),
            request->segment_id(),
            read_ids);

        tunnel->connect(sync_writer);
        LOG_DEBUG(log, "Handle FetchDisaggPages request done");
        return grpc::Status::OK;
#endif

        return record_error(grpc::StatusCode::UNIMPLEMENTED, "unimplemented");
    }
    catch (const TiFlashException & e)
    {
        LOG_ERROR(log, "TiFlash Exception: {}\n{}", e.displayText(), e.getStackTrace().toString());
        return record_error(grpc::StatusCode::INTERNAL, e.standardText());
    }
    catch (const Exception & e)
    {
        LOG_ERROR(log, "DB Exception: {}\n{}", e.message(), e.getStackTrace().toString());
        return record_error(tiflashErrorCodeToGrpcStatusCode(e.code()), e.message());
    }
    catch (const pingcap::Exception & e)
    {
        LOG_ERROR(log, "KV Client Exception: {}", e.message());
        return record_error(grpc::StatusCode::INTERNAL, e.message());
    }
    catch (const std::exception & e)
    {
        LOG_ERROR(log, "std exception: {}", e.what());
        return record_error(grpc::StatusCode::INTERNAL, e.what());
    }
    catch (...)
    {
        LOG_ERROR(log, "other exception");
        return record_error(grpc::StatusCode::INTERNAL, "other exception");
    }
}

void FlashService::setMockStorage(MockStorage * mock_storage_)
{
    mock_storage = mock_storage_;
}

void FlashService::setMockMPPServerInfo(MockMPPServerInfo & mpp_test_info_)
{
    mpp_test_info = mpp_test_info_;
}
} // namespace DB
