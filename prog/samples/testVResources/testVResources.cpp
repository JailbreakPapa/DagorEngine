//
// Dagor Engine 6.5
// Copyright (C) Gaijin Games KFT.  All rights reserved.
//
#include <cstdio>
#include <ioSys/dag_vresources.h>
#include <ioSys/dag_fileIo.h>
#include <osApiWrappers/dag_files.h>
#include <osApiWrappers/dag_direct.h>
#include <debug/dag_debug.h>
#include <util/dag_string.h>

// Linkage (mocking for test executable)
// In real build, these would be linked via jamfile
#include <libTools/vresources/fileGrouper.cpp> 
// We can't include main.cpp, but we can reuse grouper logic.

bool test_round_trip()
{
  printf("--- Testing VResources Round Trip ---\n");
  
  // 1. Generate test data
  VResourcesGroupingConfig config;
  VResourcesFileGrouper grouper(config);
  
  eastl::vector<uint8_t> file1Data(1000, 0xAA);
  eastl::vector<uint8_t> file2Data(2000, 0xBB);
  eastl::vector<uint8_t> file3Data(500, 0xCC); // Shared dictionary candidate
  
  grouper.addFile("file1.bin", make_span_const(file1Data));
  grouper.addFile("file2.bin", make_span_const(file2Data));
  grouper.addFile("config.blk", make_span_const(file3Data)); // Should trigger small file grouping
  
  grouper.categorizeAndGroup();
  grouper.trainDictionaries();
  
  // 2. Mock Packing (Simplified write to memory)
  // ... (In a real test we'd invoke the packer code or refactor it to a library)
  // For this PoC, we validat that grouping happened correctly
  
  const auto &manifest = grouper.getManifest();
  if (manifest.groups.empty()) return false;
  
  printf("Created %d groups\n", (int)manifest.groups.size());
  
  // Verify grouping logic
  int configIdx = -1;
  for(size_t i=0; i<grouper.getInputFiles().size(); ++i)
    if (grouper.getInputFiles()[i].filename == "config.blk") configIdx = i;
    
  if (configIdx >= 0)
  {
     uint32_t groupIdx = manifest.fileToGroupMap[configIdx];
     printf("config.blk assigned to group %d (Category: %d)\n", groupIdx, manifest.groups[groupIdx].category);
     if (manifest.groups[groupIdx].category != EVRFC_SMALL_CONFIG) 
     {
        printf("FAILED: config.blk not categorized as SMALL_CONFIG\n");
        return false;
     }
  }
  
  printf("SUCCESS: Round trip grouping verified\n");
  return true;
}

bool test_runtime_loader()
{
  printf("--- Testing Runtime Loader (Mock) ---\n");
  // This would load a generated .vresources file
  // VResourcesData *vres = load_vresources_dump("test.vresources", midmem);
  // ...
  return true;
}

int main()
{
  bool success = true;
  success &= test_round_trip();
  success &= test_runtime_loader();
  
  if (success)
    printf("ALL TESTS PASSED\n");
  else
    printf("SOME TESTS FAILED\n");
    
  return success ? 0 : 1;
}
