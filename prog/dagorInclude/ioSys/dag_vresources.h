//
// Dagor Engine 6.5
// Copyright (C) Gaijin Games KFT.  All rights reserved.
//
#pragma once

#include <util/dag_roNameMap.h>
#include <osApiWrappers/dag_rwLock.h>
#include <generic/dag_tabFwd.h>
#include <supp/dag_define_KRNLIMP.h>
#include <osApiWrappers/dag_vromfs.h>
#include <EASTL/unique_ptr.h>
#include <EASTL/utility.h>

class IMemAlloc;
class String;

// Forward declarations
struct VResourcesData;
struct VResourcesPack;
struct VirtualRomFsData; // For compatibility

//! VResources magic header identifier ('VRes')
static constexpr unsigned VRESOURCES_MAGIC = 0x73655256; // 'VRes' in little-endian

//! VResources format version
static constexpr uint32_t VRESOURCES_VERSION = 1;

//! VResources feature flags
enum EVResourcesFlags : uint32_t
{
  EVRF_COMPRESSION_GROUPS = 0x0001,  // Uses file groups with shared dictionaries
  EVRF_DELTA_PATCHES = 0x0002,       // Contains delta patch section
  EVRF_BLAKE3_HASHING = 0x0004,      // Uses BLAKE3 for content versioning
  EVRF_SIGNED_CONTENTS = 0x0008,     // Digital signature included
  EVRF_ZSTD_COMPRESSION = 0x0010     // ZSTD compression (vs legacy zlib)
};

//! File category for grouping strategy
enum EVResourcesFileCategory : uint8_t
{
  EVRFC_SMALL_CONFIG = 0,  // <1KB: BLK, JSON, small text -> 512KB groups
  EVRFC_SCRIPTS = 1,       // 1-50KB: DAS, larger BLK -> 1-2MB groups
  EVRFC_MEDIUM_DATA = 2,   // 50-500KB: Small textures, audio -> 2-4MB groups
  EVRFC_LARGE_TEXTURES = 3,// >500KB: DDSx, large assets -> Individual
  EVRFC_BINARY_RES = 4,    // Shaders, compiled code -> By size similarity
  EVRFC_SPECIAL = 5        // Shared namemap, dictionaries -> Special handling
};

//! Main VResources file header (must be 16-byte aligned)
struct VResourcesDataHdr
{
  uint32_t magic;              // 'VRes' magic identifier
  uint32_t version;            // Format version
  uint32_t flags;              // EVResourcesFlags bitfield
  uint32_t groupCount;         // Number of compression groups
  uint32_t fileCount;          // Total number of files
  uint32_t groupTableOffset;   // Offset to group metadata table
  uint32_t fileIndexOffset;    // Offset to file index
  uint32_t dictSectionOffset;  // Offset to embedded dictionaries
  uint32_t dataSectionOffset;  // Offset to compressed data blocks
  uint32_t patchSectionOffset; // Offset to delta patches (0 if none)
  uint32_t uncompressedSize;   // Total uncompressed size
  uint32_t compressedSize;     // Total compressed size
  uint32_t reserved[4];        // Reserved for future use

  bool hasCompressionGroups() const { return (flags & EVRF_COMPRESSION_GROUPS) != 0; }
  bool hasDeltaPatches() const { return (flags & EVRF_DELTA_PATCHES) != 0; }
  bool usesBlake3() const { return (flags & EVRF_BLAKE3_HASHING) != 0; }
  bool isSigned() const { return (flags & EVRF_SIGNED_CONTENTS) != 0; }
  bool usesZstd() const { return (flags & EVRF_ZSTD_COMPRESSION) != 0; }
};

//! BLAKE3 hash (32 bytes / 256 bits)
struct Blake3Hash
{
  uint8_t hash[32];

  bool operator==(const Blake3Hash &other) const
  {
    return memcmp(hash, other.hash, sizeof(hash)) == 0;
  }

  bool operator!=(const Blake3Hash &other) const { return !(*this == other); }

  bool isZero() const
  {
    for (int i = 0; i < 32; i++)
      if (hash[i] != 0)
        return false;
    return true;
  }
};

//! Compression group metadata entry
struct VResourcesGroupEntry
{
  EVResourcesFileCategory category; // File category
  uint8_t reserved1;
  uint16_t fileCount;               // Files in this group
  uint32_t dictOffset;              // Offset to ZSTD dictionary in dict section
  uint32_t dictSize;                // Dictionary size (128-256KB typically)
  uint32_t compressedOffset;        // Offset in data section
  uint32_t compressedSize;          // Compressed data size
  uint32_t uncompressedSize;        // Uncompressed group size
  Blake3Hash groupHash;             // BLAKE3 hash of compressed group data

  static constexpr uint32_t MIN_DICT_SIZE = 128 * 1024;  // 128KB
  static constexpr uint32_t MAX_DICT_SIZE = 256 * 1024;  // 256KB
};

//! File index entry
struct VResourcesFileEntry
{
  uint32_t nameId;            // Index into RoNameMap
  uint16_t groupId;           // Compression group ID
  uint16_t flags;             // Reserved for file-specific flags
  uint32_t offsetInGroup;     // Offset within decompressed group
  uint32_t uncompressedSize;  // Uncompressed file size
  Blake3Hash contentHash;     // BLAKE3 hash as content-based version

  static constexpr uint16_t FLAG_HAS_DELTA_PATCH = 0x0001;
};

//! Delta patch entry
struct VResourcesDeltaPatch
{
  Blake3Hash targetHash;      // Target file content hash
  Blake3Hash baseHash;        // Base version content hash
  uint32_t patchOffset;       // Offset to BSDiff patch data
  uint32_t patchSize;         // Patch data size
  uint32_t newFileSize;       // New file size after patching

  static constexpr float PATCH_SIZE_THRESHOLD = 0.8f; // 80% threshold
  
  bool shouldUsePatch() const { return patchSize < (newFileSize * PATCH_SIZE_THRESHOLD); }
};

//! Main VResources data structure (runtime representation)
struct VResourcesDataBase
{
  VirtualRomFsFileAttributes attr; // Mapped to match VirtualRomFsDataBase
  int64_t mtime = -1;
  uint32_t version = 0;
  uint16_t flags = 0;
  bool firstPriority = true;
};

// Flag to distinguish VResources from legacy VROMFS
static constexpr uint16_t EVRF_IS_VRESOURCES = 0x8000;

struct VResourcesData : public VResourcesDataBase
{
  RoNameMap files;                              // File name index
  dag::ConstSpan<VResourcesGroupEntry> groups;  // Group metadata
  dag::ConstSpan<VResourcesFileEntry> fileIndex;// File index
  dag::ConstSpan<uint8_t> dictSection;          // Dictionary data
  dag::ConstSpan<uint8_t> dataSection;          // Compressed data
  dag::ConstSpan<VResourcesDeltaPatch> patches; // Delta patches (if any)
};

//! VResources pack with additional data (similar to VirtualRomFsPack)
struct VResourcesPack : public VResourcesData
{
  struct BackedData
  {
    const Blake3Hash *groupHashBasePtr = nullptr; // Group hashes for CDN
    VResourcesPack *cache = nullptr;              // Optional cache pack
    vresources_user_get_group_data_t getData = nullptr; // CDN callback
    void *getDataArg = nullptr;
    void *resolvedGroupsList = nullptr;           // Lazy-loaded groups
  };

  int hdrSz;
  int _resv;
  PATCHABLE_DATA64(void *, ptr);

  bool isValid() const { return (void *)files.map.data() >= (void *)(this + 1) && ptr; }
  const char *getFilePath() const { return isValid() && _resv == 0 ? ((const char *)&files) + hdrSz : nullptr; }
  BackedData *getBackedData() { return isValid() && _resv == 'VRBD' ? (BackedData *)ptr : nullptr; }
};

//! Callback for CDN group fetching
typedef bool (*vresources_user_get_group_data_t)(uint32_t group_id, const Blake3Hash &group_hash, String &out_fn,
  int &out_base_ofs, bool sync_wait_ready, void *arg);

//! Group cache configuration
struct VResourcesGroupCacheConfig
{
  size_t maxMemoryBytes = 256 * 1024 * 1024; // 256MB default
  uint32_t maxGroups = 64;                     // Max cached groups
  bool enableSmartPrefetch = true;             // Adaptive prefetching
  uint32_t maxPrefetchGroups = 4;              // Groups to prefetch
  uint32_t minAccessCountForPrediction = 3;    // Learning threshold
};

// === API Functions ===

//! Load VResources from file
KRNLIMP VResourcesData *load_vresources_dump(const char *fname, IMemAlloc *mem = midmem);

//! Open VResources pack with file path tracking
KRNLIMP VResourcesPack *open_vresources_pack(const char *fname, IMemAlloc *mem = midmem);

//! Load VResources from memory buffer
KRNLIMP VResourcesData *load_vresources_from_mem(dag::ConstSpan<char> data, IMemAlloc *mem = midmem);

//! Open backed VResources with CDN support
KRNLIMP VResourcesPack *open_backed_vresources_pack(const char *fname, IMemAlloc *mem, VResourcesPack *cache_vres,
  vresources_user_get_group_data_t get_group_data, void *user_arg);

//! Replace cache in backed VResources
KRNLIMP VResourcesPack *backed_vresources_replace_cache(VResourcesPack *fs, VResourcesPack *cache);

//! Prefetch all groups in background
KRNLIMP void backed_vresources_prefetch_all_groups(VResourcesPack *fs);

//! Close and free VResources
KRNLIMP void close_vresources(VResourcesData *fs, IMemAlloc *mem = midmem);

//! Get file data from VResources (with group decompression and caching)
KRNLIMP dag::ConstSpan<char> vresources_get_file_data(const char *fname, VResourcesData *vres);

//! Check if file exists in VResources
KRNLIMP bool vresources_check_file_exists(const char *fname, const VResourcesData *vres);

//! Get file count
KRNLIMP int vresources_get_file_count(const VResourcesData *vres);

//! Get file name by index
KRNLIMP const char *vresources_get_file_name(const VResourcesData *vres, int idx);

//! Configure group cache parameters
KRNLIMP void vresources_configure_cache(const VResourcesGroupCacheConfig &config);

//! Get cache statistics
KRNLIMP void vresources_get_cache_stats(uint32_t &cached_groups, size_t &memory_used, uint32_t &cache_hits,
  uint32_t &cache_misses);

//! Clear group cache
KRNLIMP void vresources_clear_cache();

//! Verify file integrity (BLAKE3 hash check)
KRNLIMP bool vresources_verify_file(const char *fname, const VResourcesData *vres);

//! Verify entire VResources integrity
KRNLIMP bool vresources_verify_all(const VResourcesData *vres);

// === Helper Functions ===

//! Compute BLAKE3 hash of data
KRNLIMP Blake3Hash compute_blake3_hash(dag::ConstSpan<uint8_t> data);

//! Convert BLAKE3 to hex string
KRNLIMP void blake3_to_hex(const Blake3Hash &hash, char *out_hex64);

//! Parse hex string to BLAKE3
KRNLIMP bool hex_to_blake3(const char *hex64, Blake3Hash &out_hash);

//! Get recommended group size for category
inline size_t get_recommended_group_size(EVResourcesFileCategory category)
{
  switch (category)
  {
    case EVRFC_SMALL_CONFIG: return 512 * 1024;       // 512KB
    case EVRFC_SCRIPTS: return 1536 * 1024;           // 1.5MB
    case EVRFC_MEDIUM_DATA: return 3 * 1024 * 1024;   // 3MB
    case EVRFC_LARGE_TEXTURES: return 0;               // Individual files
    case EVRFC_BINARY_RES: return 2 * 1024 * 1024;    // 2MB
    case EVRFC_SPECIAL: return 0;                      // Special handling
    default: return 1024 * 1024;                       // 1MB default
  }
}

//! Categorize file by name and size
KRNLIMP EVResourcesFileCategory categorize_file(const char *filename, size_t file_size);

#include <supp/dag_undef_KRNLIMP.h>
