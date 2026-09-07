// transport example: worker_main — the desktop C++ activity worker.
// Assembles the pieces end-to-end:
//   DesktopTransport  — grpc++ generic stub, TLS or local dev
//   HostProtoAdapter (proto layer) — worker-loop proto seam
//   PayloadCodecV1    (codec) — Mistral wf_v1 envelope + json/plain compat
//   ActivityRegistry + WorkerLoop (core) — poll/dispatch/respond
//
// Registered activities mirror conformance/capture/workflows.py signatures
// (str -> str) with output branded "from C++" so the E2E gate can assert the
// activity really ran here:
//   greet(name)  -> "Hello <name> from C++"
//   shout(text)  -> upper(text) + "!"
//
//   ./mwf_worker --target localhost:7233 --namespace default \
//                --task-queue mwf-cpp-e2e --identity mwf-cpp@desktop
//   ./mwf_worker --api-key-env MISTRAL_API_KEY --whoami --task-queue default \
//                --max-ticks 1              # live Mistral long-poll probe
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

// MWF_USE_H2_TRANSPORT: build the SAME worker over the esp/grpc_h2 core (the
// ESP32's framing layer) instead of grpc++ — the mwf_worker_h2 target. Running
// the desktop_worker_e2e gate with that binary proves the device framing on a
// real task round-trip.
#ifdef MWF_USE_H2_TRANSPORT
#include "../esp/host/h2_host_stream.h"
using TransportImpl = mwf_transport::H2HostTransport;
#else
#include "desktop_transport.h"
using TransportImpl = mwf_transport::DesktopTransport;
#endif
#include "host_proto_adapter.h"
#include "mwf_codec/payload_codec.h"
#include "mwf_core/activity_registry.h"
#include "mwf_core/worker_loop.h"
#include "rest_client.h"

using mwf::Bytes;

namespace {

std::atomic<bool> g_stop{false};
void onSignal(int) { g_stop = true; }

std::string toString(const Bytes& b) { return std::string(b.begin(), b.end()); }
Bytes toBytes(const std::string& s) { return Bytes(s.begin(), s.end()); }

// Minimal JSON string codec for the str->str activity signatures. The E2E
// inputs are simple names; escapes are handled for the common cases.
bool jsonUnquote(const std::string& in, std::string& out) {
  if (in.size() < 2 || in.front() != '"' || in.back() != '"') return false;
  out.clear();
  for (size_t i = 1; i + 1 < in.size(); ++i) {
    char ch = in[i];
    if (ch == '\\' && i + 2 < in.size()) {
      char e = in[++i];
      switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        default: out += '\\'; out += e; break;  // \uXXXX etc: pass through
      }
    } else {
      out += ch;
    }
  }
  return true;
}

std::string jsonQuote(const std::string& s) {
  std::string out = "\"";
  for (char ch : s) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      default: out += ch; break;
    }
  }
  out += '"';
  return out;
}

const char* outcomeName(mwf_core::TickOutcome o) {
  switch (o) {
    case mwf_core::TickOutcome::Idle: return "idle";
    case mwf_core::TickOutcome::Completed: return "completed";
    case mwf_core::TickOutcome::Failed: return "failed";
    case mwf_core::TickOutcome::TransportError: return "transport-error";
    case mwf_core::TickOutcome::ProtocolError: return "protocol-error";
  }
  return "?";
}

struct Args {
  std::string target = "localhost:7233";
  std::string ns = "default";
  std::string task_queue = "mwf-cpp-e2e";
  std::string identity = "mwf-cpp-worker@desktop";
  std::string api_key_env;   // env var NAME holding the bearer key
  bool whoami = false;       // discover target+namespace via the REST control plane
  int tls = -1;              // unset → whoami's tls (else on); --tls 0 opts out
  int poll_ms = 30000;
  long max_ticks = 0;        // 0 = run until signal
};

bool parseArgs(int argc, char** argv, Args& a) {
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    auto next = [&](std::string& into) {
      if (i + 1 >= argc) return false;
      into = argv[++i];
      return true;
    };
    std::string v;
    if (k == "--target" && next(v)) a.target = v;
    else if (k == "--namespace" && next(v)) a.ns = v;
    else if (k == "--task-queue" && next(v)) a.task_queue = v;
    else if (k == "--identity" && next(v)) a.identity = v;
    else if (k == "--api-key-env" && next(v)) a.api_key_env = v;
    else if (k == "--poll-ms" && next(v)) a.poll_ms = std::atoi(v.c_str());
    else if (k == "--max-ticks" && next(v)) a.max_ticks = std::atol(v.c_str());
    else if (k == "--whoami") a.whoami = true;
    else if (k == "--tls" && next(v)) a.tls = std::atoi(v.c_str());
    else {
      std::cerr << "unknown/incomplete arg: " << k << "\n"
                << "usage: mwf_worker [--target host:port] [--namespace ns]\n"
                << "  [--task-queue q] [--identity id] [--api-key-env VAR]\n"
                << "  [--whoami] [--tls 0|1] [--poll-ms n] [--max-ticks n]\n";
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!parseArgs(argc, argv, args)) return 2;

  std::string bearer;
  if (!args.api_key_env.empty()) {
    const char* key = std::getenv(args.api_key_env.c_str());
    if (!key || !*key) {
      std::cerr << "[worker] env var " << args.api_key_env << " is empty/unset\n";
      return 2;
    }
    bearer = key;
  }

  // Optional control-plane discovery (Mistral): whoami → scheduler + namespace.
  if (args.whoami) {
    if (bearer.empty()) {
      std::cerr << "[worker] --whoami needs --api-key-env\n";
      return 2;
    }
    mwf_transport::RestClient rest("https://api.mistral.ai", bearer);
    auto who = rest.whoami();
    if (!who) {
      std::cerr << "[worker] whoami failed: " << who.error << "\n";
      return 1;
    }
    args.target = who.value.scheduler_url;
    args.ns = who.value.namespace_;
    if (args.tls < 0) args.tls = who.value.tls ? 1 : 0;
    std::cout << "[worker] whoami: scheduler=" << args.target
              << " namespace=" << args.ns << " tls=" << who.value.tls << "\n";
  }

  // TLS is on unless explicitly opted out (--tls 0, for a keyless local dev
  // frontend); the transport refuses to send the bearer over plaintext.
  TransportImpl transport(args.target, bearer, args.tls != 0);
  mwf_codec::PayloadCodecV1 codec;
  mwf_example::HostProtoAdapter adapter;
  mwf_core::ActivityRegistry registry;

  // greet(name: str) -> str  (workflows.py signature; output branded C++)
  registry.registerActivity("greet", [](const Bytes& arg) {
    std::string name;
    if (!jsonUnquote(toString(arg), name)) {
      return mwf::Result<Bytes>::failure("greet: argument is not a JSON string: " +
                                         toString(arg));
    }
    return mwf::Result<Bytes>::success(
        toBytes(jsonQuote("Hello " + name + " from C++")));
  });

  // shout(text: str) -> str  (upper + "!", mirrors workflows.py::shout)
  registry.registerActivity("shout", [](const Bytes& arg) {
    std::string text;
    if (!jsonUnquote(toString(arg), text)) {
      return mwf::Result<Bytes>::failure("shout: argument is not a JSON string: " +
                                         toString(arg));
    }
    for (auto& ch : text) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return mwf::Result<Bytes>::success(toBytes(jsonQuote(text + "!")));
  });

  mwf_core::WorkerConfig config;
  config.ns = args.ns;
  config.task_queue = args.task_queue;
  config.identity = args.identity;
  config.poll_deadline_ms = args.poll_ms;
  if (!bearer.empty()) config.call_metadata["temporal-namespace"] = args.ns;

  mwf_core::WorkerLoop loop(transport, codec, registry, adapter, config);

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  std::cout << "[worker] polling target=" << args.target << " ns=" << args.ns
            << " task_queue=" << args.task_queue << " identity=" << args.identity
            << " poll_ms=" << args.poll_ms << "\n"
            << std::flush;

  long ticks = 0;
  int consecutive_transport_errors = 0;
  while (!g_stop) {
    auto t0 = std::chrono::steady_clock::now();
    mwf_core::TickResult r = loop.runOnce(args.poll_ms);
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    std::cout << "[worker] tick#" << ++ticks << " " << outcomeName(r.outcome)
              << (r.activity.empty() ? "" : " activity=" + r.activity)
              << (r.detail.empty() ? "" : " (" + r.detail + ")")
              << " elapsed_ms=" << elapsed_ms << "\n"
              << std::flush;

    if (r.outcome == mwf_core::TickOutcome::TransportError) {
      if (++consecutive_transport_errors >= 5) {
        std::cerr << "[worker] 5 consecutive transport errors — giving up\n";
        transport.close();
        return 1;
      }
    } else {
      consecutive_transport_errors = 0;
    }
    if (args.max_ticks > 0 && ticks >= args.max_ticks) break;
  }

  std::cout << "[worker] stopping (ticks=" << ticks << ")\n";
  transport.close();
  return 0;
}
