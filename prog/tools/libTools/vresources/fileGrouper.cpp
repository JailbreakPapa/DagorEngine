//
// Omen Tech
// Copyright (C) Gaijin Games KFT. & WD Studios Corp. All rights reserved.
//
#include "fileGrouper.h"
#include <ioSys/dag_zstdObfuscate.h>
#include <osApiWrappers/dag_files.h>
#include <osApiWrappers/dag_direct.h>
#include <util/dag_string.h>
#include <debug/dag_debug.h>
#include <memory/dag_mem.h>
#include <algorithm>

// Helper: Get file extension
const char *get_file_extension(const char *filename)
{
  const char *dot = strrchr(filename, '.');
  if (!dot || dot == filename)
    return "";
  return dot + 1;
}

// Helper: Check if file matches extension
bool file_matches_extension(const char *filename, const char *ext)
{
  const char *file_ext = get_file_extension(filename);
  return dd_stricmp(file_ext, ext) == 0;
}

// Helper: Categorize by filename
EVResourcesFileCategory categorize_by_filename(const char *filename)
{
  // Small config files
  if (file_matches_extension(filename, "blk") || 
      file_matches_extension(filename, "json") ||
      file_matches_extension(filename, "cfg") ||
      file_matches_extension(filename, "txt"))
    return EVRFC_SMALL_CONFIG;

  // Scripts
  if (file_matches_extension(filename, "das") ||
      file_matches_extension(filename, "nut") ||
      file_matches_extension(filename, "lua"))
    return EVRFC_SCRIPTS;

  // Large textures
  if (file_matches_extension(filename, "ddsx") ||
      file_matches_extension(filename, "dds") ||
      file_matches_extension(filename, "tga") ||
      file_matches_extension(filename, "texpack"))
    return EVRFC_LARGE_TEXTURES;

  // Binary resources (shaders, compiled)
  if (file_matches_extension(filename, "bin") ||
      file_matches_extension(filename, "shdump") ||
      file_matches_extension(filename, "vkbin") ||
      file_matches_extension(filename, "dxil") ||
      file_matches_extension(filename, "spv"))
    return EVRFC_BINARY_RES;

  // Special files
  if (strstr(filename, "__shared_namemap") || strstr(filename, ".dict"))
    return EVRFC_SPECIAL;

  // Default to medium data
  return EVRFC_MEDIUM_DATA;
}

// VResourcesFileGrouper implementation
VResourcesFileGrouper::VResourcesFileGrouper(const VResourcesGroupingConfig &cfg) : config(cfg) {}

VResourcesFileGrouper::~VResourcesFileGrouper() {}

void VResourcesFileGrouper::addFile(const char *filename, dag::ConstSpan<uint8_t> data)
{
  VResourcesInputFile file;
  file.filename = filename;
  file.data = data;
  file.uncompressedSize = data.size();
  file.category = categorizeFile(filename, data.size());
  
  // Compute BLAKE3 hash
  file.contentHash = compute_blake3_hash(data);

  inputFiles.push_back(file);
}

EVResourcesFileCategory VResourcesFileGrouper::categorizeFile(const char *filename, size_t size)
{
  // First try by filename
  EVResourcesFileCategory cat = categorize_by_filename(filename);

  // Refine by size
  if (cat == EVRFC_SMALL_CONFIG && size > 50 * 1024)
    cat = EVRFC_SCRIPTS; // Large config files treated as scripts

  if (cat == EVRFC_SCRIPTS && size > 100 * 1024)
    cat = EVRFC_MEDIUM_DATA; // Very large scripts

  if (cat == EVRFC_MEDIUM_DATA && size > config.largeTextureThreshold)
    cat = EVRFC_LARGE_TEXTURES; // Large data files

  return cat;
}

size_t VResourcesFileGrouper::getGroupSizeThreshold(EVResourcesFileCategory category)
{
  switch (category)
  {
    case EVRFC_SMALL_CONFIG: return config.smallConfigGroupSize;
    case EVRFC_SCRIPTS: return config.scriptsGroupSize;
    case EVRFC_MEDIUM_DATA: return config.mediumDataGroupSize;
    case EVRFC_BINARY_RES: return config.binaryResGroupSize;
    case EVRFC_LARGE_TEXTURES: return 0; // Individual compression
    case EVRFC_SPECIAL: return 0; // Special handling
    default: return 1024 * 1024; // 1MB default
  }
}

void VResourcesFileGrouper::categorizeAndGroup()
{
  debug("VResourcesFileGrouper: Categorizing %d files...", inputFiles.size());

  // Sort files by category for better grouping
  eastl::vector<uint32_t> sortedIndices;
  sortedIndices.reserve(inputFiles.size());
  for (uint32_t i = 0; i < inputFiles.size(); i++)
    sortedIndices.push_back(i);

  std::sort(sortedIndices.begin(), sortedIndices.end(), [this](uint32_t a, uint32_t b) {
    const auto &fileA = inputFiles[a];
    const auto &fileB = inputFiles[b];
    if (fileA.category != fileB.category)
      return fileA.category < fileB.category;
    return fileA.uncompressedSize < fileB.uncompressedSize;
  });

  // Assign to groups
  manifest.fileToGroupMap.resize(inputFiles.size());
  assignFilesToGroups();

  debug("VResourcesFileGrouper: Created %d groups", manifest.groups.size());
}

void VResourcesFileGrouper::assignFilesToGroups()
{
  VResourcesCompressionGroup *currentGroup = nullptr;
  EVResourcesFileCategory currentCategory = EVRFC_SPECIAL;

  for (uint32_t i = 0; i < inputFiles.size(); i++)
  {
    const auto &file = inputFiles[i];
    size_t threshold = getGroupSizeThreshold(file.category);

    // Large files or special files get their own group
    if (threshold == 0 || file.uncompressedSize > threshold)
    {
      VResourcesCompressionGroup group;
      group.category = file.category;
      group.fileIndices.push_back(i);
      group.totalUncompressedSize = file.uncompressedSize;
      
      manifest.fileToGroupMap[i] = manifest.groups.size();
      manifest.groups.push_back(group);
      manifest.totalUncompressedSize += file.uncompressedSize;
      
      currentGroup = nullptr;
      continue;
    }

    // Start new group if category changed or group is full
    if (!currentGroup || currentCategory != file.category || 
        currentGroup->totalUncompressedSize + file.uncompressedSize > threshold)
    {
      VResourcesCompressionGroup group;
      group.category = file.category;
      manifest.groups.push_back(group);
      currentGroup = &manifest.groups.back();
      currentCategory = file.category;
    }

    // Add file to current group
    currentGroup->fileIndices.push_back(i);
    currentGroup->totalUncompressedSize += file.uncompressedSize;
    manifest.fileToGroupMap[i] = manifest.groups.size() - 1;
    manifest.totalUncompressedSize += file.uncompressedSize;
  }

  debug("Total uncompressed size: %.2f MB", manifest.totalUncompressedSize / (1024.0 * 1024.0));
}

void VResourcesFileGrouper::collectTrainingSamples(VResourcesCompressionGroup &group)
{
  // Limit samples for dictionary training
  size_t maxSamples = config.dictSampleLimit;
  size_t sampleInterval = group.fileIndices.size() / maxSamples;
  if (sampleInterval == 0)
    sampleInterval = 1;

  for (size_t i = 0; i < group.fileIndices.size(); i += sampleInterval)
  {
    uint32_t fileIdx = group.fileIndices[i];
    const auto &file = inputFiles[fileIdx];
    
    // Add file data as training sample
    const uint8_t *data = file.data.data();
    size_t size = file.data.size();
    
    group.trainingSamples.insert(group.trainingSamples.end(), data, data + size);
    group.sampleSizes.push_back(size);
    
    if (group.sampleSizes.size() >= maxSamples)
      break;
  }

  debug("Group [%s]: Collected %d samples (%.2f KB)", 
        group.category == EVRFC_SMALL_CONFIG ? "SmallConfig" :
        group.category == EVRFC_SCRIPTS ? "Scripts" :
        group.category == EVRFC_MEDIUM_DATA ? "MediumData" :
        group.category == EVRFC_BINARY_RES ? "BinaryRes" : "Other",
        group.sampleSizes.size(),
        group.trainingSamples.size() / 1024.0);
}

bool VResourcesFileGrouper::trainGroupDictionary(VResourcesCompressionGroup &group)
{
  if (group.trainingSamples.empty())
    return false;

  // Determine dictionary size based on sample data
  size_t dictSize = config.maxDictSize;
  if (group.trainingSamples.size() < dictSize * 2)
    dictSize = max(config.minDictSize, group.trainingSamples.size() / 2);

  group.dictionary.resize(dictSize);

  // Train dictionary using ZSTD
  size_t trainedSize = zstd_train_dict_buffer(
    make_span(group.dictionary),
    config.zstdCompressionLevel,
    make_span_const(group.trainingSamples),
    make_span_const(group.sampleSizes)
  );

  if (trainedSize == 0)
  {
    debug("Failed to train dictionary for group (category %d)", group.category);
    group.dictionary.clear();
    return false;
  }

  // Resize to actual trained size
  group.dictionary.resize(trainedSize);
  
  debug("Trained dictionary: %d bytes for %d samples", trainedSize, group.sampleSizes.size());
  return true;
}

bool VResourcesFileGrouper::trainDictionaries()
{
  debug("VResourcesFileGrouper: Training dictionaries for %d groups...", manifest.groups.size());

  bool allSuccess = true;
  for (auto &group : manifest.groups)
  {
    // Skip single-file groups and special groups
    if (group.fileIndices.size() <= 1 || group.category == EVRFC_SPECIAL)
      continue;

    collectTrainingSamples(group);
    
    if (!trainGroupDictionary(group))
      allSuccess = false;
  }

  debug("Dictionary training complete");
  return allSuccess;
}
