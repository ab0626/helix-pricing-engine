# Helix — Low-Latency Derivatives Pricing Infrastructure

Helix is a compact C++20/Linux options-pricing system. A market-data process publishes sequence-counter snapshots through POSIX shared memory; a pricing process evaluates only the changed underlying shard of a preallocated structure-of-arrays universe and publishes fixed-size result batches; a risk process consumes an acquire/release SPSC ring and aggregates both per-underlying and portfolio exposure; and `helixctl` controls and observes the services through a permission-restricted Unix-domain socket.

Implemented today: Black–Scholes–Merton price and analytic Greeks, Black-76, exact and fast normal CDFs, bisection/Brent/safeguarded-Newton root finding, bounded deterministic SVI and constrained multi-expiry surfaces, QR decomposition, aligned SoA batch pricing, runtime-gated AVX2 and AVX-512 kernels, optional NUMA binding, persistent pinned worker shards with condition/hybrid synchronization, robust process-shared mutex recovery, sequence-counter snapshots, broadcast multi-consumer result rings, JSON-line control commands, signals, orchestration/supervision, tests, presets, and JSON benchmarks. The runtime is Linux-only; numerical code and tests also build on Windows.

## Build and test

Prerequisites are Linux, CMake 3.24+, a C++20 GCC or Clang toolchain, Ninja,
POSIX threads/shared memory/message queues, and standard development headers.
Debian dependency auto-detection additionally uses the `file` and `dpkg-dev`
packages; CPack still produces an archive if those optional tools are absent.

```bash
cmake --preset debug
cmake --build --preset debug -j 4
ctest --preset debug --output-on-failure
cmake --preset release
cmake --build --preset release -j 4
./build/release/helix_bench kernel 200000
./build/release/helix_bench scaling 200000
./build/release/helix_bench iv 20000
./build/release/helix_ipc_bench
```

Install and consume as a CMake package:

```bash
cmake --install build/release --prefix /opt/helix
/opt/helix/bin/helix_demo --duration 3

# Downstream CMakeLists.txt:
# find_package(Helix 0.2 REQUIRED)
# target_link_libraries(my_pricer PRIVATE Helix::numerics)
```

Create reproducible binary packages after a Release build:

```bash
cpack --config build/release/CPackConfig.cmake -B _packages
# Produces a versioned Linux tarball and Debian package.
```

The exported targets are `Helix::common`, `Helix::numerics`, `Helix::pricing`, `Helix::concurrency`, `Helix::posix`, and the compatibility umbrella `Helix::core`. Public module entry points live under `helix/numerics`, `helix/pricing`, `helix/volatility`, `helix/concurrency`, `helix/posix`, and `helix/risk`.

Sanitizer presets are `asan-ubsan` and `tsan`; run each separately. The default configuration uses at most two workers and a 4,096-option universe, comfortably below the eight-core/2 GiB budget.

## Multi-process demo

In separate Linux terminals, from the build directory:

```bash
./helix_market_data
./helix_pricer
./helix_risk
./helixctl status
./helixctl shutdown
./helix_demo --duration 3
./helix_supervisor --config ../config/helix.example.toml
```

Names and paths can be isolated with `--market`, `--results`, and `--socket`. All executables reject unknown options, and configuration files reject unknown keys. Shared-memory creators take an advisory lifetime lock, refuse to replace a live or differently-owned region, and reclaim only a stale same-user object. The default `cleanup_policy = "owner"` unlinks on orderly creator exit; `"retain"` leaves regions for inspection.

When no socket path is supplied, Helix uses `/tmp/helix_<uid>.sock`. Every executable supports `--help` and `--version`; the control commands are `status`, `start`, `pause`, `resume`, `reload`, and `shutdown`.

Configuration can be loaded with `--config config/helix.example.toml`; later CLI flags override file values. Runtime topology includes `underlyings` (1–8), `ring_capacity` (2–256), `risk_consumers` (1–8), and `drop_policy` (`drop` or bounded-wait `block`). Every risk process uses a distinct `consumer_id`; the producer retains a batch until all configured cursors advance. `allowed_cpus` accepts comma-separated Linux CPU IDs and is intersected with the process affinity mask; `--no-affinity` disables pinning. `numa_node = -1` disables NUMA policy, while a nonnegative node requests `MPOL_BIND` before hot-buffer allocation and reports whether it succeeded. `--rt-priority 1..99` requests `SCHED_FIFO`; permission failure is non-fatal.

`helixctl status` aggregates live market, pricer, risk, and ring state: actual kernel, worker/affinity/scheduler outcomes, heartbeat ages, snapshots/retries, batches and options processed, ring full events/high-water/drops, invalid records, solver/calibration failures, sequence gaps, shutdown reason, and configuration revision. `helixctl reload` reparses the original configuration, applies reload-safe feed settings, and rejects topology or IPC changes while running.

Status also carries allocation-free market-to-risk latency telemetry: count, min, mean, max, and bounded logarithmic p50/p95/p99/p99.9 estimates. Pricer and risk health is classified as `starting`, `healthy`, `stale`, or `stopped` from PID, heartbeat age, and shutdown state.

## Mathematical foundations

For spot $S$, strike $K$, time to expiry $T$, continuously compounded rate $r$, dividend yield $q$, and volatility $\sigma$, Helix evaluates Black–Scholes–Merton using

$$
d_1 = \frac{\ln(S/K) + (r-q+\tfrac12\sigma^2)T}{\sigma\sqrt{T}},
\qquad
d_2 = d_1-\sigma\sqrt{T}.
$$

European call and put prices are

$$
C = Se^{-qT}N(d_1)-Ke^{-rT}N(d_2),
\qquad
P = Ke^{-rT}N(-d_2)-Se^{-qT}N(-d_1),
$$

where $N$ is the standard normal CDF and $\phi$ its density. The main analytic sensitivities used by the risk process include

$$
\Delta_C=e^{-qT}N(d_1), \qquad
\Delta_P=-e^{-qT}N(-d_1),
$$

$$
\Gamma=\frac{e^{-qT}\phi(d_1)}{S\sigma\sqrt{T}},
\qquad
\mathcal V=Se^{-qT}\phi(d_1)\sqrt{T}.
$$

Implied volatility is the bounded root $\sigma^*$ of

$$
f(\sigma)=V_{\mathrm{BSM}}(\sigma)-V_{\mathrm{market}}=0.
$$

Helix supplies bisection, Brent, and safeguarded Newton solvers. Newton uses vega as $f'(\sigma)$, but falls back to the maintained bracket when the derivative is too small or a proposed step leaves the admissible interval.

For log-moneyness $k=\ln(K/F)$, each raw SVI expiry slice models total implied variance as

$$
w(k)=a+b\left[\rho(k-m)+\sqrt{(k-m)^2+\eta^2}\right],
$$

with $b\ge 0$, $|\rho|<1$, and $\eta>0$. Across expiries, Helix constrains calendar spreads by requiring $w(k,T_{i+1})\ge w(k,T_i)$ on the declared dense domain. It checks butterfly admissibility through the Gatheral density condition

$$
g(k)=\left(1-\frac{k w'(k)}{2w(k)}\right)^2
-\frac{[w'(k)]^2}{4}\left(\frac1{w(k)}+\frac14\right)
+\frac{w''(k)}2 \ge 0.
$$

These numerical constraints are checked densely over the configured log-moneyness interval; they are deliberately described as domain-bounded validation rather than a global symbolic proof.

## Architecture and limits

```text
market_data -- seqlock shared snapshot --> pricer -- broadcast shared ring --> risk consumers
     ^
     +----------- Unix socket ----------- helixctl
```

The shared-memory atomics use a runtime lock-free check and fall back to the process-shared mutex snapshot path. Result slots become visible only with the release-store of the write cursor, so a producer crash while filling a slot cannot publish a partial batch. AVX2 and AVX-512 are isolated translation units and never execute without runtime CPU support. Multi-expiry SVI calibration orders expiries, enforces nondecreasing total variance, and checks the Gatheral density condition over the declared dense log-moneyness domain; validation reports the minimum calendar spread and density. This is a domain-bounded numerical guarantee, not a proof outside the configured domain. No performance number is presented without a labeled measurement environment.

See [architecture](docs/architecture.md), [memory model](docs/memory-model.md), [numerics](docs/numerical-methods.md), [benchmarking](docs/benchmarking.md), and [operations](docs/operations.md).

## Measured results

Measured 2026-09-01 in a Debian Bookworm Docker container with GCC 12.2, Release mode, AVX2 enabled, and 8 logical CPUs available. These are observations from this machine, not performance guarantees:

- Scalar SoA fast CDF: p50 203.3 ns/option; p99 253.7 ns/option.
- AVX2 fast: p50 133.3 ns/option; p99 184.9 ns/option.
- 20,000-option worker batches, p50: 4.74 ms (1 worker), 2.19 ms (2), 1.80 ms (4), 1.26 ms (8).
- Safeguarded Newton IV: p50 0.9 µs and 4.32 mean iterations; bisection: p50 7.6 µs and 35.96 mean iterations.
- Fast CDF on [-8, 8]: maximum absolute error 7.452e-8; RMS error 3.455e-8.
- Cross-process measurements on 2026-09-02 under Docker/WSL2: Unix socket p50/p99 54.5/182.6 µs, POSIX message queue 58.4/158.7 µs, sequence polling 0.1/0.2 µs, and process-shared condition variable 59.0/164.7 µs.
- Four-thread false-sharing demonstration: padded counters were 4.76× faster in the measured container run.

ASan/UBSan and TSan were run separately in the same Linux container; both unit and end-to-end integration tests passed. TSan emits GCC's expected warning that `atomic_thread_fence` itself is not instrumented.

The current eight-test Linux suite additionally kills a robust-mutex owner and verifies `EOWNERDEAD` repair, refuses a second live creator, tests process-shared condition signaling, rejects wrong ABI/size and partially initialized headers, publishes 500,000 correlated cross-process seqlock records, verifies small-ring backpressure and three-consumer broadcast delivery, replaces a killed supervised cohort, opens simultaneous control clients, and checks fragmented/oversized/malformed socket input.

## Operator utilities

```bash
helix_inspect --market /helix_market --results /helix_results
helix_cleanup --market /helix_market --results /helix_results \
  --socket /tmp/helix.sock
capture-benchmarks.sh results.jsonl
perf-stat.sh helix_bench kernel 200000
native-validate.sh build/native-release 3600
```

`helix_cleanup` refuses shared-memory names outside the `/helix_` namespace and refuses unowned or non-socket filesystem paths. Hardened example systemd units are installed under the platform library directory. `helix_supervisor` restarts the market/pricer/risk cohort together after an unexpected child exit. Every service and operator executable supports `--version`.
