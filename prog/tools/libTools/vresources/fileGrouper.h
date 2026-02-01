//
// Omen Tech
// Copyright (C) Gaijin Games KFT. & WD Studios Corp. All rights reserved.
//
#pragma once

#include <ioSys/dag_vresources.h>
#include <generic/dag_tab.h>
#include <EASTL/vector.h>
#include <EASTL/string.h>
#include <EASTL/unique_ptr.h>

// Forward declarations
class IMemAlloc;

//! Input file information for grouping
struct VResourcesInputFile
{
  eastl::string filename;
  dag::ConstSpan<uint8_t> data;
  size_t uncompressedSize;
  EVResourcesFileCategory category;
  Blake3Hash contentHash;
};

//! Compression group with assigned files
struct VResourcesCompressionGroup
{
  EVResourcesFileCategory category;
  eastl::vector<uint32_t> fileIndices;    // Indices into input file list
  size_t totalUncompressedSize = 0;
  eastl::vector<uint8_t> trainingSamples; // Concatenated samples for dict training
  eastl::vector<size_t> sampleSizes;      // Individual sample sizes
  eastl::vector<uint8_t> dictionary;      // Trained ZSTD dictionary
  Blake3Hash groupHash;                    // Hash of compressed group
};

//! File grouping configuration
struct VResourcesGroupingConfig
{
  // Group size thresholds by category
  size_t smallConfigGroupSize = 512 * 1024;       // 512KB
  size_t scriptsGroupSize = 1536 * 1024;          // 1.5MB
  size_t mediumDataGroupSize = 3 * 1024 * 1024;  // 3MB
  size_t binaryResGroupSize = 2 * 1024 * 1024;   // 2MB

  // Dictionary settings
  size_t minDictSize = 128 * 1024;  // 128KB
  size_t maxDictSize = 256 * 1024;  // 256KB
  size_t dictSampleLimit = 100;     // Max samples for training

  // Compression settings
  int zstdCompressionLevel = 11;

  // Special handling
  bool groupLargeTextures = false;  // Usually compress individually
  size_t largeTextureThreshold = 500 * 1024; // 500KB
};

//! File grouping results
struct VResourcesGroupManifest
{
  eastl::vector<VResourcesCompressionGroup> groups;
  eastl::vector<uint32_t> fileToGroupMap; // Maps file index -> group index
  size_t totalUncompressedSize = 0;
  size_t totalGroupsSize = 0;
};

//! File grouper - categorizes and groups files for optimal compression
class VResourcesFileGrouper
{
public:
  VResourcesFileGrouper(const VResourcesGroupingConfig &config = VResourcesGroupingConfig());
  ~VResourcesFileGrouper();

  //! Add file to grouping
  void addFile(const char *filename, dag::ConstSpan<uint8_t> data);

  //! Categorize all files and assign to groups
  void categorizeAndGroup();

  //! Train ZSTD dictionaries for all groups
  bool trainDictionaries();

  //! Get grouping results
  const VResourcesGroupManifest &getManifest() const { return manifest; }

  //! Get input files
  const eastl::vector<VResourcesInputFile> &getInputFiles() const { return inputFiles; }

  //! Get configuration
  const VResourcesGroupingConfig &getConfig() const { return config; }

private:
  VResourcesGroupingConfig config;
  eastl::vector<VResourcesInputFile> inputFiles;
  VResourcesGroupManifest manifest;

  //! Categorize single file
  EVResourcesFileCategory categorizeFile(const char *filename, size_t size);

  //! Get group size threshold for category
  size_t getGroupSizeThreshold(EVResourcesFileCategory category);

  //! Assign files to groups based on category and size
  void assignFilesToGroups();

  //! Collect training samples from group files
  void collectTrainingSamples(VResourcesCompressionGroup &group);

  //! Train dictionary for a single group
  bool trainGroupDictionary(VResourcesCompressionGroup &group);
};

//! Helper: Get file extension
const char *get_file_extension(const char *filename);

//! Helper: Check if file matches pattern
bool file_matches_extension(const char *filename, const char *ext);

//! Helper: Categorize by file type
EVResourcesFileCategory categorize_by_filename(const char *filename);
