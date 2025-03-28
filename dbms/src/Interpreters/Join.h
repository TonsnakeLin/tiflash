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

#pragma once

#include <Columns/ColumnFixedString.h>
#include <Columns/ColumnNullable.h>
#include <Columns/ColumnString.h>
#include <Common/Arena.h>
#include <Common/HashTable/HashMap.h>
#include <Common/Logger.h>
#include <DataStreams/IBlockInputStream.h>
#include <Interpreters/AggregationCommon.h>
#include <Interpreters/ExpressionActions.h>
#include <Interpreters/SettingsCommon.h>
#include <Parsers/ASTTablesInSelectQuery.h>

#include <shared_mutex>

namespace DB
{
struct ProbeProcessInfo;
/** Data structure for implementation of JOIN.
  * It is just a hash table: keys -> rows of joined ("right") table.
  * Additionally, CROSS JOIN is supported: instead of hash table, it use just set of blocks without keys.
  *
  * JOIN-s could be of nine types: ANY/ALL × LEFT/INNER/RIGHT/FULL, and also CROSS.
  *
  * If ANY is specified - then select only one row from the "right" table, (first encountered row), even if there was more matching rows.
  * If ALL is specified - usual JOIN, when rows are multiplied by number of matching rows from the "right" table.
  * ANY is more efficient.
  *
  * If INNER is specified - leave only rows that have matching rows from "right" table.
  * If LEFT is specified - in case when there is no matching row in "right" table, fill it with default values instead.
  * If RIGHT is specified - first process as INNER, but track what rows from the right table was joined,
  *  and at the end, add rows from right table that was not joined and substitute default values for columns of left table.
  * If FULL is specified - first process as LEFT, but track what rows from the right table was joined,
  *  and at the end, add rows from right table that was not joined and substitute default values for columns of left table.
  *
  * Thus, LEFT and RIGHT JOINs are not symmetric in terms of implementation.
  *
  * All JOINs (except CROSS) are done by equality condition on keys (equijoin).
  * Non-equality and other conditions are not supported.
  *
  * Implementation:
  *
  * 1. Build hash table in memory from "right" table.
  * This hash table is in form of keys -> row in case of ANY or keys -> [rows...] in case of ALL.
  * This is done in insertFromBlock method.
  *
  * 2. Process "left" table and join corresponding rows from "right" table by lookups in the map.
  * This is done in joinBlock methods.
  *
  * In case of ANY LEFT JOIN - form new columns with found values or default values.
  * This is the most simple. Number of rows in left table does not change.
  *
  * In case of ANY INNER JOIN - form new columns with found values,
  *  and also build a filter - in what rows nothing was found.
  * Then filter columns of "left" table.
  *
  * In case of ALL ... JOIN - form new columns with all found rows,
  *  and also fill 'offsets' array, describing how many times we need to replicate values of "left" table.
  * Then replicate columns of "left" table.
  *
  * How Nullable keys are processed:
  *
  * NULLs never join to anything, even to each other.
  * During building of map, we just skip keys with NULL value of any component.
  * During joining, we simply treat rows with any NULLs in key as non joined.
  *
  * Default values for outer joins (LEFT, RIGHT, FULL):
  *
  * Always generate Nullable column and substitute NULLs for non-joined rows,
  *  as in standard SQL.
  */
class Join
{
public:
    Join(const Names & key_names_left_,
         const Names & key_names_right_,
         ASTTableJoin::Kind kind_,
         ASTTableJoin::Strictness strictness_,
         const String & req_id,
         bool enable_fine_grained_shuffle_,
         size_t fine_grained_shuffle_count_,
         const TiDB::TiDBCollators & collators_ = TiDB::dummy_collators,
         const String & left_filter_column = "",
         const String & right_filter_column = "",
         const String & other_filter_column = "",
         const String & other_eq_filter_from_in_column = "",
         ExpressionActionsPtr other_condition_ptr = nullptr,
         size_t max_block_size = 0,
         const String & match_helper_name = "");

    /** Call `setBuildConcurrencyAndInitPool`, `initMapImpl` and `setSampleBlock`.
      * You must call this method before subsequent calls to insertFromBlock.
      */
    void init(const Block & sample_block, size_t build_concurrency_ = 1);

    void insertFromBlock(const Block & block);

    void insertFromBlock(const Block & block, size_t stream_index);

    /** Join data from the map (that was previously built by calls to insertFromBlock) to the block with data from "left" table.
      * Could be called from different threads in parallel.
      */
    Block joinBlock(ProbeProcessInfo & probe_process_info) const;

    void checkTypes(const Block & block) const;

    bool needReturnNonJoinedData() const;

    /** For RIGHT and FULL JOINs.
      * A stream that will contain default values from left table, joined with rows from right table, that was not joined before.
      * Use only after all calls to joinBlock was done.
      */
    BlockInputStreamPtr createStreamWithNonJoinedRows(const Block & left_sample_block, size_t index, size_t step, size_t max_block_size) const;

    /// Number of keys in all built JOIN maps.
    size_t getTotalRowCount() const;
    /// Sum size in bytes of all buffers, used for JOIN maps and for all memory pools.
    size_t getTotalByteCount() const;

    size_t getTotalBuildInputRows() const { return total_input_build_rows; }

    ASTTableJoin::Kind getKind() const { return kind; }

    const Names & getLeftJoinKeys() const { return key_names_left; }

    void setInitActiveBuildConcurrency()
    {
        std::unique_lock lock(build_probe_mutex);
        active_build_concurrency = getBuildConcurrencyInternal();
    }
    void finishOneBuild();
    void waitUntilAllBuildFinished() const;

    size_t getProbeConcurrency() const
    {
        std::unique_lock lock(build_probe_mutex);
        return probe_concurrency;
    }
    void setProbeConcurrency(size_t concurrency)
    {
        std::unique_lock lock(build_probe_mutex);
        probe_concurrency = concurrency;
        active_probe_concurrency = probe_concurrency;
    }
    void finishOneProbe();
    void waitUntilAllProbeFinished() const;

    size_t getBuildConcurrency() const
    {
        std::shared_lock lock(rwlock);
        return getBuildConcurrencyInternal();
    }

    void meetError(const String & error_message);

    /// Reference to the row in block.
    struct RowRef
    {
        const Block * block;
        size_t row_num;

        RowRef() = default;
        RowRef(const Block * block_, size_t row_num_)
            : block(block_)
            , row_num(row_num_)
        {}
    };

    /// Single linked list of references to rows. Used for ALL JOINs (non-unique JOINs)
    struct RowRefList : RowRef
    {
        RowRefList * next = nullptr;

        RowRefList() = default;
        RowRefList(const Block * block_, size_t row_num_)
            : RowRef(block_, row_num_)
        {}
    };

    /** Depending on template parameter, adds or doesn't add a flag, that element was used (row was joined).
      * For implementation of RIGHT and FULL JOINs.
      * NOTE: It is possible to store the flag in one bit of pointer to block or row_num. It seems not reasonable, because memory saving is minimal.
      */
    template <bool enable, typename Base>
    struct WithUsedFlag;

    template <typename Base>
    struct WithUsedFlag<true, Base> : Base
    {
        mutable std::atomic<bool> used{};
        using Base::Base;
        using Base_t = Base;
        void setUsed() const { used.store(true, std::memory_order_relaxed); } /// Could be set simultaneously from different threads.
        bool getUsed() const { return used; }
    };

    template <typename Base>
    struct WithUsedFlag<false, Base> : Base
    {
        using Base::Base;
        using Base_t = Base;
        void setUsed() const {}
        bool getUsed() const { return true; }
    };


/// Different types of keys for maps.
#define APPLY_FOR_JOIN_VARIANTS(M) \
    M(key8)                        \
    M(key16)                       \
    M(key32)                       \
    M(key64)                       \
    M(key_string)                  \
    M(key_strbinpadding)           \
    M(key_strbin)                  \
    M(key_fixed_string)            \
    M(keys128)                     \
    M(keys256)                     \
    M(serialized)

    enum class Type
    {
        EMPTY,
        CROSS,
#define M(NAME) NAME,
        APPLY_FOR_JOIN_VARIANTS(M)
#undef M
    };


    /** Different data structures, that are used to perform JOIN.
      */
    template <typename Mapped>
    struct MapsTemplate
    {
        std::unique_ptr<ConcurrentHashMap<UInt8, Mapped, TrivialHash, HashTableFixedGrower<8>>> key8;
        std::unique_ptr<ConcurrentHashMap<UInt16, Mapped, TrivialHash, HashTableFixedGrower<16>>> key16;
        std::unique_ptr<ConcurrentHashMap<UInt32, Mapped, HashCRC32<UInt32>>> key32;
        std::unique_ptr<ConcurrentHashMap<UInt64, Mapped, HashCRC32<UInt64>>> key64;
        std::unique_ptr<ConcurrentHashMapWithSavedHash<StringRef, Mapped>> key_string;
        std::unique_ptr<ConcurrentHashMapWithSavedHash<StringRef, Mapped>> key_strbinpadding;
        std::unique_ptr<ConcurrentHashMapWithSavedHash<StringRef, Mapped>> key_strbin;
        std::unique_ptr<ConcurrentHashMapWithSavedHash<StringRef, Mapped>> key_fixed_string;
        std::unique_ptr<ConcurrentHashMap<UInt128, Mapped, HashCRC32<UInt128>>> keys128;
        std::unique_ptr<ConcurrentHashMap<UInt256, Mapped, HashCRC32<UInt256>>> keys256;
        std::unique_ptr<ConcurrentHashMapWithSavedHash<StringRef, Mapped>> serialized;
        // TODO: add more cases like Aggregator
    };

    using MapsAny = MapsTemplate<WithUsedFlag<false, RowRef>>;
    using MapsAll = MapsTemplate<WithUsedFlag<false, RowRefList>>;
    using MapsAnyFull = MapsTemplate<WithUsedFlag<true, RowRef>>;
    using MapsAllFull = MapsTemplate<WithUsedFlag<true, RowRefList>>;

    static const String match_helper_prefix;
    static const DataTypePtr match_helper_type;

    // only use for left semi joins.
    const String match_helper_name;


private:
    friend class NonJoinedBlockInputStream;

    ASTTableJoin::Kind kind;
    ASTTableJoin::Strictness strictness;

    /// Names of key columns (columns for equi-JOIN) in "left" table (in the order they appear in USING clause).
    const Names key_names_left;
    /// Names of key columns (columns for equi-JOIN) in "right" table (in the order they appear in USING clause).
    const Names key_names_right;

    mutable std::mutex build_probe_mutex;

    mutable std::condition_variable build_cv;
    size_t build_concurrency;
    size_t active_build_concurrency;

    mutable std::condition_variable probe_cv;
    size_t probe_concurrency;
    size_t active_probe_concurrency;

    bool meet_error = false;
    String error_message;

private:
    /// collators for the join key
    const TiDB::TiDBCollators collators;

    String left_filter_column;
    String right_filter_column;
    String other_filter_column;
    String other_eq_filter_from_in_column;
    ExpressionActionsPtr other_condition_ptr;
    ASTTableJoin::Strictness original_strictness;
    size_t max_block_size_for_cross_join;
    /** Blocks of "right" table.
      */
    BlocksList blocks;
    /// mutex to protect concurrent insert to blocks
    std::mutex blocks_lock;
    /// keep original block for concurrent build
    Blocks original_blocks;

    MapsAny maps_any; /// For ANY LEFT|INNER JOIN
    MapsAll maps_all; /// For ALL LEFT|INNER JOIN
    MapsAnyFull maps_any_full; /// For ANY RIGHT|FULL JOIN
    MapsAllFull maps_all_full; /// For ALL RIGHT|FULL JOIN

    /// For right/full join, including
    /// 1. Rows with NULL join keys
    /// 2. Rows that are filtered by right join conditions
    std::vector<std::unique_ptr<RowRefList>> rows_not_inserted_to_map;

    /// Additional data - strings for string keys and continuation elements of single-linked lists of references to rows.
    Arenas pools;


private:
    Type type = Type::EMPTY;

    Type chooseMethod(const ColumnRawPtrs & key_columns, Sizes & key_sizes) const;

    Sizes key_sizes;

    /// Block with columns from the right-side table except key columns.
    Block sample_block_with_columns_to_add;
    /// Block with key columns in the same order they appear in the right-side table.
    Block sample_block_with_keys;

    const LoggerPtr log;

    std::atomic<size_t> total_input_build_rows{0};
    /** Protect state for concurrent use in insertFromBlock and joinBlock.
      * Note that these methods could be called simultaneously only while use of StorageJoin,
      *  and StorageJoin only calls these two methods.
      * That's why another methods are not guarded.
      */
    mutable std::shared_mutex rwlock;

    bool initialized = false;
    bool enable_fine_grained_shuffle = false;
    size_t fine_grained_shuffle_count = 0;

    size_t getBuildConcurrencyInternal() const
    {
        if (unlikely(build_concurrency == 0))
            throw Exception("Logical error: `setBuildConcurrencyAndInitPool` has not been called", ErrorCodes::LOGICAL_ERROR);
        return build_concurrency;
    }

    /// Initialize map implementations for various join types.
    void initMapImpl(Type type_);

    /** Set information about structure of right hand of JOIN (joined data).
      * You must call this method before subsequent calls to insertFromBlock.
      */
    void setSampleBlock(const Block & block);

    /** Set Join build concurrency and init hash map.
      * You must call this method before subsequent calls to insertFromBlock.
      */
    void setBuildConcurrencyAndInitPool(size_t build_concurrency_);

    /// Throw an exception if blocks have different types of key columns.
    void checkTypesOfKeys(const Block & block_left, const Block & block_right) const;

    /** Add block of data from right hand of JOIN to the map.
      * Returns false, if some limit was exceeded and you should not insert more data.
      */
    void insertFromBlockInternal(Block * stored_block, size_t stream_index);

    template <ASTTableJoin::Kind KIND, ASTTableJoin::Strictness STRICTNESS, typename Maps>
    void joinBlockImpl(Block & block, const Maps & maps, ProbeProcessInfo & probe_process_info) const;

    /** Handle non-equal join conditions
      *
      * @param block
      */
    void handleOtherConditions(Block & block, std::unique_ptr<IColumn::Filter> & filter, std::unique_ptr<IColumn::Offsets> & offsets_to_replicate, const std::vector<size_t> & right_table_column) const;


    template <ASTTableJoin::Kind KIND, ASTTableJoin::Strictness STRICTNESS>
    void joinBlockImplCross(Block & block) const;

    template <ASTTableJoin::Kind KIND, ASTTableJoin::Strictness STRICTNESS, bool has_null_map>
    void joinBlockImplCrossInternal(Block & block, ConstNullMapPtr null_map) const;
};

using JoinPtr = std::shared_ptr<Join>;
using Joins = std::vector<JoinPtr>;

struct ProbeProcessInfo
{
    Block block;
    UInt64 max_block_size;
    size_t start_row;
    size_t end_row;
    bool all_rows_joined_finish;

    ProbeProcessInfo(UInt64 max_block_size_)
        : max_block_size(max_block_size_)
        , all_rows_joined_finish(true){};

    void resetBlock(Block && block_);
    void updateStartRow();
};

void convertColumnToNullable(ColumnWithTypeAndName & column);

} // namespace DB
