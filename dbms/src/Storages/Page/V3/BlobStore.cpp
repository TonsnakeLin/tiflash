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

#include <Common/Checksum.h>
#include <Common/Exception.h>
#include <Common/FailPoint.h>
#include <Common/Logger.h>
#include <Common/ProfileEvents.h>
#include <Common/Stopwatch.h>
#include <Common/TiFlashMetrics.h>
#include <Common/formatReadable.h>
#include <Poco/File.h>
#include <Storages/Page/FileUsage.h>
#include <Storages/Page/V3/Blob/GCInfo.h>
#include <Storages/Page/V3/BlobStore.h>
#include <Storages/Page/V3/PageDefines.h>
#include <Storages/Page/V3/PageDirectory.h>
#include <Storages/Page/V3/PageEntriesEdit.h>
#include <Storages/Page/V3/PageEntry.h>
#include <Storages/Page/V3/Universal/UniversalWriteBatchImpl.h>
#include <Storages/Page/WriteBatchImpl.h>
#include <Storages/PathPool.h>
#include <boost_wrapper/string_split.h>
#include <common/logger_useful.h>

#include <ext/scope_guard.h>
#include <iterator>
#include <magic_enum.hpp>
#include <unordered_map>

namespace ProfileEvents
{
extern const Event PSMWritePages;
extern const Event PSMReadPages;
extern const Event PSV3MBlobExpansion;
extern const Event PSV3MBlobReused;
} // namespace ProfileEvents

namespace DB
{
namespace ErrorCodes
{
extern const int LOGICAL_ERROR;
extern const int CHECKSUM_DOESNT_MATCH;
} // namespace ErrorCodes

namespace FailPoints
{
extern const char force_change_all_blobs_to_read_only[];
} // namespace FailPoints

namespace PS::V3
{
static constexpr bool BLOBSTORE_CHECKSUM_ON_READ = true;

using BlobStatPtr = BlobStats::BlobStatPtr;
using ChecksumClass = Digest::CRC64;

/**********************
  * BlobStore methods *
  *********************/

template <typename Trait>
BlobStore<Trait>::BlobStore(const String & storage_name, const FileProviderPtr & file_provider_, PSDiskDelegatorPtr delegator_, const BlobConfig & config_)
    : delegator(std::move(delegator_))
    , file_provider(file_provider_)
    , config(config_)
    , log(Logger::get(storage_name))
    , blob_stats(log, delegator, config)
{
}

template <typename Trait>
void BlobStore<Trait>::registerPaths()
{
    for (const auto & path : delegator->listPaths())
    {
        Poco::File store_path(path);
        if (!store_path.exists())
        {
            continue;
        }

        std::vector<String> file_list;
        store_path.list(file_list);

        for (const auto & blob_name : file_list)
        {
            const auto & [blob_id, err_msg] = BlobStats::getBlobIdFromName(blob_name);
            if (blob_id != INVALID_BLOBFILE_ID)
            {
                auto lock_stats = blob_stats.lock();
                Poco::File blob(fmt::format("{}/{}", path, blob_name));
                auto blob_size = blob.getSize();
                delegator->addPageFileUsedSize({blob_id, 0}, blob_size, path, true);
                blob_stats.createStatNotChecking(blob_id,
                                                 std::max(blob_size, config.file_limit_size.get()),
                                                 lock_stats);
            }
            else
            {
                LOG_INFO(log, "Ignore not blob file [dir={}] [file={}] [err_msg={}]", path, blob_name, err_msg);
            }
        }
    }
}

template <typename Trait>
void BlobStore<Trait>::reloadConfig(const BlobConfig & rhs)
{
    // Currently, we don't add any config for `file_limit_size`, so it won't reload at run time.
    // And if we support it in the future(although it seems there is no need to do that),
    // it must be noted that if the `file_limit_size` is changed to a smaller value,
    // there may be some old BlobFile with size larger than new `file_limit_size` that can be used for rewrite
    // until it is changed to read only type by gc thread or tiflash is restarted.
    config.file_limit_size = rhs.file_limit_size;
    config.spacemap_type = rhs.spacemap_type;
    config.block_alignment_bytes = rhs.block_alignment_bytes;
    config.heavy_gc_valid_rate = rhs.heavy_gc_valid_rate;
}

template <typename Trait>
FileUsageStatistics BlobStore<Trait>::getFileUsageStatistics() const
{
    FileUsageStatistics usage;

    // Get a copy of stats map to avoid the big lock on stats map
    const auto stats_list = blob_stats.getStats();

    for (const auto & [path, stats] : stats_list)
    {
        (void)path;
        for (const auto & stat : stats)
        {
            // We can access to these type without any locking.
            if (stat->isReadOnly())
            {
                usage.total_disk_size += stat->sm_total_size;
                usage.total_valid_size += stat->sm_valid_size;
            }
            else
            {
                // Else the stat may being updated, acquire a lock to avoid data race.
                auto lock = stat->lock();
                usage.total_disk_size += stat->sm_total_size;
                usage.total_valid_size += stat->sm_valid_size;
            }
        }
        usage.total_file_num += stats.size();
    }

    return usage;
}

template <typename Trait>
typename BlobStore<Trait>::PageEntriesEdit
BlobStore<Trait>::handleLargeWrite(typename Trait::WriteBatch & wb, const WriteLimiterPtr & write_limiter)
{
    PageEntriesEdit edit;
    for (auto & write : wb.getMutWrites())
    {
        switch (write.type)
        {
        case WriteBatchWriteType::PUT:
        case WriteBatchWriteType::UPDATE_DATA_FROM_REMOTE:
        {
            ChecksumClass digest;
            PageEntryV3 entry;

            auto [blob_id, offset_in_file] = getPosFromStats(write.size);

            entry.file_id = blob_id;
            entry.size = write.size;
            entry.tag = write.tag;
            entry.offset = offset_in_file;
            // padding size won't work on big write batch
            entry.padded_size = 0;

            BufferBase::Buffer data_buf = write.read_buffer->buffer();

            digest.update(data_buf.begin(), write.size);
            entry.checksum = digest.checksum();

            UInt64 field_begin, field_end;

            for (size_t i = 0; i < write.offsets.size(); ++i)
            {
                ChecksumClass field_digest;
                field_begin = write.offsets[i].first;
                field_end = (i == write.offsets.size() - 1) ? write.size : write.offsets[i + 1].first;

                field_digest.update(data_buf.begin() + field_begin, field_end - field_begin);
                write.offsets[i].second = field_digest.checksum();
            }

            if (!write.offsets.empty())
            {
                // we can swap from WriteBatch instead of copying
                entry.field_offsets.swap(write.offsets);
            }

            try
            {
                auto blob_file = getBlobFile(blob_id);
                blob_file->write(data_buf.begin(), offset_in_file, write.size, write_limiter);
            }
            catch (DB::Exception & e)
            {
                removePosFromStats(blob_id, offset_in_file, write.size);
                LOG_ERROR(log, "[blob_id={}] [offset_in_file={}] [size={}] write failed.", blob_id, offset_in_file, write.size);
                throw e;
            }
            if (write.type == WriteBatchWriteType::PUT)
            {
                edit.put(wb.getFullPageId(write.page_id), entry);
            }
            else
            {
                edit.updateRemote(wb.getFullPageId(write.page_id), entry);
            }

            break;
        }
        case WriteBatchWriteType::PUT_REMOTE:
        {
            PageEntryV3 entry;
            entry.file_id = INVALID_BLOBFILE_ID;
            entry.tag = write.tag;
            entry.checkpoint_info = CheckpointInfo{
                .data_location = *write.data_location,
                .is_local_data_reclaimed = true,
            };
            if (!write.offsets.empty())
            {
                entry.field_offsets.swap(write.offsets);
            }
            edit.put(wb.getFullPageId(write.page_id), entry);
            break;
        }
        case WriteBatchWriteType::DEL:
        {
            edit.del(wb.getFullPageId(write.page_id));
            break;
        }
        case WriteBatchWriteType::REF:
        {
            edit.ref(wb.getFullPageId(write.page_id), wb.getFullPageId(write.ori_page_id));
            break;
        }
        case WriteBatchWriteType::PUT_EXTERNAL:
            edit.putExternal(wb.getFullPageId(write.page_id));
            break;
        case WriteBatchWriteType::UPSERT:
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Unknown write type: {}", magic_enum::enum_name(write.type));
            break;
        }
    }

    return edit;
}

template <typename Trait>
typename BlobStore<Trait>::PageEntriesEdit
BlobStore<Trait>::write(typename Trait::WriteBatch && wb, const WriteLimiterPtr & write_limiter)
{
    ProfileEvents::increment(ProfileEvents::PSMWritePages, wb.putWriteCount());

    const size_t all_page_data_size = wb.getTotalDataSize();

    PageEntriesEdit edit;

    if (all_page_data_size == 0)
    {
        // Shortcut for WriteBatch that don't need to persist blob data.
        for (auto & write : wb.getMutWrites())
        {
            switch (write.type)
            {
            case WriteBatchWriteType::PUT_REMOTE:
            {
                PageEntryV3 entry;
                entry.file_id = INVALID_BLOBFILE_ID;
                entry.tag = write.tag;
                entry.checkpoint_info = CheckpointInfo{
                    .data_location = *write.data_location,
                    .is_local_data_reclaimed = true,
                };
                if (!write.offsets.empty())
                {
                    entry.field_offsets.swap(write.offsets);
                }
                edit.put(wb.getFullPageId(write.page_id), entry);
                break;
            }
            case WriteBatchWriteType::DEL:
            {
                edit.del(wb.getFullPageId(write.page_id));
                break;
            }
            case WriteBatchWriteType::REF:
            {
                edit.ref(wb.getFullPageId(write.page_id), wb.getFullPageId(write.ori_page_id));
                break;
            }
            case WriteBatchWriteType::PUT_EXTERNAL:
            {
                // putExternal won't have data.
                edit.putExternal(wb.getFullPageId(write.page_id));
                break;
            }
            case WriteBatchWriteType::PUT:
            case WriteBatchWriteType::UPSERT:
            case WriteBatchWriteType::UPDATE_DATA_FROM_REMOTE:
                throw Exception(ErrorCodes::LOGICAL_ERROR, "write batch have a invalid total size == 0 while this kind of entry exist, write_type={}", magic_enum::enum_name(write.type));
                break;
            }
        }
        return edit;
    }

    GET_METRIC(tiflash_storage_page_write_batch_size).Observe(all_page_data_size);

    // If the WriteBatch is too big, we will split the Writes in the WriteBatch to different `BlobFile`.
    // This can avoid allocating a big buffer for writing data and can smooth memory usage.
    if (all_page_data_size > config.file_limit_size)
    {
        return handleLargeWrite(wb, write_limiter);
    }

    char * buffer = static_cast<char *>(alloc(all_page_data_size));
    SCOPE_EXIT({
        free(buffer, all_page_data_size);
    });
    char * buffer_pos = buffer;

    // Calculate alignment space
    size_t replenish_size = 0;
    if (config.block_alignment_bytes != 0 && all_page_data_size % config.block_alignment_bytes != 0)
    {
        replenish_size = config.block_alignment_bytes - all_page_data_size % config.block_alignment_bytes;
    }

    size_t actually_allocated_size = all_page_data_size + replenish_size;

    auto [blob_id, offset_in_file] = getPosFromStats(actually_allocated_size);

    size_t offset_in_allocated = 0;

    for (auto & write : wb.getMutWrites())
    {
        switch (write.type)
        {
        case WriteBatchWriteType::PUT:
        case WriteBatchWriteType::UPDATE_DATA_FROM_REMOTE:
        {
            ChecksumClass digest;
            PageEntryV3 entry;

            write.read_buffer->readStrict(buffer_pos, write.size);

            entry.file_id = blob_id;
            entry.size = write.size;
            entry.tag = write.tag;
            entry.offset = offset_in_file + offset_in_allocated;
            offset_in_allocated += write.size;

            // The last put write
            if (offset_in_allocated == all_page_data_size)
            {
                entry.padded_size = replenish_size;
            }

            digest.update(buffer_pos, write.size);
            entry.checksum = digest.checksum();

            UInt64 field_begin, field_end;

            for (size_t i = 0; i < write.offsets.size(); ++i)
            {
                ChecksumClass field_digest;
                field_begin = write.offsets[i].first;
                field_end = (i == write.offsets.size() - 1) ? write.size : write.offsets[i + 1].first;

                field_digest.update(buffer_pos + field_begin, field_end - field_begin);
                write.offsets[i].second = field_digest.checksum();
            }

            if (!write.offsets.empty())
            {
                // we can swap from WriteBatch instead of copying
                entry.field_offsets.swap(write.offsets);
            }

            buffer_pos += write.size;
            if (write.type == WriteBatchWriteType::PUT)
            {
                edit.put(wb.getFullPageId(write.page_id), entry);
            }
            else
            {
                edit.updateRemote(wb.getFullPageId(write.page_id), entry);
            }
            break;
        }
        case WriteBatchWriteType::PUT_REMOTE:
        {
            PageEntryV3 entry;
            entry.file_id = INVALID_BLOBFILE_ID;
            entry.tag = write.tag;
            entry.checkpoint_info = CheckpointInfo{
                .data_location = *write.data_location,
                .is_local_data_reclaimed = true,
            };
            if (!write.offsets.empty())
            {
                entry.field_offsets.swap(write.offsets);
            }
            edit.put(wb.getFullPageId(write.page_id), entry);
            break;
        }
        case WriteBatchWriteType::DEL:
        {
            edit.del(wb.getFullPageId(write.page_id));
            break;
        }
        case WriteBatchWriteType::REF:
        {
            edit.ref(wb.getFullPageId(write.page_id), wb.getFullPageId(write.ori_page_id));
            break;
        }
        case WriteBatchWriteType::PUT_EXTERNAL:
            edit.putExternal(wb.getFullPageId(write.page_id));
            break;
        case WriteBatchWriteType::UPSERT:
            throw Exception(fmt::format("Unknown write type: {}", magic_enum::enum_name(write.type)));
        }
    }

    if (buffer_pos != buffer + all_page_data_size)
    {
        removePosFromStats(blob_id, offset_in_file, actually_allocated_size);
        throw Exception(
            fmt::format(
                "write batch have a invalid total size, or something wrong in parse write batch "
                "[expect_offset={}] [actual_offset={}] [actually_allocated_size={}]",
                all_page_data_size,
                (buffer_pos - buffer),
                actually_allocated_size),
            ErrorCodes::LOGICAL_ERROR);
    }

    try
    {
        Stopwatch watch;
        SCOPE_EXIT({
            GET_METRIC(tiflash_storage_page_write_duration_seconds, type_blob_write).Observe(watch.elapsedSeconds());
        });
        auto blob_file = getBlobFile(blob_id);
        blob_file->write(buffer, offset_in_file, all_page_data_size, write_limiter);
    }
    catch (DB::Exception & e)
    {
        removePosFromStats(blob_id, offset_in_file, actually_allocated_size);
        LOG_ERROR(log, "[blob_id={}] [offset_in_file={}] [size={}] [actually_allocated_size={}] write failed [error={}]", blob_id, offset_in_file, all_page_data_size, actually_allocated_size, e.message());
        throw e;
    }

    return edit;
}

template <typename Trait>
void BlobStore<Trait>::remove(const PageEntries & del_entries)
{
    std::set<BlobFileId> blob_updated;
    for (const auto & entry : del_entries)
    {
        blob_updated.insert(entry.file_id);
        // External page size is 0
        if (entry.size == 0)
        {
            continue;
        }

        try
        {
            removePosFromStats(entry.file_id, entry.offset, entry.getTotalSize());
        }
        catch (DB::Exception & e)
        {
            e.addMessage(fmt::format("while removing entry [entry={}]", entry));
            e.rethrow();
        }
    }

    // After we remove position of blob, we need recalculate the blob.
    for (const auto & blob_id : blob_updated)
    {
        const auto & stat = blob_stats.blobIdToStat(blob_id,
                                                    /*ignore_not_exist*/ true);

        // Some of blob may been removed.
        // So if we can't use id find blob, just ignore it.
        if (stat)
        {
            {
                auto lock = stat->lock();
                stat->recalculateCapacity();
            }
            LOG_TRACE(log, "Blob recalculated capability [blob_id={}] [max_cap={}] "
                           "[total_size={}] [valid_size={}] [valid_rate={}]",
                      blob_id,
                      stat->sm_max_caps,
                      stat->sm_total_size,
                      stat->sm_valid_size,
                      stat->sm_valid_rate);
        }
    }
}

template <typename Trait>
std::pair<BlobFileId, BlobFileOffset> BlobStore<Trait>::getPosFromStats(size_t size)
{
    Stopwatch watch;
    BlobStatPtr stat;

    auto lock_stat = [size, this, &stat]() {
        auto lock_stats = blob_stats.lock();
        BlobFileId blob_file_id = INVALID_BLOBFILE_ID;
        std::tie(stat, blob_file_id) = blob_stats.chooseStat(size, lock_stats);
        if (stat == nullptr)
        {
            // No valid stat for putting data with `size`, create a new one
            stat = blob_stats.createStat(blob_file_id,
                                         std::max(size, config.file_limit_size.get()),
                                         lock_stats);
        }

        // We must get the lock from BlobStat under the BlobStats lock
        // to ensure that BlobStat updates are serialized.
        // Otherwise it may cause stat to fail to get the span for writing
        // and throwing exception.
        return stat->lock();
    }();
    GET_METRIC(tiflash_storage_page_write_duration_seconds, type_choose_stat).Observe(watch.elapsedSeconds());
    watch.restart();
    SCOPE_EXIT({
        GET_METRIC(tiflash_storage_page_write_duration_seconds, type_search_pos).Observe(watch.elapsedSeconds());
    });

    // We need to assume that this insert will reduce max_cap.
    // Because other threads may also be waiting for BlobStats to chooseStat during this time.
    // If max_cap is not reduced, it may cause the same BlobStat to accept multiple buffers and exceed its max_cap.
    // After the BlobStore records the buffer size, max_caps will also get an accurate update.
    // So there won't get problem in reducing max_caps here.
    auto old_max_cap = stat->sm_max_caps;
    assert(stat->sm_max_caps >= size);
    stat->sm_max_caps -= size;

    // Get Position from single stat
    BlobFileOffset offset = stat->getPosFromStat(size, lock_stat);

    // Can't insert into this spacemap
    if (offset == INVALID_BLOBFILE_OFFSET)
    {
        stat->smap->logDebugString();
        throw Exception(fmt::format("Get postion from BlobStat failed, it may caused by `sm_max_caps` is no correct. [size={}] [old_max_caps={}] [max_caps={}] [blob_id={}]",
                                    size,
                                    old_max_cap,
                                    stat->sm_max_caps,
                                    stat->id),
                        ErrorCodes::LOGICAL_ERROR);
    }

    return std::make_pair(stat->id, offset);
}

template <typename Trait>
void BlobStore<Trait>::removePosFromStats(BlobFileId blob_id, BlobFileOffset offset, size_t size)
{
    const auto & stat = blob_stats.blobIdToStat(blob_id);
    {
        auto lock = stat->lock();
        auto remaining_valid_size = stat->removePosFromStat(offset, size, lock);
        if (bool remove_file_on_disk = stat->isReadOnly() && (remaining_valid_size == 0);
            !remove_file_on_disk)
        {
            return;
        }
        // BlobFile which is read-only won't be reused for another writing,
        // so it's safe and necessary to remove it from disk.
    }


    // Note that we must release the lock on blob_stat before removing it
    // from all blob_stats, or deadlocks could happen.
    // As the blob_stat has been became read-only, it is safe to release the lock.
    LOG_INFO(log, "Removing BlobFile [blob_id={}]", blob_id);

    {
        // Remove the stat from memory
        auto lock_stats = blob_stats.lock();
        blob_stats.eraseStat(std::move(stat), lock_stats);
    }
    {
        // Remove the blob file from disk and memory
        std::lock_guard files_gurad(mtx_blob_files);
        if (auto iter = blob_files.find(blob_id);
            iter != blob_files.end())
        {
            auto blob_file = iter->second;
            blob_file->remove();
            blob_files.erase(iter);
        }
        // If the blob_id does not exist, the blob_file is never
        // opened for read/write. It is safe to ignore it.
    }
}

template <typename Trait>
typename BlobStore<Trait>::PageMap
BlobStore<Trait>::read(FieldReadInfos & to_read, const ReadLimiterPtr & read_limiter)
{
    if (to_read.empty())
    {
        return {};
    }

    ProfileEvents::increment(ProfileEvents::PSMReadPages, to_read.size());

    // Sort in ascending order by offset in file.
    std::sort(
        to_read.begin(),
        to_read.end(),
        [](const FieldReadInfo & a, const FieldReadInfo & b) { return a.entry.offset < b.entry.offset; });

    // allocate data_buf that can hold all pages with specify fields

    size_t buf_size = 0;
    for (auto & [page_id, entry, fields] : to_read)
    {
        (void)page_id;
        // Sort fields to get better read on disk
        std::sort(fields.begin(), fields.end());
        for (const auto field_index : fields)
        {
            buf_size += entry.getFieldSize(field_index);
        }
    }

    PageMap page_map;
    if (unlikely(buf_size == 0))
    {
        // We should never persist an empty column inside a block. If the buf size is 0
        // then this read with `FieldReadInfos` could be completely eliminated in the upper
        // layer. Log a warning to check if it happens.
        {
            FmtBuffer buf;
            buf.joinStr(
                to_read.begin(),
                to_read.end(),
                [](const FieldReadInfo & info, FmtBuffer & fb) {
                    fb.fmtAppend("{{page_id: {}, fields: {}, entry: {}}}", info.page_id, info.fields, info.entry);
                },
                ",");
#ifndef NDEBUG
            // throw an exception under debug mode so we should change the upper layer logic
            throw Exception(fmt::format("Reading with fields but entry size is 0, read_info=[{}]", buf.toString()), ErrorCodes::LOGICAL_ERROR);
#endif
            // Log a warning under production release
            LOG_WARNING(log, "Reading with fields but entry size is 0, read_info=[{}]", buf.toString());
        }

        // Allocating buffer with size == 0 could lead to unexpected behavior, skip the allocating and return
        for (const auto & [page_id, entry, fields] : to_read)
        {
            UNUSED(entry, fields);
            Page page(Trait::PageIdTrait::getU64ID(page_id));
            page.data = std::string_view(nullptr, 0);
            page_map.emplace(Trait::PageIdTrait::getPageMapKey(page_id), std::move(page));
        }
        return page_map;
    }


    // Allocate one for holding all pages data
    char * shared_data_buf = static_cast<char *>(alloc(buf_size));
    MemHolder shared_mem_holder = createMemHolder(shared_data_buf, [&, buf_size](char * p) {
        free(p, buf_size);
    });

    std::set<FieldOffsetInsidePage> fields_offset_in_page;
    char * pos = shared_data_buf;
    for (const auto & [page_id_v3, entry, fields] : to_read)
    {
        size_t read_size_this_entry = 0;
        char * write_offset = pos;
        for (const auto field_index : fields)
        {
            // TODO: Continuously fields can read by one system call.
            const auto [beg_offset, end_offset] = entry.getFieldOffsets(field_index);
            const auto size_to_read = end_offset - beg_offset;
            auto blob_file = read(page_id_v3, entry.file_id, entry.offset + beg_offset, write_offset, size_to_read, read_limiter);
            fields_offset_in_page.emplace(field_index, read_size_this_entry);

            if constexpr (BLOBSTORE_CHECKSUM_ON_READ)
            {
                const auto expect_checksum = entry.field_offsets[field_index].second;
                ChecksumClass digest;
                digest.update(write_offset, size_to_read);
                auto field_checksum = digest.checksum();
                if (unlikely(entry.size != 0 && field_checksum != expect_checksum))
                {
                    throw Exception(
                        fmt::format("Reading with fields meet checksum not match "
                                    "[page_id={}] [expected=0x{:X}] [actual=0x{:X}] "
                                    "[field_index={}] [field_offset={}] [field_size={}] "
                                    "[entry={}] [file={}]",
                                    page_id_v3,
                                    expect_checksum,
                                    field_checksum,
                                    field_index,
                                    beg_offset,
                                    size_to_read,
                                    entry,
                                    blob_file->getPath()),
                        ErrorCodes::CHECKSUM_DOESNT_MATCH);
                }
            }

            read_size_this_entry += size_to_read;
            write_offset += size_to_read;
        }

        Page page(Trait::PageIdTrait::getU64ID(page_id_v3));
        RUNTIME_CHECK(write_offset >= pos);
        page.data = std::string_view(pos, write_offset - pos);
        page.mem_holder = shared_mem_holder;
        page.field_offsets.swap(fields_offset_in_page);
        fields_offset_in_page.clear();
        page_map.emplace(Trait::PageIdTrait::getPageMapKey(page_id_v3), std::move(page));

        pos = write_offset;
    }

    if (unlikely(pos != shared_data_buf + buf_size))
    {
        FmtBuffer buf;
        buf.joinStr(
            to_read.begin(),
            to_read.end(),
            [](const FieldReadInfo & info, FmtBuffer & fb) {
                fb.fmtAppend("{{page_id: {}, fields: {}, entry: {}}}", info.page_id, info.fields, info.entry);
            },
            ",");
        throw Exception(fmt::format("unexpected read size, end_pos={} current_pos={} read_info=[{}]",
                                    shared_data_buf + buf_size,
                                    pos,
                                    buf.toString()),
                        ErrorCodes::LOGICAL_ERROR);
    }
    return page_map;
}

template <typename Trait>
typename BlobStore<Trait>::PageMap
BlobStore<Trait>::read(PageIdAndEntries & entries, const ReadLimiterPtr & read_limiter)
{
    if (entries.empty())
    {
        return {};
    }

    ProfileEvents::increment(ProfileEvents::PSMReadPages, entries.size());

    // Sort in ascending order by offset in file.
    std::sort(entries.begin(), entries.end(), [](const auto & a, const auto & b) {
        return a.second.offset < b.second.offset;
    });

    // allocate data_buf that can hold all pages
    size_t buf_size = 0;
    for (const auto & p : entries)
    {
        buf_size += p.second.size;
    }

    // When we read `WriteBatch` which is `WriteType::PUT_EXTERNAL`.
    // The `buf_size` will be 0, we need avoid calling malloc/free with size 0.
    if (buf_size == 0)
    {
        PageMap page_map;
        for (const auto & [page_id_v3, entry] : entries)
        {
            // Unexpected behavior but do no harm
            LOG_INFO(log, "Read entry without entry size, page_id={} entry={}", page_id_v3, entry);
            Page page(Trait::PageIdTrait::getU64ID(page_id_v3));
            page_map.emplace(Trait::PageIdTrait::getPageMapKey(page_id_v3), page);
        }
        return page_map;
    }

    char * data_buf = static_cast<char *>(alloc(buf_size));
    MemHolder mem_holder = createMemHolder(data_buf, [&, buf_size](char * p) {
        free(p, buf_size);
    });

    char * pos = data_buf;
    PageMap page_map;
    for (const auto & [page_id_v3, entry] : entries)
    {
        auto blob_file = read(page_id_v3, entry.file_id, entry.offset, pos, entry.size, read_limiter);

        if constexpr (BLOBSTORE_CHECKSUM_ON_READ)
        {
            ChecksumClass digest;
            digest.update(pos, entry.size);
            auto checksum = digest.checksum();
            if (unlikely(entry.size != 0 && checksum != entry.checksum))
            {
                throw Exception(
                    fmt::format("Reading with entries meet checksum not match [page_id={}] [expected=0x{:X}] [actual=0x{:X}] [entry={}] [file={}]",
                                page_id_v3,
                                entry.checksum,
                                checksum,
                                entry,
                                blob_file->getPath()),
                    ErrorCodes::CHECKSUM_DOESNT_MATCH);
            }
        }

        Page page(Trait::PageIdTrait::getU64ID(page_id_v3));
        page.data = std::string_view(pos, entry.size);
        page.mem_holder = mem_holder;

        // Calculate the field_offsets from page entry
        for (size_t index = 0; index < entry.field_offsets.size(); index++)
        {
            const auto offset = entry.field_offsets[index].first;
            page.field_offsets.emplace(index, offset);
        }

        page_map.emplace(Trait::PageIdTrait::getPageMapKey(page_id_v3), std::move(page));

        pos += entry.size;
    }

    if (unlikely(pos != data_buf + buf_size))
    {
        FmtBuffer buf;
        buf.joinStr(
            entries.begin(),
            entries.end(),
            [](const PageIdAndEntry & id_entry, FmtBuffer & fb) {
                fb.fmtAppend("{{page_id: {}, entry: {}}}", id_entry.first, id_entry.second);
            },
            ",");
        throw Exception(fmt::format("unexpected read size, end_pos={} current_pos={} read_info=[{}]",
                                    data_buf + buf_size,
                                    pos,
                                    buf.toString()),
                        ErrorCodes::LOGICAL_ERROR);
    }

    return page_map;
}

template <typename Trait>
Page BlobStore<Trait>::read(const PageIdAndEntry & id_entry, const ReadLimiterPtr & read_limiter)
{
    const auto & [page_id_v3, entry] = id_entry;
    const size_t buf_size = entry.size;

    if (!entry.isValid())
    {
        return Page::invalidPage();
    }

    // When we read `WriteBatch` which is `WriteType::PUT_EXTERNAL`.
    // The `buf_size` will be 0, we need avoid calling malloc/free with size 0.
    if (buf_size == 0)
    {
        // Unexpected behavior but do no harm
        LOG_INFO(log, "Read entry without entry size, page_id={} entry={}", page_id_v3, entry);
        Page page(Trait::PageIdTrait::getU64ID(page_id_v3));
        return page;
    }

    char * data_buf = static_cast<char *>(alloc(buf_size));
    MemHolder mem_holder = createMemHolder(data_buf, [&, buf_size](char * p) {
        free(p, buf_size);
    });

    auto blob_file = read(page_id_v3, entry.file_id, entry.offset, data_buf, buf_size, read_limiter);
    if constexpr (BLOBSTORE_CHECKSUM_ON_READ)
    {
        ChecksumClass digest;
        digest.update(data_buf, entry.size);
        auto checksum = digest.checksum();
        if (unlikely(entry.size != 0 && checksum != entry.checksum))
        {
            throw Exception(
                fmt::format("Reading with entries meet checksum not match [page_id={}] [expected=0x{:X}] [actual=0x{:X}] [entry={}] [file={}]",
                            page_id_v3,
                            entry.checksum,
                            checksum,
                            entry,
                            blob_file->getPath()),
                ErrorCodes::CHECKSUM_DOESNT_MATCH);
        }
    }

    Page page(Trait::PageIdTrait::getU64ID(page_id_v3));
    page.data = std::string_view(data_buf, buf_size);
    page.mem_holder = mem_holder;

    // Calculate the field_offsets from page entry
    for (size_t index = 0; index < entry.field_offsets.size(); index++)
    {
        const auto offset = entry.field_offsets[index].first;
        page.field_offsets.emplace(index, offset);
    }

    return page;
}

template <typename Trait>
BlobFilePtr BlobStore<Trait>::read(const typename BlobStore<Trait>::PageId & page_id_v3, BlobFileId blob_id, BlobFileOffset offset, char * buffers, size_t size, const ReadLimiterPtr & read_limiter, bool background)
{
    assert(buffers != nullptr);
    BlobFilePtr blob_file = getBlobFile(blob_id);
    try
    {
        blob_file->read(buffers, offset, size, read_limiter, background);
    }
    catch (DB::Exception & e)
    {
        // add debug message
        e.addMessage(fmt::format("(error while reading page data [page_id={}] [blob_id={}] [offset={}] [size={}] [background={}])", page_id_v3, blob_id, offset, size, background));
        e.rethrow();
    }
    return blob_file;
}


template <typename Trait>
std::vector<BlobFileId> BlobStore<Trait>::getGCStats()
{
    // Get a copy of stats map to avoid the big lock on stats map
    const auto stats_list = blob_stats.getStats();
    std::vector<BlobFileId> blob_need_gc;
    BlobStoreGCInfo blobstore_gc_info;

    fiu_do_on(FailPoints::force_change_all_blobs_to_read_only,
              {
                  for (const auto & [path, stats] : stats_list)
                  {
                      (void)path;
                      for (const auto & stat : stats)
                      {
                          stat->changeToReadOnly();
                      }
                  }
                  LOG_WARNING(log, "enabled force_change_all_blobs_to_read_only. All of BlobStat turn to READ-ONLY");
              });

    for (const auto & [path, stats] : stats_list)
    {
        (void)path;
        for (const auto & stat : stats)
        {
            if (stat->isReadOnly())
            {
                blobstore_gc_info.appendToReadOnlyBlob(stat->id, stat->sm_valid_rate);
                LOG_TRACE(log, "Current [blob_id={}] is read-only", stat->id);
                continue;
            }

            auto lock = stat->lock();
            auto right_boundary = stat->smap->getUsedBoundary();

            // Avoid divide by zero
            if (right_boundary == 0)
            {
                // Note `stat->sm_total_size` isn't strictly the same as the actual size of underlying BlobFile after restart tiflash,
                // because some entry may be deleted but the actual disk space is not reclaimed in previous run.
                // TODO: avoid always truncate on empty BlobFile
                RUNTIME_CHECK_MSG(stat->sm_valid_size == 0, "Current blob is empty, but valid size is not 0. [blob_id={}] [valid_size={}] [valid_rate={}]", stat->id, stat->sm_valid_size, stat->sm_valid_rate);

                // If current blob empty, the size of in disk blob may not empty
                // So we need truncate current blob, and let it be reused.
                auto blobfile = getBlobFile(stat->id);
                LOG_INFO(log, "Current blob file is empty, truncated to zero [blob_id={}] [total_size={}] [valid_rate={}]", stat->id, stat->sm_total_size, stat->sm_valid_rate);
                blobfile->truncate(right_boundary);
                blobstore_gc_info.appendToTruncatedBlob(stat->id, stat->sm_total_size, right_boundary, stat->sm_valid_rate);
                stat->sm_total_size = right_boundary;
                continue;
            }

            stat->sm_valid_rate = stat->sm_valid_size * 1.0 / right_boundary;

            if (stat->sm_valid_rate > 1.0)
            {
                LOG_ERROR(
                    log,
                    "Current blob got an invalid rate {:.2f}, total size is {}, valid size is {}, right boundary is {} [blob_id={}]",
                    stat->sm_valid_rate,
                    stat->sm_total_size,
                    stat->sm_valid_size,
                    right_boundary,
                    stat->id);
                assert(false);
                continue;
            }

            // Check if GC is required
            if (stat->sm_valid_rate <= config.heavy_gc_valid_rate)
            {
                LOG_TRACE(log, "Current [blob_id={}] valid rate is {:.2f}, full GC", stat->id, stat->sm_valid_rate);
                blob_need_gc.emplace_back(stat->id);

                // Change current stat to read only
                stat->changeToReadOnly();
                blobstore_gc_info.appendToNeedGCBlob(stat->id, stat->sm_valid_rate);
            }
            else
            {
                blobstore_gc_info.appendToNoNeedGCBlob(stat->id, stat->sm_valid_rate);
                LOG_TRACE(log, "Current [blob_id={}] valid rate is {:.2f}, unchange", stat->id, stat->sm_valid_rate);
            }

            if (right_boundary != stat->sm_total_size)
            {
                auto blobfile = getBlobFile(stat->id);
                LOG_TRACE(log, "Truncate blob file [blob_id={}] [origin size={}] [truncated size={}]", stat->id, stat->sm_total_size, right_boundary);
                blobfile->truncate(right_boundary);
                blobstore_gc_info.appendToTruncatedBlob(stat->id, stat->sm_total_size, right_boundary, stat->sm_valid_rate);

                stat->sm_total_size = right_boundary;
                stat->sm_valid_rate = stat->sm_valid_size * 1.0 / stat->sm_total_size;
            }
        }
    }

    LOG_IMPL(log, blobstore_gc_info.getLoggingLevel(), "BlobStore gc get status done. blob_ids details {}", blobstore_gc_info.toString());

    return blob_need_gc;
}

template <typename Trait>
typename BlobStore<Trait>::PageEntriesEdit
BlobStore<Trait>::gc(GcEntriesMap & entries_need_gc,
                     const PageSize & total_page_size,
                     const WriteLimiterPtr & write_limiter,
                     const ReadLimiterPtr & read_limiter)
{
    std::vector<std::tuple<BlobFileId, BlobFileOffset, PageSize>> written_blobs;
    PageEntriesEdit edit;

    if (total_page_size == 0)
    {
        throw Exception("BlobStore can't do gc if nothing need gc.", ErrorCodes::LOGICAL_ERROR);
    }
    LOG_INFO(log, "BlobStore gc will migrate {} into new blob files", formatReadableSizeWithBinarySuffix(total_page_size));

    auto write_blob = [this, total_page_size, &written_blobs, &write_limiter](const BlobFileId & file_id,
                                                                              char * data_begin,
                                                                              const BlobFileOffset & file_offset,
                                                                              const PageSize & data_size) {
        try
        {
            auto blob_file = getBlobFile(file_id);
            // Should append before calling BlobStore::write, so that we can rollback the
            // first allocated span from stats.
            written_blobs.emplace_back(file_id, file_offset, data_size);
            LOG_INFO(
                log,
                "BlobStore gc write (partially) done [blob_id={}] [file_offset={}] [size={}] [total_size={}]",
                file_id,
                file_offset,
                data_size,
                total_page_size);
            blob_file->write(data_begin, file_offset, data_size, write_limiter, /*background*/ true);
        }
        catch (DB::Exception & e)
        {
            LOG_ERROR(
                log,
                "BlobStore gc write failed [blob_id={}] [offset={}] [size={}] [total_size={}]",
                file_id,
                file_offset,
                data_size,
                total_page_size);
            for (const auto & [blobfile_id_revert, file_offset_beg_revert, page_size_revert] : written_blobs)
            {
                removePosFromStats(blobfile_id_revert, file_offset_beg_revert, page_size_revert);
            }
            throw e;
        }
    };

    auto alloc_size = config.file_limit_size.get();
    // If `total_page_size` is greater than `config_file_limit`, we will try to write the page data into multiple `BlobFile`s to
    // make the memory consumption smooth during GC.
    if (total_page_size > alloc_size)
    {
        size_t biggest_page_size = 0;
        for (const auto & [file_id, versioned_pageid_entry_list] : entries_need_gc)
        {
            (void)file_id;
            for (const auto & [page_id, version, entry] : versioned_pageid_entry_list)
            {
                (void)page_id;
                (void)version;
                biggest_page_size = std::max(biggest_page_size, entry.size);
            }
        }
        alloc_size = std::max(alloc_size, biggest_page_size);
    }
    else
    {
        alloc_size = total_page_size;
    }

    BlobFileOffset remaining_page_size = total_page_size - alloc_size;

    char * data_buf = static_cast<char *>(alloc(alloc_size));
    SCOPE_EXIT({
        free(data_buf, alloc_size);
    });

    char * data_pos = data_buf;
    BlobFileOffset offset_in_data = 0;
    BlobFileId blobfile_id;
    BlobFileOffset file_offset_begin;
    std::tie(blobfile_id, file_offset_begin) = getPosFromStats(alloc_size);

    // blob_file_0, [<page_id_0, ver0, entry0>,
    //               <page_id_0, ver1, entry1>,
    //               <page_id_1, ver1, entry1>, ... ]
    // blob_file_1, [...]
    // ...
    for (const auto & [file_id, versioned_pageid_entry_list] : entries_need_gc)
    {
        for (const auto & [page_id, version, entry] : versioned_pageid_entry_list)
        {
            /// If `total_page_size` is greater than `config_file_limit`, we need to write the page data into multiple `BlobFile`s.
            /// So there may be some page entry that cannot be fit into the current blob file, and we need to write it into the next one.
            /// And we need perform the following steps before writing data into the current blob file:
            ///   1. reclaim unneeded space allocated from current blob stat if `offset_in_data` < `alloc_size`;
            ///   2. update `remaining_page_size`;
            /// After writing data into the current blob file, we reuse the original buffer for future write.
            if (offset_in_data + entry.size > alloc_size)
            {
                assert(file_offset_begin == 0);
                // Remove the span that is not actually used
                if (offset_in_data != alloc_size)
                {
                    removePosFromStats(blobfile_id, offset_in_data, alloc_size - offset_in_data);
                }
                remaining_page_size += alloc_size - offset_in_data;

                // Write data into Blob.
                write_blob(blobfile_id, data_buf, file_offset_begin, offset_in_data);

                // Reset the position to reuse the buffer allocated
                data_pos = data_buf;
                offset_in_data = 0;

                // Acquire a span from stats for remaining data
                auto next_alloc_size = (remaining_page_size > alloc_size ? alloc_size : remaining_page_size);
                remaining_page_size -= next_alloc_size;
                std::tie(blobfile_id, file_offset_begin) = getPosFromStats(next_alloc_size);
            }
            assert(offset_in_data + entry.size <= alloc_size);

            // Read the data into buffer by old entry
            read(page_id, file_id, entry.offset, data_pos, entry.size, read_limiter, /*background*/ true);

            // Most vars of the entry is not changed, but the file id and offset
            // need to be updated.
            PageEntryV3 new_entry = entry;
            new_entry.file_id = blobfile_id;
            new_entry.offset = file_offset_begin + offset_in_data;
            new_entry.padded_size = 0; // reset padded size to be zero

            offset_in_data += new_entry.size;
            data_pos += new_entry.size;

            edit.upsertPage(page_id, version, new_entry);
        }
    }

    // write remaining data in `data_buf` into BlobFile
    if (offset_in_data != 0)
    {
        write_blob(blobfile_id, data_buf, file_offset_begin, offset_in_data);
    }

    return edit;
}

template <typename Trait>
String BlobStore<Trait>::getBlobFileParentPath(BlobFileId blob_id)
{
    PageFileIdAndLevel id_lvl{blob_id, 0};
    String parent_path = delegator->getPageFilePath(id_lvl);

    if (auto f = Poco::File(parent_path); !f.exists())
        f.createDirectories();

    return parent_path;
}

template <typename Trait>
BlobFilePtr BlobStore<Trait>::getBlobFile(BlobFileId blob_id)
{
    std::lock_guard files_gurad(mtx_blob_files);
    if (auto iter = blob_files.find(blob_id); iter != blob_files.end())
        return iter->second;
    auto file = std::make_shared<BlobFile>(getBlobFileParentPath(blob_id), blob_id, file_provider, delegator);
    blob_files.emplace(blob_id, file);
    return file;
}

template class BlobStore<u128::BlobStoreTrait>;
template class BlobStore<universal::BlobStoreTrait>;
} // namespace PS::V3
} // namespace DB
