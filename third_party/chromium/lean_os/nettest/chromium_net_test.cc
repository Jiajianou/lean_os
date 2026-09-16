// Chromium's //url and //net, built for this machine by this project's own
// clang and run here. Compiling proves the headers are right and linking
// proves the symbols exist; only running says anything about the machine,
// and the checks below are chosen for the parts where this OS had to
// answer a question rather than where Chromium is portable already.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <resolv.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/run_loop.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/memory/ref_counted.h"
#include "base/message_loop/message_pump_type.h"
#include "base/task/single_thread_task_executor.h"
#include "base/task/thread_pool/thread_pool_instance.h"

#include "net/base/host_port_pair.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/base/network_interfaces.h"
#include "net/dns/public/resolv_reader.h"
#include "net/http/http_util.h"
#include "net/proxy_resolution/proxy_config.h"
#include "net/proxy_resolution/proxy_config_with_annotation.h"
#include "net/proxy_resolution/configured_proxy_resolution_service.h"
#include "net/proxy_resolution/proxy_config_service.h"
#include "net/base/address_list.h"
#include "net/base/completion_once_callback.h"
#include "net/base/io_buffer.h"
#include "net/base/net_errors.h"
#include "net/base/test_completion_callback.h"
#include "net/log/net_log_source.h"
#include "net/socket/stream_socket.h"
#include "net/socket/tcp_client_socket.h"
#include "net/socket/tcp_server_socket.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "net/url_request/url_request.h"
#include "net/url_request/url_request_context.h"
#include "net/url_request/url_request_context_builder.h"
#include "url/gurl.h"

namespace {

int failures = 0;
int checks = 0;

// Every piece of //net that sends bytes wants one of these, so that a
// reviewer can find out what a connection is for. This program's is the
// truth about it.
constexpr net::NetworkTrafficAnnotationTag kAnnotation =
    net::DefineNetworkTrafficAnnotation("lean_os_nettest", R"(
      semantics {
        sender: "lean_os boot self-test"
        description: "Fetches one page from a server in this same process."
        trigger: "The [m151] boot self-test."
        data: "Nothing."
        destination: LOCAL
      }
      policy {
        cookies_allowed: NO
        setting: "Not settable - it is a self-test."
        policy_exception_justification: "Not a user-visible request."
      })");

void Check(const char* what, bool ok) {
  ++checks;
  if (ok) {
    std::printf("chromiumnet: %s\n", what);
  } else {
    std::printf("chromiumnet: FAIL %s\n", what);
    ++failures;
  }
}

// //url. GURL is the type every other piece of a browser passes addresses
// around in, and its parser is the one that decides what a URL means.
bool UrlsParse() {
  GURL url("https://user:secret@example.org:8443/a/b/c.html?q=1#frag");
  if (!url.is_valid() || url.scheme() != "https" || url.host() != "example.org" ||
      url.port() != "8443" || url.path() != "/a/b/c.html" ||
      url.query() != "q=1" || url.ref() != "frag" ||
      url.username() != "user" || url.password() != "secret") {
    return false;
  }
  if (url.EffectiveIntPort() != 8443) {
    return false;
  }

  GURL relative = url.Resolve("../d/e.html");
  if (!relative.is_valid() ||
      relative.spec() != "https://user:secret@example.org:8443/a/d/e.html") {
    return false;
  }

  // A scheme nobody registered, and a URL that is not one.
  if (GURL("not a url").is_valid()) {
    return false;
  }

  // The canonicaliser, which is where the interesting bugs live.
  GURL messy("HTTP://ExAmPle.COM:80/./x/../y/%2e%2e/z");
  return messy.is_valid() && messy.spec() == "http://example.com/z";
}

bool AddressesParse() {
  net::IPAddress four;
  if (!four.AssignFromIPLiteral("192.0.2.17") || !four.IsIPv4() ||
      four.ToString() != "192.0.2.17") {
    return false;
  }
  net::IPAddress six;
  if (!six.AssignFromIPLiteral("2001:db8::1") || !six.IsIPv6() ||
      six.ToString() != "2001:db8::1") {
    return false;
  }
  net::IPEndPoint endpoint(four, 443);
  if (endpoint.ToString() != "192.0.2.17:443") {
    return false;
  }
  net::HostPortPair pair =
      net::HostPortPair::FromString("example.org:8080");
  return pair.host() == "example.org" && pair.port() == 8080;
}

// net::GetNetworkList on a POSIX platform without netlink is
// network_interfaces_getifaddrs.cc over this libc's getifaddrs(3), which
// this milestone wrote. The loopback is filtered out by net itself, so
// what is left is the interface this machine actually has.
bool InterfacesAreListed() {
  net::NetworkInterfaceList interfaces;
  if (!net::GetNetworkList(&interfaces,
                           net::INCLUDE_HOST_SCOPE_VIRTUAL_INTERFACES)) {
    return false;
  }
  for (const net::NetworkInterface& interface : interfaces) {
    if (!interface.name.empty() && interface.address.IsValid() &&
        !interface.address.IsLoopback()) {
      std::printf("chromiumnet:   %s %s/%u\n", interface.name.c_str(),
                  interface.address.ToString().c_str(),
                  interface.prefix_length);
      return true;
    }
  }
  return false;
}

// The BIND resolver. Chromium reads the nameservers through res_ninit(3),
// and this libc's res_ninit is M114's resolver wearing the interface every
// other system has. The expectation does NOT come from the same place: the
// test reads /etc/resolv.conf itself and requires every address written
// there to come back out of Chromium.
bool NameserversAreRead() {
  std::string configuration;
  std::vector<std::string> written;
  if (base::ReadFileToString(base::FilePath("/etc/resolv.conf"),
                             &configuration)) {
    for (const std::string& line : base::SplitString(
             configuration, "\n", base::TRIM_WHITESPACE,
             base::SPLIT_WANT_NONEMPTY)) {
      std::vector<std::string> words = base::SplitString(
          line, " \t", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
      if (words.size() >= 2 && words[0] == "nameserver") {
        written.push_back(words[1]);
      }
    }
  }

  std::unique_ptr<net::ScopedResState> state = net::ResolvReader().GetResState();
  if (!state || !state->IsValid()) {
    return false;
  }
  std::optional<std::vector<net::IPEndPoint>> nameservers =
      net::GetNameservers(state->state());
  if (!nameservers || nameservers->empty()) {
    return false;
  }
  for (const net::IPEndPoint& nameserver : *nameservers) {
    std::printf("chromiumnet:   nameserver %s\n",
                nameserver.ToString().c_str());
    if (nameserver.port() != 53 || !nameserver.address().IsValid()) {
      return false;
    }
  }
  for (const std::string& expected : written) {
    bool found = false;
    for (const net::IPEndPoint& nameserver : *nameservers) {
      if (nameserver.address().ToString() == expected) {
        found = true;
      }
    }
    if (!found) {
      std::printf("chromiumnet:   /etc/resolv.conf names %s and res_ninit "
                  "did not return it\n", expected.c_str());
      return false;
    }
  }
  return true;
}

// The proxy configuration service. This platform has no gsettings and no
// inotify, so net's own fallback is the one that has to be chosen - a
// direct configuration rather than a crash or a pretence.
bool ProxyConfigurationIsDirect() {
  base::SingleThreadTaskExecutor executor;
  std::unique_ptr<net::ProxyConfigService> service =
      net::ProxyConfigService::CreateSystemProxyConfigService(
          base::SingleThreadTaskRunner::GetCurrentDefault());
  if (!service) {
    return false;
  }
  net::ProxyConfigWithAnnotation config;
  if (service->GetLatestProxyConfig(&config) !=
      net::ProxyConfigService::CONFIG_VALID) {
    return false;
  }
  // Direct means: no PAC script, no auto-detect and no proxy rules - which
  // is the only honest answer on a machine with no system proxy settings to
  // read.
  return !config.value().HasAutomaticSettings() &&
         config.value().proxy_rules().empty();
}

// A piece of //net that is pure parsing, to prove the HTTP layer is more
// than linked.
bool HttpHeadersParse() {
  std::string headers =
      "HTTP/1.1 200 OK\nContent-Type: text/html; charset=utf-8\n"
      "Content-Length: 42\n\n";
  std::string raw = net::HttpUtil::AssembleRawHeaders(headers);
  net::HttpUtil::HeadersIterator it(raw, std::string("\0", 1));
  bool saw_type = false;
  bool saw_length = false;
  while (it.GetNext()) {
    if (base::EqualsCaseInsensitiveASCII(it.name(), "content-type") &&
        it.values() == "text/html; charset=utf-8") {
      saw_type = true;
    }
    if (base::EqualsCaseInsensitiveASCII(it.name(), "content-length") &&
        it.values() == "42") {
      saw_length = true;
    }
  }
  return saw_type && saw_length;
}

// A connection, which is the first thing here that is not parsing. Both
// ends are Chromium's - TCPServerSocket and TCPClientSocket - and both run
// on this kernel's TCP through a non-blocking connect(2) whose completion
// arrives on base::MessagePumpEpoll. Loopback, so the check needs nothing
// outside this machine.
class Waiter {
 public:
  net::CompletionOnceCallback callback() {
    return base::BindOnce(&Waiter::Done, base::Unretained(this));
  }
  int Wait(int immediate) {
    if (immediate != net::ERR_IO_PENDING) {
      return immediate;
    }
    loop_.Run();
    return result_;
  }

 private:
  void Done(int result) {
    result_ = result;
    loop_.Quit();
  }
  base::RunLoop loop_;
  int result_ = net::ERR_UNEXPECTED;
};

bool SocketsConnectOverLoopback() {
  net::TCPServerSocket server(nullptr, net::NetLogSource());
  int listened = server.Listen(
      net::IPEndPoint(net::IPAddress::IPv4Localhost(), 0), 4, std::nullopt);
  if (listened != net::OK) {
    std::printf("chromiumnet:   Listen: %s\n",
                net::ErrorToShortString(listened).c_str());
    return false;
  }
  net::IPEndPoint bound;
  int named = server.GetLocalAddress(&bound);
  if (named != net::OK || bound.port() == 0) {
    std::printf("chromiumnet:   GetLocalAddress: %s port %u\n",
                net::ErrorToShortString(named).c_str(), bound.port());
    return false;
  }
  std::printf("chromiumnet:   listening on %s\n", bound.ToString().c_str());

  std::unique_ptr<net::StreamSocket> accepted;
  Waiter accept_waiter;
  int accept_result = server.Accept(&accepted, accept_waiter.callback());

  net::TCPClientSocket client(net::AddressList(bound), nullptr, nullptr,
                              nullptr, net::NetLogSource(),
                              net::handles::kInvalidNetworkHandle);
  Waiter connect_waiter;
  int connected = connect_waiter.Wait(client.Connect(connect_waiter.callback()));
  if (connected != net::OK) {
    std::printf("chromiumnet:   Connect: %s\n",
                net::ErrorToShortString(connected).c_str());
    return false;
  }
  int accepted_result = accept_waiter.Wait(accept_result);
  if (accepted_result != net::OK || !accepted) {
    std::printf("chromiumnet:   Accept: %s\n",
                net::ErrorToShortString(accepted_result).c_str());
    return false;
  }

  const char kUpstream[] = "a connection on this machine";
  auto out = base::MakeRefCounted<net::StringIOBuffer>(std::string(kUpstream));
  Waiter write_waiter;
  int written = write_waiter.Wait(client.Write(
      out.get(), out->size(), write_waiter.callback(),
      kAnnotation));
  if (written != out->size()) {
    std::printf("chromiumnet:   Write: %d\n", written);
    return false;
  }

  auto in = base::MakeRefCounted<net::IOBufferWithSize>(64);
  Waiter read_waiter;
  int read = read_waiter.Wait(
      accepted->Read(in.get(), in->size(), read_waiter.callback()));
  if (read != static_cast<int>(sizeof(kUpstream) - 1) ||
      std::memcmp(in->data(), kUpstream, sizeof(kUpstream) - 1) != 0) {
    std::printf("chromiumnet:   Read: %d\n", read);
    return false;
  }
  return true;
}

// And the whole HTTP stack on top of it. The server is eleven lines of
// POSIX in a thread of its own - deliberately, so that what is being
// graded is entirely on Chromium's side: a URLRequest, the host resolver,
// the socket pool, HttpNetworkTransaction and the response parser, all on
// this kernel's sockets.
struct TinyServer {
  int listener = -1;
  uint16_t port = 0;
  pthread_t thread = 0;
};

const char kBody[] = "<html><body>served by this machine</body></html>";

void* ServeOnce(void* argument) {
  TinyServer* server = static_cast<TinyServer*>(argument);
  int connection = accept(server->listener, nullptr, nullptr);
  if (connection < 0) {
    return nullptr;
  }
  char request[1024];
  read(connection, request, sizeof(request));
  char response[512];
  int n = std::snprintf(response, sizeof(response),
                        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                        "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
                        sizeof(kBody) - 1, kBody);
  write(connection, response, (size_t)n);
  close(connection);
  return nullptr;
}

bool StartTinyServer(TinyServer* server) {
  server->listener = socket(AF_INET, SOCK_STREAM, 0);
  if (server->listener < 0) {
    return false;
  }
  struct sockaddr_in address;
  std::memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (bind(server->listener, (struct sockaddr*)&address, sizeof(address)) != 0 ||
      listen(server->listener, 4) != 0) {
    return false;
  }
  socklen_t length = sizeof(address);
  if (getsockname(server->listener, (struct sockaddr*)&address, &length) != 0) {
    return false;
  }
  server->port = ntohs(address.sin_port);
  return pthread_create(&server->thread, nullptr, ServeOnce, server) == 0;
}

class Collector : public net::URLRequest::Delegate {
 public:
  explicit Collector(base::RunLoop* loop) : loop_(loop) {}

  void OnResponseStarted(net::URLRequest* request, int net_error) override {
    if (net_error != net::OK) {
      error_ = net_error;
      loop_->Quit();
      return;
    }
    status_ = request->GetResponseCode();
    ReadMore(request);
  }

  void OnReadCompleted(net::URLRequest* request, int bytes_read) override {
    if (bytes_read > 0) {
      body_.append(buffer_->data(), static_cast<size_t>(bytes_read));
      ReadMore(request);
      return;
    }
    error_ = bytes_read;
    loop_->Quit();
  }

  int status() const { return status_; }
  int error() const { return error_; }
  const std::string& body() const { return body_; }

 private:
  void ReadMore(net::URLRequest* request) {
    for (;;) {
      int read = request->Read(buffer_.get(), buffer_->size());
      if (read == net::ERR_IO_PENDING) {
        return;
      }
      if (read > 0) {
        body_.append(buffer_->data(), static_cast<size_t>(read));
        continue;
      }
      error_ = read;
      loop_->Quit();
      return;
    }
  }

  raw_ptr<base::RunLoop> loop_;
  scoped_refptr<net::IOBufferWithSize> buffer_ =
      base::MakeRefCounted<net::IOBufferWithSize>(4096);
  std::string body_;
  int status_ = 0;
  int error_ = net::OK;
};

bool HttpRequestIsAnswered() {
  TinyServer server;
  if (!StartTinyServer(&server)) {
    return false;
  }

  net::URLRequestContextBuilder builder;
  // Explicitly direct. Letting the builder make one for itself reaches
  // ProxyConfigService::CreateSystemProxyConfigService, which on a platform
  // with no system proxy settings to read returns a service the rest of
  // ConfiguredProxyResolutionService's constructor is not expecting - and
  // which faulted on a null pointer here before this line existed.
  builder.set_proxy_resolution_service(
      net::ConfiguredProxyResolutionService::CreateDirect());
  std::unique_ptr<net::URLRequestContext> context = builder.Build();
  if (!context) {
    return false;
  }

  std::string url = "http://127.0.0.1:" + std::to_string(server.port) + "/";
  std::printf("chromiumnet:   GET %s\n", url.c_str());

  base::RunLoop loop;
  Collector collector(&loop);
  std::unique_ptr<net::URLRequest> request = context->CreateRequest(
      GURL(url), net::DEFAULT_PRIORITY, &collector, kAnnotation,
      net::handles::kInvalidNetworkHandle);
  request->Start();
  loop.Run();

  pthread_join(server.thread, nullptr);
  close(server.listener);

  if (collector.error() != net::OK) {
    std::printf("chromiumnet:   net error %d\n", collector.error());
    return false;
  }
  return collector.status() == 200 && collector.body() == kBody;
}

}  // namespace

int main(int argc, char** argv) {
  base::AtExitManager at_exit;
  base::CommandLine::Init(argc, argv);

  std::printf(
      "chromiumnet: Chromium's //url and //net, built for this machine and "
      "running on it\n");

  Check("GURL parses, canonicalises and resolves a relative reference",
        UrlsParse());
  Check("net::IPAddress, IPEndPoint and HostPortPair", AddressesParse());
  Check("net::GetNetworkList found this machine's interface through "
        "getifaddrs",
        InterfacesAreListed());
  Check("net read this machine's nameservers through res_ninit",
        NameserversAreRead());
  Check("the proxy configuration service is the direct one", 
        ProxyConfigurationIsDirect());
  Check("net::HttpUtil parsed a response's headers", HttpHeadersParse());

  // Everything above is parsing and configuration. These two open a
  // connection, which needs this kernel's TCP, a non-blocking connect and
  // the readiness M119 built.
  base::SingleThreadTaskExecutor io_executor(base::MessagePumpType::IO);
  base::ThreadPoolInstance::CreateAndStartWithDefaultParams("chromiumnet");

  Check("net's own client and server sockets met over this kernel's loopback",
        SocketsConnectOverLoopback());
  Check("a net::URLRequest fetched a page and parsed the response",
        HttpRequestIsAnswered());

  if (failures != 0) {
    std::printf("chromiumnet: FAILED - %d of %d checks\n", failures, checks);
    return 1;
  }
  std::printf(
      "[m150] Chromium's //url and //net run on this machine: %d checks - "
      "GURL's parser, net's own addresses, the interface list through "
      "getifaddrs, this machine's nameservers through res_ninit, and a "
      "proxy configuration that is honestly direct.\n",
      checks);
  std::printf(
      "[m151] Chromium's //net opens a connection here: a TCPServerSocket "
      "and a TCPClientSocket met over this kernel's loopback, and a "
      "net::URLRequest fetched a page through the host resolver, the socket "
      "pool and HttpNetworkTransaction and parsed what came back.\n");
  std::printf("chromiumnet: done\n");
  return 0;
}
