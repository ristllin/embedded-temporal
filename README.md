# embedded-temporal

Run [Mistral Workflows](https://docs.mistral.ai/studio-api/workflows/) (durable,
Temporal-based orchestration) on an **ESP32-S3 microcontroller**, in C++.

A workflow is a small JSON document — a sequence of steps that call *activities*
and branch on their results. Normally a worker that runs those workflows is a
Python or Go process on a server. This project is a worker small enough to run on
a ~$10 board: the ESP32 registers a workflow with Mistral, polls for work, runs
the orchestration itself (deterministic replay — ~50 KB of static RAM for the
engine; history and TLS buffers live in the board's PSRAM), executes its
own activities, and reports back. The same C++ core also builds and runs on your
desktop, so you can develop and test without hardware.

Why you might want this: the device becomes a durable, observable actor in a
larger system. A sensor reading, a button press, or an LLM call on the device is
a workflow step that Mistral records, retries, and can pause/resume — without a
server babysitting the board.

## What's here

| You want to… | Go to |
|---|---|
| Understand what a workflow is and **write your own** | **[docs/writing-a-workflow.md](docs/writing-a-workflow.md)** |
| Copy a working example as a starting point | [cookbooks/](cookbooks/) |
| Build and run it yourself | [Quickstart](#quickstart-desktop) below |
| Use it as a dependency in your firmware | [Install](#install) below |
| Flash a device | [firmware/README.md](firmware/README.md) |
| Know how it works inside | [docs/architecture.md](docs/architecture.md) |
| Know its limits vs full Temporal | [docs/limitations.md](docs/limitations.md), [ROADMAP.md](ROADMAP.md) |

## Quickstart (desktop)

No hardware needed. You need a Mistral API key and a C++ toolchain.

```bash
# 1. deps (macOS/Homebrew — the desktop build is macOS-only for now;
#    proto/gen_host.sh depends on Homebrew paths)
brew install cmake protobuf grpc abseil nlohmann-json curl openssl

# 2. generate the protobuf C++ (git-ignored), then build the worker
./proto/gen_host.sh
cd transport && cmake -B build && cmake --build build --target cookbook_worker

# 3. point it at your Mistral account and run the hello-world cookbook
echo "MISTRAL_API_KEY=sk-..." > ../.env        # gitignored
cd ../cookbooks/01-hello-world && ./run_local.sh
```

`run_local.sh` registers a workflow with Mistral, triggers it, and prints the
result. You'll see the execution show up in your
[Mistral Studio](https://console.mistral.ai) → Workflows dashboard.

## Install

To use the device worker as a library in your own ESP32 firmware (needs the
arduino-esp32 core 3.x):

**PlatformIO** — add to `platformio.ini`:

```ini
lib_deps = ristllin/embedded-temporal@^0.1.1
```

Or depend on the repository directly, no registry needed:

```ini
lib_deps = https://github.com/ristllin/embedded-temporal.git#v0.1.1
```

**Arduino IDE** — Library Manager → search **EmbeddedTemporal** → Install.

## What a workflow looks like

The workflow is data (`spec.json`):

```json
{
  "name": "greet",
  "steps": [
    { "type": "activity", "name": "say_hello", "id": "greeting", "args_from": "/input" },
    { "type": "complete", "result_from": "/results/greeting" }
  ]
}
```

The activity is a C++ function you register by name — this is the code that runs
on the worker (device or desktop):

```cpp
registry.registerActivity("say_hello", [](const mwf::Bytes& arg) {
  auto in = mwf::json::parse(arg);                       // {"name": "..."}
  std::string out = mwf::json{{"greeting", "Hello " + in.value("name","world")}}.dump();
  return mwf::Result<mwf::Bytes>::success({out.begin(), out.end()});  // -> /results/greeting
});
```

That's the whole model: **a JSON spec of steps, and named C++ activities the
steps call.** The full walkthrough — data flow, conditionals, human-in-the-loop
pauses, and how to put it on a device — is in
[docs/writing-a-workflow.md](docs/writing-a-workflow.md).

## Cookbooks

Each is a runnable, self-contained starting point you can copy:

- **[01-hello-world](cookbooks/01-hello-world/)** — the minimal workflow: one activity, complete.
- **[02-ai-workflow](cookbooks/02-ai-workflow/)** — call the Mistral chat API from an activity, branch on the result.
- **[03-reproducibility](cookbooks/03-reproducibility/)** — the same input always produces the same run (Temporal's determinism guarantee).
- **[04-human-in-the-loop](cookbooks/04-human-in-the-loop/)** — a workflow that pauses until a person approves it (a button press on the device), then resumes — surviving reboots.

## Repository layout

```
docs/          how-to guide + technical reference
cookbooks/     runnable examples (start here to learn)
firmware/      the ESP32-S3 application (PlatformIO)
transport/     desktop gRPC + the ESP32 gRPC-over-HTTP2 stack
core/          the portable engine: workflow replay + the poll loops
codec/         the Mistral payload format
proto/         the Temporal protobuf messages (desktop + device builds)
contracts/     the interfaces that let one worker core run on host and device
conformance/   test oracle, golden vectors, and end-to-end checks
```

## Status

This implements a working, deliberately-scoped slice of Temporal — enough to run
real workflows on a device, not the whole product. It runs on an ESP32-S3 with
PSRAM and on desktop. See [docs/limitations.md](docs/limitations.md) for what it
does and doesn't do, and [ROADMAP.md](ROADMAP.md) for what's next.

**Security:** the device firmware currently skips TLS certificate validation and
can carry your API key in the firmware image — fine for a trusted LAN and a
scoped key, not for production. Read [SECURITY.md](SECURITY.md) before using a
real key.

## License

MIT — see [LICENSE](LICENSE). Third-party components under [NOTICE](NOTICE). An
independent, clean-room implementation; not affiliated with Temporal Technologies
or Mistral AI.
