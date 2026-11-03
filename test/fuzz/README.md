# Parser fuzzing

Four targets, each exposing the standard `LLVMFuzzerTestOneInput` entry point:

| target | input |
|---|---|
| `fuzz_h1` | HTTP/1.x request head; whole-block and byte-at-a-time parsing |
| `fuzz_h2` | HTTP/2 frame header + HPACK header blocks |
| `fuzz_ws` | WebSocket frames, masked (server) and unmasked (client) parsers |
| `fuzz_multipart` | incremental `multipart/form-data`; first line is the boundary |

Corpora live in `corpus/<name>/` as ordinary files.

## Default: GCC + ASan/UBSan

The repository's default toolchain is GCC, which has no libFuzzer runtime.
`standalone_main.cpp` is a bounded deterministic mutation driver: it runs every
seed verbatim, then mutates them for a fixed number of iterations. It links the
same target source against ASan + UBSan:

```sh
test/fuzz/run.sh
FUZZ_ITERATIONS=100000 test/fuzz/run.sh
FUZZ_JOBS=7 test/fuzz/run.sh
```

This is not coverage-guided, but it catches crashes, out-of-bounds reads,
use-after-free and UB in the parsers on every machine.

## libFuzzer (clang)

`/usr/lib/llvm-24` (the SDK used by `v2ray-cpp/build.sh`) is detected
automatically; otherwise `clang++` on `PATH` is used. The script configures an
isolated `build-fuzz` directory with `--toolchain=llvm`,
`-stdlib=libstdc++` (to match the GCC-built dependencies) and
`--fuzz_libfuzzer=y`:

```sh
test/fuzz/run.sh --libfuzzer --time 60
test/fuzz/run.sh --libfuzzer --time 300 --jobs 7
```

The mutated corpus is written to `.cache/fuzz/corpus-<name>/`; the committed
seed corpus is copied there first and never modified.

## Adding a target

1. Add `fuzz_<name>.cpp` defining `extern "C" int LLVMFuzzerTestOneInput(const
   std::uint8_t *, std::size_t)`.
2. Add `"<name>"` to the loop in `xmake.lua` (the `fuzz_` prefix is added).
3. Add `corpus/<name>/` with a few valid and boundary-case seeds.
4. Add it to `TARGETS`/`NAMES` in `run.sh`.

The target must be deterministic, must not perform I/O or network access, and
should bound any loops/allocation by the input size.

## Reports

- Default mode prints `fuzz: N iterations, M seeds` per target; ASan/UBSan
  abort with a report on a finding.
- libFuzzer prints its own coverage/crash report and writes a reproducer under
  the corpus directory.
- A crash is a real finding: minimize it with `-minimize_crash=1`, add it to
  the corpus, and fix the parser.
