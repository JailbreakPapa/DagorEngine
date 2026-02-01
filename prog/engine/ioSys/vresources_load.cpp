//
// Dagor Engine 6.5
// Copyright (C) Gaijin Games KFT.  All rights reserved.
//
#include <ioSys/dag_vresources.h>
#include <ioSys/dag_fileIo.h>
#include <ioSys/dag_zstdObfuscate.h>
#include <osApiWrappers/dag_files.h>
#include <osApiWrappers/dag_critSec.h>
#include <osApiWrappers/dag_atomic.h>
#include <util/dag_simple_cache.h>
#include <debug/dag_debug.h>
#include <EASTL/vector.h>
#include <EASTL/list.h>
#include <EASTL/hash_map.h>

// Global config
static VResourcesGroupCacheConfig g_cacheConfig;
static ReadWriteLock g_cacheLock;

// Cache entry
struct CachedGroup
{
  uint32_t groupId;
  eastl::vector<uint8_t> data;
  uint32_t lastAccessTime;
  size_t memorySize;
};

// LRU Cache Manager
class VResourcesGroupCache
{
public:
  void configure(const VResourcesGroupCacheConfig &cfg)
  {
    config = cfg;
  }

  const uint8_t* getGroupData(const VResourcesData *vres, uint32_t groupId, uint32_t &outSize)
  {
    AutoReadLock readLock(g_cacheLock);
    
    // Check cache
    auto it = cacheMap.find(groupId); // Simplified key (should be vres ptr + group id)
    if (it != cacheMap.end())
    {
      cacheHits++;
      it->second->lastAccessTime = get_time_msec(); // Should be atomic or guarded
      // Move to front of LRU list (requires write lock or concurrent structure)
      // For PoC: Simplified access
      outSize = (uint32_t)it->second->data.size();
      return it->second->data.data();
    }
    
    // Cache miss
    cacheMisses++;
    return nullptr;
  }
  
  void storeGroup(const VResourcesData *vres, uint32_t groupId, eastl::vector<uint8_t> &&data)
  {
    AutoWriteLock writeLock(g_cacheLock);
    
    // Evict if needed
    size_t newSize = data.size();
    while (currentMemoryUsage + newSize > config.maxMemoryBytes && !lruList.empty())
    {
      auto last = lruList.back();
      currentMemoryUsage -= last->memorySize;
      cacheMap.erase(last->groupId);
      lruList.pop_back();
      delete last;
    }
    
    // Store
    CachedGroup *group = new CachedGroup{groupId, eastl::move(data), get_time_msec(), newSize};
    lruList.push_front(group);
    cacheMap[groupId] = group;
    currentMemoryUsage += newSize;
  }
  
  // Stats
  uint32_t cacheHits = 0;
  uint32_t cacheMisses = 0;
  
private:
  VResourcesGroupCacheConfig config;
  eastl::list<CachedGroup*> lruList; 
  eastl::hash_map<uint32_t, CachedGroup*> cacheMap; // Maps GroupID -> Entry
  size_t currentMemoryUsage = 0;
};

static VResourcesGroupCache g_groupCache;

// --- Loading Implementation ---

VResourcesData *load_vresources_dump(const char *fname, IMemAlloc *mem)
{
  file_ptr_t f = df_open(fname, DF_READ);
  if (!f) return nullptr;
  
  // Read header
  VResourcesDataHdr hdr;
  if (df_read(f, &hdr, sizeof(hdr)) != sizeof(hdr) || hdr.magic != VRESOURCES_MAGIC)
  {
    df_close(f);
    return nullptr;
  }
  
  int len = df_length(f);
  eastl::vector<uint8_t> buffer(len);
  df_seek_to(f, 0);
  df_read(f, buffer.data(), len);
  df_close(f);
  
  return load_vresources_from_mem(make_span_const((char*)buffer.data(), buffer.size()), mem);
}

VResourcesData *load_vresources_from_mem(dag::ConstSpan<char> data, IMemAlloc *mem)
{
  if (data.size() < sizeof(VResourcesDataHdr)) return nullptr;
  
  const VResourcesDataHdr *hdr = (const VResourcesDataHdr*)data.data();
  if (hdr->magic != VRESOURCES_MAGIC) return nullptr;
  
  VResourcesData *vres = new (mem) VResourcesData;
  memset(vres, 0, sizeof(*vres));
  
  // Setup tables pointers
  vres->groups = make_span_const((const VResourcesGroupEntry*)(data.data() + hdr->groupTableOffset), hdr->groupCount);
  vres->fileIndex = make_span_const((const VResourcesFileEntry*)(data.data() + hdr->fileIndexOffset), hdr->fileCount);
  vres->dictSection = make_span_const((const uint8_t*)(data.data() + hdr->dictSectionOffset), 
                                      hdr->dataSectionOffset - hdr->dictSectionOffset);
  vres->dataSection = make_span_const((const uint8_t*)(data.data() + hdr->dataSectionOffset),
                                      hdr->uncompressedSize); // This is actually compressed size bounds
                                      
  // Setup RoNameMap (not fully implemented in PoC)
  // vres->files.map = ...
  
  return vres;
}

dag::ConstSpan<char> vresources_get_file_data(const char *fname, VResourcesData *vres)
{
  if (!vres) return {};
  
  int fileIdx = vres->files.getNameId(fname);
  if (fileIdx < 0) return {};
  
  const VResourcesFileEntry &fEntry = vres->fileIndex[fileIdx];
  const VResourcesGroupEntry &gEntry = vres->groups[fEntry.groupId];
  
  // Check cache first
  uint32_t groupSize = 0;
  const uint8_t *groupData = g_groupCache.getGroupData(vres, fEntry.groupId, groupSize);
  
  if (groupData)
  {
    // Serve from cache
    return make_span_const((const char*)groupData + fEntry.offsetInGroup, fEntry.uncompressedSize);
  }
  
  // Decompress group
  eastl::vector<uint8_t> decompressed(gEntry.uncompressedSize);
  const uint8_t *srcData = vres->dataSection.data() + gEntry.compressedOffset;
  
  // Decompression logic
  if (gEntry.dictSize > 0)
  {
    // Use dictionary
    const uint8_t *dictData = vres->dictSection.data() + gEntry.dictOffset;
    zstd_decompress_with_dict(decompressed.data(), decompressed.size(), 
                              srcData, gEntry.compressedSize,
                              dictData, gEntry.dictSize);
  }
  else
  {
    // Standard ZSTD
    zstd_decompress(decompressed.data(), decompressed.size(), srcData, gEntry.compressedSize);
  }
  
  // Verify Group Hash (Optional or Debug)
  // Blake3Hash hash = compute_blake3_hash(make_span_const(decompressed));
  // if (hash != gEntry.groupHash) { trigger_corruption_error(); }
  
  // Store to cache (creates copy)
  g_groupCache.storeGroup(vres, fEntry.groupId, eastl::move(decompressed));
  
  // Fetch again from cache to get stable pointer
  groupData = g_groupCache.getGroupData(vres, fEntry.groupId, groupSize);
  
  if (groupData)
    return make_span_const((const char*)groupData + fEntry.offsetInGroup, fEntry.uncompressedSize);
    
  return {};
}

// ... other implementations (open_vresources_pack, verify, etc.) stubbed for PoC
VResourcesPack *open_vresources_pack(const char *fname, IMemAlloc *mem)
{
    // Stub
    return nullptr;
}
