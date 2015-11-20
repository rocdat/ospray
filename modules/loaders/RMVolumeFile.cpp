// ======================================================================== //
// Copyright 2009-2015 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#include <stdio.h>
#include <string.h>
#include "modules/loaders/RMVolumeFile.h"
#include "ospray/common/OSPCommon.h"
#include "common/sys/sysinfo.h"
#include "common/sys/thread.h"
#ifdef __LINUX__
# include <sched.h>
#endif
#include <stdint.h>

struct RMLoaderThreads {
  OSPVolume volume;
  embree::MutexSys mutex;
  int nextBlockID;
  int nextPinID;
  int numThreads;
  int timeStep;
  pthread_t *thread;
  std::string inFilesDir;
  bool useGZip;
  ospray::vec2f voxelRange;
  struct Block {
    uint8_t voxel[256*256*128];
  };

  RMLoaderThreads(
                  OSPVolume volume, const std::string &fileName, int numThreads=10) 
    : volume(volume), nextBlockID(0), thread(NULL), nextPinID(0),
      numThreads(numThreads)
  {
    inFilesDir = fileName;

    useGZip = (getenv("OSPRAY_RM_NO_GZIP") == NULL);

    const char *slash = rindex(fileName.c_str(),'/');
    std::string base 
      = slash != NULL
      ? std::string(slash+1)
      : fileName;
      
    int rc = sscanf(base.c_str(),"bob%03d.bob",&timeStep);
    if (rc != 1)
      throw std::runtime_error("could not extract time step from bob file name "+base);

    this->voxelRange.x = +std::numeric_limits<float>::infinity();
    this->voxelRange.y = -std::numeric_limits<float>::infinity();

    thread = new pthread_t[numThreads];
    for (int i=0;i<numThreads;i++)
      pthread_create(thread+i,NULL,(void*(*)(void*))threadFunc,this);
    void *result = NULL;
    for (int i=0;i<numThreads;i++)
      pthread_join(thread[i],&result);
#ifdef OSPRAY_VOLUME_VOXELRANGE_IN_APP
    VolumeFile::voxelRangeOf[volume] = voxelRange;
#endif
  };

  void loadBlock(Block &block, 
                 const std::string &fileNameBase, size_t blockID)
  {
    char fileName[10000];
    FILE *file;
    if (useGZip) {
      sprintf(fileName,"%s/d_%04d_%04li.gz",
              fileNameBase.c_str(),timeStep,blockID);
      const std::string cmd = "/usr/bin/gunzip -c "+std::string(fileName);
      file = popen(cmd.c_str(),"r");
      if (!file)
        throw std::runtime_error("could not open file in popen command '"+cmd+"'");
    } else {
      sprintf(fileName,"%s/d_%04d_%04li",
              fileNameBase.c_str(),timeStep,blockID);
      file = fopen(fileName,"rb");
      if (!file)
        throw std::runtime_error("could not open '"+std::string(fileName)+"'");
    }

    assert(file);
    int rc = fread(block.voxel,sizeof(uint8_t),256*256*128,file);
    if (rc != 256*256*128)
      throw std::runtime_error("could not read enough data from "+std::string(fileName));

    if (useGZip) 
      pclose(file);
    else
      fclose(file);
  }
  
  void run() 
  {
    mutex.lock();
    int threadID = nextPinID++;
    embree::setAffinity(threadID);
    mutex.unlock();

    Block *block = new Block;
    while(1) {
      mutex.lock();
      int blockID = nextBlockID++;
      mutex.unlock();
      if (blockID >= 8*8*15) break;

      // int b = K*64+J*8+I;
      int I = blockID % 8;
      int J = (blockID / 8) % 8;
      int K = (blockID / 64);
      
      int cpu = -1;
#ifdef __LINUX__
      cpu = sched_getcpu();
#endif
      printf("[b%i:%i,%i,%i,(%i)]",blockID,I,J,K,cpu); fflush(0);
      loadBlock(*block,inFilesDir,blockID);
 
      mutex.lock();
#ifdef OSPRAY_VOLUME_VOXELRANGE_IN_APP
      for (int i=0;i<5;i++)
        printf("[%i]",block->voxel[i]);
#endif
      ospSetRegion(volume,block->voxel,osp::vec3i(I*256,J*256,K*128),osp::vec3i(256,256,128));
      mutex.unlock();
      
      ospray::vec2f blockRange(block->voxel[0]);
      extendVoxelRange(blockRange,&block->voxel[0],256*256*128);
      
#ifdef OSPRAY_VOLUME_VOXELRANGE_IN_APP
      mutex.lock();
      this->voxelRange.x = std::min(this->voxelRange.x,blockRange.x);
      this->voxelRange.y = std::max(this->voxelRange.y,blockRange.y);
      mutex.unlock();
#endif
    }
    delete block;
  }

  static void *threadFunc(void *arg)
  { ((RMLoaderThreads *)arg)->run(); return NULL; }
  ~RMLoaderThreads() { delete[] thread; };
};

OSPVolume RMVolumeFile::importVolume(OSPVolume volume)
{
  // Update the provided dimensions of the volume for the subvolume specified.
  ospSetVec3i(volume, "dimensions", osp::vec3i(2048,2048,1920));
  ospSetString(volume,"voxelType", "uchar");
  
  int numThreads = embree::getNumberOfLogicalThreads(); //20;

  double t0 = ospray::getSysTime();
  

  RMLoaderThreads(volume,fileName,numThreads);
  double t1 = ospray::getSysTime();
  std::cout << "done loading " << fileName 
       << ", needed " << (t1-t0) << " seconds" << std::endl;

  return(volume);
}


