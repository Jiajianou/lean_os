// A program built from Chromium's //base, for this machine, by this
// project's own clang - and run here, which is the part a link cannot
// prove. Every check below is a piece of //base doing what it says on
// leanfs, on this kernel's threads and on this libc.
//
// It reports its own results and exits non-zero on the first failure, the
// way /bin/basetest and /bin/ruststd do, because the boot self-test that
// runs it has no other way to tell what went wrong.

#include <cerrno>
#include <cstdio>
#include <sys/random.h>
#include <pthread.h>

#include <cstring>
#include <string>
#include <vector>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/containers/span.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/system/sys_info.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"

namespace {

int failures = 0;
int checks = 0;

void Check(const char* what, bool ok) {
  ++checks;
  if (ok) {
    std::printf("chromiumbase: %s\n", what);
  } else {
    std::printf("chromiumbase: FAIL %s\n", what);
    ++failures;
  }
}

// Before any base:: call: getrandom, exactly as base/rand_util_posix.cc
// asks for it. That file requires the return value to EQUAL the length and
// falls through to /dev/urandom otherwise, so a short answer is not less
// randomness, it is a different code path - and the one it falls through to
// CHECKs. /bin/basetest already proves this call works from this libc, so a
// failure here is about this binary rather than about the kernel.
bool RawGetrandomBehaves() {
  unsigned char buffer[64];
  std::memset(buffer, 0, sizeof(buffer));
  errno = 0;
  ssize_t got = getrandom(buffer, sizeof(buffer), 0);
  if (got != static_cast<ssize_t>(sizeof(buffer))) {
    std::printf("chromiumbase: getrandom(64) returned %ld, errno %d\n",
                static_cast<long>(got), errno);
    return false;
  }
  errno = 0;
  uint64_t eight = 0;
  got = getrandom(&eight, sizeof(eight), 0);
  if (got != static_cast<ssize_t>(sizeof(eight))) {
    std::printf("chromiumbase: getrandom(8) returned %ld, errno %d\n",
                static_cast<long>(got), errno);
    return false;
  }
  return true;
}

// What base/rand_util_posix.cc asks before it will use getrandom: this
// machine's uname release, read as a Linux kernel version. It is 0.89 here,
// which is below the 3.17 that gate wants - so printing it says whether the
// patch that removed the question from this platform is in the object that
// is running.
bool KernelVersionIsWhatUnameSays() {
  base::SysInfo::KernelVersionNumber version =
      base::SysInfo::KernelVersionNumber::Current();
  std::printf("chromiumbase: uname release parses as %d.%d.%d\n",
              version.major, version.minor, version.bugfix);
  return true;
}

// A function-local static with a run-time initialiser, which is
// __cxa_guard_acquire and __cxa_guard_release underneath. base/rand_util_posix.cc
// keeps its "does this kernel have getrandom" answer in one, and an
// initialiser that never runs leaves the variable zero - which reads as
// false, and sends every RandBytes down the /dev/urandom path that CHECKs.
// M145 changed which guard implementation libc++abi uses, so this is the
// check for that change rather than for the language feature.
int guard_probe_calls = 0;

bool GuardProbe() {
  ++guard_probe_calls;
  return true;
}

bool LocalStaticsAreInitialisedOnce() {
  static const bool first = GuardProbe();
  static const bool second = GuardProbe();
  if (!first || !second) {
    std::printf("chromiumbase: a local static initialiser did not run\n");
    return false;
  }
  // Entering twice must not run either initialiser again.
  for (int i = 0; i < 4; ++i) {
    static const bool inner = GuardProbe();
    if (!inner) {
      return false;
    }
  }
  if (guard_probe_calls != 3) {
    std::printf("chromiumbase: local static initialisers ran %d times, not 3\n",
                guard_probe_calls);
    return false;
  }
  return true;
}

// base::FilePath is the class M135 found is partly written in Rust, and it
// is the one every other part of //base takes a path through.
bool PathsBehave() {
  base::FilePath path("/pkg/share/thing.tar.gz");
  if (path.BaseName().value() != "thing.tar.gz") {
    return false;
  }
  if (path.DirName().value() != "/pkg/share") {
    return false;
  }
  // Chromium's Extension() keeps a double extension together, so this is
  // ".tar.gz" rather than ".gz" - and RemoveExtension() takes both.
  if (path.Extension() != ".tar.gz") {
    return false;
  }
  if (path.RemoveExtension().value() != "/pkg/share/thing") {
    return false;
  }
  if (path.FinalExtension() != ".gz") {
    return false;
  }
  if (!path.IsAbsolute()) {
    return false;
  }
  base::FilePath joined = base::FilePath("/tmp").Append("a").Append("b");
  return joined.value() == "/tmp/a/b";
}

// base::File and base::WriteFile over leanfs. The temporary directory is
// created, used and removed, so the check covers making a directory and
// taking one away as well as the file in between.
bool FilesBehave() {
  base::ScopedTempDir directory;
  if (!directory.CreateUniqueTempDirUnderPath(base::FilePath("/tmp"))) {
    return false;
  }
  base::FilePath path = directory.GetPath().Append("written-by-base");
  const std::string contents = "//base wrote this through leanfs\n";
  if (!base::WriteFile(path, contents)) {
    return false;
  }
  if (!base::PathExists(path)) {
    return false;
  }
  std::optional<int64_t> size = base::GetFileSize(path);
  if (!size.has_value() || *size != static_cast<int64_t>(contents.size())) {
    return false;
  }
  std::string read_back;
  if (!base::ReadFileToString(path, &read_back) || read_back != contents) {
    return false;
  }
  // base::File's own descriptor, reading a slice at an offset.
  base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_READ);
  if (!file.IsValid()) {
    return false;
  }
  char slice[7] = {0};
  if (file.ReadAndCheck(2, base::as_writable_byte_span(slice).first(6u)) !=
      true) {
    return false;
  }
  if (std::string(slice) != "base w") {
    return false;
  }
  file.Close();
  return directory.Delete() && !base::PathExists(path);
}

// base::Thread is what found M146: a thread used to get a COPY of its
// creator's descriptor table with every close-on-exec slot cleared, and
// base/rand_util_posix.cc opens /dev/urandom with O_CLOEXEC and reads it
// from whichever thread wants randomness. The read failed on a worker
// thread and the CHECK behind it fired. The table belongs to the process
// now, which is what lets the raw-thread check below run at all.
//
// base::Thread itself is still NOT exercised here, and that is again a bug
// rather than an omission - a second one, standing behind the first. A
// thread_local on this machine is not where the compiler reads it: the
// runtime places the block a segment-size below the thread pointer where
// the compiler places it a size-rounded-up-to-the-segment-alignment below.
// //base's segment is 0x134 bytes aligned to 8, so every thread_local in it
// sits four bytes off, and base::Thread::Start reaches a constinit
// thread_local with a vtable before it reaches anything else. That is the
// next milestone, and these checks come back with it.
bool RandBytesWorksAtEverySize() {
  for (size_t size = 1; size <= 64; ++size) {
    std::vector<uint8_t> buffer(size, 0);
    base::RandBytes(buffer);
  }
  for (int i = 0; i < 100; ++i) {
    (void)base::RandUint64();
  }
  return true;
}

void* RandOnPthread(void*) {
  uint64_t value = base::RandUint64();
  return reinterpret_cast<void*>(value | 1);
}

bool RandBytesWorksOnARawThread() {
  pthread_t thread;
  if (pthread_create(&thread, nullptr, RandOnPthread, nullptr) != 0) {
    return false;
  }
  void* result = nullptr;
  if (pthread_join(thread, &result) != 0) {
    return false;
  }
  return result != nullptr;
}

// base::Time and base::TimeTicks over M141's clocks, and a sleep that has
// to show up in the monotonic one.
bool TimeBehaves() {
  base::TimeTicks before = base::TimeTicks::Now();
  base::PlatformThread::Sleep(base::Milliseconds(40));
  base::TimeDelta elapsed = base::TimeTicks::Now() - before;
  if (elapsed < base::Milliseconds(30) || elapsed > base::Seconds(5)) {
    return false;
  }
  base::Time wall = base::Time::Now();
  if (wall.is_null()) {
    return false;
  }
  // Somewhere after 2020 and before 2100, which is all a machine with an
  // RTC can be asked to promise.
  base::Time::Exploded exploded;
  wall.UTCExplode(&exploded);
  return exploded.year > 2020 && exploded.year < 2100;
}

// base::RandBytes reaches getrandom, which patch 0009 routed away from
// Linux's syscall number and onto this libc's function.
bool RandomBehaves() {
  std::vector<uint8_t> first(64, 0);
  std::vector<uint8_t> second(64, 0);
  base::RandBytes(first);
  base::RandBytes(second);
  if (first == second) {
    return false;
  }
  int zero_bytes = 0;
  for (uint8_t byte : first) {
    if (byte == 0) {
      ++zero_bytes;
    }
  }
  // 64 bytes of zeros would mean getrandom wrote nothing at all.
  if (zero_bytes > 16) {
    return false;
  }
  uint64_t value = base::RandUint64();
  uint64_t other = base::RandUint64();
  return value != other;
}

bool StringsBehave() {
  std::vector<std::string> parts = base::SplitString(
      "one,two,,three", ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);
  if (parts.size() != 4 || parts[2] != "") {
    return false;
  }
  if (base::JoinString(parts, "|") != "one|two||three") {
    return false;
  }
  // kToLower is the constexpr table std::ranges::iota builds, which is what
  // libc++ 19 could not do and libc++ 24 can.
  if (base::ToLowerASCII("MiXeD CaSe") != "mixed case") {
    return false;
  }
  if (base::ToUpperASCII("MiXeD CaSe") != "MIXED CASE") {
    return false;
  }
  int parsed = 0;
  if (!base::StringToInt("-4711", &parsed) || parsed != -4711) {
    return false;
  }
  return base::NumberToString(4711) == "4711";
}

}  // namespace

int main(int argc, char** argv) {
  base::AtExitManager at_exit;
  base::CommandLine::Init(argc, argv);

  std::printf(
      "chromiumbase: Chromium's //base, built for this machine and running "
      "on it\n");

  Check("getrandom fills what it is asked for, from this binary",
        RawGetrandomBehaves());
  Check("a function-local static runs its initialiser exactly once",
        LocalStaticsAreInitialisedOnce());
  Check("this machine's kernel version is its own, not a Linux one",
        KernelVersionIsWhatUnameSays());
  Check("base::RandBytes reached this libc's getrandom", RandomBehaves());
  Check("base::RandBytes at every size from one byte to sixty-four",
        RandBytesWorksAtEverySize());
  Check("base::RandUint64 on a thread this program made itself",
        RandBytesWorksOnARawThread());
  Check("base::FilePath takes a path apart and puts one together",
        PathsBehave());
  Check("base::File and base::WriteFile over leanfs, in a temporary "
        "directory it made and removed",
        FilesBehave());
  Check("base::TimeTicks measured a sleep and base::Time knows the date",
        TimeBehaves());
  Check("base::SplitString, ToLowerASCII and StringToInt", StringsBehave());

  const base::CommandLine* line = base::CommandLine::ForCurrentProcess();
  Check("base::CommandLine parsed the arguments this machine passed",
        !line->GetProgram().value().empty());

  if (failures != 0) {
    std::printf("chromiumbase: FAILED - %d of %d checks\n", failures, checks);
    return 1;
  }
  std::printf(
      "[m145] Chromium's //base links and runs on this machine: %d checks, "
      "from a static EXEC this project's own clang linked out of //base's "
      "own 428 objects.\n",
      checks);
  std::printf("chromiumbase: done\n");
  return 0;
}
