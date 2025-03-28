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

#include <Storages/Page/V3/CheckpointFile/CPWriteDataSource.h>

namespace DB::PS::V3
{

Page CPWriteDataSourceBlobStore::read(const BlobStore<universal::BlobStoreTrait>::PageIdAndEntry & page_id_and_entry)
{
    return blob_store.read(page_id_and_entry);
}

Page CPWriteDataSourceFixture::read(const BlobStore<universal::BlobStoreTrait>::PageIdAndEntry & id_and_entry)
{
    auto it = data.find(id_and_entry.second.offset);
    if (it == data.end())
        return Page::invalidPage();

    auto & value = it->second;

    Page page(1);
    page.mem_holder = nullptr;
    page.data = std::string_view(value);
    return page;
}

} // namespace DB::PS::V3
