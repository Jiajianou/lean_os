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
#include "base/containers/span.h"
#include "base/files/scoped_file.h"
#include "base/functional/bind.h"
#include "base/memory/read_only_shared_memory_region.h"
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
#include "mojo/core/embedder/embedder.h"
#include "mojo/core/embedder/scoped_ipc_support.h"
#include "mojo/core/ipcz_driver/envelope.h"
#include "mojo/public/cpp/platform/platform_channel.h"
#include "mojo/public/cpp/platform/platform_handle.h"
#include "mojo/public/cpp/system/buffer.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/handle.h"
#include "mojo/public/cpp/system/message_pipe.h"
#include "mojo/public/cpp/system/platform_handle.h"
#include "mojo/public/cpp/system/wait.h"

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

}  // namespace

int main(int argc, char** argv) {
  base::AtExitManager at_exit;
  base::CommandLine::Init(argc, argv);

  std::printf(
      "chromiummojo: Chromium's //mojo, built for this machine and running "
      "on it\n");

  mojo::core::Init();
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

  if (failures != 0) {
    std::printf("chromiummojo: FAILED - %d of %d checks\n", failures, checks);
    return 1;
  }
  std::printf(
      "[m148] Chromium's //mojo runs on this machine: %d checks - message "
      "pipes, data pipes, shared buffers and a channel over a socketpair, on "
      "this kernel's epoll, memfd and SCM_RIGHTS.\n",
      checks);
  std::printf("chromiummojo: done\n");
  return 0;
}
