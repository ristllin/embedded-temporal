# Flashing the ESP32-S3 worker

This is the PlatformIO project that turns an ESP32-S3 into a Mistral Workflows
worker. Once flashed and provisioned, the board connects to Wi-Fi, discovers the
Mistral scheduler (a `whoami` call), self-registers its workflow, and then runs
**both loops itself** — replaying the workflow spec *and* executing the
activities. It is the whole worker on one chip; the desktop
[`cookbook_worker`](../transport/example/cookbook_worker_main.cpp) is the same
[`core/`](../core/) engine, just on your laptop.

This guide covers build → provision → flash → watch. If you're new to the
workflow model itself, read [../docs/writing-a-workflow.md](../docs/writing-a-workflow.md)
first — this page assumes you know what a spec and an activity are.

## Requirements

| You need | Notes |
|---|---|
| An **ESP32-S3 with PSRAM** | e.g. DevKitC-1 N16R8 (16 MB flash / 8 MB PSRAM). A classic ESP32 without PSRAM will not work. |
| **PlatformIO Core** (`pio`) | `pip install platformio`, or the VS Code extension. |
| A **Mistral API key** and **2.4 GHz Wi-Fi** | The board holds a long-poll TLS connection to Mistral. |

**Why PSRAM is mandatory:** one workflow history can decode into a multi-megabyte
buffer, which cannot fit in the S3's ~320 KB internal SRAM — the firmware routes
that buffer, plus the mbedTLS/HTTP-2 session, into the 8 MB PSRAM. See
[../docs/esp32-compatibility.md](../docs/esp32-compatibility.md) for the full
hardware list and measured footprint.

## 1. Build

```bash
cd firmware
pio run -e esp32s3          # compile only, to check your toolchain
```

The first build pulls the platform toolchain and the sibling libraries
(`../core`, `../codec`, `../transport`, `../proto`, `../contracts` via
`symlink://`), so it takes a few minutes; later builds are fast.

## 2. Provision (Wi-Fi + API key → NVS)

The board reads its config from the NVS namespace **`mwf`** — keys `apiKey`,
`staSsid`, `staPass`, and `taskQueue` (default `mwf-esp`). With no `apiKey` it
just boots and idles.

The built-in way to load them is a **first-boot seed** from a gitignored header.
Create `firmware/src/mwf_prov_secrets.h` — it is listed in `.gitignore` and must
never be committed:

```c
// firmware/src/mwf_prov_secrets.h  — gitignored, never commit
#define MWF_PROV_APIKEY "sk-your-mistral-key"
#define MWF_PROV_SSID   "your-2.4GHz-ssid"
#define MWF_PROV_PASS   "your-wifi-password"
#define MWF_PROV_QUEUE  "mwf-esp"        // optional; task queue, defaults to "mwf-esp"
```

If that header exists at build time, `main.cpp` includes it and, **on first boot
when `apiKey` is still empty**, copies these values into the `mwf` NVS namespace
once. Only `MWF_PROV_APIKEY`, `MWF_PROV_SSID`, and `MWF_PROV_PASS` are required;
`MWF_PROV_QUEUE` is optional.

Because the seed only fires when `apiKey` is unset, editing the header and
reflashing will **not** overwrite an already-provisioned board. To re-provision,
erase NVS first, then flash again:

```bash
pio run -e esp32s3 -t erase      # wipes flash incl. the mwf NVS namespace
```

> **These values get compiled into the firmware image** (plaintext), and the
> default build does not encrypt flash. Do not share a built `.bin`, and prefer a
> scoped/disposable key. Read [../SECURITY.md](../SECURITY.md) before pointing a
> real key at the device.

## 3. Flash

```bash
pio run -e esp32s3 -t upload
```

`-t upload` builds and uploads in one step; add `--upload-port /dev/tty...` if
auto-detection picks the wrong port. After it reboots, the board joins Wi-Fi,
runs `whoami`, registers its workflow, and starts polling. Trigger a run exactly
as you would any Mistral workflow — see
[../docs/writing-a-workflow.md](../docs/writing-a-workflow.md#step-3--run-it-on-your-desktop)
(the device polls the queue it registered; the result comes back from the board).

## Build environments

| env | What it builds |
|---|---|
| `esp32s3` | Production firmware. **Quiet serial** — errors only (`CORE_DEBUG_LEVEL=1`), so it doesn't stream scheduler URLs, namespaces, or heap to anyone with a cable. |
| `esp32s3_debug` | The same firmware with the **verbose** bring-up trail (`whoami` / register / poll / "workflow completed"). Use this while getting a board working. |
| `esp32s3_cb4` | [Cookbook 04](../cookbooks/04-human-in-the-loop/) — the durable human-in-the-loop workflow that pauses on a signal, delivered by the onboard **BOOT button** (GPIO0) or typing `SIGNAL` on the console. Verbose. |

Swap the env in any command, e.g. `pio run -e esp32s3_cb4 -t upload`.

## Watching serial logs

```bash
pio device monitor -e esp32s3_debug     # 115200 baud, exception decoder
```

The production `esp32s3` env is intentionally quiet, so if you build it you'll
see almost nothing on the wire — flash `esp32s3_debug` (or `esp32s3_cb4`) when
you want to watch the worker come up and process tasks.

## How `main.cpp` wires a workflow

The device build is the on-device version of
[step 4 of the workflow guide](../docs/writing-a-workflow.md#step-4--put-it-on-an-esp32).
Three pieces live in [`src/main.cpp`](src/main.cpp):

1. **The spec, as an embedded string** — `kWorkflowSpecJson`, parsed once in
   `setup()` with `mwf_core::parseWorkflowSpec`. (The `-DMWF_COOKBOOK=4` build
   swaps in the human-in-the-loop spec with a `wait_signal` step.)
2. **The activities** — registered by name on the `ActivityRegistry`; the default
   build registers one, `device.echo`.
3. **Both loops** — `loop()` drives a `WorkflowLoop` (replays the spec) and a
   `WorkerLoop` (runs the activities) round-robin over one shared TLS transport,
   so the single board is both the orchestrator and the executor.

To run your own workflow on the device, edit those three things and reflash —
there is no dynamic loading; activities are compiled in.

## Security

The device firmware skips TLS certificate validation (`setInsecure()`) and can
carry your API key inside the firmware image. That's fine for a trusted LAN with
a scoped key, and not for production. **Read [../SECURITY.md](../SECURITY.md).**
