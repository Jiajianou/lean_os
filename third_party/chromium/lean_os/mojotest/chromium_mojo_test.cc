// A program built from Chromium's //mojo, for this machine, by this
// project's own clang - and run here. Mojo is the layer every
// multi-process piece of Chromium is made of, and the three things it is
// made from are all this kernel's: M118's AF_UNIX with SCM_RIGHTS, M119's
// epoll and M120's memfd_create.
//
// It reports its own results and exits non-zero on the first failure, the
// way /bin/chromiumbase does, because the boot self-test that runs it has
// no other way to tell what went wrong.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#include "base/containers/span.h"
#include "base/files/scoped_file.h"
#include "base/functional/bind.h"
#include "base/memory/read_only_shared_memory_region.h"
#include "base/memory/shared_memory_mapping.h"
#include "base/memory/unsafe_shared_memory_region.h"
#include "base/message_loop/message_pump_type.h"
#include "base/run_loop.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/single_thread_task_executor.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/thread.h"
#include "base/time/time.h"

#include "mojo/core/channel.h"
#include "mojo/core/connection_params.h"
#include "mojo/core/embedder/configuration.h"
#include "mojo/core/embedder/embedder.h"
#include "mojo/core/embedder/scoped_ipc_support.h"
#include "mojo/core/ipcz_driver/envelope.h"
#include "mojo/public/cpp/platform/platform_channel.h"
#include "mojo/public/cpp/platform/platform_handle.h"
#include "mojo/public/cpp/system/buffer.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/handle.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/isolated_connection.h"
#include "mojo/public/cpp/system/message_pipe.h"
#include "mojo/public/cpp/system/platform_handle.h"
#include "mojo/public/cpp/system/wait.h"

#include "lean_os/mojotest/lean_os_echo.mojom.h"

namespace {

int failures = 0;
int checks = 0;

void Check(const char* what, bool ok) {
  ++checks;
  if (ok) {
    std::printf("chromiummojo: %s\n", what);
  } else {
    std::printf("chromiummojo: FAIL %s\n", what);
    ++failures;
  }
}

const char kGreeting[] = "a message pipe carries bytes";

bool MessagePipesCarryBytes() {
  mojo::MessagePipe pipe;
  if (mojo::WriteMessageRaw(pipe.handle0.get(), kGreeting, sizeof(kGreeting),
                            nullptr, 0,
                            MOJO_WRITE_MESSAGE_FLAG_NONE) != MOJO_RESULT_OK) {
    return false;
  }
  if (mojo::Wait(pipe.handle1.get(), MOJO_HANDLE_SIGNAL_READABLE) !=
      MOJO_RESULT_OK) {
    return false;
  }
  std::vector<uint8_t> payload;
  std::vector<mojo::ScopedHandle> handles;
  if (mojo::ReadMessageRaw(pipe.handle1.get(), &payload, &handles,
                           MOJO_READ_MESSAGE_FLAG_NONE) != MOJO_RESULT_OK) {
    return false;
  }
  return payload.size() == sizeof(kGreeting) &&
         std::memcmp(payload.data(), kGreeting, sizeof(kGreeting)) == 0 &&
         handles.empty();
}

// A pipe's other end, sent down a pipe. The receiver gets a handle to the
// same kernel object rather than a copy of anything, which is the property
// the whole design rests on - so the test is that writing into the handle
// that stayed here comes out of the handle that travelled.
bool MessagePipesCarryHandles() {
  mojo::MessagePipe carrier;
  mojo::MessagePipe passenger;

  MojoHandle to_send = passenger.handle1.release().value();
  if (mojo::WriteMessageRaw(carrier.handle0.get(), nullptr, 0, &to_send, 1,
                            MOJO_WRITE_MESSAGE_FLAG_NONE) != MOJO_RESULT_OK) {
    return false;
  }
  if (mojo::Wait(carrier.handle1.get(), MOJO_HANDLE_SIGNAL_READABLE) !=
      MOJO_RESULT_OK) {
    return false;
  }
  std::vector<uint8_t> payload;
  std::vector<mojo::ScopedHandle> handles;
  if (mojo::ReadMessageRaw(carrier.handle1.get(), &payload, &handles,
                           MOJO_READ_MESSAGE_FLAG_NONE) != MOJO_RESULT_OK) {
    return false;
  }
  if (handles.size() != 1) {
    return false;
  }

  mojo::ScopedMessagePipeHandle received(
      mojo::MessagePipeHandle(handles[0].release().value()));
  const char kThrough[] = "through the handle that travelled";
  if (mojo::WriteMessageRaw(passenger.handle0.get(), kThrough, sizeof(kThrough),
                            nullptr, 0,
                            MOJO_WRITE_MESSAGE_FLAG_NONE) != MOJO_RESULT_OK) {
    return false;
  }
  if (mojo::Wait(received.get(), MOJO_HANDLE_SIGNAL_READABLE) !=
      MOJO_RESULT_OK) {
    return false;
  }
  payload.clear();
  handles.clear();
  if (mojo::ReadMessageRaw(received.get(), &payload, &handles,
                           MOJO_READ_MESSAGE_FLAG_NONE) != MOJO_RESULT_OK) {
    return false;
  }
  return payload.size() == sizeof(kThrough) &&
         std::memcmp(payload.data(), kThrough, sizeof(kThrough)) == 0;
}

bool DataPipesCarryBytes() {
  MojoCreateDataPipeOptions options;
  options.struct_size = sizeof(options);
  options.flags = MOJO_CREATE_DATA_PIPE_FLAG_NONE;
  options.element_num_bytes = 1;
  options.capacity_num_bytes = 64 * 1024;

  mojo::ScopedDataPipeProducerHandle producer;
  mojo::ScopedDataPipeConsumerHandle consumer;
  if (mojo::CreateDataPipe(&options, producer, consumer) != MOJO_RESULT_OK) {
    return false;
  }

  std::vector<uint8_t> written(4096);
  for (size_t i = 0; i < written.size(); ++i) {
    written[i] = static_cast<uint8_t>(i * 7 + 3);
  }
  size_t wrote = 0;
  if (producer->WriteData(base::span<const uint8_t>(written),
                          MOJO_WRITE_DATA_FLAG_ALL_OR_NONE,
                          wrote) != MOJO_RESULT_OK ||
      wrote != written.size()) {
    return false;
  }
  if (mojo::Wait(consumer.get(), MOJO_HANDLE_SIGNAL_READABLE) !=
      MOJO_RESULT_OK) {
    return false;
  }
  std::vector<uint8_t> read(written.size());
  size_t got = 0;
  if (consumer->ReadData(MOJO_READ_DATA_FLAG_ALL_OR_NONE,
                         base::span<uint8_t>(read), got) != MOJO_RESULT_OK ||
      got != written.size()) {
    return false;
  }
  return read == written;
}

// A mojo shared buffer, which on this machine is memfd_create - a descriptor
// nobody can name, sized with ftruncate and mapped MAP_SHARED. The second
// mapping has to see what the first wrote, or it is two allocations rather
// than one buffer.
bool SharedBuffersAreOneBuffer() {
  mojo::ScopedSharedBufferHandle buffer = mojo::SharedBufferHandle::Create(4096);
  if (!buffer.is_valid()) {
    return false;
  }
  mojo::ScopedSharedBufferMapping first = buffer->Map(4096);
  if (!first) {
    return false;
  }
  uint8_t* bytes = static_cast<uint8_t*>(first.get());
  for (size_t i = 0; i < 4096; ++i) {
    bytes[i] = static_cast<uint8_t>(i ^ 0x5a);
  }

  mojo::ScopedSharedBufferHandle clone =
      buffer->Clone(mojo::SharedBufferHandle::AccessMode::READ_ONLY);
  if (!clone.is_valid()) {
    return false;
  }
  mojo::ScopedSharedBufferMapping second = clone->Map(4096);
  if (!second) {
    return false;
  }
  const uint8_t* seen = static_cast<const uint8_t*>(second.get());
  for (size_t i = 0; i < 4096; ++i) {
    if (seen[i] != static_cast<uint8_t>(i ^ 0x5a)) {
      return false;
    }
  }
  return true;
}

// The same buffer, sent down a message pipe as a handle. base's own shared
// memory region goes in one end and comes out the other still naming the
// pages this side wrote.
bool SharedMemoryTravelsDownAPipe() {
  base::UnsafeSharedMemoryRegion region =
      base::UnsafeSharedMemoryRegion::Create(8192);
  if (!region.IsValid()) {
    return false;
  }
  base::WritableSharedMemoryMapping mapping = region.Map();
  if (!mapping.IsValid()) {
    return false;
  }
  base::span<uint8_t> span = mapping.GetMemoryAsSpan<uint8_t>();
  for (size_t i = 0; i < span.size(); ++i) {
    span[i] = static_cast<uint8_t>(i * 31 + 11);
  }

  mojo::ScopedSharedBufferHandle wrapped =
      mojo::WrapUnsafeSharedMemoryRegion(std::move(region));
  if (!wrapped.is_valid()) {
    return false;
  }

  mojo::MessagePipe pipe;
  MojoHandle to_send = wrapped.release().value();
  if (mojo::WriteMessageRaw(pipe.handle0.get(), nullptr, 0, &to_send, 1,
                            MOJO_WRITE_MESSAGE_FLAG_NONE) != MOJO_RESULT_OK) {
    return false;
  }
  if (mojo::Wait(pipe.handle1.get(), MOJO_HANDLE_SIGNAL_READABLE) !=
      MOJO_RESULT_OK) {
    return false;
  }
  std::vector<uint8_t> payload;
  std::vector<mojo::ScopedHandle> handles;
  if (mojo::ReadMessageRaw(pipe.handle1.get(), &payload, &handles,
                           MOJO_READ_MESSAGE_FLAG_NONE) != MOJO_RESULT_OK ||
      handles.size() != 1) {
    return false;
  }

  base::UnsafeSharedMemoryRegion back = mojo::UnwrapUnsafeSharedMemoryRegion(
      mojo::ScopedSharedBufferHandle(
          mojo::SharedBufferHandle(handles[0].release().value())));
  if (!back.IsValid()) {
    return false;
  }
  base::WritableSharedMemoryMapping again = back.Map();
  if (!again.IsValid()) {
    return false;
  }
  base::span<const uint8_t> seen = again.GetMemoryAsSpan<uint8_t>();
  if (seen.size() < 8192) {
    return false;
  }
  for (size_t i = 0; i < 8192; ++i) {
    if (seen[i] != static_cast<uint8_t>(i * 31 + 11)) {
      return false;
    }
  }
  return true;
}

// The transport itself. Two Channels over the two ends of one socketpair,
// both driven by one IO thread - which is Chromium's own epoll message pump
// on this kernel's epoll. A Channel message carries a descriptor as well as
// bytes, so this is sendmsg(SCM_RIGHTS) with mojo's framing on top of it.
class RecordingDelegate : public mojo::core::Channel::Delegate {
 public:
  RecordingDelegate()
      : arrived_(base::WaitableEvent::ResetPolicy::MANUAL,
                 base::WaitableEvent::InitialState::NOT_SIGNALED) {}

  void OnChannelMessage(
      const void* payload,
      size_t payload_size,
      std::vector<mojo::PlatformHandle> handles,
      scoped_refptr<mojo::core::ipcz_driver::Envelope> envelope) override {
    const char* bytes = static_cast<const char*>(payload);
    payload_.assign(bytes, bytes + payload_size);
    handles_ = std::move(handles);
    arrived_.Signal();
  }

  void OnChannelError(mojo::core::Channel::Error error) override {
    failed_ = true;
    arrived_.Signal();
  }

  bool WaitForMessage() {
    return arrived_.TimedWait(base::Seconds(30)) && !failed_;
  }

  const std::string& payload() const { return payload_; }
  std::vector<mojo::PlatformHandle>& handles() { return handles_; }

 private:
  base::WaitableEvent arrived_;
  bool failed_ = false;
  std::string payload_;
  std::vector<mojo::PlatformHandle> handles_;
};

bool ChannelsCarryBytesAndADescriptor(
    scoped_refptr<base::SingleThreadTaskRunner> io_runner) {
  mojo::PlatformChannel socket_pair;
  RecordingDelegate sender_delegate;
  RecordingDelegate receiver_delegate;

  scoped_refptr<mojo::core::Channel> sender = mojo::core::Channel::Create(
      &sender_delegate,
      mojo::core::ConnectionParams(socket_pair.TakeLocalEndpoint()),
      mojo::core::Channel::HandlePolicy::kAcceptHandles, io_runner);
  scoped_refptr<mojo::core::Channel> receiver = mojo::core::Channel::Create(
      &receiver_delegate,
      mojo::core::ConnectionParams(socket_pair.TakeRemoteEndpoint()),
      mojo::core::Channel::HandlePolicy::kAcceptHandles, io_runner);

  base::WaitableEvent started(base::WaitableEvent::ResetPolicy::MANUAL,
                              base::WaitableEvent::InitialState::NOT_SIGNALED);
  io_runner->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](scoped_refptr<mojo::core::Channel> a,
             scoped_refptr<mojo::core::Channel> b,
             base::WaitableEvent* done) {
            a->Start();
            b->Start();
            done->Signal();
          },
          sender, receiver, &started));
  started.Wait();

  int descriptors[2];
  if (pipe(descriptors) != 0) {
    return false;
  }
  base::ScopedFD read_end(descriptors[0]);
  base::ScopedFD write_end(descriptors[1]);

  const char kOverTheWire[] = "a channel carries bytes and a descriptor";
  mojo::core::Channel::MessagePtr message =
      mojo::core::Channel::Message::CreateMessage(sizeof(kOverTheWire), 1);
  std::memcpy(message->mutable_payload(), kOverTheWire, sizeof(kOverTheWire));
  std::vector<mojo::PlatformHandle> to_send;
  to_send.emplace_back(std::move(read_end));
  message->SetHandles(std::move(to_send));
  sender->Write(std::move(message));

  bool ok = receiver_delegate.WaitForMessage();
  if (ok) {
    ok = receiver_delegate.payload().size() == sizeof(kOverTheWire) &&
         std::memcmp(receiver_delegate.payload().data(), kOverTheWire,
                     sizeof(kOverTheWire)) == 0 &&
         receiver_delegate.handles().size() == 1;
  }
  if (ok) {
    // The descriptor that arrived has to be the other end of the pipe whose
    // write end never left this process - not merely a valid descriptor.
    const char kProof[] = "same pipe";
    char seen[sizeof(kProof)] = {};
    ok = write(write_end.get(), kProof, sizeof(kProof)) ==
             static_cast<ssize_t>(sizeof(kProof)) &&
         read(receiver_delegate.handles()[0].GetFD().get(), seen,
              sizeof(seen)) == static_cast<ssize_t>(sizeof(kProof)) &&
         std::memcmp(seen, kProof, sizeof(kProof)) == 0;
  }

  sender->ShutDown();
  receiver->ShutDown();
  return ok;
}

// A SECOND PROCESS, which is the one thing everything above is not. The
// parent makes a socketpair, hands one end to a child it spawns, and the two
// mojo nodes meet over it: the child is another copy of this program, found
// by the path base::CommandLine says this one was started by.
//
// The launch happens before any thread in this program is started. A fork in
// a process with threads leaves the child holding one of them and locks the
// others were inside, and the only safe thing between that fork and the
// execve is nothing - so the child is on its way before mojo has an IO
// thread to lose.
const char kChildSwitch[] = "mojo-child";
const char kToChild[] = "a message pipe between two processes";
const char kToParent[] = "sessecorp owt neewteb epip egassem a";
const size_t kPatternBytes = 4096;

bool LaunchTheOtherProcess(mojo::PlatformChannel* channel,
                           base::Process* child) {
  base::CommandLine child_line(
      base::CommandLine::ForCurrentProcess()->GetProgram());
  child_line.AppendSwitch(kChildSwitch);
  base::LaunchOptions options;
  channel->PrepareToPassRemoteEndpoint(&options, &child_line);
  *child = base::LaunchProcess(child_line, options);
  channel->RemoteProcessLaunchAttempted();
  return child->IsValid();
}

// The interface call, in the shape Chromium makes every one of its own: a
// mojo::Remote on this side, a mojo::Receiver on the other, C++ generated
// from lean_os_echo.mojom on both, an asynchronous reply delivered to a
// callback on this thread's run loop - and a handle coming back in it.
bool CallTheOtherProcess(mojo::PlatformChannel* channel,
                         base::Process* child) {
  mojo::IsolatedConnection connection;
  mojo::ScopedMessagePipeHandle pipe =
      connection.Connect(channel->TakeLocalEndpoint());
  if (!pipe.is_valid()) {
    return false;
  }

  mojo::Remote<lean_os::mojom::Echo> echo(
      mojo::PendingRemote<lean_os::mojom::Echo>(std::move(pipe), 0));

  base::RunLoop loop;
  std::string reversed;
  int64_t answered_by = 0;
  mojo::ScopedSharedBufferHandle pattern;
  uint64_t pattern_size = 0;
  bool disconnected = false;
  echo.set_disconnect_handler(
      base::BindOnce(
          [](bool* flag, base::RunLoop* loop) {
            *flag = true;
            loop->Quit();
          },
          &disconnected, &loop));
  echo->Reverse(
      kToChild,
      base::BindOnce(
          [](std::string* out, int64_t* who,
             mojo::ScopedSharedBufferHandle* buffer, uint64_t* size,
             base::RunLoop* loop, const std::string& answer, int64_t pid,
             mojo::ScopedSharedBufferHandle handle, uint64_t bytes) {
            *out = answer;
            *who = pid;
            *buffer = std::move(handle);
            *size = bytes;
            loop->Quit();
          },
          &reversed, &answered_by, &pattern, &pattern_size, &loop));
  loop.Run();

  if (disconnected || reversed != kToParent || pattern_size != kPatternBytes ||
      !pattern.is_valid()) {
    return false;
  }
  // Somebody else answered. Without this the whole check would pass on a
  // LaunchProcess that had quietly done the work in this process.
  if (answered_by == static_cast<int64_t>(getpid()) ||
      answered_by != static_cast<int64_t>(child->Pid())) {
    return false;
  }

  // The buffer the other process made, mapped here. Unwrapping it as a
  // READ-ONLY region is base's own assertion that the descriptor which
  // crossed the channel reports O_RDONLY - which on this machine means the
  // child's memfd, reopened through /proc/self/fd with less access than the
  // one it kept.
  base::ReadOnlySharedMemoryRegion region =
      mojo::UnwrapReadOnlySharedMemoryRegion(std::move(pattern));
  if (!region.IsValid()) {
    return false;
  }
  base::ReadOnlySharedMemoryMapping mapping = region.Map();
  if (!mapping.IsValid()) {
    return false;
  }
  base::span<const uint8_t> seen = mapping.GetMemoryAsSpan<uint8_t>();
  if (seen.size() < kPatternBytes) {
    return false;
  }
  for (size_t i = 0; i < kPatternBytes; ++i) {
    if (seen[i] != static_cast<uint8_t>(i * 13 + 5)) {
      return false;
    }
  }

  echo.reset();
  int code = -1;
  if (!child->WaitForExitWithTimeout(base::Seconds(60), &code)) {
    return false;
  }
  return code == 0;
}

class EchoImplementation : public lean_os::mojom::Echo {
 public:
  EchoImplementation(mojo::PendingReceiver<lean_os::mojom::Echo> receiver,
                     base::RunLoop* loop)
      : receiver_(this, std::move(receiver)) {
    receiver_.set_disconnect_handler(base::BindOnce(
        [](base::RunLoop* loop) { loop->Quit(); }, loop));
  }

  void Reverse(const std::string& text, ReverseCallback callback) override {
    std::string reversed(text.rbegin(), text.rend());

    base::MappedReadOnlyRegion pair =
        base::ReadOnlySharedMemoryRegion::Create(kPatternBytes);
    if (!pair.IsValid()) {
      std::move(callback).Run(std::string(), 0,
                              mojo::ScopedSharedBufferHandle(), 0);
      return;
    }
    base::span<uint8_t> span = pair.mapping.GetMemoryAsSpan<uint8_t>();
    for (size_t i = 0; i < span.size(); ++i) {
      span[i] = static_cast<uint8_t>(i * 13 + 5);
    }
    mojo::ScopedSharedBufferHandle wrapped =
        mojo::WrapReadOnlySharedMemoryRegion(std::move(pair.region));
    std::move(callback).Run(reversed, static_cast<int64_t>(getpid()),
                            std::move(wrapped), kPatternBytes);
  }

 private:
  mojo::Receiver<lean_os::mojom::Echo> receiver_;
};

// The other half of the program above, reached by the switch the parent put
// on the command line.
int RunAsTheOtherProcess() {
  // Both ends of an isolated connection have to be brokers - ipcz says so in
  // as many words, because an isolated invitation has no third node to
  // allocate shared memory on either side's behalf.
  mojo::core::Configuration config;
  config.is_broker_process = true;
  mojo::core::Init(config);

  base::SingleThreadTaskExecutor main_task_executor;
  base::Thread io_thread("mojo-child-io");
  if (!io_thread.StartWithOptions(
          base::Thread::Options(base::MessagePumpType::IO, 0))) {
    return 2;
  }
  mojo::core::ScopedIPCSupport ipc_support(
      io_thread.task_runner(),
      mojo::core::ScopedIPCSupport::ShutdownPolicy::CLEAN);

  mojo::PlatformChannelEndpoint endpoint =
      mojo::PlatformChannel::RecoverPassedEndpointFromCommandLine(
          *base::CommandLine::ForCurrentProcess());
  if (!endpoint.is_valid()) {
    return 3;
  }
  mojo::IsolatedConnection connection;
  mojo::ScopedMessagePipeHandle pipe = connection.Connect(std::move(endpoint));
  if (!pipe.is_valid()) {
    return 4;
  }

  base::RunLoop loop;
  EchoImplementation echo(
      mojo::PendingReceiver<lean_os::mojom::Echo>(std::move(pipe)), &loop);
  loop.Run();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  base::AtExitManager at_exit;
  base::CommandLine::Init(argc, argv);

  if (base::CommandLine::ForCurrentProcess()->HasSwitch(kChildSwitch)) {
    return RunAsTheOtherProcess();
  }

  std::printf(
      "chromiummojo: Chromium's //mojo, built for this machine and running "
      "on it\n");

  // Before any thread exists in this process. See LaunchTheOtherProcess.
  mojo::PlatformChannel to_the_other_process;
  base::Process other_process;
  Check("base::LaunchProcess started a second copy of this program",
        LaunchTheOtherProcess(&to_the_other_process, &other_process));

  mojo::core::Configuration config;
  config.is_broker_process = true;
  mojo::core::Init(config);
  Check("mojo::core::Init brought up the ipcz driver and the node", true);

  base::SingleThreadTaskExecutor main_task_executor;
  base::Thread io_thread("mojo-io");
  bool io_started = io_thread.StartWithOptions(
      base::Thread::Options(base::MessagePumpType::IO, 0));
  Check("an IO thread started on Chromium's own epoll message pump",
        io_started);

  mojo::core::ScopedIPCSupport ipc_support(
      io_thread.task_runner(),
      mojo::core::ScopedIPCSupport::ShutdownPolicy::CLEAN);
  Check("mojo's IPC support is running on that thread", true);

  Check("a message pipe carries bytes", MessagePipesCarryBytes());
  Check("a message pipe carries another pipe's endpoint",
        MessagePipesCarryHandles());
  Check("a data pipe carries four kilobytes in order", DataPipesCarryBytes());
  Check("a shared buffer and its read-only clone are one buffer",
        SharedBuffersAreOneBuffer());
  Check("base's shared memory region travels down a message pipe",
        SharedMemoryTravelsDownAPipe());
  Check("two channels over one socketpair carry bytes and a descriptor",
        ChannelsCarryBytesAndADescriptor(io_thread.task_runner()));
  Check("a mojom interface call to another process, and its buffer back",
        CallTheOtherProcess(&to_the_other_process, &other_process));

  if (failures != 0) {
    std::printf("chromiummojo: FAILED - %d of %d checks\n", failures, checks);
    return 1;
  }
  std::printf(
      "[m148] Chromium's //mojo runs on this machine: %d checks - message "
      "pipes, data pipes, shared buffers and a channel over a socketpair, on "
      "this kernel's epoll, memfd and SCM_RIGHTS.\n",
      checks);
  std::printf(
      "[m149] two processes on one mojo connection: base::LaunchProcess "
      "spawned a second copy of this program, the two nodes met over a "
      "socketpair, and a mojom interface generated from lean_os_echo.mojom "
      "carried a call and an asynchronous reply between them - with a "
      "read-only buffer the CHILD created mapped and read in the parent.\n");
  std::printf("chromiummojo: done\n");
  return 0;
}
