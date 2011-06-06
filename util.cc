// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/util.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>

#include "base/eintr_wrapper.h"
#include "base/memory/scoped_ptr.h"
#include "base/string_util.h"
#include "base/time.h"

using base::TimeDelta;
using base::TimeTicks;
using std::max;
using std::min;
using std::string;
using std::vector;

namespace {

// Directory listing the current process's open file descriptors.
const char kFdListDir[] = "/proc/self/fd";

// Close all open file descriptors besides stdin, stdout, and stderr.
// Returns false on failure.
bool CloseExtraFds() {
  DIR* dir = opendir(kFdListDir);
  if (!dir) {
    RAW_LOG(ERROR, "Unable to read FD list from /proc");
    return false;
  }

  bool success = true;
  while (true) {
    errno = 0;
    struct dirent* entry = readdir(dir);
    if (!entry) {
      success = !errno;
      break;
    }

    // Skip '.' and '..'.
    if (entry->d_name[0] == '.')
      continue;

    char* end_ptr = NULL;
    const long int fd = strtol(entry->d_name, &end_ptr, 10);
    if (*(entry->d_name) == '\0' || *end_ptr != '\0')
      continue;

    if (fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO)
      continue;

    // Don't close the FD that we're using to read the directory.
    if (fd == dirfd(dir))
      continue;

    HANDLE_EINTR(close(fd));
  }

  closedir(dir);
  return success;
}

}  // namespace

namespace window_manager {

// If non-negative, contains a hardcoded time to be returned by
// GetCurrentTimeSecs() and GetCurrentTimeMs().
static int64_t current_time_ms_for_test = -1;

// If non-zero, contains a hardcoded time to be returned by
// GetMonotonicTimeMs().
static TimeTicks monotonic_time_for_test;

ByteMap::ByteMap(const Size& size) : bytes_(NULL) {
  Resize(size);
}

ByteMap::~ByteMap() {
  delete[] bytes_;
  bytes_ = NULL;
}

void ByteMap::Resize(const Size& new_size) {
  size_ = new_size;
  delete[] bytes_;
  bytes_ = size_.empty() ? NULL : new unsigned char[size_.area()];
  Clear(0);
}

void ByteMap::Copy(const ByteMap& other) {
  if (size_.empty() || other.size_.empty())
    return;

  if (size_ == other.size_) {
    memcpy(bytes_, other.bytes_, size_.area());
  } else {
    const int copy_width = min(size_.width, other.size_.width);
    const int copy_height = min(size_.height, other.size_.height);
    for (int y = 0; y < copy_height; ++y) {
      memcpy(bytes_ + y * size_.width,
             other.bytes_ + y * other.size_.width,
             copy_width);
    }
  }
}

void ByteMap::Clear(unsigned char value) {
  if (size_.empty())
    return;
  memset(bytes_, value, size_.area());
}

void ByteMap::SetRectangle(const Rect& rect, unsigned char value) {
  if (rect.empty() || size_.empty())
    return;

  const int limit_x = min(rect.x + rect.width, size_.width);
  const int limit_y = min(rect.y + rect.height, size_.height);
  const int capped_x = max(rect.x, 0);
  const int capped_y = max(rect.y, 0);

  if (capped_x >= limit_x)
    return;

  for (int y = capped_y; y < limit_y; ++y)
    memset(bytes_ + y * size_.width + capped_x, value, limit_x - capped_x);
}

bool ByteMap::operator==(const ByteMap& other) {
  if (size_ != other.size_)
    return false;
  if (size_.empty())
    return true;
  return memcmp(bytes_, other.bytes_, size_.area()) == 0;
}


namespace util {

string XidStr(unsigned long xid) {
  return StringPrintf("0x%lx", xid);
}

string GetTimeAsString(time_t utime) {
  struct tm tm;
  CHECK(localtime_r(&utime, &tm) == &tm);
  char str[16];
  CHECK(strftime(str, sizeof(str), "%Y%m%d-%H%M%S", &tm) == 15);
  return string(str);
}

time_t GetCurrentTimeSec() {
  if (current_time_ms_for_test >= 0)
    return static_cast<time_t>(current_time_ms_for_test / 1000);
  return time(NULL);
}

int64_t GetCurrentTimeMs() {
  if (current_time_ms_for_test >= 0)
    return current_time_ms_for_test;
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return 1000ULL * tv.tv_sec + tv.tv_usec / 1000ULL;
}

void SetCurrentTimeForTest(time_t sec, int ms) {
  current_time_ms_for_test =
      (sec < 0) ? -1 : static_cast<int64_t>(sec) * 1000 + ms;
}

TimeTicks GetMonotonicTime() {
  if (!monotonic_time_for_test.is_null())
    return monotonic_time_for_test;
  return TimeTicks::Now();
}

void SetMonotonicTimeForTest(const TimeTicks& now) {
  monotonic_time_for_test = now;
}

TimeTicks CreateTimeTicksFromMs(int64_t time_ms) {
  TimeTicks t;
  int64_t diff_usec = time_ms * 1000 - t.ToInternalValue();
  t += TimeDelta::FromMicroseconds(diff_usec);
  return t;
}

bool SetUpLogSymlink(const std::string& symlink_path,
                     const std::string& log_basename) {
  if (access(symlink_path.c_str(), F_OK) == 0 &&
      unlink(symlink_path.c_str()) == -1) {
    PLOG(ERROR) << "Unable to unlink " << symlink_path;
    return false;
  }
  if (symlink(log_basename.c_str(), symlink_path.c_str()) == -1) {
    PLOG(ERROR) << "Unable to create symlink " << symlink_path
                << " pointing at " << log_basename;
    return false;
  }
  return true;
}

string GetHostname() {
  char hostname[256];
  if (gethostname(hostname, sizeof(hostname)) != 0) {
    PLOG(ERROR) << "Unable to look up hostname";
    return string();
  }
  hostname[sizeof(hostname) - 1] = '\0';
  return string(hostname);
}

pid_t RunCommandInBackground(const vector<string>& argv) {
  if (argv.empty() || argv[0].empty())
    return -1;

  string command = JoinString(argv, ' ');
  DLOG(INFO) << "Running command \"" << command << "\"";

  pid_t pid = fork();
  if (pid == -1) {
    PLOG(ERROR) << "fork() failed";
    return -1;
  }

  if (pid == 0) {
    // Child.
    if (!CloseExtraFds()) {
      RAW_LOG(ERROR, "Got error while closing FDs");
      _exit(127);
    }

    // Set stdin to /dev/null.
    int null_fd = HANDLE_EINTR(open("/dev/null", O_RDONLY));
    if (null_fd == -1) {
      RAW_LOG(ERROR, "Failed to open /dev/null");
      _exit(127);
    }
    int new_fd = HANDLE_EINTR(dup2(null_fd, STDIN_FILENO));
    HANDLE_EINTR(close(null_fd));
    if (new_fd == -1) {
      RAW_LOG(ERROR, "Failed to set stdin to /dev/null");
      _exit(127);
    }

    scoped_array<char*> args(new char*[argv.size() + 1]);
    for (size_t i = 0; i < argv.size(); ++i)
      args[i] = const_cast<char*>(argv.at(i).c_str());
    args[argv.size()] = NULL;
    execvp(argv.at(0).c_str(), args.get());
    RAW_LOG(ERROR, "execvp() failed");
    _exit(127);

  } else {
    // Parent.
    return pid;
  }
}

void RunCommandInBackgroundCallback(vector<string> argv) {
  RunCommandInBackground(argv);
}

void ToggleBool(bool* value) {
  DCHECK(value);
  *value = !(*value);
}

}  // namespace util

}  // namespace window_manager
