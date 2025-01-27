/*
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define _GNU_SOURCE 1
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include <backtrace/Backtrace.h>
#include <backtrace/BacktraceMap.h>

#include <android-base/macros.h>
#include <android-base/stringprintf.h>
#include <android-base/unique_fd.h>
#include <cutils/atomic.h>
#include <cutils/threads.h>

#include <gtest/gtest.h>

// For the THREAD_SIGNAL definition.
#include "BacktraceCurrent.h"
#include "thread_utils.h"

// Number of microseconds per milliseconds.
#define US_PER_MSEC             1000

// Number of nanoseconds in a second.
#define NS_PER_SEC              1000000000ULL

// Number of simultaneous dumping operations to perform.
#define NUM_THREADS  40

// Number of simultaneous threads running in our forked process.
#define NUM_PTRACE_THREADS 5

struct thread_t {
  pid_t tid;
  int32_t state;
  pthread_t threadId;
  void* data;
};

struct dump_thread_t {
  thread_t thread;
  Backtrace* backtrace;
  int32_t* now;
  int32_t done;
};

extern "C" {
// Prototypes for functions in the test library.
int test_level_one(int, int, int, int, void (*)(void*), void*);

int test_recursive_call(int, void (*)(void*), void*);
}

static uint64_t NanoTime() {
  struct timespec t = { 0, 0 };
  clock_gettime(CLOCK_MONOTONIC, &t);
  return static_cast<uint64_t>(t.tv_sec * NS_PER_SEC + t.tv_nsec);
}

static std::string DumpFrames(Backtrace* backtrace) {
  if (backtrace->NumFrames() == 0) {
    return "   No frames to dump.\n";
  }

  std::string frame;
  for (size_t i = 0; i < backtrace->NumFrames(); i++) {
    frame += "   " + backtrace->FormatFrameData(i) + '\n';
  }
  return frame;
}

static void WaitForStop(pid_t pid) {
  uint64_t start = NanoTime();

  siginfo_t si;
  while (ptrace(PTRACE_GETSIGINFO, pid, 0, &si) < 0 && (errno == EINTR || errno == ESRCH)) {
    if ((NanoTime() - start) > NS_PER_SEC) {
      printf("The process did not get to a stopping point in 1 second.\n");
      break;
    }
    usleep(US_PER_MSEC);
  }
}

static void CreateRemoteProcess(pid_t* pid) {
  if ((*pid = fork()) == 0) {
    while (true)
      ;
    _exit(0);
  }
  ASSERT_NE(-1, *pid);

  ASSERT_TRUE(ptrace(PTRACE_ATTACH, *pid, 0, 0) == 0);

  // Wait for the process to get to a stopping point.
  WaitForStop(*pid);
}

static void FinishRemoteProcess(pid_t pid) {
  ASSERT_TRUE(ptrace(PTRACE_DETACH, pid, 0, 0) == 0);

  kill(pid, SIGKILL);
  ASSERT_EQ(waitpid(pid, nullptr, 0), pid);
}

static bool ReadyLevelBacktrace(Backtrace* backtrace) {
  // See if test_level_four is in the backtrace.
  bool found = false;
  for (Backtrace::const_iterator it = backtrace->begin(); it != backtrace->end(); ++it) {
    if (it->func_name == "test_level_four") {
      found = true;
      break;
    }
  }

  return found;
}

static void VerifyLevelDump(Backtrace* backtrace) {
  ASSERT_GT(backtrace->NumFrames(), static_cast<size_t>(0))
    << DumpFrames(backtrace);
  ASSERT_LT(backtrace->NumFrames(), static_cast<size_t>(MAX_BACKTRACE_FRAMES))
    << DumpFrames(backtrace);

  // Look through the frames starting at the highest to find the
  // frame we want.
  size_t frame_num = 0;
  for (size_t i = backtrace->NumFrames()-1; i > 2; i--) {
    if (backtrace->GetFrame(i)->func_name == "test_level_one") {
      frame_num = i;
      break;
    }
  }
  ASSERT_LT(static_cast<size_t>(0), frame_num) << DumpFrames(backtrace);
  ASSERT_LE(static_cast<size_t>(3), frame_num) << DumpFrames(backtrace);

  ASSERT_EQ(backtrace->GetFrame(frame_num)->func_name, "test_level_one")
    << DumpFrames(backtrace);
  ASSERT_EQ(backtrace->GetFrame(frame_num-1)->func_name, "test_level_two")
    << DumpFrames(backtrace);
  ASSERT_EQ(backtrace->GetFrame(frame_num-2)->func_name, "test_level_three")
    << DumpFrames(backtrace);
  ASSERT_EQ(backtrace->GetFrame(frame_num-3)->func_name, "test_level_four")
    << DumpFrames(backtrace);
}

static void VerifyLevelBacktrace(void*) {
  std::unique_ptr<Backtrace> backtrace(
      Backtrace::Create(BACKTRACE_CURRENT_PROCESS, BACKTRACE_CURRENT_THREAD));
  ASSERT_TRUE(backtrace.get() != nullptr);
  ASSERT_TRUE(backtrace->Unwind(0));
  ASSERT_EQ(BACKTRACE_UNWIND_NO_ERROR, backtrace->GetError());

  VerifyLevelDump(backtrace.get());
}

static bool ReadyMaxBacktrace(Backtrace* backtrace) {
  return (backtrace->NumFrames() == MAX_BACKTRACE_FRAMES);
}

static void VerifyMaxDump(Backtrace* backtrace) {
  ASSERT_EQ(backtrace->NumFrames(), static_cast<size_t>(MAX_BACKTRACE_FRAMES))
    << DumpFrames(backtrace);
  // Verify that the last frame is our recursive call.
  ASSERT_EQ(backtrace->GetFrame(MAX_BACKTRACE_FRAMES-1)->func_name, "test_recursive_call")
    << DumpFrames(backtrace);
}

static void VerifyMaxBacktrace(void*) {
  std::unique_ptr<Backtrace> backtrace(
      Backtrace::Create(BACKTRACE_CURRENT_PROCESS, BACKTRACE_CURRENT_THREAD));
  ASSERT_TRUE(backtrace.get() != nullptr);
  ASSERT_TRUE(backtrace->Unwind(0));
  ASSERT_EQ(BACKTRACE_UNWIND_NO_ERROR, backtrace->GetError());

  VerifyMaxDump(backtrace.get());
}

static void ThreadSetState(void* data) {
  thread_t* thread = reinterpret_cast<thread_t*>(data);
  android_atomic_acquire_store(1, &thread->state);
  volatile int i = 0;
  while (thread->state) {
    i++;
  }
}

static bool WaitForNonZero(int32_t* value, uint64_t seconds) {
  uint64_t start = NanoTime();
  do {
    if (android_atomic_acquire_load(value)) {
      return true;
    }
  } while ((NanoTime() - start) < seconds * NS_PER_SEC);
  return false;
}

TEST(libbacktrace, local_no_unwind_frames) {
  // Verify that a local unwind does not include any frames within
  // libunwind or libbacktrace.
  std::unique_ptr<Backtrace> backtrace(Backtrace::Create(getpid(), getpid()));
  ASSERT_TRUE(backtrace.get() != nullptr);
  ASSERT_TRUE(backtrace->Unwind(0));
  ASSERT_EQ(BACKTRACE_UNWIND_NO_ERROR, backtrace->GetError());

  ASSERT_TRUE(backtrace->NumFrames() != 0);
  for (const auto& frame : *backtrace ) {
    if (BacktraceMap::IsValid(frame.map)) {
      const std::string name = basename(frame.map.name.c_str());
      ASSERT_TRUE(name != "libunwind.so" && name != "libbacktrace.so")
        << DumpFrames(backtrace.get());
    }
    break;
  }
}

TEST(libbacktrace, local_trace) {
  ASSERT_NE(test_level_one(1, 2, 3, 4, VerifyLevelBacktrace, nullptr), 0);
}

static void VerifyIgnoreFrames(Backtrace* bt_all, Backtrace* bt_ign1, Backtrace* bt_ign2,
                               const char* cur_proc) {
  EXPECT_EQ(bt_all->NumFrames(), bt_ign1->NumFrames() + 1)
    << "All backtrace:\n" << DumpFrames(bt_all) << "Ignore 1 backtrace:\n" << DumpFrames(bt_ign1);
  EXPECT_EQ(bt_all->NumFrames(), bt_ign2->NumFrames() + 2)
    << "All backtrace:\n" << DumpFrames(bt_all) << "Ignore 2 backtrace:\n" << DumpFrames(bt_ign2);

  // Check all of the frames are the same > the current frame.
  bool check = (cur_proc == nullptr);
  for (size_t i = 0; i < bt_ign2->NumFrames(); i++) {
    if (check) {
      EXPECT_EQ(bt_ign2->GetFrame(i)->pc, bt_ign1->GetFrame(i+1)->pc);
      EXPECT_EQ(bt_ign2->GetFrame(i)->sp, bt_ign1->GetFrame(i+1)->sp);
      EXPECT_EQ(bt_ign2->GetFrame(i)->stack_size, bt_ign1->GetFrame(i+1)->stack_size);

      EXPECT_EQ(bt_ign2->GetFrame(i)->pc, bt_all->GetFrame(i+2)->pc);
      EXPECT_EQ(bt_ign2->GetFrame(i)->sp, bt_all->GetFrame(i+2)->sp);
      EXPECT_EQ(bt_ign2->GetFrame(i)->stack_size, bt_all->GetFrame(i+2)->stack_size);
    }
    if (!check && bt_ign2->GetFrame(i)->func_name == cur_proc) {
      check = true;
    }
  }
}

static void VerifyLevelIgnoreFrames(void*) {
  std::unique_ptr<Backtrace> all(
      Backtrace::Create(BACKTRACE_CURRENT_PROCESS, BACKTRACE_CURRENT_THREAD));
  ASSERT_TRUE(all.get() != nullptr);
  ASSERT_TRUE(all->Unwind(0));
  ASSERT_EQ(BACKTRACE_UNWIND_NO_ERROR, all->GetError());

  std::unique_ptr<Backtrace> ign1(
      Backtrace::Create(BACKTRACE_CURRENT_PROCESS, BACKTRACE_CURRENT_THREAD));
  ASSERT_TRUE(ign1.get() != nullptr);
  ASSERT_TRUE(ign1->Unwind(1));
  ASSERT_EQ(BACKTRACE_UNWIND_NO_ERROR, ign1->GetError());

  std::unique_ptr<Backtrace> ign2(
      Backtrace::Create(BACKTRACE_CURRENT_PROCESS, BACKTRACE_CURRENT_THREAD));
  ASSERT_TRUE(ign2.get() != nullptr);
  ASSERT_TRUE(ign2->Unwind(2));
  ASSERT_EQ(BACKTRACE_UNWIND_NO_ERROR, ign2->GetError());

  VerifyIgnoreFrames(all.get(), ign1.get(), ign2.get(), "VerifyLevelIgnoreFrames");
}

TEST(libbacktrace, local_trace_ignore_frames) {
  ASSERT_NE(test_level_one(1, 2, 3, 4, VerifyLevelIgnoreFrames, nullptr), 0);
}

TEST(libbacktrace, local_max_trace) {
  ASSERT_NE(test_recursive_call(MAX_BACKTRACE_FRAMES+10, VerifyMaxBacktrace, nullptr), 0);
}

static void VerifyProcTest(pid_t pid, pid_t tid, bool share_map, bool (*ReadyFunc)(Backtrace*),
                           void (*VerifyFunc)(Backtrace*)) {
  pid_t ptrace_tid;
  if (tid < 0) {
    ptrace_tid = pid;
  } else {
    ptrace_tid = tid;
  }
  uint64_t start = NanoTime();
  bool verified = false;
  std::string last_dump;
  do {
    usleep(US_PER_MSEC);
    if (ptrace(PTRACE_ATTACH, ptrace_tid, 0, 0) == 0) {
      // Wait for the process to get to a stopping point.
      WaitForStop(ptrace_tid);

      std::unique_ptr<BacktraceMap> map;
      if (share_map) {
        map.reset(BacktraceMap::Create(pid));
      }
      std::unique_ptr<Backtrace> backtrace(Backtrace::Create(pid, tid, map.get()));
      ASSERT_TRUE(backtrace.get() != nullptr);
      ASSERT_TRUE(backtrace->Unwind(0));
      ASSERT_EQ(BACKTRACE_UNWIND_NO_ERROR, backtrace->GetError());
      if (ReadyFunc(backtrace.get())) {
        VerifyFunc(backtrace.get());
        verified = true;
      } else {
        last_dump = DumpFrames(backtrace.get());
      }

      ASSERT_TRUE(ptrace(PTRACE_DETACH, ptrace_tid, 0, 0) == 0);
    }
    // If 5 seconds have passed, then we are done.
  } while (!verified && (NanoTime() - start) <= 5 * NS_PER_SEC);
  ASSERT_TRUE(verified) << "Last backtrace:\n" << last_dump;
}

TEST(libbacktrace, ptrace_trace) {
  pid_t pid;
  if ((pid = fork()) == 0) {
    ASSERT_NE(test_level_one(1, 2, 3, 4, nullptr, nullptr), 0);
    _exit(1);
  }
  VerifyProcTest(pid, BACKTRACE_CURRENT_THREAD, false, ReadyLevelBacktrace, VerifyLevelDump);

  kill(pid, SIGKILL);
  int status;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
}

TEST(libbacktrace, ptrace_trace_shared_map) {
  pid_t pid;
  if ((pid = fork()) == 0) {
    ASSERT_NE(test_level_one(1, 2, 3, 4, nullptr, nullptr), 0);
    _exit(1);
  }

  VerifyProcTest(pid, BACKTRACE_CURRENT_THREAD, true, ReadyLevelBacktrace, VerifyLevelDump);

  kill(pid, SIGKILL);
  int status;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
}

TEST(libbacktrace, ptrace_max_trace) {
  pid_t pid;
  if ((pid = fork()) == 0) {
    ASSERT_NE(test_recursive_call(MAX_BACKTRACE_FRAMES+10, nullptr, nullptr), 0);
    _exit(1);
  }
  VerifyProcTest(pid, BACKTRACE_CURRENT_THREAD, false, ReadyMaxBacktrace, VerifyMaxDump);

  kill(pid, SIGKILL);
  int status;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
}

static void VerifyProcessIgnoreFrames(Backtrace* bt_all) {
  std::unique_ptr<Backtrace> ign1(Backtrace::Create(bt_all->Pid(), BACKTRACE_CURRENT_THREAD));
  ASSERT_TRUE(ign1.get() != nullptr);
  ASSERT_TRUE(ign1->Unwind(1));
  ASSERT_EQ(BACKTRACE_UNWIND_NO_ERROR, ign1->GetError());

  std::unique_ptr<Backtrace> ign2(Backtrace::Create(bt_all->Pid(), BACKTRACE_CURRENT_THREAD));
  ASSERT_TRUE(ign2.get() != nullptr);
  ASSERT_TRUE(ign2->Unwind(2));
  ASSERT_EQ(BACKTRACE_UNWIND_NO_ERROR, ign2->GetError());

  VerifyIgnoreFrames(bt_all, ign1.get(), ign2.get(), nullptr);
}

TEST(libbacktrace, ptrace_ignore_frames) {
  pid_t pid;
  if ((pid = fork()) == 0) {
    ASSERT_NE(test_level_one(1, 2, 3, 4, nullptr, nullptr), 0);
    _exit(1);
  }
  VerifyProcTest(pid, BACKTRACE_CURRENT_THREAD, false, ReadyLevelBacktrace, VerifyProcessIgnoreFrames);

  kill(pid, SIGKILL);
  int status;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
}

// Create a process with multiple threads and dump all of the threads.
static void* PtraceThreadLevelRun(void*) {
  EXPECT_NE(test_level_one(1, 2, 3, 4, nullptr, nullptr), 0);
  return nullptr;
}

static void GetThreads(pid_t pid, std::vector<pid_t>* threads) {
  // Get the list of tasks.
  char task_path[128];
  snprintf(task_path, sizeof(task_path), "/proc/%d/task", pid);

  std::unique_ptr<DIR, decltype(&closedir)> tasks_dir(opendir(task_path), closedir);
  ASSERT_TRUE(tasks_dir != nullptr);
  struct dirent* entry;
  while ((entry = readdir(tasks_dir.get())) != nullptr) {
    char* end;
    pid_t tid = strtoul(entry->d_name, &end, 10);
    if (*end == '\0') {
      threads->push_back(tid);
    }
  }
}

TEST(libbacktrace, ptrace_threads) {
  pid_t pid;
  if ((pid = fork()) == 0) {
    for (size_t i = 0; i < NUM_PTRACE_THREADS; i++) {
      pthread_attr_t attr;
      pthread_attr_init(&attr);
      pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

      pthread_t thread;
      ASSERT_TRUE(pthread_create(&thread, &attr, PtraceThreadLevelRun, nullptr) == 0);
    }
    ASSERT_NE(test_level_one(1, 2, 3, 4, nullptr, nullptr), 0);
    _exit(1);
  }

  // Check to see that all of the threads are running before unwinding.
  std::vector<pid_t> threads;
  uint64_t start = NanoTime();
  do {
    usleep(US_PER_MSEC);
    threads.clear();
    GetThreads(pid, &threads);
  } while ((threads.size() != NUM_PTRACE_THREADS + 1) &&
      ((NanoTime() - start) <= 5 * NS_PER_SEC));
  ASSERT_EQ(threads.size(), static_cast<size_t>(NUM_PTRACE_THREADS + 1));

  ASSERT_TRUE(ptrace(PTRACE_ATTACH, pid, 0, 0) == 0);
  WaitForStop(pid);
  for (std::vector<int>::const_iterator it = threads.begin(); it != threads.end(); ++it) {
    // Skip the current forked process, we only care about the threads.
    if (pid == *it) {
      continue;
    }
    VerifyProcTest(pid, *it, false, ReadyLevelBacktrace, VerifyLevelDump);
  }

  FinishRemoteProcess(pid);
}

void VerifyLevelThread(void*) {
  std::unique_ptr<Backtrace> backtrace(Backtrace::Create(getpid(), gettid()));
  ASSERT_TRUE(backtrace.get() != nullptr);
  ASSERT_TRUE(backtrace->Unwind(0));
  ASSERT_EQ(BACKTRACE_UNWIND_NO_ERROR, backtrace->GetError());

  VerifyLevelDump(backtrace.get());
}

TEST(libbacktrace, thread_current_level) {
  ASSERT_NE(test_level_one(1, 2, 3, 4, VerifyLevelThread, nullptr), 0);
}

static void VerifyMaxThread(void*) {
  std::unique_ptr<Backtrace> backtrace(Backtrace::Create(getpid(), gettid()));
  ASSERT_TRUE(backtrace.get() != nullptr);
  ASSERT_TRUE(backtrace->Unwind(0));
  ASSERT_EQ(BACKTRACE_UNWIND_NO_ERROR, backtrace->GetError());

  VerifyMaxDump(backtrace.get());
}

TEST(libbacktrace, thread_current_max) {
  ASSERT_NE(test_recursive_call(MAX_BACKTRACE_FRAMES+10, VerifyMaxThread, nullptr), 0);
}

static void* ThreadLevelRun(void* data) {
  thread_t* thread = reinterpret_cast<thread_t*>(data);

  thread->tid = gettid();
  EXPECT_NE(test_level_one(1, 2, 3, 4, ThreadSetState, data), 0);
  return nullptr;
}

TEST(libbacktrace, thread_level_trace) {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  thread_t thread_data = { 0, 0, 0, nullptr };
  pthread_t thread;
  ASSERT_TRUE(pthread_create(&thread, &attr, ThreadLevelRun, &thread_data) == 0);

  // Wait up to 2 seconds for the tid to be set.
  ASSERT_TRUE(WaitForNonZero(&thread_data.state, 2));

  // Make sure that the thread signal used is not visible when compiled for
  // the target.
#if !defined(__GLIBC__)
  ASSERT_LT(THREAD_SIGNAL, SIGRTMIN);
#endif

  // Save the current signal action and make sure it is restored afterwards.
  struct sigaction cur_action;
  ASSERT_TRUE(sigaction(THREAD_SIGNAL, nullptr, &cur_action) == 0);

  std::unique_ptr<Backtrace> backtrace(Backtrace::Create(getpid(), thread_data.tid));
  ASSERT_TRUE(backtrace.get() != nullptr);
  ASSERT_TRUE(backtrace->Unwind(0));
  ASSERT_EQ(BACKTRACE_UNWIND_NO_ERROR, backtrace->GetError());

  VerifyLevelDump(backtrace.get());

  // Tell the thread to exit its infinite loop.
  android_atomic_acquire_store(0, &thread_data.state);

  // Verify that the old action was restored.
  struct sigaction new_action;
  ASSERT_TRUE(sigaction(THREAD_SIGNAL, nullptr, &new_action) == 0);
  EXPECT_EQ(cur_action.sa_sigaction, new_action.sa_sigaction);
  // The SA_RESTORER flag gets set behind our back, so a direct comparison
  // doesn't work unless we mask the value off. Mips doesn't have this
  // flag, so skip this on that platform.
#if defined(SA_RESTORER)
  cur_action.sa_flags &= ~SA_RESTORER;
  new_action.sa_flags &= ~SA_RESTORER;
#elif defined(__GLIBC__)
  // Our host compiler doesn't appear to define this flag for some reason.
  cur_action.sa_flags &= ~0x04000000;
  new_action.sa_flags &= ~0x04000000;
#endif
  EXPECT_EQ(cur_action.sa_flags, new_action.sa_flags);
}

TEST(libbacktrace, thread_ignore_frames) {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  thread_t thread_data = { 0, 0, 0, nullptr };
  pthread_t thread;
  ASSERT_TRUE(pthread_create(&thread, &attr, ThreadLevelRun, &thread_data) == 0);

  // Wait up to 2 seconds for the tid to be set.
  ASSERT_TRUE(WaitForNonZero(&thread_data.state, 2));

  std::unique_ptr<Backtrace> all(Backtrace::Create(getpid(), thread_data.tid));
  ASSERT_TRUE(all.get() != nullptr);
  ASSERT_TRUE(all->Unwind(0));
  ASSERT_EQ(BACKTRACE_UNWIND_NO_ERROR, all->GetError());

  std::unique_ptr<Backtrace> ign1(Backtrace::Create(getpid(), thread_data.tid));
  ASSERT_TRUE(ign1.get() != nullptr);
  ASSERT_TRUE(ign1->Unwind(1));
  ASSERT_EQ(BACKTRACE_UNWIND_NO_ERROR, ign1->GetError());

  std::unique_ptr<Backtrace> ign2(Backtrace::Create(getpid(), thread_data.tid));
  ASSERT_TRUE(ign2.get() != nullptr);
  ASSERT_TRUE(ign2->Unwind(2));
  ASSERT_EQ(BACKTRACE_UNWIND_NO_ERROR, ign2->GetError());

  VerifyIgnoreFrames(all.get(), ign1.get(), ign2.get(), nullptr);

  // Tell the thread to exit its infinite loop.
  android_atomic_acquire_store(0, &thread_data.state);
}

static void* ThreadMaxRun(void* data) {
  thread_t* thread = reinterpret_cast<thread_t*>(data);

  thread->tid = gettid();
  EXPECT_NE(test_recursive_call(MAX_BACKTRACE_FRAMES+10, ThreadSetState, data), 0);
  return nullptr;
}

TEST(libbacktrace, thread_max_trace) {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  thread_t thread_data = { 0, 0, 0, nullptr };
  pthread_t thread;
  ASSERT_TRUE(pthread_create(&thread, &attr, ThreadMaxRun, &thread_data) == 0);

  // Wait for the tid to be set.
  ASSERT_TRUE(WaitForNonZero(&thread_data.state, 2));

  std::unique_ptr<Backtrace> backtrace(Backtrace::Create(getpid(), thread_data.tid));
  ASSERT_TRUE(backtrace.get() != nullptr);
  ASSERT_TRUE(backtrace->Unwind(0));
  ASSERT_EQ(BACKTRACE_UNWIND_NO_ERROR, backtrace->GetError());

  VerifyMaxDump(backtrace.get());

  // Tell the thread to exit its infinite loop.
  android_atomic_acquire_store(0, &thread_data.state);
}

static void* ThreadDump(void* data) {
  dump_thread_t* dump = reinterpret_cast<dump_thread_t*>(data);
  while (true) {
    if (android_atomic_acquire_load(dump->now)) {
      break;
    }
  }

  // The status of the actual unwind will be checked elsewhere.
  dump->backtrace = Backtrace::Create(getpid(), dump->thread.tid);
  dump->backtrace->Unwind(0);

  android_atomic_acquire_store(1, &dump->done);

  return nullptr;
}

TEST(libbacktrace, thread_multiple_dump) {
  // Dump NUM_THREADS simultaneously.
  std::vector<thread_t> runners(NUM_THREADS);
  std::vector<dump_thread_t> dumpers(NUM_THREADS);

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  for (size_t i = 0; i < NUM_THREADS; i++) {
    // Launch the runners, they will spin in hard loops doing nothing.
    runners[i].tid = 0;
    runners[i].state = 0;
    ASSERT_TRUE(pthread_create(&runners[i].threadId, &attr, ThreadMaxRun, &runners[i]) == 0);
  }

  // Wait for tids to be set.
  for (std::vector<thread_t>::iterator it = runners.begin(); it != runners.end(); ++it) {
    ASSERT_TRUE(WaitForNonZero(&it->state, 30));
  }

  // Start all of the dumpers at once, they will spin until they are signalled
  // to begin their dump run.
  int32_t dump_now = 0;
  for (size_t i = 0; i < NUM_THREADS; i++) {
    dumpers[i].thread.tid = runners[i].tid;
    dumpers[i].thread.state = 0;
    dumpers[i].done = 0;
    dumpers[i].now = &dump_now;

    ASSERT_TRUE(pthread_create(&dumpers[i].thread.threadId, &attr, ThreadDump, &dumpers[i]) == 0);
  }

  // Start all of the dumpers going at once.
  android_atomic_acquire_store(1, &dump_now);

  for (size_t i = 0; i < NUM_THREADS; i++) {
    ASSERT_TRUE(WaitForNonZero(&dumpers[i].done, 30));

    // Tell the runner thread to exit its infinite loop.
    android_atomic_acquire_store(0, &runners[i].state);

    ASSERT_TRUE(dumpers[i].backtrace != nullptr);
    VerifyMaxDump(dumpers[i].backtrace);

    delete dumpers[i].backtrace;
    dumpers[i].backtrace = nullptr;
  }
}

TEST(libbacktrace, thread_multiple_dump_same_thread) {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  thread_t runner;
  runner.tid = 0;
  runner.state = 0;
  ASSERT_TRUE(pthread_create(&runner.threadId, &attr, ThreadMaxRun, &runner) == 0);

  // Wait for tids to be set.
  ASSERT_TRUE(WaitForNonZero(&runner.state, 30));

  // Start all of the dumpers at once, they will spin until they are signalled
  // to begin their dump run.
  int32_t dump_now = 0;
  // Dump the same thread NUM_THREADS simultaneously.
  std::vector<dump_thread_t> dumpers(NUM_THREADS);
  for (size_t i = 0; i < NUM_THREADS; i++) {
    dumpers[i].thread.tid = runner.tid;
    dumpers[i].thread.state = 0;
    dumpers[i].done = 0;
    dumpers[i].now = &dump_now;

    ASSERT_TRUE(pthread_create(&dumpers[i].thread.threadId, &attr, ThreadDump, &dumpers[i]) == 0);
  }

  // Start all of the dumpers going at once.
  android_atomic_acquire_store(1, &dump_now);

  for (size_t i = 0; i < NUM_THREADS; i++) {
    ASSERT_TRUE(WaitForNonZero(&dumpers[i].done, 30));

    ASSERT_TRUE(dumpers[i].backtrace != nullptr);
    VerifyMaxDump(dumpers[i].backtrace);

    delete dumpers[i].backtrace;
    dumpers[i].backtrace = nullptr;
  }

  // Tell the runner thread to exit its infinite loop.
  android_atomic_acquire_store(0, &runner.state);
}

// This test is for UnwindMaps that should share the same map cursor when
// multiple maps are created for the current process at the same time.
TEST(libbacktrace, simultaneous_maps) {
  BacktraceMap* map1 = BacktraceMap::Create(getpid());
  BacktraceMap* map2 = BacktraceMap::Create(getpid());
  BacktraceMap* map3 = BacktraceMap::Create(getpid());

  Backtrace* back1 = Backtrace::Create(getpid(), BACKTRACE_CURRENT_THREAD, map1);
  ASSERT_TRUE(back1 != nullptr);
  EXPECT_TRUE(back1->Unwind(0));
  ASSERT_EQ(BACKTRACE_UNWIND_NO_ERROR, back1->GetError());
  delete back1;
  delete map1;

  Backtrace* back2 = Backtrace::Create(getpid(), BACKTRACE_CURRENT_THREAD, map2);
  ASSERT_TRUE(back2 != nullptr);
  EXPECT_TRUE(back2->Unwind(0));
  ASSERT_EQ(BACKTRACE_UNWIND_NO_ERROR, back2->GetError());
  delete back2;
  delete map2;

  Backtrace* back3 = Backtrace::Create(getpid(), BACKTRACE_CURRENT_THREAD, map3);
  ASSERT_TRUE(back3 != nullptr);
  EXPECT_TRUE(back3->Unwind(0));
  ASSERT_EQ(BACKTRACE_UNWIND_NO_ERROR, back3->GetError());
  delete back3;
  delete map3;
}

TEST(libbacktrace, fillin_erases) {
  BacktraceMap* back_map = BacktraceMap::Create(getpid());

  backtrace_map_t map;

  map.start = 1;
  map.end = 3;
  map.flags = 1;
  map.name = "Initialized";
  back_map->FillIn(0, &map);
  delete back_map;

  ASSERT_FALSE(BacktraceMap::IsValid(map));
  ASSERT_EQ(static_cast<uintptr_t>(0), map.start);
  ASSERT_EQ(static_cast<uintptr_t>(0), map.end);
  ASSERT_EQ(0, map.flags);
  ASSERT_EQ("", map.name);
}

TEST(libbacktrace, format_test) {
  std::unique_ptr<Backtrace> backtrace(Backtrace::Create(getpid(), BACKTRACE_CURRENT_THREAD));
  ASSERT_TRUE(backtrace.get() != nullptr);

  backtrace_frame_data_t frame;
  frame.num = 1;
  frame.pc = 2;
  frame.sp = 0;
  frame.stack_size = 0;
  frame.func_offset = 0;

  // Check no map set.
  frame.num = 1;
#if defined(__LP64__)
  EXPECT_EQ("#01 pc 0000000000000002  <unknown>",
#else
  EXPECT_EQ("#01 pc 00000002  <unknown>",
#endif
            backtrace->FormatFrameData(&frame));

  // Check map name empty, but exists.
  frame.pc = 0xb0020;
  frame.map.start = 0xb0000;
  frame.map.end = 0xbffff;
  frame.map.load_base = 0;
#if defined(__LP64__)
  EXPECT_EQ("#01 pc 0000000000000020  <anonymous:00000000000b0000>",
#else
  EXPECT_EQ("#01 pc 00000020  <anonymous:000b0000>",
#endif
            backtrace->FormatFrameData(&frame));

  // Check map name begins with a [.
  frame.pc = 0xc0020;
  frame.map.start = 0xc0000;
  frame.map.end = 0xcffff;
  frame.map.load_base = 0;
  frame.map.name = "[anon:thread signal stack]";
#if defined(__LP64__)
  EXPECT_EQ("#01 pc 0000000000000020  [anon:thread signal stack:00000000000c0000]",
#else
  EXPECT_EQ("#01 pc 00000020  [anon:thread signal stack:000c0000]",
#endif
            backtrace->FormatFrameData(&frame));

  // Check relative pc is set and map name is set.
  frame.pc = 0x12345679;
  frame.map.name = "MapFake";
  frame.map.start =  1;
  frame.map.end =  1;
#if defined(__LP64__)
  EXPECT_EQ("#01 pc 0000000012345678  MapFake",
#else
  EXPECT_EQ("#01 pc 12345678  MapFake",
#endif
            backtrace->FormatFrameData(&frame));

  // Check func_name is set, but no func offset.
  frame.func_name = "ProcFake";
#if defined(__LP64__)
  EXPECT_EQ("#01 pc 0000000012345678  MapFake (ProcFake)",
#else
  EXPECT_EQ("#01 pc 12345678  MapFake (ProcFake)",
#endif
            backtrace->FormatFrameData(&frame));

  // Check func_name is set, and func offset is non-zero.
  frame.func_offset = 645;
#if defined(__LP64__)
  EXPECT_EQ("#01 pc 0000000012345678  MapFake (ProcFake+645)",
#else
  EXPECT_EQ("#01 pc 12345678  MapFake (ProcFake+645)",
#endif
            backtrace->FormatFrameData(&frame));

  // Check func_name is set, func offset is non-zero, and load_base is non-zero.
  frame.func_offset = 645;
  frame.map.load_base = 100;
#if defined(__LP64__)
  EXPECT_EQ("#01 pc 00000000123456dc  MapFake (ProcFake+645)",
#else
  EXPECT_EQ("#01 pc 123456dc  MapFake (ProcFake+645)",
#endif
            backtrace->FormatFrameData(&frame));

  // Check a non-zero map offset.
  frame.map.offset = 0x1000;
#if defined(__LP64__)
  EXPECT_EQ("#01 pc 00000000123456dc  MapFake (offset 0x1000) (ProcFake+645)",
#else
  EXPECT_EQ("#01 pc 123456dc  MapFake (offset 0x1000) (ProcFake+645)",
#endif
            backtrace->FormatFrameData(&frame));
}

struct map_test_t {
  uintptr_t start;
  uintptr_t end;
};

static bool map_sort(map_test_t i, map_test_t j) { return i.start < j.start; }

static void VerifyMap(pid_t pid) {
  char buffer[4096];
  snprintf(buffer, sizeof(buffer), "/proc/%d/maps", pid);

  FILE* map_file = fopen(buffer, "r");
  ASSERT_TRUE(map_file != nullptr);
  std::vector<map_test_t> test_maps;
  while (fgets(buffer, sizeof(buffer), map_file)) {
    map_test_t map;
    ASSERT_EQ(2, sscanf(buffer, "%" SCNxPTR "-%" SCNxPTR " ", &map.start, &map.end));
    test_maps.push_back(map);
  }
  fclose(map_file);
  std::sort(test_maps.begin(), test_maps.end(), map_sort);

  std::unique_ptr<BacktraceMap> map(BacktraceMap::Create(pid));

  // Basic test that verifies that the map is in the expected order.
  ScopedBacktraceMapIteratorLock lock(map.get());
  std::vector<map_test_t>::const_iterator test_it = test_maps.begin();
  for (BacktraceMap::const_iterator it = map->begin(); it != map->end(); ++it) {
    ASSERT_TRUE(test_it != test_maps.end());
    ASSERT_EQ(test_it->start, it->start);
    ASSERT_EQ(test_it->end, it->end);
    ++test_it;
  }
  ASSERT_TRUE(test_it == test_maps.end());
}

TEST(libbacktrace, verify_map_remote) {
  pid_t pid;
  CreateRemoteProcess(&pid);

  // The maps should match exactly since the forked process has been paused.
  VerifyMap(pid);

  FinishRemoteProcess(pid);
}

static void InitMemory(uint8_t* memory, size_t bytes) {
  for (size_t i = 0; i < bytes; i++) {
    memory[i] = i;
    if (memory[i] == '\0') {
      // Don't use '\0' in our data so we can verify that an overread doesn't
      // occur by using a '\0' as the character after the read data.
      memory[i] = 23;
    }
  }
}

static void* ThreadReadTest(void* data) {
  thread_t* thread_data = reinterpret_cast<thread_t*>(data);

  thread_data->tid = gettid();

  // Create two map pages.
  // Mark the second page as not-readable.
  size_t pagesize = static_cast<size_t>(sysconf(_SC_PAGE_SIZE));
  uint8_t* memory;
  if (posix_memalign(reinterpret_cast<void**>(&memory), pagesize, 2 * pagesize) != 0) {
    return reinterpret_cast<void*>(-1);
  }

  if (mprotect(&memory[pagesize], pagesize, PROT_NONE) != 0) {
    return reinterpret_cast<void*>(-1);
  }

  // Set up a simple pattern in memory.
  InitMemory(memory, pagesize);

  thread_data->data = memory;

  // Tell the caller it's okay to start reading memory.
  android_atomic_acquire_store(1, &thread_data->state);

  // Loop waiting for the caller to finish reading the memory.
  while (thread_data->state) {
  }

  // Re-enable read-write on the page so that we don't crash if we try
  // and access data on this page when freeing the memory.
  if (mprotect(&memory[pagesize], pagesize, PROT_READ | PROT_WRITE) != 0) {
    return reinterpret_cast<void*>(-1);
  }
  free(memory);

  android_atomic_acquire_store(1, &thread_data->state);

  return nullptr;
}

static void RunReadTest(Backtrace* backtrace, uintptr_t read_addr) {
  size_t pagesize = static_cast<size_t>(sysconf(_SC_PAGE_SIZE));

  // Create a page of data to use to do quick compares.
  uint8_t* expected = new uint8_t[pagesize];
  InitMemory(expected, pagesize);

  uint8_t* data = new uint8_t[2*pagesize];
  // Verify that we can only read one page worth of data.
  size_t bytes_read = backtrace->Read(read_addr, data, 2 * pagesize);
  ASSERT_EQ(pagesize, bytes_read);
  ASSERT_TRUE(memcmp(data, expected, pagesize) == 0);

  // Verify unaligned reads.
  for (size_t i = 1; i < sizeof(word_t); i++) {
    bytes_read = backtrace->Read(read_addr + i, data, 2 * sizeof(word_t));
    ASSERT_EQ(2 * sizeof(word_t), bytes_read);
    ASSERT_TRUE(memcmp(data, &expected[i], 2 * sizeof(word_t)) == 0)
        << "Offset at " << i << " failed";
  }

  // Verify small unaligned reads.
  for (size_t i = 1; i < sizeof(word_t); i++) {
    for (size_t j = 1; j < sizeof(word_t); j++) {
      // Set one byte past what we expect to read, to guarantee we don't overread.
      data[j] = '\0';
      bytes_read = backtrace->Read(read_addr + i, data, j);
      ASSERT_EQ(j, bytes_read);
      ASSERT_TRUE(memcmp(data, &expected[i], j) == 0)
          << "Offset at " << i << " length " << j << " miscompared";
      ASSERT_EQ('\0', data[j])
          << "Offset at " << i << " length " << j << " wrote too much data";
    }
  }
  delete[] data;
  delete[] expected;
}

TEST(libbacktrace, thread_read) {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_t thread;
  thread_t thread_data = { 0, 0, 0, nullptr };
  ASSERT_TRUE(pthread_create(&thread, &attr, ThreadReadTest, &thread_data) == 0);

  ASSERT_TRUE(WaitForNonZero(&thread_data.state, 10));

  std::unique_ptr<Backtrace> backtrace(Backtrace::Create(getpid(), thread_data.tid));
  ASSERT_TRUE(backtrace.get() != nullptr);

  RunReadTest(backtrace.get(), reinterpret_cast<uintptr_t>(thread_data.data));

  android_atomic_acquire_store(0, &thread_data.state);

  ASSERT_TRUE(WaitForNonZero(&thread_data.state, 10));
}

volatile uintptr_t g_ready = 0;
volatile uintptr_t g_addr = 0;

static void ForkedReadTest() {
  // Create two map pages.
  size_t pagesize = static_cast<size_t>(sysconf(_SC_PAGE_SIZE));
  uint8_t* memory;
  if (posix_memalign(reinterpret_cast<void**>(&memory), pagesize, 2 * pagesize) != 0) {
    perror("Failed to allocate memory\n");
    exit(1);
  }

  // Mark the second page as not-readable.
  if (mprotect(&memory[pagesize], pagesize, PROT_NONE) != 0) {
    perror("Failed to mprotect memory\n");
    exit(1);
  }

  // Set up a simple pattern in memory.
  InitMemory(memory, pagesize);

  g_addr = reinterpret_cast<uintptr_t>(memory);
  g_ready = 1;

  while (1) {
    usleep(US_PER_MSEC);
  }
}

TEST(libbacktrace, process_read) {
  g_ready = 0;
  pid_t pid;
  if ((pid = fork()) == 0) {
    ForkedReadTest();
    exit(0);
  }
  ASSERT_NE(-1, pid);

  bool test_executed = false;
  uint64_t start = NanoTime();
  while (1) {
    if (ptrace(PTRACE_ATTACH, pid, 0, 0) == 0) {
      WaitForStop(pid);

      std::unique_ptr<Backtrace> backtrace(Backtrace::Create(pid, pid));
      ASSERT_TRUE(backtrace.get() != nullptr);

      uintptr_t read_addr;
      size_t bytes_read = backtrace->Read(reinterpret_cast<uintptr_t>(&g_ready),
                                          reinterpret_cast<uint8_t*>(&read_addr),
                                          sizeof(uintptr_t));
      ASSERT_EQ(sizeof(uintptr_t), bytes_read);
      if (read_addr) {
        // The forked process is ready to be read.
        bytes_read = backtrace->Read(reinterpret_cast<uintptr_t>(&g_addr),
                                     reinterpret_cast<uint8_t*>(&read_addr),
                                     sizeof(uintptr_t));
        ASSERT_EQ(sizeof(uintptr_t), bytes_read);

        RunReadTest(backtrace.get(), read_addr);

        test_executed = true;
        break;
      }
      ASSERT_TRUE(ptrace(PTRACE_DETACH, pid, 0, 0) == 0);
    }
    if ((NanoTime() - start) > 5 * NS_PER_SEC) {
      break;
    }
    usleep(US_PER_MSEC);
  }
  kill(pid, SIGKILL);
  ASSERT_EQ(waitpid(pid, nullptr, 0), pid);

  ASSERT_TRUE(test_executed);
}

static void VerifyFunctionsFound(const std::vector<std::string>& found_functions) {
  // We expect to find these functions in libbacktrace_test. If we don't
  // find them, that's a bug in the memory read handling code in libunwind.
  std::list<std::string> expected_functions;
  expected_functions.push_back("test_recursive_call");
  expected_functions.push_back("test_level_one");
  expected_functions.push_back("test_level_two");
  expected_functions.push_back("test_level_three");
  expected_functions.push_back("test_level_four");
  for (const auto& found_function : found_functions) {
    for (const auto& expected_function : expected_functions) {
      if (found_function == expected_function) {
        expected_functions.remove(found_function);
        break;
      }
    }
  }
  ASSERT_TRUE(expected_functions.empty()) << "Not all functions found in shared library.";
}

static const char* CopySharedLibrary() {
#if defined(__LP64__)
  const char* lib_name = "lib64";
#else
  const char* lib_name = "lib";
#endif

#if defined(__BIONIC__)
  const char* tmp_so_name = "/data/local/tmp/libbacktrace_test.so";
  std::string cp_cmd = android::base::StringPrintf("cp /system/%s/libbacktrace_test.so %s",
                                                   lib_name, tmp_so_name);
#else
  const char* tmp_so_name = "/tmp/libbacktrace_test.so";
  if (getenv("ANDROID_HOST_OUT") == NULL) {
    fprintf(stderr, "ANDROID_HOST_OUT not set, make sure you run lunch.");
    return nullptr;
  }
  std::string cp_cmd = android::base::StringPrintf("cp %s/%s/libbacktrace_test.so %s",
                                                   getenv("ANDROID_HOST_OUT"), lib_name,
                                                   tmp_so_name);
#endif

  // Copy the shared so to a tempory directory.
  system(cp_cmd.c_str());

  return tmp_so_name;
}

TEST(libbacktrace, check_unreadable_elf_local) {
  const char* tmp_so_name = CopySharedLibrary();
  ASSERT_TRUE(tmp_so_name != nullptr);

  struct stat buf;
  ASSERT_TRUE(stat(tmp_so_name, &buf) != -1);
  uintptr_t map_size = buf.st_size;

  int fd = open(tmp_so_name, O_RDONLY);
  ASSERT_TRUE(fd != -1);

  void* map = mmap(NULL, map_size, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
  ASSERT_TRUE(map != MAP_FAILED);
  close(fd);
  ASSERT_TRUE(unlink(tmp_so_name) != -1);

  std::vector<std::string> found_functions;
  std::unique_ptr<Backtrace> backtrace(Backtrace::Create(BACKTRACE_CURRENT_PROCESS,
                                                         BACKTRACE_CURRENT_THREAD));
  ASSERT_TRUE(backtrace.get() != nullptr);

  // Needed before GetFunctionName will work.
  backtrace->Unwind(0);

  // Loop through the entire map, and get every function we can find.
  map_size += reinterpret_cast<uintptr_t>(map);
  std::string last_func;
  for (uintptr_t read_addr = reinterpret_cast<uintptr_t>(map);
       read_addr < map_size; read_addr += 4) {
    uintptr_t offset;
    std::string func_name = backtrace->GetFunctionName(read_addr, &offset);
    if (!func_name.empty() && last_func != func_name) {
      found_functions.push_back(func_name);
    }
    last_func = func_name;
  }

  ASSERT_TRUE(munmap(map, map_size - reinterpret_cast<uintptr_t>(map)) == 0);

  VerifyFunctionsFound(found_functions);
}

TEST(libbacktrace, check_unreadable_elf_remote) {
  const char* tmp_so_name = CopySharedLibrary();
  ASSERT_TRUE(tmp_so_name != nullptr);

  g_ready = 0;

  struct stat buf;
  ASSERT_TRUE(stat(tmp_so_name, &buf) != -1);
  uintptr_t map_size = buf.st_size;

  pid_t pid;
  if ((pid = fork()) == 0) {
    int fd = open(tmp_so_name, O_RDONLY);
    if (fd == -1) {
      fprintf(stderr, "Failed to open file %s: %s\n", tmp_so_name, strerror(errno));
      unlink(tmp_so_name);
      exit(0);
    }

    void* map = mmap(NULL, map_size, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
      fprintf(stderr, "Failed to map in memory: %s\n", strerror(errno));
      unlink(tmp_so_name);
      exit(0);
    }
    close(fd);
    if (unlink(tmp_so_name) == -1) {
      fprintf(stderr, "Failed to unlink: %s\n", strerror(errno));
      exit(0);
    }

    g_addr = reinterpret_cast<uintptr_t>(map);
    g_ready = 1;
    while (true) {
      usleep(US_PER_MSEC);
    }
    exit(0);
  }
  ASSERT_TRUE(pid > 0);

  std::vector<std::string> found_functions;
  uint64_t start = NanoTime();
  while (true) {
    ASSERT_TRUE(ptrace(PTRACE_ATTACH, pid, 0, 0) == 0);

    // Wait for the process to get to a stopping point.
    WaitForStop(pid);

    std::unique_ptr<Backtrace> backtrace(Backtrace::Create(pid, BACKTRACE_CURRENT_THREAD));
    ASSERT_TRUE(backtrace.get() != nullptr);

    uintptr_t read_addr;
    ASSERT_EQ(sizeof(uintptr_t), backtrace->Read(reinterpret_cast<uintptr_t>(&g_ready), reinterpret_cast<uint8_t*>(&read_addr), sizeof(uintptr_t)));
    if (read_addr) {
      ASSERT_EQ(sizeof(uintptr_t), backtrace->Read(reinterpret_cast<uintptr_t>(&g_addr), reinterpret_cast<uint8_t*>(&read_addr), sizeof(uintptr_t)));

      // Needed before GetFunctionName will work.
      backtrace->Unwind(0);

      // Loop through the entire map, and get every function we can find.
      map_size += read_addr;
      std::string last_func;
      for (; read_addr < map_size; read_addr += 4) {
        uintptr_t offset;
        std::string func_name = backtrace->GetFunctionName(read_addr, &offset);
        if (!func_name.empty() && last_func != func_name) {
          found_functions.push_back(func_name);
        }
        last_func = func_name;
      }
      break;
    }
    ASSERT_TRUE(ptrace(PTRACE_DETACH, pid, 0, 0) == 0);

    if ((NanoTime() - start) > 5 * NS_PER_SEC) {
      break;
    }
    usleep(US_PER_MSEC);
  }

  kill(pid, SIGKILL);
  ASSERT_EQ(waitpid(pid, nullptr, 0), pid);

  VerifyFunctionsFound(found_functions);
}

static bool FindFuncFrameInBacktrace(Backtrace* backtrace, uintptr_t test_func, size_t* frame_num) {
  backtrace_map_t map;
  backtrace->FillInMap(test_func, &map);
  if (!BacktraceMap::IsValid(map)) {
    return false;
  }

  // Loop through the frames, and find the one that is in the map.
  *frame_num = 0;
  for (Backtrace::const_iterator it = backtrace->begin(); it != backtrace->end(); ++it) {
    if (BacktraceMap::IsValid(it->map) && map.start == it->map.start &&
        it->pc >= test_func) {
      *frame_num = it->num;
      return true;
    }
  }
  return false;
}

static void VerifyUnreadableElfFrame(Backtrace* backtrace, uintptr_t test_func, size_t frame_num) {
  ASSERT_LT(backtrace->NumFrames(), static_cast<size_t>(MAX_BACKTRACE_FRAMES))
    << DumpFrames(backtrace);

  ASSERT_TRUE(frame_num != 0) << DumpFrames(backtrace);
  // Make sure that there is at least one more frame above the test func call.
  ASSERT_LT(frame_num, backtrace->NumFrames()) << DumpFrames(backtrace);

  uintptr_t diff = backtrace->GetFrame(frame_num)->pc - test_func;
  ASSERT_LT(diff, 200U) << DumpFrames(backtrace);
}

static void VerifyUnreadableElfBacktrace(uintptr_t test_func) {
  std::unique_ptr<Backtrace> backtrace(Backtrace::Create(BACKTRACE_CURRENT_PROCESS,
                                                         BACKTRACE_CURRENT_THREAD));
  ASSERT_TRUE(backtrace.get() != nullptr);
  ASSERT_TRUE(backtrace->Unwind(0));
  ASSERT_EQ(BACKTRACE_UNWIND_NO_ERROR, backtrace->GetError());

  size_t frame_num;
  ASSERT_TRUE(FindFuncFrameInBacktrace(backtrace.get(), test_func, &frame_num));

  VerifyUnreadableElfFrame(backtrace.get(), test_func, frame_num);
}

typedef int (*test_func_t)(int, int, int, int, void (*)(uintptr_t), uintptr_t);

TEST(libbacktrace, unwind_through_unreadable_elf_local) {
  const char* tmp_so_name = CopySharedLibrary();
  ASSERT_TRUE(tmp_so_name != nullptr);
  void* lib_handle = dlopen(tmp_so_name, RTLD_NOW);
  ASSERT_TRUE(lib_handle != nullptr);
  ASSERT_TRUE(unlink(tmp_so_name) != -1);

  test_func_t test_func;
  test_func = reinterpret_cast<test_func_t>(dlsym(lib_handle, "test_level_one"));
  ASSERT_TRUE(test_func != nullptr);

  ASSERT_NE(test_func(1, 2, 3, 4, VerifyUnreadableElfBacktrace,
                      reinterpret_cast<uintptr_t>(test_func)), 0);

  ASSERT_TRUE(dlclose(lib_handle) == 0);
}

TEST(libbacktrace, unwind_through_unreadable_elf_remote) {
  const char* tmp_so_name = CopySharedLibrary();
  ASSERT_TRUE(tmp_so_name != nullptr);
  void* lib_handle = dlopen(tmp_so_name, RTLD_NOW);
  ASSERT_TRUE(lib_handle != nullptr);
  ASSERT_TRUE(unlink(tmp_so_name) != -1);

  test_func_t test_func;
  test_func = reinterpret_cast<test_func_t>(dlsym(lib_handle, "test_level_one"));
  ASSERT_TRUE(test_func != nullptr);

  pid_t pid;
  if ((pid = fork()) == 0) {
    test_func(1, 2, 3, 4, 0, 0);
    exit(0);
  }
  ASSERT_TRUE(pid > 0);
  ASSERT_TRUE(dlclose(lib_handle) == 0);

  uint64_t start = NanoTime();
  bool done = false;
  while (!done) {
    ASSERT_TRUE(ptrace(PTRACE_ATTACH, pid, 0, 0) == 0);

    // Wait for the process to get to a stopping point.
    WaitForStop(pid);

    std::unique_ptr<Backtrace> backtrace(Backtrace::Create(pid, BACKTRACE_CURRENT_THREAD));
    ASSERT_TRUE(backtrace.get() != nullptr);
    ASSERT_TRUE(backtrace->Unwind(0));
    ASSERT_EQ(BACKTRACE_UNWIND_NO_ERROR, backtrace->GetError());

    size_t frame_num;
    if (FindFuncFrameInBacktrace(backtrace.get(),
                                 reinterpret_cast<uintptr_t>(test_func), &frame_num)) {

      VerifyUnreadableElfFrame(backtrace.get(), reinterpret_cast<uintptr_t>(test_func), frame_num);
      done = true;
    }

    ASSERT_TRUE(ptrace(PTRACE_DETACH, pid, 0, 0) == 0);

    if ((NanoTime() - start) > 5 * NS_PER_SEC) {
      break;
    }
    usleep(US_PER_MSEC);
  }

  kill(pid, SIGKILL);
  ASSERT_EQ(waitpid(pid, nullptr, 0), pid);

  ASSERT_TRUE(done) << "Test function never found in unwind.";
}

TEST(libbacktrace, unwind_thread_doesnt_exist) {
  std::unique_ptr<Backtrace> backtrace(
      Backtrace::Create(BACKTRACE_CURRENT_PROCESS, 99999999));
  ASSERT_TRUE(backtrace.get() != nullptr);
  ASSERT_FALSE(backtrace->Unwind(0));
  ASSERT_EQ(BACKTRACE_UNWIND_ERROR_THREAD_DOESNT_EXIST, backtrace->GetError());
}

TEST(libbacktrace, local_get_function_name_before_unwind) {
  std::unique_ptr<Backtrace> backtrace(
      Backtrace::Create(BACKTRACE_CURRENT_PROCESS, BACKTRACE_CURRENT_THREAD));
  ASSERT_TRUE(backtrace.get() != nullptr);

  // Verify that trying to get a function name before doing an unwind works.
  uintptr_t cur_func_offset = reinterpret_cast<uintptr_t>(&test_level_one) + 1;
  size_t offset;
  ASSERT_NE(std::string(""), backtrace->GetFunctionName(cur_func_offset, &offset));
}

TEST(libbacktrace, remote_get_function_name_before_unwind) {
  pid_t pid;
  CreateRemoteProcess(&pid);

  // Now create an unwind object.
  std::unique_ptr<Backtrace> backtrace(Backtrace::Create(pid, pid));

  // Verify that trying to get a function name before doing an unwind works.
  uintptr_t cur_func_offset = reinterpret_cast<uintptr_t>(&test_level_one) + 1;
  size_t offset;
  ASSERT_NE(std::string(""), backtrace->GetFunctionName(cur_func_offset, &offset));

  FinishRemoteProcess(pid);
}

static void SetUcontextSp(uintptr_t sp, ucontext_t* ucontext) {
#if defined(__arm__)
  ucontext->uc_mcontext.arm_sp = sp;
#elif defined(__aarch64__)
  ucontext->uc_mcontext.sp = sp;
#elif defined(__i386__)
  ucontext->uc_mcontext.gregs[REG_ESP] = sp;
#elif defined(__x86_64__)
  ucontext->uc_mcontext.gregs[REG_RSP] = sp;
#else
  UNUSED(sp);
  UNUSED(ucontext);
  ASSERT_TRUE(false) << "Unsupported architecture";
#endif
}

static void SetUcontextPc(uintptr_t pc, ucontext_t* ucontext) {
#if defined(__arm__)
  ucontext->uc_mcontext.arm_pc = pc;
#elif defined(__aarch64__)
  ucontext->uc_mcontext.pc = pc;
#elif defined(__i386__)
  ucontext->uc_mcontext.gregs[REG_EIP] = pc;
#elif defined(__x86_64__)
  ucontext->uc_mcontext.gregs[REG_RIP] = pc;
#else
  UNUSED(pc);
  UNUSED(ucontext);
  ASSERT_TRUE(false) << "Unsupported architecture";
#endif
}

static void SetUcontextLr(uintptr_t lr, ucontext_t* ucontext) {
#if defined(__arm__)
  ucontext->uc_mcontext.arm_lr = lr;
#elif defined(__aarch64__)
  ucontext->uc_mcontext.regs[30] = lr;
#elif defined(__i386__)
  // The lr is on the stack.
  ASSERT_TRUE(lr != 0);
  ASSERT_TRUE(ucontext != nullptr);
#elif defined(__x86_64__)
  // The lr is on the stack.
  ASSERT_TRUE(lr != 0);
  ASSERT_TRUE(ucontext != nullptr);
#else
  UNUSED(lr);
  UNUSED(ucontext);
  ASSERT_TRUE(false) << "Unsupported architecture";
#endif
}

static constexpr size_t DEVICE_MAP_SIZE = 1024;

static void SetupDeviceMap(void** device_map) {
  // Make sure that anything in a device map will result in fails
  // to read.
  android::base::unique_fd device_fd(open("/dev/zero", O_RDONLY | O_CLOEXEC));

  *device_map = mmap(nullptr, 1024, PROT_READ, MAP_PRIVATE, device_fd, 0);
  ASSERT_TRUE(*device_map != MAP_FAILED);

  // Make sure the map is readable.
  ASSERT_EQ(0, reinterpret_cast<int*>(*device_map)[0]);
}

static void UnwindFromDevice(Backtrace* backtrace, void* device_map) {
  uintptr_t device_map_uint = reinterpret_cast<uintptr_t>(device_map);

  backtrace_map_t map;
  backtrace->FillInMap(device_map_uint, &map);
  // Verify the flag is set.
  ASSERT_EQ(PROT_DEVICE_MAP, map.flags & PROT_DEVICE_MAP);

  // Quick sanity checks.
  size_t offset;
  ASSERT_EQ(std::string(""), backtrace->GetFunctionName(device_map_uint, &offset));
  ASSERT_EQ(std::string(""), backtrace->GetFunctionName(device_map_uint, &offset, &map));
  ASSERT_EQ(std::string(""), backtrace->GetFunctionName(0, &offset));

  uintptr_t cur_func_offset = reinterpret_cast<uintptr_t>(&test_level_one) + 1;
  // Now verify the device map flag actually causes the function name to be empty.
  backtrace->FillInMap(cur_func_offset, &map);
  ASSERT_TRUE((map.flags & PROT_DEVICE_MAP) == 0);
  ASSERT_NE(std::string(""), backtrace->GetFunctionName(cur_func_offset, &offset, &map));
  map.flags |= PROT_DEVICE_MAP;
  ASSERT_EQ(std::string(""), backtrace->GetFunctionName(cur_func_offset, &offset, &map));

  ucontext_t ucontext;

  // Create a context that has the pc in the device map, but the sp
  // in a non-device map.
  memset(&ucontext, 0, sizeof(ucontext));
  SetUcontextSp(reinterpret_cast<uintptr_t>(&ucontext), &ucontext);
  SetUcontextPc(device_map_uint, &ucontext);
  SetUcontextLr(cur_func_offset, &ucontext);

  ASSERT_TRUE(backtrace->Unwind(0, &ucontext));

  // The buffer should only be a single element.
  ASSERT_EQ(1U, backtrace->NumFrames());
  const backtrace_frame_data_t* frame = backtrace->GetFrame(0);
  ASSERT_EQ(device_map_uint, frame->pc);
  ASSERT_EQ(reinterpret_cast<uintptr_t>(&ucontext), frame->sp);

  // Check what happens when skipping the first frame.
  ASSERT_TRUE(backtrace->Unwind(1, &ucontext));
  ASSERT_EQ(0U, backtrace->NumFrames());

  // Create a context that has the sp in the device map, but the pc
  // in a non-device map.
  memset(&ucontext, 0, sizeof(ucontext));
  SetUcontextSp(device_map_uint, &ucontext);
  SetUcontextPc(cur_func_offset, &ucontext);
  SetUcontextLr(cur_func_offset, &ucontext);

  ASSERT_TRUE(backtrace->Unwind(0, &ucontext));

  // The buffer should only be a single element.
  ASSERT_EQ(1U, backtrace->NumFrames());
  frame = backtrace->GetFrame(0);
  ASSERT_EQ(cur_func_offset, frame->pc);
  ASSERT_EQ(device_map_uint, frame->sp);

  // Check what happens when skipping the first frame.
  ASSERT_TRUE(backtrace->Unwind(1, &ucontext));
  ASSERT_EQ(0U, backtrace->NumFrames());
}

TEST(libbacktrace, unwind_disallow_device_map_local) {
  void* device_map;
  SetupDeviceMap(&device_map);

  // Now create an unwind object.
  std::unique_ptr<Backtrace> backtrace(
      Backtrace::Create(BACKTRACE_CURRENT_PROCESS, BACKTRACE_CURRENT_THREAD));
  ASSERT_TRUE(backtrace);

  UnwindFromDevice(backtrace.get(), device_map);

  munmap(device_map, DEVICE_MAP_SIZE);
}

TEST(libbacktrace, unwind_disallow_device_map_remote) {
  void* device_map;
  SetupDeviceMap(&device_map);

  // Fork a process to do a remote backtrace.
  pid_t pid;
  CreateRemoteProcess(&pid);

  // Now create an unwind object.
  std::unique_ptr<Backtrace> backtrace(Backtrace::Create(pid, pid));

  // TODO: Currently unwind from context doesn't work on remote
  // unwind. Keep this test because the new unwinder should support
  // this eventually, or we can delete this test.
  // properly with unwind from context.
  // UnwindFromDevice(backtrace.get(), device_map);

  FinishRemoteProcess(pid);

  munmap(device_map, DEVICE_MAP_SIZE);
}

#if defined(ENABLE_PSS_TESTS)
#include "GetPss.h"

#define MAX_LEAK_BYTES (32*1024UL)

static void CheckForLeak(pid_t pid, pid_t tid) {
  // Do a few runs to get the PSS stable.
  for (size_t i = 0; i < 100; i++) {
    Backtrace* backtrace = Backtrace::Create(pid, tid);
    ASSERT_TRUE(backtrace != nullptr);
    ASSERT_TRUE(backtrace->Unwind(0));
    ASSERT_EQ(BACKTRACE_UNWIND_NO_ERROR, backtrace->GetError());
    delete backtrace;
  }
  size_t stable_pss = GetPssBytes();
  ASSERT_TRUE(stable_pss != 0);

  // Loop enough that even a small leak should be detectable.
  for (size_t i = 0; i < 4096; i++) {
    Backtrace* backtrace = Backtrace::Create(pid, tid);
    ASSERT_TRUE(backtrace != nullptr);
    ASSERT_TRUE(backtrace->Unwind(0));
    ASSERT_EQ(BACKTRACE_UNWIND_NO_ERROR, backtrace->GetError());
    delete backtrace;
  }
  size_t new_pss = GetPssBytes();
  ASSERT_TRUE(new_pss != 0);
  if (new_pss > stable_pss) {
    ASSERT_LE(new_pss - stable_pss, MAX_LEAK_BYTES);
  }
}

TEST(libbacktrace, check_for_leak_local) {
  CheckForLeak(BACKTRACE_CURRENT_PROCESS, BACKTRACE_CURRENT_THREAD);
}

TEST(libbacktrace, check_for_leak_local_thread) {
  thread_t thread_data = { 0, 0, 0, nullptr };
  pthread_t thread;
  ASSERT_TRUE(pthread_create(&thread, nullptr, ThreadLevelRun, &thread_data) == 0);

  // Wait up to 2 seconds for the tid to be set.
  ASSERT_TRUE(WaitForNonZero(&thread_data.state, 2));

  CheckForLeak(BACKTRACE_CURRENT_PROCESS, thread_data.tid);

  // Tell the thread to exit its infinite loop.
  android_atomic_acquire_store(0, &thread_data.state);

  ASSERT_TRUE(pthread_join(thread, nullptr) == 0);
}

TEST(libbacktrace, check_for_leak_remote) {
  pid_t pid;
  CreateRemoteProcess(&pid);

  CheckForLeak(pid, BACKTRACE_CURRENT_THREAD);

  FinishRemoteProcess(pid);
}
#endif
