// Copyright (c) 2011, Cornell University
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//     * Redistributions of source code must retain the above copyright notice,
//       this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of HyperDex nor the names of its contributors may be
//       used to endorse or promote products derived from this software without
//       specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#define __STDC_LIMIT_MACROS

// C
#include <cstdio>

// POSIX
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// po6
#include <po6/io/fd.h>

// HyperspaceHashing
#include <hyperspacehashing/mask.h>

// HyperDisk
#include "shard.h"
#include "shard_constants.h"
#include "shard_snapshot.h"

using hyperspacehashing::mask::coordinate;

e::intrusive_ptr<hyperdisk::shard>
hyperdisk :: shard :: create(const po6::io::fd& base,
                             const po6::pathname& filename)
{
    // Try removing the old shard.
    unlinkat(base.get(), filename.get(), 0);
    po6::io::fd fd(openat(base.get(), filename.get(), O_CREAT|O_EXCL|O_RDWR, S_IRWXU));

    if (fd.get() < 0)
    {
        throw po6::error(errno);
    }

    std::vector<char> buf(1 << 20, '\0');
    std::vector<iovec> iovs;
    size_t rem = FILE_SIZE;

    while (rem)
    {
        struct iovec iov;
        iov.iov_base = &buf.front();
        iov.iov_len = std::min(buf.size(), rem);
        iovs.push_back(iov);
        rem -= iov.iov_len;
    }

    if (writev(fd.get(), &iovs.front(), iovs.size()) != FILE_SIZE)
    {
        throw po6::error(errno);
    }

    if (fsync(fd.get()) < 0)
    {
        throw po6::error(errno);
    }

    // Create the shard object.
    e::intrusive_ptr<shard> ret = new shard(&fd);
    return ret;
}

hyperdisk::returncode
hyperdisk :: shard :: get(uint32_t primary_hash,
                          const e::buffer& key,
                          std::vector<e::buffer>* value,
                          uint64_t* version)
{
    // Find the bucket.
    size_t table_entry;
    uint64_t table_value;
    hash_lookup(primary_hash, key, &table_entry, &table_value);
    uint32_t table_hash = static_cast<uint32_t>(table_value);
    uint32_t table_offset = static_cast<uint32_t>(table_value >> 32);

    if (table_offset == 0 || table_offset >= HASH_OFFSET_INVALID)
    {
        return NOTFOUND;
    }

    // Load the information.
    *version = data_version(table_offset);
    // const size_t key_size = data_key_size(offset);
    // data_key(offset, &key);
    // ^ Skipped because hash_lookup ensures that the key matches.
    data_value(table_offset, key.size(), value);
    return SUCCESS;
}

hyperdisk::returncode
hyperdisk :: shard :: put(uint32_t primary_hash,
                          uint32_t secondary_hash,
                          const e::buffer& key,
                          const std::vector<e::buffer>& value,
                          uint64_t version)
{
    if (data_size(key, value) + m_data_offset > FILE_SIZE)
    {
        return DATAFULL;
    }

    if (m_search_offset == SEARCH_INDEX_ENTRIES)
    {
        return SEARCHFULL;
    }

    // Find the bucket.
    size_t entry;
    uint64_t table_value;
    hash_lookup(primary_hash, key, &entry, &table_value);
    uint32_t table_offset = static_cast<uint32_t>(table_value >> 32);

    // Values to pack.
    uint32_t key_size = key.size();
    uint16_t value_arity = value.size();

    // Pack the values on disk.
    uint32_t curr_offset = m_data_offset;
    memmove(m_data + curr_offset, &version, sizeof(version));
    curr_offset += sizeof(version);
    memmove(m_data + curr_offset, &key_size, sizeof(key_size));
    curr_offset += sizeof(key_size);
    memmove(m_data + curr_offset, key.get(), key.size());
    curr_offset += key.size();
    memmove(m_data + curr_offset, &value_arity, sizeof(value_arity));
    curr_offset += sizeof(value_arity);

    for (size_t i = 0; i < value.size(); ++i)
    {
        uint32_t size = value[i].size();
        memmove(m_data + curr_offset, &size, sizeof(size));
        curr_offset += sizeof(size);
        memmove(m_data + curr_offset, value[i].get(), value[i].size());
        curr_offset += value[i].size();
    }

    // Invalidate anything pointing to the old version.
    if (table_offset < HASH_OFFSET_INVALID)
    {
        invalidate_search_log(table_offset, m_data_offset);
    }

    // Insert into the search log.
    m_search_log[m_search_offset * 2] = (static_cast<uint64_t>(secondary_hash) << 32)
                                      | static_cast<uint64_t>(primary_hash);
    m_search_log[m_search_offset * 2 + 1] = static_cast<uint64_t>(m_data_offset);

    // Insert into the hash table.
    m_hash_table[entry] = (static_cast<uint64_t>(m_data_offset) << 32)
                        | static_cast<uint64_t>(primary_hash);

    // Update the offsets
    ++m_search_offset;
    m_data_offset = (curr_offset + 7) & ~7; // Keep everything 8-byte aligned.
    return SUCCESS;
}

hyperdisk::returncode
hyperdisk :: shard :: del(uint32_t primary_hash,
                          const e::buffer& key)
{
    size_t table_entry;
    uint64_t table_value;
    hash_lookup(primary_hash, key, &table_entry, &table_value);
    uint32_t table_hash = static_cast<uint32_t>(table_value);
    uint32_t table_offset = static_cast<uint32_t>(table_value >> 32);

    if (table_offset == 0 || table_offset >= HASH_OFFSET_INVALID)
    {
        return NOTFOUND;
    }

    if (m_data_offset + sizeof(uint64_t) > FILE_SIZE)
    {
        return DATAFULL;
    }

    invalidate_search_log(table_offset, m_data_offset);
    m_data_offset += sizeof(uint64_t);
    m_hash_table[table_entry] = (static_cast<uint64_t>(table_offset) << 32)
                              | (static_cast<uint64_t>(HASH_OFFSET_INVALID) << 32)
                              | static_cast<uint64_t>(primary_hash);
    return SUCCESS;
}

int
hyperdisk :: shard :: stale_space() const
{
    size_t stale_data = 0;
    size_t stale_num = 0;
    uint32_t start = static_cast<uint32_t>(m_search_log[1]);
    uint32_t end;
    size_t i;

    for (i = 1; i < SEARCH_INDEX_ENTRIES; ++i)
    {
        end = static_cast<uint32_t>(m_search_log[2 * i + 1]);

        if (end == 0)
        {
            end = m_data_offset;
            break;
        }

        if (static_cast<uint32_t>(m_search_log[2 * i + 1] >> 32) > 0)
        {
            stale_data += end - start;
            ++stale_num;
        }

        start = end;
    }

    if (i == SEARCH_INDEX_ENTRIES)
    {
        end = m_data_offset;
    }

    stale_data += end - start;
    stale_num += (end - start) ? 1 : 0;

    double data = 100.0 * static_cast<double>(stale_data) / DATA_SEGMENT_SIZE;
    double num = 100.0 * static_cast<double>(stale_num) / SEARCH_INDEX_ENTRIES;
    return std::max(data, num);
}

int
hyperdisk :: shard :: used_space() const
{
    double data = 100 * static_cast<double>(m_data_offset - INDEX_SEGMENT_SIZE)
                        / DATA_SEGMENT_SIZE;
    double num = 100 * static_cast<double>(m_search_offset) / SEARCH_INDEX_ENTRIES;
    return std::max(data, num);
}

hyperdisk::returncode
hyperdisk :: shard :: async()
{
    if (msync(m_data, FILE_SIZE, MS_ASYNC) < 0)
    {
        return SYNCFAILED;
    }

    return SUCCESS;
}

hyperdisk::returncode
hyperdisk :: shard :: sync()
{
    if (msync(m_data, FILE_SIZE, MS_SYNC) < 0)
    {
        return SYNCFAILED;
    }

    return SUCCESS;
}

e::intrusive_ptr<hyperdisk::shard_snapshot>
hyperdisk :: shard :: make_snapshot()
{
    e::intrusive_ptr<shard> d = this;
    assert(m_ref >= 2); // LCOV_EXCL_LINE
    e::intrusive_ptr<shard_snapshot> ret = new shard_snapshot(d);
    return ret;
}

void
hyperdisk :: shard :: copy_to(const coordinate& c, e::intrusive_ptr<shard> s)
{
    assert(m_data != s->m_data); // LCOV_EXCL_LINE
    memset(s->m_hash_table, 0, HASH_TABLE_SIZE);
    memset(s->m_search_log, 0, SEARCH_INDEX_SIZE);
    s->m_data_offset = INDEX_SEGMENT_SIZE;
    s->m_search_offset = 0;

    for (size_t ent = 0; ent < SEARCH_INDEX_ENTRIES; ++ent)
    {
        // Skip stale entries.
        if (static_cast<uint32_t>(m_search_log[ent * 2 + 1] >> 32) != 0)
        {
            continue;
        }

        uint32_t primary_hash = static_cast<uint32_t>(m_search_log[ent * 2]);
        uint32_t secondary_hash = static_cast<uint32_t>(m_search_log[ent * 2] >> 32);

        if (!c.intersects(coordinate(UINT32_MAX, primary_hash, UINT32_MAX, secondary_hash)))
        {
            continue;
        }

        // Figure out how big the entry is.
        uint32_t entry_start = static_cast<uint32_t>(m_search_log[ent * 2 + 1]);

        if (entry_start == 0)
        {
            break;
        }

        uint32_t entry_end = 0;

        if (ent < SEARCH_INDEX_ENTRIES - 1 && m_search_log[(ent + 1) * 2 + 1])
        {
            entry_end = static_cast<uint32_t>(m_search_log[(ent + 1) * 2 + 1]);
        }
        else
        {
            entry_end = m_data_offset;
        }

        assert(entry_start <= entry_end); // LCOV_EXCL_LINE
        assert(entry_end <= FILE_SIZE); // LCOV_EXCL_LINE
        assert(s->m_data_offset + (entry_end - entry_start) <= FILE_SIZE); // LCOV_EXCL_LINE

        // Copy the entry's data
        memmove(s->m_data + s->m_data_offset, m_data + entry_start, (entry_end - entry_start));
        // Insert into the search log.
        s->m_search_log[s->m_search_offset * 2] = (static_cast<uint64_t>(secondary_hash) << 32)
                                                | static_cast<uint64_t>(primary_hash);
        s->m_search_log[s->m_search_offset * 2 + 1] = static_cast<uint64_t>(s->m_data_offset);
        // Insert into the hash table.
        size_t bucket;
        s->hash_lookup(primary_hash, &bucket);
        s->m_hash_table[bucket] = (static_cast<uint64_t>(s->m_data_offset) << 32)
                                | static_cast<uint64_t>(primary_hash);
        // Update the position trackers.
        ++s->m_search_offset;
        s->m_data_offset = (s->m_data_offset + (entry_end - entry_start) + 7) & ~7; // Keep everything 8-byte aligned.
    }
}

hyperdisk :: shard :: shard(po6::io::fd* fd)
    : m_ref(0)
    , m_hash_table(NULL)
    , m_search_log(NULL)
    , m_data(NULL)
    , m_data_offset(INDEX_SEGMENT_SIZE)
    , m_search_offset(0)
{
    m_data = static_cast<char*>(mmap(NULL, FILE_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd->get(), 0));

    if (m_data == MAP_FAILED)
    {
        throw po6::error(errno);
    }

    m_hash_table = reinterpret_cast<uint64_t*>(m_data);
    m_search_log = reinterpret_cast<uint64_t*>(m_data + HASH_TABLE_SIZE);
}

hyperdisk :: shard :: ~shard()
                    throw ()
{
    munmap(m_data, FILE_SIZE);
}

size_t
hyperdisk :: shard :: data_size(const e::buffer& key,
                              const std::vector<e::buffer>& value) const
{
    size_t hypothetical_size = sizeof(uint64_t) + sizeof(uint32_t)
                             + sizeof(uint16_t) + key.size()
                             + sizeof(uint32_t) * value.size();

    for (size_t i = 0; i < value.size(); ++i)
    {
        hypothetical_size += value[i].size();
    }

    return hypothetical_size;
}

uint64_t
hyperdisk :: shard :: data_version(uint32_t offset) const
{
    assert(((offset + 7) & ~7) == offset); // LCOV_EXCL_LINE
    return *reinterpret_cast<uint64_t*>(m_data + offset);
}

size_t
hyperdisk :: shard :: data_key_size(uint32_t offset) const
{
    assert(((offset + 7) & ~7) == offset); // LCOV_EXCL_LINE
    return *reinterpret_cast<uint32_t*>(m_data + offset + sizeof(uint64_t));
}

void
hyperdisk :: shard :: data_key(uint32_t offset,
                             size_t keysize,
                             e::buffer* key) const
{
    assert(((offset + 7) & ~7) == offset); // LCOV_EXCL_LINE
    uint32_t cur_offset = offset + sizeof(uint64_t) + sizeof(uint32_t);
    *key = e::buffer(m_data + cur_offset, keysize);
}

void
hyperdisk :: shard :: data_value(uint32_t offset,
                               size_t keysize,
                               std::vector<e::buffer>* value) const
{
    assert(((offset + 7) & ~7) == offset); // LCOV_EXCL_LINE
    uint32_t cur_offset = offset + sizeof(uint64_t) + sizeof(uint32_t) + keysize;
    uint16_t num_dims;
    memmove(&num_dims, m_data + cur_offset, sizeof(uint16_t));
    cur_offset += sizeof(uint16_t);
    value->clear();

    for (uint16_t i = 0; i < num_dims; ++i)
    {
        uint32_t size;
        memmove(&size, m_data + cur_offset, sizeof(size));
        cur_offset += sizeof(size);
        value->push_back(e::buffer());
        e::buffer buf(m_data + cur_offset, size);
        value->back().swap(buf);
        cur_offset += size;
    }
}

// This hash lookup preserves the property that once a location in the table is
// assigned to a particular key, it remains assigned to that key forever.
void
hyperdisk :: shard :: hash_lookup(uint32_t primary_hash, const e::buffer& key,
                                  size_t* entry, uint64_t* value)
{
    size_t start = HASH_INTO_TABLE(primary_hash);

    for (size_t off = 0; off < HASH_TABLE_ENTRIES; ++off)
    {
        size_t bucket = HASH_INTO_TABLE(start + off);
        uint64_t this_entry = m_hash_table[bucket];
        uint32_t this_hash = static_cast<uint32_t>(this_entry);
        uint32_t this_offset = static_cast<uint32_t>(this_entry >> 32) & (HASH_OFFSET_INVALID - 1);

        if (this_hash == primary_hash)
        {
            size_t key_size = data_key_size(this_offset);

            if (key_size == key.size() &&
                memcmp(m_data + data_key_offset(this_offset), key.get(), key_size) == 0)
            {
                *entry = bucket;
                *value = this_entry;
                return;
            }
        }

        if (static_cast<uint32_t>(this_entry >> 32) == 0)
        {
            *entry = bucket;
            *value = this_entry;
            return;
        }
    }

    assert(false);
}

void
hyperdisk :: shard :: hash_lookup(uint32_t primary_hash, size_t* entry)
{
    size_t start = HASH_INTO_TABLE(primary_hash);

    for (size_t off = 0; off < HASH_TABLE_ENTRIES; ++off)
    {
        size_t bucket = HASH_INTO_TABLE(start + off);
        uint64_t this_entry = m_hash_table[bucket];

        if (static_cast<uint32_t>(this_entry >> 32) == 0)
        {
            *entry = bucket;
            return;
        }
    }

    assert(false);
}

void
hyperdisk :: shard :: invalidate_search_log(uint32_t to_invalidate, uint32_t invalidate_with)
{
    int64_t low = 0;
    int64_t high = SEARCH_INDEX_ENTRIES;

    while (low <= high)
    {
        int64_t mid = low + ((high - low) / 2);
        const uint32_t mid_offset = m_search_log[mid * 2 + 1] & 0xffffffffUL;

        if (mid_offset == 0 || mid_offset > to_invalidate)
        {
            high = mid - 1;
        }
        else if (mid_offset < to_invalidate)
        {
            low = mid + 1;
        }
        else if (mid_offset == to_invalidate)
        {
            m_search_log[mid * 2 + 1] = (static_cast<uint64_t>(invalidate_with) << 32)
                                        | static_cast<uint64_t>(to_invalidate);
            return;
        }
    }
}
