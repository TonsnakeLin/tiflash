// Copyright 2023 PingCAP, Ltd.
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

#pragma once

#include <Common/Checksum.h>
#include <Core/Defines.h>
#include <Core/Field.h>
#include <Interpreters/SettingsCommon.h>
#include <Poco/Util/AbstractConfiguration.h>


namespace DB
{
namespace Constant
{
inline static constexpr UInt64 MB = 1024UL * 1024UL;
}

/** Settings of query execution.
  */
struct Settings
{
    /** List of settings: type, name, default value.
      *
      * This looks rather unconvenient. It is done that way to avoid repeating settings in different places.
      * Note: as an alternative, we could implement settings to be completely dynamic in form of map: String -> Field,
      *  but we are not going to do it, because settings is used everywhere as static struct fields.
      */

// clang-format off
#define APPLY_FOR_SETTINGS(M)                                                                                                                                                                                                           \
    M(SettingString, regions, "", "the region need to be read.")                                                                                                                                                                        \
    M(SettingBool, resolve_locks, false, "tmt resolve locks.")                                                                                                                                                                          \
    M(SettingBool, group_by_collation_sensitive, false, "do group by with collation info.")                                                                                                                                             \
    M(SettingUInt64, read_tso, DEFAULT_MAX_READ_TSO, "tmt read tso.")                                                                                                                                                                   \
    M(SettingInt64, dag_records_per_chunk, DEFAULT_DAG_RECORDS_PER_CHUNK, "default chunk size of a DAG response.")                                                                                                                      \
    M(SettingInt64, batch_send_min_limit, DEFAULT_BATCH_SEND_MIN_LIMIT, "default minimal chunk size of exchanging data among TiFlash.")                                                                                                 \
    M(SettingInt64, batch_send_min_limit_compression, -1, "default minimal chunk size of exchanging data among TiFlash when using data compression.")                                                                                   \
    M(SettingInt64, schema_version, DEFAULT_UNSPECIFIED_SCHEMA_VERSION, "tmt schema version.")                                                                                                                                          \
    M(SettingUInt64, mpp_task_timeout, DEFAULT_MPP_TASK_TIMEOUT, "mpp task max endurable time.")                                                                                                                                        \
    M(SettingUInt64, mpp_task_running_timeout, DEFAULT_MPP_TASK_RUNNING_TIMEOUT, "mpp task max time that running without any progress.")                                                                                                \
    M(SettingUInt64, mpp_task_waiting_timeout, DEFAULT_MPP_TASK_WAITING_TIMEOUT, "mpp task max time that waiting first data block from source input stream.")                                                                           \
    M(SettingUInt64, disagg_task_snapshot_timeout, DEFAULT_DISAGG_TASK_TIMEOUT_SEC, "disagg task max endurable time, unit is second.")                                                                                                  \
    M(SettingInt64, safe_point_update_interval_seconds, 1, "The interval in seconds to update safe point from PD.")                                                                                                                     \
    M(SettingUInt64, min_compress_block_size, DEFAULT_MIN_COMPRESS_BLOCK_SIZE, "The actual size of the block to compress, if the uncompressed data less than max_compress_block_size is no less than this value "                       \
                                                                               "and no less than the volume of data for one mark.")                                                                                                     \
    M(SettingUInt64, max_compress_block_size, DEFAULT_MAX_COMPRESS_BLOCK_SIZE, "The maximum size of blocks of uncompressed data before compressing for writing to a table.")                                                            \
    M(SettingUInt64, max_block_size, DEFAULT_BLOCK_SIZE, "Maximum block size for reading")                                                                                                                                              \
    M(SettingUInt64, max_insert_block_size, DEFAULT_INSERT_BLOCK_SIZE, "The maximum block size for insertion, if we control the creation of blocks for insertion.")                                                                     \
    M(SettingUInt64, min_insert_block_size_rows, DEFAULT_INSERT_BLOCK_SIZE, "Squash blocks passed to INSERT query to specified size in rows, if blocks are not big enough.")                                                            \
    M(SettingUInt64, min_insert_block_size_bytes, (DEFAULT_INSERT_BLOCK_SIZE * 256), "Squash blocks passed to INSERT query to specified size in bytes, if blocks are not big enough.")                                                  \
    M(SettingMaxThreads, max_threads, 0, "The maximum number of threads to execute the request. By default, it is determined automatically.")                                                                                           \
    M(SettingUInt64, cop_pool_size, 0, "The number of threads to handle cop requests. By default, it is determined automatically.")                                                                                                     \
    M(SettingInt64, cop_pool_handle_limit, 0, "The maximum number of requests can be handled by cop pool, include executing and queuing tasks. More cop requests will get error \"TiFlash Server is Busy\". -1 means unlimited, 0 means determined automatically (10 times of cop-pool-size).") \
    M(SettingInt64, cop_pool_max_queued_seconds, 15, "The maximum queuing duration of coprocessor task, unit is second. When task starts to run, it checks whether queued more than this config, if so, it will directly return error \"TiFlash Server is Busy\". <=0 means unlimited, default is 15. The upper limit of this config is 20.")                                                                                                     \
    M(SettingUInt64, batch_cop_pool_size, 0, "The number of threads to handle batch cop requests. By default, it is determined automatically.")                                                                                         \
    M(SettingUInt64, max_read_buffer_size, DBMS_DEFAULT_BUFFER_SIZE, "The maximum size of the buffer to read from the filesystem.")                                                                                                     \
    M(SettingUInt64, max_distributed_connections, DEFAULT_MAX_DISTRIBUTED_CONNECTIONS, "The maximum number of connections for distributed processing of one query (should be greater than max_threads).")                               \
    M(SettingUInt64, max_query_size, DEFAULT_MAX_QUERY_SIZE, "Which part of the query can be read into RAM for parsing (the remaining data for INSERT, if any, is read later)")                                                         \
    M(SettingUInt64, interactive_delay, DEFAULT_INTERACTIVE_DELAY, "The interval in microseconds to check if the request is cancelled, and to send progress info.")                                                                     \
    M(SettingSeconds, connect_timeout, DBMS_DEFAULT_CONNECT_TIMEOUT_SEC, "Connection timeout if there are no replicas.")                                                                                                                \
    M(SettingMilliseconds, connect_timeout_with_failover_ms, DBMS_DEFAULT_CONNECT_TIMEOUT_WITH_FAILOVER_MS, "Connection timeout for selecting first healthy replica.")                                                                  \
    M(SettingSeconds, receive_timeout, DBMS_DEFAULT_RECEIVE_TIMEOUT_SEC, "")                                                                                                                                                            \
    M(SettingSeconds, send_timeout, DBMS_DEFAULT_SEND_TIMEOUT_SEC, "")                                                                                                                                                                  \
    M(SettingMilliseconds, queue_max_wait_ms, DEFAULT_QUERIES_QUEUE_WAIT_TIME_MS, "The wait time in the request queue, if the number of concurrent requests exceeds the maximum.")                                                      \
    M(SettingUInt64, poll_interval, DBMS_DEFAULT_POLL_INTERVAL, "Block at the query wait loop on the server for the specified number of seconds.")                                                                                      \
    M(SettingUInt64, connections_with_failover_max_tries, DBMS_CONNECTION_POOL_WITH_FAILOVER_DEFAULT_MAX_TRIES, "The maximum number of attempts to connect to replicas.")                                                               \
    M(SettingBool, extremes, false, "Calculate minimums and maximums of the result columns. They can be output in JSON-formats.")                                                                                                       \
    M(SettingBool, use_uncompressed_cache, true, "Whether to use the cache of uncompressed blocks.")                                                                                                                                    \
    M(SettingBool, replace_running_query, false, "Whether the running request should be canceled with the same id as the new one.")                                                                                                     \
    M(SettingUInt64, background_pool_size, DBMS_DEFAULT_BACKGROUND_POOL_SIZE, "Number of threads performing background work for tables (for example, merging in merge tree). Only has meaning at server "                               \
                                                                              "startup.")                                                                                                                                               \
                                                                                                                                                                                                                                        \
    M(SettingBool, optimize_move_to_prewhere, true, "Allows disabling WHERE to PREWHERE optimization in SELECT queries from MergeTree.")                                                                                                \
                                                                                                                                                                                                                                        \
    M(SettingLoadBalancing, load_balancing, LoadBalancing::RANDOM, "Which replicas (among healthy replicas) to preferably send a query to (on the first attempt) for distributed processing.")                                          \
                                                                                                                                                                                                                                        \
    M(SettingUInt64, group_by_two_level_threshold, 100000, "From what number of keys, a two-level aggregation starts. 0 - the threshold is not set.")                                                                                   \
    M(SettingUInt64, group_by_two_level_threshold_bytes, 100000000, "From what size of the aggregation state in bytes, a two-level aggregation begins to be used. 0 - the threshold is not set. "                                       \
                                                                    "Two-level aggregation is used when at least one of the thresholds is triggered.")                                                                                  \
    M(SettingUInt64, aggregation_memory_efficient_merge_threads, 0, "Number of threads to use for merge intermediate aggregation results in memory efficient mode. When bigger, then more memory is "                                   \
                                                                    "consumed. 0 means - same as 'max_threads'.")                                                                                                                       \
                                                                                                                                                                                                                                        \
    M(SettingUInt64, max_parallel_replicas, 1, "The maximum number of replicas of each shard used when the query is executed. For consistency (to get different parts of the "                                                          \
                                               "same partition), this option only works for the specified sampling key. The lag of the replicas is not controlled.")                                                                    \
    M(SettingUInt64, parallel_replicas_count, 0, "")                                                                                                                                                                                    \
    M(SettingUInt64, parallel_replica_offset, 0, "")                                                                                                                                                                                    \
                                                                                                                                                                                                                                        \
    M(SettingBool, skip_unavailable_shards, false, "Silently skip unavailable shards.")                                                                                                                                                 \
                                                                                                                                                                                                                                        \
    M(SettingUInt64, optimize_min_equality_disjunction_chain_length, 3, "The minimum length of the expression `expr = x1 OR ... expr = xN` for optimization ")                                                                          \
                                                                                                                                                                                                                                        \
    M(SettingUInt64, min_bytes_to_use_direct_io, 0, "The minimum number of bytes for input/output operations is bypassing the page cache. 0 - disabled.")                                                                               \
    M(SettingUInt64, mark_cache_min_lifetime, 0, "Deprecated setting. Do not affect the mark cache")                                                                                                                                    \
                                                                                                                                                                                                                                        \
                                                                                                                                                                                                                                        \
    M(SettingCompressionMethod, network_compression_method, CompressionMethod::LZ4, "Allows you to select the method of data compression when writing.")                                                                                \
                                                                                                                                                                                                                                        \
    M(SettingInt64, network_zstd_compression_level, 1, "Allows you to select the level of ZSTD compression.")                                                                                                                           \
    M(SettingUInt64, priority, 0, "Priority of the query. 1 - the highest, higher value - lower priority; 0 - do not use priorities.")                                                                                                  \
                                                                                                                                                                                                                                        \
    M(SettingBool, log_queries, 0, "Log requests and write the log to the system table.")                                                                                                                                               \
                                                                                                                                                                                                                                        \
    M(SettingUInt64, log_queries_cut_to_length, 100000, "If query length is greater than specified threshold (in bytes), then cut query when writing to query log. Also limit length of "                                               \
                                                        "printed query in ordinary text log.")                                                                                                                                          \
                                                                                                                                                                                                                                        \
    M(SettingUInt64, max_concurrent_queries_for_user, 0, "The maximum number of concurrent requests per user.")                                                                                                                         \
                                                                                                                                                                                                                                        \
                                                                                                                                                                                                                                        \
    M(SettingFloat, memory_tracker_fault_probability, 0., "For testing of `exception safety` - throw an exception every time you allocate memory with the specified probability.")                                                      \
                                                                                                                                                                                                                                        \
    M(SettingInt64 , memory_tracker_accuracy_diff_for_test, 0, "For testing of the accuracy of the memory tracker - throw an exception when real_rss is much larger than tracked amount.")                                              \
                                                                                                                                                                                                                                        \
    M(SettingBool, enable_http_compression, 0, "Compress the result if the client over HTTP said that it understands data compressed by gzip or deflate.")                                                                              \
    M(SettingInt64, http_zlib_compression_level, 3, "Compression level - used if the client on HTTP said that it understands data compressed by gzip or deflate.")                                                                      \
                                                                                                                                                                                                                                        \
    M(SettingBool, http_native_compression_disable_checksumming_on_decompress, 0, "If you uncompress the POST data from the client compressed by the native format, do not check the checksum.")                                        \
                                                                                                                                                                                                                                        \
    M(SettingString, count_distinct_implementation, "uniqExact", "What aggregate function to use for implementation of count(DISTINCT ...)")                                                                                            \
                                                                                                                                                                                                                                        \
    M(SettingBool, output_format_write_statistics, true, "Write statistics about read rows, bytes, time elapsed in suitable output formats.")                                                                                           \
                                                                                                                                                                                                                                        \
    M(SettingBool, add_http_cors_header, false, "Write add http CORS header.")                                                                                                                                                          \
                                                                                                                                                                                                                                        \
    M(SettingBool, input_format_skip_unknown_fields, false, "Skip columns with unknown names from input data (it works for JSONEachRow and TSKV formats).")                                                                             \
                                                                                                                                                                                                                                        \
    M(SettingBool, input_format_values_interpret_expressions, true, "For Values format: if field could not be parsed by streaming parser, run SQL parser and try to interpret it as SQL expression.")                                   \
                                                                                                                                                                                                                                        \
    M(SettingBool, output_format_json_quote_64bit_integers, true, "Controls quoting of 64-bit integers in JSON output format.")                                                                                                         \
                                                                                                                                                                                                                                        \
    M(SettingBool, output_format_json_quote_denormals, false, "Enables '+nan', '-nan', '+inf', '-inf' outputs in JSON output format.")                                                                                                  \
                                                                                                                                                                                                                                        \
    M(SettingUInt64, output_format_pretty_max_rows, 10000, "Rows limit for Pretty formats.")                                                                                                                                            \
                                                                                                                                                                                                                                        \
    M(SettingBool, use_client_time_zone, false, "Use client timezone for interpreting DateTime string values, instead of adopting server timezone.")                                                                                    \
                                                                                                                                                                                                                                        \
    M(SettingBool, send_progress_in_http_headers, false, "Send progress notifications using X-ClickHouse-Progress headers. Some clients do not support high amount of HTTP headers "                                                    \
                                                         "(Python requests in particular), so it is disabled by default.")                                                                                                              \
                                                                                                                                                                                                                                        \
    M(SettingUInt64, http_headers_progress_interval_ms, 100, "Do not send HTTP headers X-ClickHouse-Progress more frequently than at each specified interval.")                                                                         \
                                                                                                                                                                                                                                        \
    M(SettingBool, fsync_metadata, 1, "Do fsync after changing metadata for tables and databases (.sql files). Could be disabled in case of poor latency on server "                                                                    \
                                      "with high load of DDL queries and high load of disk subsystem.")                                                                                                                                 \
                                                                                                                                                                                                                                        \
    M(SettingUInt64, max_replica_delay_for_distributed_queries, 300, "If set, distributed queries of Replicated tables will choose servers with replication delay in seconds less than the specified "                                  \
                                                                     "value (not inclusive). Zero means do not take delay into account.")                                                                                               \
    M(SettingBool, fallback_to_stale_replicas_for_distributed_queries, 1, "Suppose max_replica_delay_for_distributed_queries is set and all replicas for the queried table are stale. If this setting is "                              \
                                                                          "enabled, the query will be performed anyway, otherwise the error will be reported.")                                                                         \
                                                                                                                                                                                                                                        \
    M(SettingSeconds, http_connection_timeout, DEFAULT_HTTP_READ_BUFFER_CONNECTION_TIMEOUT, "HTTP connection timeout.")                                                                                                                 \
    M(SettingSeconds, http_send_timeout, DEFAULT_HTTP_READ_BUFFER_TIMEOUT, "HTTP send timeout")                                                                                                                                         \
    M(SettingSeconds, http_receive_timeout, DEFAULT_HTTP_READ_BUFFER_TIMEOUT, "HTTP receive timeout")                                                                                                                                   \
    M(SettingBool, use_index_for_in_with_subqueries, true, "Try using an index if there is a subquery or a table expression on the right side of the IN operator.")                                                                     \
                                                                                                                                                                                                                                        \
    M(SettingUInt64, max_bytes_before_external_group_by, 0, "")                                                                                                                                                                         \
                                                                                                                                                                                                                                        \
    M(SettingUInt64, max_bytes_before_external_sort, 0, "")                                                                                                                                                                             \
                                                                                                                                                                                                                                        \
                                                                                                                                                                                                                                        \
    /* TODO: Check also when merging and finalizing aggregate functions. */                                                                                                                                                             \
                                                                                                                                                                                                                                        \
                                                                                                                                                                                                                                        \
    M(SettingUInt64, max_ast_depth, 1000, "Maximum depth of query syntax tree. Checked after parsing.")                                                                                                                                 \
    M(SettingUInt64, max_ast_elements, 50000, "Maximum size of query syntax tree in number of nodes. Checked after parsing.")                                                                                                           \
    M(SettingUInt64, max_expanded_ast_elements, 500000, "Maximum size of query syntax tree in number of nodes after expansion of aliases and the asterisk.")                                                                            \
                                                                                                                                                                                                                                        \
    M(SettingUInt64, readonly, 0, "0 - everything is allowed. 1 - only read requests. 2 - only read requests, as well as changing settings, except for the "                                                                            \
                                  "'readonly' setting.")                                                                                                                                                                                \
                                                                                                                                                                                                                                        \
    M(SettingString, query_id, "", "The query_id, only for testing.")                                                                                                                                                                   \
    M(SettingUInt64, mutable_deduper, 5, "The deduper used by MutableMergeTree storage. By default 5. 0: OriginStreams, 1: OriginUnity, 2: ReplacingUnity, 3: "                                                                         \
                                         "ReplacingPartitioning, 4: DedupPartitioning, 5: ReplacingPartitioningOpt.")                                                                                                                   \
    M(SettingUInt64, dt_segment_limit_rows, 1000000, "Base rows of segments in DeltaTree Engine.")                                                                                                                                      \
    M(SettingUInt64, dt_segment_limit_size, 536870912, "Base size of segments in DeltaTree Engine. 500MB by default.")                                                                                                                  \
    M(SettingUInt64, dt_segment_force_split_size, 1610612736, "The threshold of the foreground split segment. in DeltaTree Engine. 1.5GB by default.")                                                                                  \
    M(SettingUInt64, dt_segment_delta_limit_rows, 80000, "Max rows of segment delta in DeltaTree Engine")                                                                                                                               \
    M(SettingUInt64, dt_segment_delta_limit_size, 42991616, "Max size of segment delta in DeltaTree Engine. 41 MB by default.")                                                                                                         \
    M(SettingUInt64, dt_segment_force_merge_delta_deletes, 10, "Delta delete ranges before force merge into stable.")                                                                                                                   \
    M(SettingUInt64, dt_segment_force_merge_delta_rows, 134217728, "Delta rows before force merge into stable.")                                                                                                                        \
    M(SettingUInt64, dt_segment_force_merge_delta_size, 1073741824, "Delta size before force merge into stable. 1 GB by default.")                                                                                                      \
    M(SettingUInt64, dt_segment_stop_write_delta_rows, 268435456, "Delta rows before stop new writes.")                                                                                                                                 \
    M(SettingUInt64, dt_segment_stop_write_delta_size, 2147483648, "Delta size before stop new writes. 2 GB by default.")                                                                                                               \
    M(SettingUInt64, dt_segment_delta_cache_limit_rows, 4096, "Max rows of cache in segment delta in DeltaTree Engine.")                                                                                                                \
    M(SettingUInt64, dt_segment_delta_cache_limit_size, 4194304, "Max size of cache in segment delta in DeltaTree Engine. 4 MB by default.")                                                                                            \
    M(SettingUInt64, dt_segment_delta_small_pack_rows, 2048, "Deprecated. Reserved for backward compatibility. Use dt_segment_delta_small_column_file_rows instead")                                                                    \
    M(SettingUInt64, dt_segment_delta_small_pack_size, 8388608, "Deprecated. Reserved for backward compatibility. Use dt_segment_delta_small_column_file_size instead")                                                                 \
    M(SettingUInt64, dt_segment_delta_small_column_file_rows, 2048, "Determine whether a column file in delta is small or not. 8MB by default.")                                                                                        \
    M(SettingUInt64, dt_segment_delta_small_column_file_size, 8388608, "Determine whether a column file in delta is small or not. 8MB by default.")                                                                                     \
    M(SettingUInt64, dt_segment_stable_pack_rows, DEFAULT_MERGE_BLOCK_SIZE, "Expected stable pack rows in DeltaTree Engine.")                                                                                                           \
    M(SettingFloat, dt_segment_wait_duration_factor, 1, "The factor of wait duration in a write stall.")                                                                                                                                \
    M(SettingUInt64, dt_bg_gc_check_interval, 60, "Background gc thread check interval, the unit is second.")                                                                                                                           \
    M(SettingInt64, dt_bg_gc_max_segments_to_check_every_round, 100, "Max segments to check in every gc round, value less than or equal to 0 means gc no segments.")                                                                    \
    M(SettingFloat, dt_bg_gc_ratio_threhold_to_trigger_gc, 1.2, "Trigger segment's gc when the ratio of invalid version exceed this threhold. Values smaller than or equal to 1.0 means gc all "                                        \
                                                                "segments")                                                                                                                                                             \
    M(SettingFloat, dt_bg_gc_delta_delete_ratio_to_trigger_gc, 0.3, "Trigger segment's gc when the ratio of delta delete range to stable exceeds this ratio.")                                                                          \
    M(SettingUInt64, dt_insert_max_rows, 0, "Max rows of insert blocks when write into DeltaTree Engine. By default 0 means no limit.")                                                                                                 \
    M(SettingBool, dt_enable_rough_set_filter, true, "Whether to parse where expression as Rough Set Index filter or not.")                                                                                                             \
    M(SettingBool, dt_raw_filter_range, true, "[unused] Do range filter or not when read data in raw mode in DeltaTree Engine.")                                                                                                        \
    M(SettingBool, dt_read_delta_only, false, "Only read delta data in DeltaTree Engine.")                                                                                                                                              \
    M(SettingBool, dt_read_stable_only, false, "Only read stable data in DeltaTree Engine.")                                                                                                                                            \
    M(SettingBool, dt_enable_logical_split, false, "Enable logical split or not in DeltaTree Engine.")                                                                                                                                  \
    M(SettingBool, dt_flush_after_write, false, "Flush cache or not after write in DeltaTree Engine.")                                                                                                                                  \
    M(SettingBool, dt_enable_relevant_place, false, "Enable relevant place or not in DeltaTree Engine.")                                                                                                                                \
    M(SettingBool, dt_enable_skippable_place, true, "Enable skippable place or not in DeltaTree Engine.")                                                                                                                               \
    M(SettingBool, dt_enable_stable_column_cache, true, "Enable column cache for StorageDeltaMerge.")                                                                                                                                   \
    M(SettingUInt64, dt_open_file_max_idle_seconds, 15, "Max idle time of opening files, 0 means infinite.")                                                                                                                            \
    M(SettingUInt64, dt_page_num_max_expect_legacy_files, 100, "Max number of legacy file expected")                                                                                                                                    \
    M(SettingFloat, dt_page_num_max_gc_valid_rate, 1.0, "Max valid rate of deciding a page file can be compact when exising legacy files are more over than "                                                                           \
                                                        "`dt_stroage_num_max_expect_legacy_files`")                                                                                                                                     \
    M(SettingFloat, dt_page_gc_low_write_prob, 0.10, "Probability to run gc when write there is few writes.")                                                                                                                           \
                                                                                                                                                                                                                                        \
                                                                                                                                                                                                                                        \
    M(SettingUInt64, dt_storage_pool_log_write_slots, 4, "Max write concurrency for each StoragePool.log.")                                                                                                                             \
    M(SettingUInt64, dt_storage_pool_log_gc_min_file_num, 10, "Min number of page files to compact")                                                                                                                                    \
    M(SettingUInt64, dt_storage_pool_log_gc_min_legacy_num, 3, "Min number of legacy page files to compact")                                                                                                                            \
    M(SettingUInt64, dt_storage_pool_log_gc_min_bytes, 128 * Constant::MB, "Min bytes of page data to compact")                                                                                                                         \
    M(SettingFloat, dt_storage_pool_log_gc_max_valid_rate, 0.35, "Max valid rate of deciding a page file can be compact")                                                                                                               \
                                                                                                                                                                                                                                        \
    M(SettingUInt64, dt_storage_pool_data_write_slots, 1, "Max write concurrency for each StoragePool.data.")                                                                                                                           \
    M(SettingUInt64, dt_storage_pool_data_gc_min_file_num, 10, "Min number of page files to compact")                                                                                                                                   \
    M(SettingUInt64, dt_storage_pool_data_gc_min_legacy_num, 3, "Min number of legacy page files to compact")                                                                                                                           \
    M(SettingUInt64, dt_storage_pool_data_gc_min_bytes, 128 * Constant::MB, "Min bytes of page data to compact")                                                                                                                        \
    M(SettingFloat, dt_storage_pool_data_gc_max_valid_rate, 0.35, "Max valid rate of deciding a page file can be compact")                                                                                                              \
                                                                                                                                                                                                                                        \
    M(SettingUInt64, dt_storage_pool_meta_write_slots, 2, "Max write concurrency for each StoragePool.meta.")                                                                                                                           \
    M(SettingUInt64, dt_storage_pool_meta_gc_min_file_num, 10, "Min number of page files to compact")                                                                                                                                   \
    M(SettingUInt64, dt_storage_pool_meta_gc_min_legacy_num, 3, "Min number of legacy page files to compact")                                                                                                                           \
    M(SettingUInt64, dt_storage_pool_meta_gc_min_bytes, 128 * Constant::MB, "Min bytes of page data to compact")                                                                                                                        \
    M(SettingFloat, dt_storage_pool_meta_gc_max_valid_rate, 0.35, "Max valid rate of deciding a page file can be compact")                                                                                                              \
                                                                                                                                                                                                                                        \
    M(SettingUInt64, dt_checksum_frame_size, DBMS_DEFAULT_BUFFER_SIZE, "Frame size for delta tree stable storage")                                                                                                                      \
                                                                                                                                                                                                                                        \
    M(SettingDouble, dt_page_gc_threshold, 0.5, "Max valid rate of deciding to do a GC in PageStorage")                                                                                                                                 \
    M(SettingBool, dt_enable_read_thread, true, "Enable storage read thread or not")                                                                                                                                                    \
    M(SettingBool, dt_enable_bitmap_filter, true, "Use bitmap filter to read data or not")                                                                                                                                              \
    M(SettingDouble, dt_read_thread_count_scale, 1.0, "Number of read thread = number of logical cpu cores * dt_read_thread_count_scale.  Only has meaning at server startup.")                                                         \
    M(SettingDouble, io_thread_count_scale, 5.0, "Number of thread of IOThreadPool = number of logical cpu cores * io_thread_count_scale.  Only has meaning at server startup.")                                                        \
                                                                                                                                                                                                                                        \
    M(SettingChecksumAlgorithm, dt_checksum_algorithm, ChecksumAlgo::XXH3, "Checksum algorithm for delta tree stable storage")                                                                                                          \
    M(SettingCompressionMethod, dt_compression_method, CompressionMethod::LZ4, "The method of data compression when writing.")                                                                                                          \
    M(SettingInt64, dt_compression_level, 1, "The compression level.")                                                                                                                                                                  \
    M(SettingUInt64, max_rows_in_set, 0, "Maximum size of the set (in number of elements) resulting from the execution of the IN section.")                                                                                             \
    M(SettingUInt64, max_bytes_in_set, 0, "Maximum size of the set (in bytes in memory) resulting from the execution of the IN section.")                                                                                               \
    M(SettingOverflowMode<false>, set_overflow_mode, OverflowMode::THROW, "What to do when the limit is exceeded.")                                                                                                                     \
                                                                                                                                                                                                                                        \
    M(SettingUInt64, max_rows_to_transfer, 0, "Maximum size (in rows) of the transmitted external table obtained when the GLOBAL IN/JOIN section is executed.")                                                                         \
    M(SettingUInt64, max_bytes_to_transfer, 0, "Maximum size (in uncompressed bytes) of the transmitted external table obtained when the GLOBAL IN/JOIN section is executed.")                                                          \
    M(SettingOverflowMode<false>, transfer_overflow_mode, OverflowMode::THROW, "What to do when the limit is exceeded.")                                                                                                                \
                                                                                                                                                                                                                                        \
    M(SettingUInt64, max_rows_in_distinct, 0, "Maximum number of elements during execution of DISTINCT.")                                                                                                                               \
    M(SettingUInt64, max_bytes_in_distinct, 0, "Maximum total size of state (in uncompressed bytes) in memory for the execution of DISTINCT.")                                                                                          \
    M(SettingOverflowMode<false>, distinct_overflow_mode, OverflowMode::THROW, "What to do when the limit is exceeded.")                                                                                                                \
                                                                                                                                                                                                                                        \
    M(SettingMemoryLimit, max_memory_usage, UInt64(0), "Maximum memory usage for processing of single query. Can either be an UInt64 (means memory limit in bytes), "                                                                   \
                        "or be a float-point number (means memory limit in percent of total RAM, from 0.0 to 1.0). 0 or 0.0 means unlimited.")                                                                                          \
    M(SettingMemoryLimit, max_memory_usage_for_user, UInt64(0), "Maximum memory usage for processing all concurrently running queries for the user. Can either be an UInt64 (means memory limit in bytes), "                            \
                        "or be a float-point number (means memory limit in percent of total RAM, from 0.0 to 1.0). 0 or 0.0 means unlimited.")                                                                                          \
    M(SettingMemoryLimit, max_memory_usage_for_all_queries, 0.80, "Maximum memory usage for processing all concurrently running queries on the server. Can either be an UInt64 (means memory limit in bytes), "                         \
                        "or be a float-point number (means memory limit in percent of total RAM, from 0.0 to 1.0). 0 or 0.0 means unlimited.")                                                                                          \
    M(SettingUInt64, bytes_that_rss_larger_than_limit, 1073741824, "How many bytes RSS(Resident Set Size) can be larger than limit(max_memory_usage_for_all_queries). Default: 1GB ")                                                   \
                                                                                                                                                                                                                                        \
    M(SettingUInt64, max_network_bandwidth, 0, "The maximum speed of data exchange over the network in bytes per second for a query. Zero means unlimited.")                                                                            \
    M(SettingUInt64, max_network_bytes, 0, "The maximum number of bytes (compressed) to receive or transmit over the network for execution of the query.")                                                                              \
    M(SettingUInt64, max_network_bandwidth_for_user, 0, "The maximum speed of data exchange over the network in bytes per second for all concurrently running user queries. Zero means "                                                \
                                                        "unlimited.")                                                                                                                                                                   \
    M(SettingUInt64, max_network_bandwidth_for_all_users, 0, "The maximum speed of data exchange over the network in bytes per second for all concurrently running queries. Zero means "                                                \
                                                             "unlimited.")                                                                                                                                                              \
    M(SettingUInt64, task_scheduler_thread_soft_limit, 5000, "The soft limit of threads for min_tso task scheduler.")                                                                                                                   \
    M(SettingUInt64, task_scheduler_thread_hard_limit, 10000, "The hard limit of threads for min_tso task scheduler.")                                                                                                                  \
    M(SettingUInt64, task_scheduler_active_set_soft_limit, 0, "The soft limit of count of active query set for min_tso task scheduler.")                                                                                                \
    M(SettingUInt64, max_grpc_pollers, 200, "The maximum number of grpc thread pool's non-temporary threads, better tune it up to avoid frequent creation/destruction of threads.")                                                     \
    M(SettingBool, enable_elastic_threadpool, true, "Enable elastic thread pool for thread create usages.")                                                                                                                             \
    M(SettingUInt64, elastic_threadpool_init_cap, 400, "The size of elastic thread pool.")                                                                                                                                              \
    M(SettingUInt64, elastic_threadpool_shrink_period_ms, 300000, "The shrink period(ms) of elastic thread pool.")                                                                                                                      \
    M(SettingBool, enable_local_tunnel, true, "Enable local data transfer between local MPP tasks.")                                                                                                                                    \
    M(SettingBool, enable_async_grpc_client, true, "Enable async grpc in MPP.")                                                                                                                                                         \
    M(SettingUInt64, grpc_completion_queue_pool_size, 0, "The size of gRPC completion queue pool. 0 means using hardware_concurrency.")                                                                                                 \
    M(SettingBool, enable_async_server, true, "Enable async rpc server.")                                                                                                                                                               \
    M(SettingUInt64, async_pollers_per_cq, 200, "grpc async pollers per cqs")                                                                                                                                                           \
    M(SettingUInt64, async_cqs, 1, "grpc async cqs")                                                                                                                                                                                    \
    M(SettingUInt64, preallocated_request_count_per_poller, 20, "grpc preallocated_request_count_per_poller")                                                                                                                           \
                                                                                                                                                                                                                                        \
    M(SettingUInt64, manual_compact_pool_size, 1, "The number of worker threads to handle manual compact requests.")                                                                                                                    \
    M(SettingUInt64, manual_compact_max_concurrency, 10, "Max concurrent tasks. It should be larger than pool size.")                                                                                                                   \
    M(SettingUInt64, manual_compact_more_until_ms, 60000, "Continuously compact more segments until reaching specified elapsed time. If 0 is specified, only one segment will be compacted each round.")                                \
    M(SettingUInt64, max_cached_data_bytes_in_spiller, 1024ULL * 1024 * 100, "Max cached data bytes in spiller before spilling, 100MB as the default value, 0 means no limit")                                                              \
    M(SettingUInt64, max_spilled_rows_per_file, 200000, "Max spilled data rows per spill file, 200000 as the default value, 0 mean no limit.")                                                                                          \
    M(SettingUInt64, max_spilled_bytes_per_file, 0, "Max spilled data bytes per spill file, 1GB as the default value, 0 means no limit.")                                                                                               \
                                                                                                                                                                                                                                        \
    M(SettingBool, enable_planner, true, "Enable planner")                                                                                                                                                                              \
    M(SettingBool, enable_pipeline, false, "Enable pipeline model")                                                                                                                                                                     \
    M(SettingUInt64, pipeline_task_thread_pool_size, 0, "The size of task thread pool. 0 means using number_of_logical_cpu_cores.") \
    M(SettingUInt64, local_tunnel_version, 1, "1: not refined, 2: refined")
// clang-format on
#define DECLARE(TYPE, NAME, DEFAULT, DESCRIPTION) TYPE NAME{DEFAULT};

    APPLY_FOR_SETTINGS(DECLARE)

#undef DECLARE

    /// Set setting by name.
    void set(const String & name, const Field & value);

    /// Set setting by name. Read value, serialized in binary form from buffer (for inter-server communication).
    void set(const String & name, ReadBuffer & buf);

    /// Skip value, serialized in binary form in buffer.
    void ignore(const String & name, ReadBuffer & buf);

    /// Set setting by name. Read value in text form from string (for example, from configuration file or from URL parameter).
    void set(const String & name, const String & value);

    /// Get setting by name. Converts value to String.
    String get(const String & name) const;

    bool tryGet(const String & name, String & value) const;

    /** Set multiple settings from "profile" (in server configuration file (users.xml), profiles contain groups of multiple settings).
      * The profile can also be set using the `set` functions, like the profile setting.
      */
    void setProfile(const String & profile_name, Poco::Util::AbstractConfiguration & config);

    /// Load settings from configuration file, at "path" prefix in configuration.
    void loadSettingsFromConfig(const String & path, const Poco::Util::AbstractConfiguration & config);

    /// Read settings from buffer. They are serialized as list of contiguous name-value pairs, finished with empty name.
    /// If readonly=1 is set, ignore read settings.
    void deserialize(ReadBuffer & buf);

    /// Write changed settings to buffer. (For example, to be sent to remote server.)
    void serialize(WriteBuffer & buf) const;
};


} // namespace DB
