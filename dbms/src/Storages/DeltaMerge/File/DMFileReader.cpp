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

#include <Columns/ColumnsCommon.h>
#include <Common/CurrentMetrics.h>
#include <Common/Stopwatch.h>
#include <Common/escapeForFileName.h>
#include <DataTypes/IDataType.h>
#include <Encryption/FileProvider.h>
#include <Encryption/createReadBufferFromFileBaseByFileProvider.h>
#include <Flash/Coprocessor/DAGContext.h>
#include <Poco/File.h>
#include <Poco/Thread_STD.h>
#include <Storages/DeltaMerge/DMContext.h>
#include <Storages/DeltaMerge/File/DMFileBlockInputStream.h>
#include <Storages/DeltaMerge/File/DMFilePackFilter.h>
#include <Storages/DeltaMerge/File/DMFileReader.h>
#include <Storages/DeltaMerge/ScanContext.h>
#include <Storages/DeltaMerge/convertColumnTypeHelpers.h>
#include <Storages/Page/PageUtil.h>
#include <fmt/format.h>

namespace CurrentMetrics
{
extern const Metric OpenFileForRead;
}

namespace DB
{
namespace ErrorCodes
{
extern const int LOGICAL_ERROR;
}
namespace DM
{
DMFileReader::Stream::Stream(
    DMFileReader & reader,
    ColId col_id,
    const String & file_name_base,
    size_t aio_threshold,
    size_t max_read_buffer_size,
    const LoggerPtr & log,
    const ReadLimiterPtr & read_limiter)
    : avg_size_hint(reader.dmfile->getColumnStat(col_id).avg_size)
{
    // load mark data
    auto mark_load = [&]() -> MarksInCompressedFilePtr {
        auto res = std::make_shared<MarksInCompressedFile>(reader.dmfile->getPacks());
        if (res->empty()) // 0 rows.
            return res;
        size_t size = sizeof(MarkInCompressedFile) * reader.dmfile->getPacks();
        if (reader.dmfile->configuration)
        {
            auto buffer = createReadBufferFromFileBaseByFileProvider(
                reader.file_provider,
                reader.dmfile->colMarkPath(file_name_base),
                reader.dmfile->encryptionMarkPath(file_name_base),
                reader.dmfile->getConfiguration()->getChecksumFrameLength(),
                read_limiter,
                reader.dmfile->getConfiguration()->getChecksumAlgorithm(),
                reader.dmfile->getConfiguration()->getChecksumFrameLength());
            buffer->readBig(reinterpret_cast<char *>(res->data()), size);
        }
        else
        {
            auto file = reader.file_provider->newRandomAccessFile(reader.dmfile->colMarkPath(file_name_base),
                                                                  reader.dmfile->encryptionMarkPath(file_name_base));
            PageUtil::readFile(file, 0, reinterpret_cast<char *>(res->data()), size, read_limiter);
        }
        return res;
    };

    if (reader.mark_cache)
        marks = reader.mark_cache->getOrSet(reader.dmfile->colMarkCacheKey(file_name_base), mark_load);
    else
        marks = mark_load();

    const String data_path = reader.dmfile->colDataPath(file_name_base);
    size_t data_file_size = reader.dmfile->colDataSize(col_id);
    size_t packs = reader.dmfile->getPacks();
    size_t buffer_size = 0;
    size_t estimated_size = 0;

    const auto & use_packs = reader.pack_filter.getUsePacks();
    if (!reader.dmfile->configuration)
    {
        for (size_t i = 0; i < packs;)
        {
            if (!use_packs[i])
            {
                ++i;
                continue;
            }
            size_t cur_offset_in_file = getOffsetInFile(i);
            size_t end = i + 1;
            // First find the end of current available range.
            while (end < packs && use_packs[end])
                ++end;

            // Second If the end of range is inside the block, we will need to read it too.
            if (end < packs)
            {
                size_t last_offset_in_file = getOffsetInFile(end);
                if (getOffsetInDecompressedBlock(end) > 0)
                {
                    while (end < packs && getOffsetInFile(end) == last_offset_in_file)
                        ++end;
                }
            }

            size_t range_end_in_file = (end == packs) ? data_file_size : getOffsetInFile(end);

            size_t range = range_end_in_file - cur_offset_in_file;
            buffer_size = std::max(buffer_size, range);

            estimated_size += range;
            i = end;
        }
    }
    else
    {
        estimated_size = reader.dmfile->colDataSize(col_id);
    }

    buffer_size = std::min(buffer_size, max_read_buffer_size);

    LOG_TRACE(log,
              "file size: {}, estimated read size: {}, buffer_size: {} (aio_threshold: {}, max_read_buffer_size: {})",
              data_file_size,
              estimated_size,
              buffer_size,
              aio_threshold,
              max_read_buffer_size);

    if (!reader.dmfile->configuration)
    {
        buf = std::make_unique<CompressedReadBufferFromFileProvider<true>>(reader.file_provider,
                                                                           reader.dmfile->colDataPath(file_name_base),
                                                                           reader.dmfile->encryptionDataPath(file_name_base),
                                                                           estimated_size,
                                                                           aio_threshold,
                                                                           read_limiter,
                                                                           buffer_size);
    }
    else
    {
        buf = std::make_unique<CompressedReadBufferFromFileProvider<false>>(
            reader.file_provider,
            reader.dmfile->colDataPath(file_name_base),
            reader.dmfile->encryptionDataPath(file_name_base),
            estimated_size,
            read_limiter,
            reader.dmfile->configuration->getChecksumAlgorithm(),
            reader.dmfile->configuration->getChecksumFrameLength());
    }
}

DMFileReader::DMFileReader(
    const DMFilePtr & dmfile_,
    const ColumnDefines & read_columns_,
    bool is_common_handle_,
    // clean read
    bool enable_handle_clean_read_,
    bool enable_del_clean_read_,
    bool is_fast_scan_,
    UInt64 max_read_version_,
    // filters
    DMFilePackFilter && pack_filter_,
    // caches
    const MarkCachePtr & mark_cache_,
    bool enable_column_cache_,
    const ColumnCachePtr & column_cache_,
    size_t aio_threshold,
    size_t max_read_buffer_size,
    const FileProviderPtr & file_provider_,
    const ReadLimiterPtr & read_limiter,
    size_t rows_threshold_per_read_,
    bool read_one_pack_every_time_,
    const String & tracing_id_,
    bool enable_col_sharing_cache,
    const ScanContextPtr & scan_context_)
    : dmfile(dmfile_)
    , read_columns(read_columns_)
    , is_common_handle(is_common_handle_)
    , read_one_pack_every_time(read_one_pack_every_time_)
    , enable_handle_clean_read(enable_handle_clean_read_)
    , enable_del_clean_read(enable_del_clean_read_)
    , is_fast_scan(is_fast_scan_)
    , max_read_version(max_read_version_)
    , pack_filter(std::move(pack_filter_))
    , skip_packs_by_column(read_columns.size(), 0)
    , mark_cache(mark_cache_)
    , enable_column_cache(enable_column_cache_ && column_cache_)
    , column_cache(column_cache_)
    , scan_context(scan_context_)
    , rows_threshold_per_read(rows_threshold_per_read_)
    , file_provider(file_provider_)
    , log(Logger::get(tracing_id_))
{
    for (const auto & cd : read_columns)
    {
        // New inserted column, will be filled with default value later
        if (!dmfile->isColumnExist(cd.id))
            continue;

        // Load stream for existing columns according to DataType in disk
        auto callback = [&](const IDataType::SubstreamPath & substream) {
            const auto stream_name = DMFile::getFileNameBase(cd.id, substream);
            auto stream = std::make_unique<Stream>( //
                *this,
                cd.id,
                stream_name,
                aio_threshold,
                max_read_buffer_size,
                log,
                read_limiter);
            column_streams.emplace(stream_name, std::move(stream));
        };
        const auto data_type = dmfile->getColumnStat(cd.id).type;
        data_type->enumerateStreams(callback, {});
    }
    if (enable_col_sharing_cache)
    {
        col_data_cache = std::make_unique<ColumnSharingCacheMap>(path(), read_columns, log);
        for (const auto & cd : read_columns)
        {
            last_read_from_cache[cd.id] = false;
        }
    }
}

bool DMFileReader::shouldSeek(size_t pack_id)
{
    // If current pack is the first one, or we just finished reading the last pack, then no need to seek.
    return pack_id != 0 && !pack_filter.getUsePacks()[pack_id - 1];
}

bool DMFileReader::getSkippedRows(size_t & skip_rows)
{
    skip_rows = 0;
    const auto & use_packs = pack_filter.getUsePacks();
    const auto & pack_stats = dmfile->getPackStats();
    for (; next_pack_id < use_packs.size() && !use_packs[next_pack_id]; ++next_pack_id)
    {
        skip_rows += pack_stats[next_pack_id].rows;
        scan_context->total_dmfile_skipped_packs += 1;
        scan_context->total_dmfile_skipped_rows += pack_stats[next_pack_id].rows;
    }
    next_row_offset += skip_rows;
    return next_pack_id < use_packs.size();
}

size_t DMFileReader::skipNextBlock()
{
    // Go to next available pack.
    size_t skip;
    if (!getSkippedRows(skip))
        return 0;

    // Find the next contiguous packs will be read in next read,
    // let next_pack_id point to the next pack of the contiguous packs.
    // For example, if we have 10 packs, use_packs is [0, 1, 1, 0, 1, 1, 0, 0, 1, 1],
    // and now next_pack_id is 1, then we will skip 2 packs(index 1 and 2), and next_pack_id will be 3.
    size_t read_pack_limit = read_one_pack_every_time ? 1 : 0;
    const std::vector<RSResult> & handle_res = pack_filter.getHandleRes();
    RSResult expected_handle_res = handle_res[next_pack_id];
    auto & use_packs = pack_filter.getUsePacks();
    size_t start_pack_id = next_pack_id;
    const auto & pack_stats = dmfile->getPackStats();
    size_t read_rows = 0;
    for (; next_pack_id < use_packs.size() && use_packs[next_pack_id] && read_rows < rows_threshold_per_read; ++next_pack_id)
    {
        if (read_pack_limit != 0 && next_pack_id - start_pack_id >= read_pack_limit)
            break;
        if (enable_handle_clean_read && handle_res[next_pack_id] != expected_handle_res)
            break;

        read_rows += pack_stats[next_pack_id].rows;
        scan_context->total_dmfile_skipped_packs += 1;
    }

    scan_context->total_dmfile_skipped_rows += read_rows;
    next_row_offset += read_rows;
    return read_rows;
}

Block DMFileReader::readWithFilter(const IColumn::Filter & filter)
{
    size_t skip_rows;
    if (!getSkippedRows(skip_rows))
        return {};

    const auto & pack_stats = dmfile->getPackStats();
    auto & use_packs = pack_filter.getUsePacks();

    size_t start_row_offset = next_row_offset;

    size_t read_rows = 0;
    size_t next_pack_id_cp = next_pack_id;
    while (next_pack_id_cp < use_packs.size() && read_rows + pack_stats[next_pack_id_cp].rows <= filter.size())
    {
        const auto begin = filter.cbegin() + read_rows;
        const auto end = filter.cbegin() + read_rows + pack_stats[next_pack_id_cp].rows;
        use_packs[next_pack_id_cp] = use_packs[next_pack_id_cp] && std::find(begin, end, 1) != end;
        read_rows += pack_stats[next_pack_id_cp].rows;
        ++next_pack_id_cp;
    }
    // filter.size() equals to the number of rows in the next block
    // so read_rows should be equal to filter.size() here.
    RUNTIME_CHECK(read_rows == filter.size());

    // mark the next pack after next read as not used temporarily
    // to avoid reading it and its following packs in this round
    bool next_pack_id_use_packs_cp = false;
    if (next_pack_id_cp < use_packs.size())
    {
        next_pack_id_use_packs_cp = use_packs[next_pack_id_cp];
        use_packs[next_pack_id_cp] = false;
    }

    Blocks blocks;
    blocks.reserve(next_pack_id_cp - next_pack_id);

    read_rows = 0;
    for (size_t i = next_pack_id; i < next_pack_id_cp; ++i)
    {
        // When the next pack is not used or the pack is the last pack, call read() to read theses packs and filter them
        // For example:
        //  When next_pack_id_cp = use_packs.size() and use_packs[next_pack_id:next_pack_id_cp] = [true, true, false, true, true, true]
        //  The algorithm runs as follows:
        //      When i = next_pack_id + 2, call read() to read {next_pack_id, next_pack_id + 1}th packs
        //      When i = next_pack_id + 5, call read() to read {next_pack_id + 3, next_pack_id + 4, next_pack_id + 5}th packs
        if (use_packs[i] && (i + 1 == use_packs.size() || !use_packs[i + 1]))
        {
            Block block = read();

            IColumn::Filter block_filter;
            block_filter.resize(block.rows());
            std::copy(filter.cbegin() + read_rows, filter.cbegin() + read_rows + block.rows(), block_filter.begin());
            read_rows += block.rows();

            if (size_t passed_count = countBytesInFilter(block_filter); passed_count != block.rows())
            {
                for (auto & col : block)
                {
                    col.column = col.column->filter(block_filter, passed_count);
                }
            }

            blocks.emplace_back(std::move(block));
        }
        else if (!use_packs[i])
        {
            read_rows += pack_stats[i].rows;
        }
    }

    // restore the use_packs of next pack after next read
    if (next_pack_id_cp < use_packs.size())
        use_packs[next_pack_id_cp] = next_pack_id_use_packs_cp;

    // merge blocks
    Block res = vstackBlocks(std::move(blocks));
    res.setStartOffset(start_row_offset);
    return res;
}

inline bool isExtraColumn(const ColumnDefine & cd)
{
    return cd.id == EXTRA_HANDLE_COLUMN_ID || cd.id == VERSION_COLUMN_ID || cd.id == TAG_COLUMN_ID;
}

inline bool isCacheableColumn(const ColumnDefine & cd)
{
    return cd.id == EXTRA_HANDLE_COLUMN_ID || cd.id == VERSION_COLUMN_ID;
}

Block DMFileReader::read()
{
    Stopwatch watch;
    SCOPE_EXIT(
        scan_context->total_dmfile_read_time_ns += watch.elapsed(););

    // Go to next available pack.
    size_t skip_rows;

    getSkippedRows(skip_rows);

    const auto & use_packs = pack_filter.getUsePacks();
    if (next_pack_id >= use_packs.size())
        return {};
    // Find max continuing rows we can read.
    size_t start_pack_id = next_pack_id;
    size_t start_row_offset = next_row_offset;
    // When read_one_pack_every_time is true, we can just read one pack every time.
    // 0 means no limit
    size_t read_pack_limit = read_one_pack_every_time ? 1 : 0;

    const auto & pack_stats = dmfile->getPackStats();

    const auto & pack_properties = dmfile->getPackProperties();

    size_t read_rows = 0;
    size_t not_clean_rows = 0;
    size_t deleted_rows = 0;

    const std::vector<RSResult> & handle_res = pack_filter.getHandleRes(); // alias of handle_res in pack_filter
    RSResult expected_handle_res = handle_res[next_pack_id];
    for (; next_pack_id < use_packs.size() && use_packs[next_pack_id] && read_rows < rows_threshold_per_read; ++next_pack_id)
    {
        if (read_pack_limit != 0 && next_pack_id - start_pack_id >= read_pack_limit)
            break;
        if (enable_handle_clean_read && handle_res[next_pack_id] != expected_handle_res)
            break;

        read_rows += pack_stats[next_pack_id].rows;
        not_clean_rows += pack_stats[next_pack_id].not_clean;
        // Because deleted_rows is a new field in pack_properties, we need to check whehter this pack has this field.
        // If this pack doesn't have this field, then we can't know whether this pack contains deleted rows.
        // Thus we just deleted_rows += 1, to make sure we will not do the optimization with del column(just to make deleted_rows != 0).
        if (static_cast<size_t>(pack_properties.property_size()) > next_pack_id && pack_properties.property(next_pack_id).has_deleted_rows())
        {
            deleted_rows += pack_properties.property(next_pack_id).deleted_rows();
        }
        else
        {
            deleted_rows += 1;
        }
    }
    next_row_offset += read_rows;

    if (read_rows == 0)
        return {};

    Block res;
    res.setStartOffset(start_row_offset);

    size_t read_packs = next_pack_id - start_pack_id;

    scan_context->total_dmfile_scanned_packs += read_packs;
    scan_context->total_dmfile_scanned_rows += read_rows;

    // TODO: this will need better algorithm: we should separate those packs which can and can not do clean read.
    bool do_clean_read_on_normal_mode = enable_handle_clean_read && expected_handle_res == All && not_clean_rows == 0 && (!is_fast_scan);

    bool do_clean_read_on_handle_on_fast_mode = enable_handle_clean_read && is_fast_scan && expected_handle_res == All;
    bool do_clean_read_on_del_on_fast_mode = enable_del_clean_read && is_fast_scan && deleted_rows == 0;


    if (do_clean_read_on_normal_mode)
    {
        UInt64 max_version = 0;
        for (size_t pack_id = start_pack_id; pack_id < next_pack_id; ++pack_id)
            max_version = std::max(pack_filter.getMaxVersion(pack_id), max_version);
        do_clean_read_on_normal_mode = max_version <= max_read_version;
    }

    for (size_t i = 0; i < read_columns.size(); ++i)
    {
        try
        {
            // For clean read of column pk, version, tag, instead of loading data from disk, just create placeholder column is OK.
            auto & cd = read_columns[i];
            if (cd.id == EXTRA_HANDLE_COLUMN_ID && do_clean_read_on_handle_on_fast_mode)
            {
                // Return the first row's handle
                ColumnPtr column;
                if (is_common_handle)
                {
                    StringRef min_handle = pack_filter.getMinStringHandle(start_pack_id);
                    column = cd.type->createColumnConst(read_rows, Field(min_handle.data, min_handle.size));
                }
                else
                {
                    Handle min_handle = pack_filter.getMinHandle(start_pack_id);
                    column = cd.type->createColumnConst(read_rows, Field(min_handle));
                }
                res.insert(ColumnWithTypeAndName{column, cd.type, cd.name, cd.id});
                skip_packs_by_column[i] = read_packs;
            }
            else if (cd.id == TAG_COLUMN_ID && do_clean_read_on_del_on_fast_mode)
            {
                ColumnPtr column;

                column = cd.type->createColumnConst(read_rows, Field(static_cast<UInt64>(pack_stats[start_pack_id].first_tag)));
                res.insert(ColumnWithTypeAndName{column, cd.type, cd.name, cd.id});

                skip_packs_by_column[i] = read_packs;
            }
            else if (do_clean_read_on_normal_mode && isExtraColumn(cd))
            {
                ColumnPtr column;
                if (cd.id == EXTRA_HANDLE_COLUMN_ID)
                {
                    // Return the first row's handle
                    if (is_common_handle)
                    {
                        StringRef min_handle = pack_filter.getMinStringHandle(start_pack_id);
                        column = cd.type->createColumnConst(read_rows, Field(min_handle.data, min_handle.size));
                    }
                    else
                    {
                        Handle min_handle = pack_filter.getMinHandle(start_pack_id);
                        column = cd.type->createColumnConst(read_rows, Field(min_handle));
                    }
                }
                else if (cd.id == VERSION_COLUMN_ID)
                {
                    column = cd.type->createColumnConst(read_rows, Field(pack_stats[start_pack_id].first_version));
                }
                else if (cd.id == TAG_COLUMN_ID)
                {
                    column = cd.type->createColumnConst(read_rows, Field(static_cast<UInt64>(pack_stats[start_pack_id].first_tag)));
                }

                res.insert(ColumnWithTypeAndName{column, cd.type, cd.name, cd.id});

                skip_packs_by_column[i] = read_packs;
            }
            else
            {
                const auto stream_name = DMFile::getFileNameBase(cd.id);
                if (auto iter = column_streams.find(stream_name); iter != column_streams.end())
                {
                    if (enable_column_cache && isCacheableColumn(cd))
                    {
                        auto read_strategy = column_cache->getReadStrategy(start_pack_id, read_packs, cd.id);

                        auto data_type = dmfile->getColumnStat(cd.id).type;
                        auto column = data_type->createColumn();
                        column->reserve(read_rows);
                        for (auto & [range, strategy] : read_strategy)
                        {
                            if (strategy == ColumnCache::Strategy::Memory)
                            {
                                for (size_t cursor = range.first; cursor < range.second; cursor++)
                                {
                                    auto cache_element = column_cache->getColumn(cursor, cd.id);
                                    column->insertRangeFrom(
                                        *(cache_element.first),
                                        cache_element.second.first,
                                        cache_element.second.second);
                                }
                                skip_packs_by_column[i] += (range.second - range.first);
                            }
                            else if (strategy == ColumnCache::Strategy::Disk)
                            {
                                size_t rows_count = 0;
                                for (size_t cursor = range.first; cursor < range.second; cursor++)
                                {
                                    rows_count += pack_stats[cursor].rows;
                                }
                                ColumnPtr col;
                                readColumn(cd, col, range.first, range.second - range.first, rows_count, skip_packs_by_column[i]);
                                column->insertRangeFrom(*col, 0, col->size());
                                skip_packs_by_column[i] = 0;
                            }
                            else
                            {
                                throw Exception("Unknown strategy", ErrorCodes::LOGICAL_ERROR);
                            }
                        }
                        ColumnPtr result_column = std::move(column);
                        size_t rows_offset = 0;
                        for (size_t cursor = start_pack_id; cursor < start_pack_id + read_packs; cursor++)
                        {
                            column_cache->tryPutColumn(cursor, cd.id, result_column, rows_offset, pack_stats[cursor].rows);
                            rows_offset += pack_stats[cursor].rows;
                        }
                        // Cast column's data from DataType in disk to what we need now
                        auto converted_column = convertColumnByColumnDefineIfNeed(data_type, std::move(result_column), cd);
                        res.insert(ColumnWithTypeAndName{converted_column, cd.type, cd.name, cd.id});
                    }
                    else
                    {
                        auto data_type = dmfile->getColumnStat(cd.id).type;
                        ColumnPtr column;
                        readColumn(cd, column, start_pack_id, read_packs, read_rows, skip_packs_by_column[i]);
                        auto converted_column = convertColumnByColumnDefineIfNeed(data_type, std::move(column), cd);

                        res.insert(ColumnWithTypeAndName{std::move(converted_column), cd.type, cd.name, cd.id});
                        skip_packs_by_column[i] = 0;
                    }
                }
                else
                {
                    LOG_TRACE(
                        log,
                        "Column [id: {}, name: {}, type: {}] not found, use default value. DMFile: {}",
                        cd.id,
                        cd.name,
                        cd.type->getName(),
                        dmfile->path());
                    // New column after ddl is not exist in this DMFile, fill with default value
                    ColumnPtr column = createColumnWithDefaultValue(cd, read_rows);

                    res.insert(ColumnWithTypeAndName{std::move(column), cd.type, cd.name, cd.id});
                    skip_packs_by_column[i] = 0;
                }
            }
        }
        catch (DB::Exception & e)
        {
            e.addMessage("(while reading from DTFile: " + this->dmfile->path() + ")");
            e.rethrow();
        }
    }
    return res;
}

void DMFileReader::readFromDisk(
    ColumnDefine & column_define,
    MutableColumnPtr & column,
    size_t start_pack_id,
    size_t read_rows,
    size_t skip_packs,
    bool force_seek)
{
    const auto stream_name = DMFile::getFileNameBase(column_define.id);
    if (auto iter = column_streams.find(stream_name); iter != column_streams.end())
    {
        auto & top_stream = iter->second;
        bool should_seek = force_seek || shouldSeek(start_pack_id) || skip_packs > 0;

        auto data_type = dmfile->getColumnStat(column_define.id).type;
        data_type->deserializeBinaryBulkWithMultipleStreams( //
            *column,
            [&](const IDataType::SubstreamPath & substream_path) {
                const auto substream_name = DMFile::getFileNameBase(column_define.id, substream_path);
                auto & sub_stream = column_streams.at(substream_name);

                if (should_seek)
                {
                    sub_stream->buf->seek(sub_stream->getOffsetInFile(start_pack_id),
                                          sub_stream->getOffsetInDecompressedBlock(start_pack_id));
                }

                return sub_stream->buf.get();
            },
            read_rows,
            top_stream->avg_size_hint,
            true,
            {});
        IDataType::updateAvgValueSizeHint(*column, top_stream->avg_size_hint);
    }
}

void DMFileReader::readColumn(ColumnDefine & column_define,
                              ColumnPtr & column,
                              size_t start_pack_id,
                              size_t pack_count,
                              size_t read_rows,
                              size_t skip_packs)
{
    if (!getCachedPacks(column_define.id, start_pack_id, pack_count, read_rows, column))
    {
        auto data_type = dmfile->getColumnStat(column_define.id).type;
        auto col = data_type->createColumn();
        readFromDisk(column_define, col, start_pack_id, read_rows, skip_packs, last_read_from_cache[column_define.id]);
        column = std::move(col);
        last_read_from_cache[column_define.id] = false;
    }
    else
    {
        last_read_from_cache[column_define.id] = true;
    }

    if (col_data_cache != nullptr)
    {
        DMFileReaderPool::instance().set(*this, column_define.id, start_pack_id, pack_count, column);
    }
}

void DMFileReader::addCachedPacks(ColId col_id, size_t start_pack_id, size_t pack_count, ColumnPtr & col)
{
    if (col_data_cache == nullptr)
    {
        return;
    }
    if (next_pack_id >= start_pack_id + pack_count)
    {
        col_data_cache->addStale();
    }
    else
    {
        col_data_cache->add(col_id, start_pack_id, pack_count, col);
    }
}

bool DMFileReader::getCachedPacks(ColId col_id, size_t start_pack_id, size_t pack_count, size_t read_rows, ColumnPtr & col)
{
    if (col_data_cache == nullptr)
    {
        return false;
    }
    auto found = col_data_cache->get(col_id, start_pack_id, pack_count, read_rows, col, dmfile->getColumnStat(col_id).type);
    col_data_cache->del(col_id, next_pack_id);
    return found;
}
} // namespace DM
} // namespace DB
