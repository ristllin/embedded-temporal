# Security

`embedded-temporal` is a from-scratch, research-grade implementation of a
Temporal / Mistral Workflows worker for microcontrollers. It is **not audited for
production use**. Read this before pointing a real API key at it.

## ⚠ Device TLS does not validate certificates (yet)

The **ESP32 firmware** connects to `api.mistral.ai` (REST control plane) and
`wf-scheduler.mistral.ai:443` (gRPC data plane) with `WiFiClientSecure::setInsecure()`
— i.e. **no certificate validation**, and it attaches your `MISTRAL_API_KEY` as a
Bearer token on every call. On an untrusted network an on-path attacker (rogue AP,
ARP/DNS spoofing, captive portal) can terminate the TLS, **read the key verbatim**,
and inject forged workflow tasks. The key is account-scoped and billable.

Consequences and guidance:

- **Do not use a high-value or production Mistral key** with the device firmware
  on an untrusted network. Use a scoped/disposable key on a trusted LAN.
- Adding an on-device **CA bundle / certificate pinning** (`esp_crt_bundle_attach`)
  to remove `setInsecure()` is the **top item on the [ROADMAP](ROADMAP.md)**.
- The **desktop worker does validate certificates** (libcurl default `VERIFYPEER`,
  gRPC `SslCredentials` with the system root store) — this caveat is device-only.

## Secrets at rest on the device

The firmware can be provisioned at build time via `-DMWF_PROV_*` (a **gitignored**
`firmware/src/mwf_prov_secrets.h`, never committed). That bakes the key and WiFi
password into the firmware `.bin` (plaintext rodata) and copies them into the NVS
`mwf` namespace on first boot. The default PlatformIO build enables **neither flash
encryption nor secure boot**, so:

- **Never share a built `.bin`** — it contains your key + WiFi password.
- For any real deployment, enable **ESP32 flash encryption + secure boot**, and
  prefer **runtime provisioning** (write NVS over serial) so the key never lands in
  a shareable image.

## What is safe

- **No secret is committed to this repository** (verified: the provisioning header
  was never tracked; no key/password/token literals in history). Scripts read
  `MISTRAL_API_KEY` from the environment (or a local, gitignored `.env`).
- The API key is only ever sent in an `Authorization: Bearer` header — never in a
  URL, query string, or log line (the one config log redacts it to `set`/`MISSING`).
- Production firmware ships at `CORE_DEBUG_LEVEL=1` (errors only) — the verbose
  bring-up trail is a separate `esp32s3_debug` env.

## Untrusted input

The worker parses server-controlled data (Temporal event histories, Mistral
payloads) via nanopb (device) / libprotobuf (host) and a hand-rolled
gRPC-over-HTTP/2 framing layer. Field sizes are capped (nanopb `max_size`), the
gRPC frame length is bounds-checked without integer-overflow, and the response
accumulator is size-capped. Because device TLS is currently unauthenticated, treat
the "server" as potentially attacker-controlled on an untrusted network (see above).

## Reporting a vulnerability

This is a personal research project. Please open a GitHub issue (or, for sensitive
reports, a private security advisory on the repository) rather than disclosing
publicly. There is no formal SLA.
