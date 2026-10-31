# Fuzzing

libFuzzer harnesses for the parsers that read bytes from the network, from show files and from user folders. They
run CPU code only (no GPU, no AVFoundation), under AddressSanitizer and UndefinedBehaviorSanitizer.

| Harness | Reads |
|---|---|
| `djlink_fuzz` | Pro DJ Link packets (`azl::djlink::parse`) on the three ports |
| `snappy_fuzz` | Snappy blocks (`snappyLength`, `snappyDecompress` up to 64 MiB), and a compress round trip |
| `hap_fuzz` | HAP frames (`decodeHapFrame`, then `hapToRgba`); the first four bytes are the size, up to 1024x1024 |
| `protocol_fuzz` | protocol messages (`azl::decode`, MessagePack and JSON), encoded again both ways |
| `composition_fuzz` | composition files (JSON, `migrateCompositionFile`, `compositionFileFromJson`) |
| `xml_fuzz` | Resolume XML (`parseXml`) |
| `isf_fuzz` | user ISF shaders (`parseIsf`) |
| `websocket_fuzz` | WebSocket frames (`ws::MessageReader`, either side) and handshake heads (`ws::parseHead`) |

## Build

Apple's clang has no libFuzzer, so the harnesses build with Homebrew LLVM (`brew install llvm`) from a configured
build folder, which holds the generated sources they need:

```sh
cmake -S . -B build
fuzz/build.sh build            # harnesses in build/fuzz
```

or `cmake -S . -B build -DNOCTURNE_FUZZ=ON && cmake --build build --target fuzz`. `NOCTURNE_FUZZ_CXX` names another
clang++, `NOCTURNE_FUZZ_JOBS` the parallel compiles (3).

## Run

Each takes a corpus folder it adds to; seed it from the test fixtures where there are some:

```sh
mkdir -p build/fuzz/corpus/xml && cp tests/golden/import/*.xml tests/golden/import/*.avc build/fuzz/corpus/xml/
nice -n 15 build/fuzz/xml_fuzz build/fuzz/corpus/xml -max_total_time=60

mkdir -p build/fuzz/corpus/djlink && cp tests/djlink/*.bin build/fuzz/corpus/djlink/
mkdir -p build/fuzz/corpus/isf && cp third_party/isf/*/*.fs build/fuzz/corpus/isf/
mkdir -p build/fuzz/corpus/composition && cp examples/first-show/show.json build/fuzz/corpus/composition/
```

`hap_fuzz` and `snappy_fuzz` have no fixtures; they get much further from a few frames of `encodeHapFrame` (with the
size in front) and blocks of `snappyCompress` than from an empty folder.

A crash leaves `crash-<sha1>` in the working folder with the input; run the harness on that file alone to reproduce it.
Fuzzing is not part of CI.
