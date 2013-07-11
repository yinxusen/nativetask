/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <signal.h>
#ifndef __CYGWIN__
#include <execinfo.h>
#endif
#include "commons.h"
#include "NativeTask.h"
#include "NativeObjectFactory.h"
#include "NativeLibrary.h"
#include "BufferStream.h"
#include "util/StringUtil.h"
#include "util/SyncUtils.h"
#include "util/WritableUtils.h"
#include "handler/BatchHandler.h"
#include "handler/EchoBatchHandler.h"
#include "handler/MCollectorOutputHandler.h"
#include "handler/MMapperHandler.h"
#include "handler/MMapTaskHandler.h"
#include "handler/RReducerHandler.h"
#include "lib/LineRecordReader.h"
#include "lib/LineRecordWriter.h"
#include "lib/TotalOrderPartitioner.h"
#include "lib/TeraSort.h"
#include "lib/WordCount.h"

using namespace NativeTask;

// TODO: just for debug, should be removed
extern "C" void handler(int sig) {
  void *array[10];
  size_t size;

  // print out all the frames to stderr
  fprintf(stderr, "Error: signal %d:\n", sig);

#ifndef __CYGWIN__
  // get void*'s for all entries on the stack
  size = backtrace(array, 10);

  backtrace_symbols_fd(array, size, 2);
#endif

  exit(1);
}


DEFINE_NATIVE_LIBRARY(NativeTask) {
  //signal(SIGSEGV, handler);
  REGISTER_CLASS(BatchHandler, NativeTask);
  REGISTER_CLASS(EchoBatchHandler, NativeTask);
  REGISTER_CLASS(MCollectorOutputHandler, NativeTask);
  REGISTER_CLASS(MMapperHandler, NativeTask);
  REGISTER_CLASS(MMapTaskHandler, NativeTask);
  REGISTER_CLASS(RReducerHandler, NativeTask);
  REGISTER_CLASS(Mapper, NativeTask);
  REGISTER_CLASS(Reducer, NativeTask);
  REGISTER_CLASS(Partitioner, NativeTask);
  REGISTER_CLASS(Folder, NativeTask);
  REGISTER_CLASS(LineRecordReader, NativeTask);
  REGISTER_CLASS(KeyValueLineRecordReader, NativeTask);
  REGISTER_CLASS(LineRecordWriter, NativeTask);
  REGISTER_CLASS(TotalOrderPartitioner, NativeTask);
  REGISTER_CLASS(TeraRecordReader, NativeTask);
  REGISTER_CLASS(TeraRecordWriter, NativeTask);
  REGISTER_CLASS(WordCountMapper, NativeTask);
  REGISTER_CLASS(IntSumReducer, NativeTask);
  REGISTER_CLASS(IntSumMapper, NativeTask);
  REGISTER_CLASS(TextIntRecordWriter, NativeTask);
  NativeObjectFactory::SetDefaultClass(BatchHandlerType, "NativeTask.BatchHandler");
  NativeObjectFactory::SetDefaultClass(MapperType, "NativeTask.Mapper");
  NativeObjectFactory::SetDefaultClass(ReducerType, "NativeTask.Reducer");
  NativeObjectFactory::SetDefaultClass(PartitionerType, "NativeTask.Partitioner");
  NativeObjectFactory::SetDefaultClass(FolderType, "NativeTask.Folder");
}

namespace NativeTask {

static Config G_CONFIG;

vector<NativeLibrary *> NativeObjectFactory::Libraries;
map<NativeObjectType, string> NativeObjectFactory::DefaultClasses;
Config * NativeObjectFactory::GlobalConfig = &G_CONFIG;
float NativeObjectFactory::LastProgress = 0;
Progress * NativeObjectFactory::TaskProgress = NULL;
string NativeObjectFactory::LastStatus;
set<Counter *> NativeObjectFactory::CounterSet;
vector<Counter *> NativeObjectFactory::Counters;
vector<uint64_t> NativeObjectFactory::CounterLastUpdateValues;
bool NativeObjectFactory::Inited = false;

static Lock FactoryLock;

bool NativeObjectFactory::Init() {
  ScopeLock<Lock> autolocak(FactoryLock);
  if (Inited == false) {
    // setup log device
    string device = GetConfig().get(NATIVE_LOG_DEVICE, "stderr");
    if (device == "stdout") {
      LOG_DEVICE = stdout;
    } else if (device == "stderr") {
      LOG_DEVICE = stderr;
    } else {
      LOG_DEVICE = fopen(device.c_str(), "w");
    }
    NativeTaskInit();
    NativeLibrary * library = new NativeLibrary("libnativetask.so", "NativeTask");
    library->_getObjectCreatorFunc = NativeTaskGetObjectCreator;
    Libraries.push_back(library);
    Inited = true;
    // load extra user provided libraries
    string libraryConf = GetConfig().get(NATIVE_CLASS_LIBRARY, "");
    if (libraryConf.length()>0) {
      vector<string> libraries;
      vector<string> pair;
      StringUtil::Split(libraryConf, ",", libraries, true);
      for (size_t i=0;i<libraries.size();i++) {
        pair.clear();
        StringUtil::Split(libraries[i], "=", pair, true);
        if (pair.size() == 2) {
          string & name = pair[0];
          string & path = pair[1];
          LOG("[NativeObjectLibrary] Try to load library [%s] with file [%s]", name.c_str(), path.c_str());
          if (false == RegisterLibrary(path, name)) {
            LOG("[NativeObjectLibrary] RegisterLibrary failed: name=%s path=%s", name.c_str(), path.c_str());
            return false;
          } else {
            LOG("[NativeObjectLibrary] RegisterLibrary success: name=%s path=%s", name.c_str(), path.c_str());
          }
        } else {
          LOG("[NativeObjectLibrary] Illegal native.class.libray: [%s] in [%s]", libraries[i].c_str(), libraryConf.c_str());
        }
      }
    }
    const char * version = GetConfig().get(NATIVE_HADOOP_VERSION);
    LOG("[NativeObjectLibrary] NativeTask library initialized with hadoop %s", version==NULL?"unkown":version);
  }
  return true;
}

void NativeObjectFactory::Release() {
  ScopeLock<Lock> autolocak(FactoryLock);
  for (ssize_t i = Libraries.size() - 1; i >= 0; i--) {
    delete Libraries[i];
    Libraries[i] = NULL;
  }
  Libraries.clear();
  for (size_t i = 0; i < Counters.size(); i++) {
    delete Counters[i];
  }
  Counters.clear();
  if (LOG_DEVICE != stdout &&
      LOG_DEVICE != stderr) {
    fclose(LOG_DEVICE);
    LOG_DEVICE = stderr;
  }
  Inited = false;
}

void NativeObjectFactory::CheckInit() {
  if (Inited == false) {
    if (!Init()) {
      throw new IOException("Init NativeTask library failed.");
    }
  }
}

Config & NativeObjectFactory::GetConfig() {
  return *GlobalConfig;
}

Config * NativeObjectFactory::GetConfigPtr() {
  return GlobalConfig;
}

void NativeObjectFactory::SetTaskProgressSource(Progress * progress) {
  TaskProgress = progress;
}

float NativeObjectFactory::GetTaskProgress() {
  if (TaskProgress != NULL) {
    LastProgress = TaskProgress->getProgress();
    LOG("[NativeObjectLibrary] Native side get progress %.3f", LastProgress);
  }
  return LastProgress;
}

void NativeObjectFactory::SetTaskStatus(const string & status) {
  LastStatus = status;
}

static Lock CountersLock;

void NativeObjectFactory::GetTaskStatusUpdate(string & statusData) {
  // Encoding:
  // progress:float
  // status:Text
  // Counter number
  // Counters[group:Text, name:Text, incrCount:Long]
  OutputStringStream os(statusData);
  float progress = GetTaskProgress();
  WritableUtils::WriteFloat(&os, progress);
  WritableUtils::WriteText(&os, LastStatus);
  LastStatus.clear();
  {
    ScopeLock<Lock> AutoLock(CountersLock);
    uint32_t numCounter = (uint32_t)Counters.size();
    WritableUtils::WriteInt(&os, numCounter);
    for (size_t i = 0; i < numCounter; i++) {
      Counter * counter = Counters[i];
      uint64_t newCount = counter->get();
      uint64_t incr = newCount - CounterLastUpdateValues[i];
      CounterLastUpdateValues[i] = newCount;
      WritableUtils::WriteText(&os, counter->group());
      WritableUtils::WriteText(&os, counter->name());
      WritableUtils::WriteLong(&os, incr);
    }
  }
}

Counter * NativeObjectFactory::GetCounter(const string & group, const string & name) {
  ScopeLock<Lock> AutoLock(CountersLock);
  Counter tmpCounter(group, name);
  set<Counter *>::iterator itr =
      CounterSet.find(&tmpCounter);
  if (itr != CounterSet.end()) {
    return *itr;
  }
  Counter * ret = new Counter(group, name);
  Counters.push_back(ret);
  CounterLastUpdateValues.push_back(0);
  CounterSet.insert(ret);
  return ret;
}

void NativeObjectFactory::RegisterClass(const string & clz, ObjectCreatorFunc func) {
  NativeTaskClassMap__[clz] = func;
}

NativeObject * NativeObjectFactory::CreateObject(const string & clz) {
  ObjectCreatorFunc creator = GetObjectCreator(clz);
  return creator ? creator() : NULL;
}

void * NativeObjectFactory::GetFunction(const string & funcName) {
  CheckInit();
  {
    for (vector<NativeLibrary*>::reverse_iterator ritr = Libraries.rbegin();
        ritr != Libraries.rend(); ritr++) {
      void * ret = (*ritr)->getFunction(funcName);
      if (NULL!=ret) {
        return ret;
      }
    }
    return NULL;
  }
}

ObjectCreatorFunc NativeObjectFactory::GetObjectCreator(const string & clz) {
  CheckInit();
  {
    for (vector<NativeLibrary*>::reverse_iterator ritr = Libraries.rbegin();
        ritr != Libraries.rend(); ritr++) {
      ObjectCreatorFunc ret = (*ritr)->getObjectCreator(clz);
      if (NULL!=ret) {
        return ret;
      }
    }
    return NULL;
  }
}


void NativeObjectFactory::ReleaseObject(NativeObject * obj) {
  delete obj;
}

bool NativeObjectFactory::RegisterLibrary(const string & path, const string & name) {
  CheckInit();
  {
    NativeLibrary * library = new NativeLibrary(path, name);
    bool ret = library->init();
    if (!ret) {
      delete library;
      return false;
    }
    Libraries.push_back(library);
    return true;
  }
}

static Lock DefaultClassesLock;

void NativeObjectFactory::SetDefaultClass(NativeObjectType type, const string & clz) {
  ScopeLock<Lock> autolocak(DefaultClassesLock);
  DefaultClasses[type] = clz;
}

NativeObject * NativeObjectFactory::CreateDefaultObject(NativeObjectType type) {
  CheckInit();
  {
    if (DefaultClasses.find(type) != DefaultClasses.end()) {
      string clz = DefaultClasses[type];
      return CreateObject(clz);
    }
    LOG("[NativeObjectLibrary] Default class for NativeObjectType %s not found",
        NativeObjectTypeToString(type).c_str());
    return NULL;
  }
}


int BytesComparator(const char * src, uint32_t srcLength, const char * dest, uint32_t destLength) {

    uint32_t minlen = std::min(srcLength, destLength);
    int ret = fmemcmp(src, dest, minlen);
    if (ret) {
      return ret;
    }
    return srcLength - destLength;
};

int FloatComparator(const char * src, uint32_t srcLength, const char * dest, uint32_t destLength) {
  if (srcLength != 4 || destLength != 4) {
      THROW_EXCEPTION_EX(IOException, "float comparator, while src/dest lengt is not 4");
    }
    float * srcValue = (float *)src;
    float * destValue = (float *)dest;
    return ( (*srcValue) - (* destValue) >= 0) ? 1 : -1;
};

int DoubleComparator(const char * src, uint32_t srcLength, const char * dest, uint32_t destLength) {
  if (srcLength != 8 || destLength != 8) {
      THROW_EXCEPTION_EX(IOException, "double comparator, while src/dest lengt is not 4");
    }
    double * srcValue = (double *)src;
    double * destValue = (double *)dest;
    return (((*srcValue) - (* destValue) >= 0) ? 1 : -1);
};

ComparatorPtr get_default_comparator(const KeyValueType keyType, const char * comparatorName) {
  if (NULL == comparatorName) {
    if (keyType == BytesType ||
        keyType == TextType ||
        keyType == ByteType ||
        keyType == BoolType ||
        keyType == IntType ||
        keyType == LongType) {
      return &BytesComparator;
    }
    else if (keyType == FloatType) {
      return &FloatComparator;
    }
    else if (keyType == DoubleType) {
      return &DoubleComparator;
    }
  }
  else {
    void * func = NativeObjectFactory::GetFunction(string(comparatorName));
    return (ComparatorPtr)func;
  }
  return NULL;
}

} // namespace NativeTask

