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

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/run_loop.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/task/single_thread_task_executor.h"

#include "net/base/host_port_pair.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/base/network_interfaces.h"
#include "net/dns/public/resolv_reader.h"
#include "net/http/http_util.h"
#include "net/proxy_resolution/proxy_config.h"
#include "net/proxy_resolution/proxy_config_with_annotation.h"
#include "net/proxy_resolution/proxy_config_service.h"
#include "url/gurl.h"

namespace {

int failures = 0;
int checks = 0;

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
  std::printf("chromiumnet: done\n");
  return 0;
}
