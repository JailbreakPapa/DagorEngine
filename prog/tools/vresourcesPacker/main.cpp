//
// Dagor Engine 6.5
// Copyright (C) Gaijin Games KFT.  All rights reserved.
//
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ioSys/dag_dataBlock.h>
#include <ioSys/dag_fileIo.h>
#include <ioSys/dag_ioUtils.h>
#include <ioSys/dag_zstdIo.h>
#include <osApiWrappers/dag_files.h>
#include <osApiWrappers/dag_direct.h>
#include <libTools/util/fileUtils.h>
#include <libTools/vresources/fileGrouper.h>
#include <ioSys/dag_vresources.h>
#include <hash/BLAKE3/blake3.h>
#include <debug/dag_debug.h>
#include <util/dag_string.h>
#include <util/dag_globDef.h>

// For BSDiff
// Note: In a real implementation, we'd link against the bsdiff library
// extern "C" int bsdiff(const uint8_t* old, off_t oldsize, const uint8_t* new, off_t newsize, const char* patchfile);

// Global settings
static bool quietMode = false;
static bool verboseMode = false;

struct PackerConfig
{
  eastl::string outputFile;
  eastl::string baseFile; // For delta patching
  eastl::string rootDir;
  eastl::string inputVromfs; // New: Convert from vromfs
  VResourcesGroupingConfig grouping;
};

// ... scan_directory ...

// Helper: Scan vromfs
static void scan_vromfs(const char *vromfsPath, VResourcesFileGrouper &grouper)
{
  VirtualRomFsData *fs = load_vromfs_dump(vromfsPath, midmem);
  if (!fs) 
  {
    printf("ERROR: Failed to load vromfs %s\n", vromfsPath);
    return;
  }
  
  printf("Converting vromfs with %d files...\n", fs->files.map.size());
  for (int i = 0; i < fs->files.map.size(); ++i)
  {
     const char *name = fs->files.map[i];
     dag::ConstSpan<char> data = fs->data[i]; // Implicit cast
     grouper.addFile(name, make_span_const((const uint8_t*)data.data(), data.size()));
  }
}

// ... compress_group_data ...

int main(int argc, char **argv)
{
  // ...
  
  config.outputFile = blk.getStr("output", "out.vresources");
  config.baseFile = blk.getStr("base", "");
  config.rootDir = blk.getStr("root", ".");
  config.inputVromfs = blk.getStr("input_vromfs", "");
  
  // Setup grouper
  VResourcesFileGrouper grouper(config.grouping);
  
  if (!config.inputVromfs.empty())
  {
     scan_vromfs(config.inputVromfs.c_str(), grouper);
  }
  else
  {
     // Scan files
     printf("Scanning files in %s...\n", config.rootDir.c_str());
     scan_directory(config.rootDir.c_str(), "", grouper);
  }
  
  // ...
  
  // Group and train
  printf("Categorizing and grouping...\n");
  grouper.categorizeAndGroup();
  
  printf("Training dictionaries...\n");
  grouper.trainDictionaries();
  
  const auto &manifest = grouper.getManifest();
  const auto &inputFiles = grouper.getInputFiles();
  
  // Prepare output structure
  eastl::vector<VResourcesGroupEntry> groupTable;
  eastl::vector<VResourcesFileEntry> fileIndex;
  eastl::vector<uint8_t> dictSection;
  eastl::vector<uint8_t> dataSection;
  
  // Process groups
  printf("Compressing %d groups...\n", (int)manifest.groups.size());
  
  for (size_t i = 0; i < manifest.groups.size(); i++)
  {
    const auto &group = manifest.groups[i];
    VResourcesGroupEntry entry;
    entry.category = group.category;
    entry.fileCount = (uint16_t)group.fileIndices.size();
    entry.uncompressedSize = (uint32_t)group.totalUncompressedSize;
    
    // embedded dictionary
    if (!group.dictionary.empty())
    {
      entry.dictOffset = (uint32_t)dictSection.size();
      entry.dictSize = (uint32_t)group.dictionary.size();
      dictSection.insert(dictSection.end(), group.dictionary.begin(), group.dictionary.end());
    }
    else
    {
      entry.dictOffset = 0;
      entry.dictSize = 0;
    }
    
    // Compress data
    eastl::vector<uint8_t> compressedData;
    if (!compress_group_data(group, manifest, inputFiles, compressedData))
      return 1;
      
    entry.compressedOffset = (uint32_t)dataSection.size();
    entry.compressedSize = (uint32_t)compressedData.size();
    dataSection.insert(dataSection.end(), compressedData.begin(), compressedData.end());
    
    // Compute group hash
    entry.groupHash = compute_blake3_hash(make_span_const(compressedData));
    
    groupTable.push_back(entry);
    
    // Fill file index
    uint32_t offsetInGroup = 0;
    for (uint32_t fileIdx : group.fileIndices)
    {
      const auto &file = inputFiles[fileIdx];
      VResourcesFileEntry fEntry;
      // nameId will be filled later with RoNameMap
      fEntry.groupId = (uint16_t)i;
      fEntry.offsetInGroup = offsetInGroup;
      fEntry.uncompressedSize = (uint32_t)file.uncompressedSize;
      fEntry.contentHash = file.contentHash;
      fEntry.flags = 0;
      
      fileIndex.push_back(fEntry);
      offsetInGroup += file.uncompressedSize;
    }
  }
  
  // Create RoNameMap
  RoNameMapBuilder nameMap;
  for (size_t i = 0; i < inputFiles.size(); i++)
  {
    nameMap.add(inputFiles[i].filename.c_str());
  }
  nameMap.prepare();
  
  // Update nameIds in file index (assuming implicit order matches inputFiles if we iterate same way, 
  // but RoNameMap sorts names. We need to map back.)
  // Actually, RoNameMapBuilder sorts names. We need to reorder fileIndex to match nameMap order.
  
  eastl::vector<VResourcesFileEntry> sortedFileIndex;
  sortedFileIndex.resize(fileIndex.size());
  
  for (int i = 0; i < nameMap.count(); i++)
  {
    const char *name = nameMap.getName(i);
    // Find original index - this is slow (O(N^2)). In production we optimize.
    for (size_t j = 0; j < inputFiles.size(); j++)
    {
      if (inputFiles[j].filename == name)
      {
        sortedFileIndex[i] = fileIndex[j];
        sortedFileIndex[i].nameId = i; // implicit
        break;
      }
    }
  }
  
  // Write output file
  file_ptr_t outFile = df_open(config.outputFile.c_str(), DF_WRITE | DF_CREATE);
  if (!outFile)
  {
    printf("ERROR: Failed to create output file %s\n", config.outputFile.c_str());
    return 1;
  }
  
  VResourcesDataHdr hdr;
  memset(&hdr, 0, sizeof(hdr));
  hdr.magic = VRESOURCES_MAGIC;
  hdr.version = VRESOURCES_VERSION;
  hdr.flags = EVRF_COMPRESSION_GROUPS | EVRF_BLAKE3_HASHING | EVRF_ZSTD_COMPRESSION;
  hdr.groupCount = (uint32_t)groupTable.size();
  hdr.fileCount = (uint32_t)sortedFileIndex.size();
  
  // Calculate offsets
  // Layout: Header | GroupTable | FileIndex | RoNameMap | DictSection | DataSection | Patches
  
  size_t currentOffset = sizeof(VResourcesDataHdr);
  
  hdr.groupTableOffset = (uint32_t)currentOffset;
  currentOffset += groupTable.size() * sizeof(VResourcesGroupEntry);
  
  hdr.fileIndexOffset = (uint32_t)currentOffset;
  currentOffset += sortedFileIndex.size() * sizeof(VResourcesFileEntry);
  
  // Write header placeholder
  df_write(outFile, &hdr, sizeof(hdr));
  
  // Write tables
  df_write(outFile, groupTable.data(), groupTable.size() * sizeof(VResourcesGroupEntry));
  df_write(outFile, sortedFileIndex.data(), sortedFileIndex.size() * sizeof(VResourcesFileEntry));
  
  // Write RoNameMap (not implemented fully here, need RoNameMap serialization)
  // For PoC we skip it or assume simplified write
  // nameMap.write(outFile); 
  
  // Write sections
  hdr.dictSectionOffset = (uint32_t)df_tell(outFile);
  df_write(outFile, dictSection.data(), dictSection.size());
  
  hdr.dataSectionOffset = (uint32_t)df_tell(outFile);
  df_write(outFile, dataSection.data(), dataSection.size());
  
  hdr.uncompressedSize = manifest.totalUncompressedSize;
  hdr.compressedSize = dataSection.size(); // Approximate
  
  // Update header
  df_seek_to(outFile, 0);
  df_write(outFile, &hdr, sizeof(hdr));
  
  df_close(outFile);
  
  printf("Successfully created %s\n", config.outputFile.c_str());
  
  return 0;
}
