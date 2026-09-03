# Operations

Use unique POSIX names and socket paths for concurrent runs. The default control endpoint is `/tmp/helix_<uid>.sock`. A normal shutdown is `helixctl shutdown`; SIGINT and SIGTERM only set an async-signal-safe flag. If a process is killed, inspect `/dev/shm` and the configured socket before removing only the known Helix endpoints. Permission failures for `/dev/shm` or the socket are fatal and reported with context.

Real-time scheduling is opt-in through `--rt-priority`. Helix requests `SCHED_FIFO` only for pricing workers; an `EPERM` result is non-fatal, normal scheduling remains active, and the startup report shows how many workers were successfully configured. Never grant broad scheduler privileges merely to run the default demo.

NUMA binding is opt-in through `--numa-node`. Helix calls Linux `set_mempolicy(MPOL_BIND)` before allocating the pricing universe and result buffers; unsupported nodes or permission/policy failures leave normal allocation active and are visible as `numa_applied:false`. Run `native-validate.sh` on bare-metal Linux to capture topology, affinity, scheduler availability, tests, benchmarks, and a configurable soak.

Reload requires that the service was started with `--config`. Feed frequency and deterministic seed are reloadable. Shared-memory names and the Unix socket path define live ownership and are immutable; a reload attempting to change them returns a structured error and leaves the active configuration untouched.

Control connections have bounded one-second send and receive timeouts. Commands larger than 4096 bytes are rejected. Status labels a service stale after two seconds without a heartbeat; operators should inspect ring backpressure, process state, and scheduling before removing any IPC endpoint.

Lifecycle and periodic risk logs are line-delimited JSON with monotonic timestamps. Status distinguishes full-ring observations from dropped batches and reports the terminal reason as `control`, `signal`, or `dependency`.

`helix_inspect` reads region headers without taking ownership. `helix_cleanup` is intended for positively identified stale endpoints and applies namespace, type, and ownership guards. Prefer graceful `helixctl shutdown`; cleanup cannot repair application state. Installed example systemd units use `NoNewPrivileges=true` and normal scheduling by default.
