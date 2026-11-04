# Direct-transfer demo: A → B → A

This narrated demo runs Host, Table and two clients as separate processes over
real loopback TCP. A and B successively update the same Host-local Dataset while
Table is stopped. The direct path uses the existing Dynabridge codec, FF async
completion and Lease-backed capabilities.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCAPSTAN_UV_WORKER=ON
cmake --build build --parallel 2
python3 scripts/run_direct_demo.py --build-dir build
```

The usual build prerequisites and locked dependencies apply; see the root
[README](../README.md). Linux is required for this GIO/SIGSTOP demonstration.
For an existing optimized build, replace `build` with its directory name.

## Walkthrough

```text
Table bootstraps exclusive(A, epoch 1)
Table stops
  A: add 3 to elements [0, 1024)
  A: start summary RPC
  A → Host: transfer to B while summary is in flight
      Close → Drain old summary → Install(B, epoch 2)
  B → Host: claim with B's own authenticated identity
  B: add 5 to elements [1024, 2048)
  B → Host: transfer back to A
      Close → Drain → Install(A, epoch 3)
  A → Host: claim and verify the updated Dataset
Table resumes
  Table fetch for B rejects the old holder
  Table management destroys the object after Drain
```

The script prints seven steps and checks each result. It confirms an actual Host
admission before issuing the first transfer on the independent control connection.
Host must report Pending, and B's early claim must return Pending. Only after the
old pin completes may the new grant be claimed. A's sealed wrapper refuses new
calls, and the saved old grant receives Stale from Host. Returning the grant to A
also fences B's old grant.

The first update adds 3072 to the sum; the second adds 5120. The Dataset version
advances twice, and its size stays constant. Each handoff preserves the complete
summary (count, sum, checksum and version). Sum arithmetic follows the Dataset's
64-bit arithmetic. Only authority moves; the Dataset stays on Host.

The demo waits for the operating system to confirm that Table is stopped, and
resumes it in a `finally` block, including on failure or Ctrl-C. Table counters
must remain unchanged across both transfers, claims and business updates. After
resuming, Table consults Host and refuses B's stale authority. A final management
request destroys the object exactly once with zero active pins, demonstrating
that both control paths still work together.

## Output and options

By default, `BUILD_DIR/direct-demo/` contains:

- `report.json`: checks, observed epochs, data changes, directory counter delta,
  per-command results and the script exit code.
- `host.log`, `table.log`, `client_A.log`, `client_B.log`: actual process output.
- `processes.json`: launch commands, process exits and cleanup explanations.

Session credentials live in temporary private files and are deleted during cleanup.
The supervisor terminates persistent Host/Table services; these are documented
cleanup exits rather than natural zero exits. Failed checks make the demo exit
nonzero and leave diagnostics in the output directory.

```sh
python3 scripts/run_direct_demo.py --build-dir build-fast-strict \
  --output build-fast-strict/my-direct-demo
ctest --test-dir build-fast-strict -R '^direct_transfer_demo$' --output-on-failure
```

`--bytes` changes Dataset size (default 64 MiB, minimum 16 KiB).
`--work-delay-ms` is 250 by default, with a range of 100–1000. This artificial
business delay makes the Drain phase observable. **Demo durations are not
benchmark results.** Use `scripts/bench_direct_transfer.py` for zero-delay
measurements and `scripts/run_direct_transfer.py` for disconnect/replay faults.

The independent [TLA+ model](formal-verification.md#holder-transfer-model) checks
bounded transfer safety and intentional rule-breaking mutations. This demo adds
executable integration evidence; it does not establish HA or cross-machine safety.
