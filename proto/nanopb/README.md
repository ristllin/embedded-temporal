# nanopb/ — device protobuf codec

The ESP32-S3 cannot link libprotobuf, so the device side of `IProtoCodec` is
generated with [nanopb](https://github.com/nanopb/nanopb) (static-allocation C
protobuf). Landed:

- `mwf.options` — field caps (`max_size` / `max_count`) bounding every
  string/bytes/repeated/map field so the generated structs are fixed-size (no
  heap churn on the device). ⚠ nanopb applies the **last** matching line, so the
  `*` wildcard defaults come first and specific overrides after.
- the vendored nanopb runtime + `protoc-gen-nanopb` plugin under
  `../third_party/nanopb/` (pinned **0.4.9.1** — see the README recursion note).
- `../gen/nanopb/` — the generated `*.pb.h` / `*.pb.c` stubs, produced by
  `../gen_nanopb.sh` (git-ignored; regenerate with that script).

The nanopb encoders/decoders are wrapped by the device `IProtoCodec<Msg>` twin;
`mwf-core` and `mwf-device` only ever see the contract interface, never nanopb.
The generated C is host-compiled by the `mwf_proto_nanopb_smoke` CMake target as
a build+round-trip gate (see the top-level README "Device twin" section for the
maps / oneof / recursion / Any / huge-message handling and trimming guidance).
