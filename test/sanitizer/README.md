# Sanitizer runs

Sanitizers do not get a second test suite. They rebuild the current suites and
binaries with instrumentation and run the same `test/run.py` suites.

```sh
test/run.sh sanitize asan   [suite...]
test/run.sh sanitize tsan   [suite...]
test/run.sh sanitize ubsan  [suite...]
```

Default matrix:

| sanitizer | suites |
|---|---|
| `asan` (ASan + LSan + UBSan) | `unit openapi client-diff server-diff concurrency` |
| `tsan` | `unit concurrency server-diff` |
| `ubsan` | `unit client-diff server-diff` |

`conformance` and `stress` are available explicitly; they are much heavier
under instrumentation:

```sh
test/run.sh sanitize asan conformance
test/run.sh sanitize asan stress
test/run.sh sanitize asan conformance --with-autobahn
```

Sanitizer suites run serially by default (`--parallel 1`): each instrumented
process uses much more memory, and timing-sensitive tests are easier to reason
about one at a time. If the machine has memory to spare, pass
`--parallel 2`/`3`; port lanes are the same as the normal runner.

## Compile parallelism

The default is **7** (`--jobs 7`). The unit suite is compiled from 35 translation
units; ASan/TSan increase memory per compiler process, and `-j14` OOMs on a
typical WSL2 machine. Override only if the machine has enough memory:

```sh
test/run.sh sanitize asan unit --jobs 4
```

## Build isolation

By default each sanitizer gets a detached worktree at
`.cache/sanitize/worktree-<san>` and its own `build/`. The normal `build/` and
`xmake` config are untouched. The worktree is removed after the run unless
`--keep-worktree` is passed.

To reuse an existing `build-asan`/`build-tsan` directory in the main checkout:

```sh
test/run.sh sanitize asan unit --build-dir build-asan --jobs 7
```

This reconfigures the main checkout with `xmake f -c -o build-asan ...` and
restores the default config (`xmake f -c -o build`) afterwards. Do not run it
concurrently with a normal build.

## What is instrumented

- C++ test binaries: `test_unit`, `diff_client_*`, `diff_server_cpp`,
  `diff_openapi_server`, `fixture_conformance_server`,
  `test_sanitizer_concurrency`.
- Go reference servers and probes are **not** instrumented (they are ordinary
  Go binaries); only the C++ side is checked.
- Python `httpx` used by `openapi` is not instrumented.

## Concurrency target

`concurrency` is `test/sanitizer/concurrency.cpp`:

- one `Server` with 4 worker threads;
- 6 clients, each with its own single-threaded `io_context` and `Client`;
- shared `RateLimiter`, compression middleware, WebSocket echo;
- a handler that writes `ResponseWriter` from a different executor;
- a thread that repeatedly replaces the global log sink.

ASan catches use-after-free during teardown; TSan catches races in handler-layer
shared state, log-sink replacement and the cross-executor write hop.

OpenSSL 4 initializes its algorithm/property hash table lazily; without a
warm-up, TSan reports `ossl_ht_get` as a race when several `Client` objects
construct their SSL contexts concurrently. The library now performs that
warm-up once under `std::call_once` in
`include/simple_http/transport/openssl_init.h`, called by both the client and
server context builders. The harness no longer needs any OpenSSL-specific
setup. A context still carries per-Client TLS policy; callers that want to
share one across Clients can build it with `make_client_tls_context()` and set
`TlsClientConfig::shared_context`.

## Logs and findings

`sanitize.py` scans the report directory for:

- `ERROR: AddressSanitizer` / `ERROR: LeakSanitizer` / `runtime error:`
- `WARNING: ThreadSanitizer` / `ThreadSanitizer: data race`

Report:

```
.cache/sanitize/<san>/<timestamp>/
├── sanitize-report.md
├── sanitize-report.json
├── runner.log
├── conformance.log
├── conformance-server.log     # copied from the fixture's cache
└── ...
```

A forced `SIGKILL` (server did not exit within
`SIMPLE_HTTP_TEST_STOP_GRACE_MS`, default 15s here) is reported as
`STOP_TIMEOUT` and means the leak check was skipped, not that it passed.

## Suppressions

`lsan.supp`, `tsan.supp`, `ubsan.supp` start empty on purpose. Only add a rule
for a confirmed third-party allocation/race, with a comment explaining why; do
not hide simple_http findings.
