# PR #318: local walk lookups and movement spectator union

Research branch: `perf/realistic-load-latency`. This experiment does not merge
into `main` and does not certify production readiness or 1,000-player capacity.

## Scope and references

The starting TFS revision was `04854c381a337eaeab0a8982d0bec1ae5d54536b`.
The BlackTek source comparison used
`828b89395f28852fe2476655d30f1b90507c6066` from
[BlackTek-Server](https://github.com/Black-Tek/BlackTek-Server).
The work remains in [PR #318](https://github.com/Mateuzkl/forgottenserver-downgrade-1.8-8.60/pull/318).

Existing attack/follow generation guards, incremental monster target updates,
balanced think buckets, thread-local A* workspace, path fast rejects, bounded
small packet writers, output pooling, queue protections, per-connection
ASIO framing/XTEA and reactor/combat telemetry were retained. No game timing,
attack rate, condition interval, network budget or packet format was changed.

## BlackTek comparison and decisions

| Concept | Decision | Reason |
| --- | --- | --- |
| Monster-local walk/map cache | Conservative port | Cache tile lookups, not dynamic permissions; reject only currently impossible tiles and validate all potential destinations. |
| Incremental local movement | Port | A lazy 3x3 ring retains overlapping entries after an adjacent step. Teleports, floor/instance changes and tile-layout mutations invalidate it. |
| One movement spectator snapshot | Port | One scan of the exact old/new viewport union replaces two overlapping scans on adjacent same-floor moves. |
| Chunk/SoA coordinate arrays and SIMD masks | Deferred | Requires a second spatial index or substantial map rewrite, with mutation and instance semantics to prove independently. |
| Synchronous A* and thread-local workspace | Already present | Workspace reuse is not parallel pathfinding; no workers were added. |
| Rotating think slots | Existing balancing retained | TFS already chooses the shortest bucket; combat and condition timing were preserved. |
| Reuse a snapshot after movement callbacks | Not ported | Callbacks can move/remove/add spectators before tile notifications. The later queries remain fresh. |
| Uniform player stack index | Not ported | Instance-filtered items, invisible/ghost creatures and viewer permissions can change stack indices. Per-viewer computation remains. |
| One thread-local scratch vector/span | Not ported | Recursive callbacks can overwrite borrowed storage. The event-local owning snapshot has a clear synchronous lifetime. |
| Shared encrypted output | Rejected | Framing, key state and output ownership remain per connection. |

BlackTek's wider chunk/SoA architecture is a plausible additional advantage,
not a measured result of this experiment. BlackTek itself was not run against
the same database, map, protocol and bots, so its relative capacity is unproven.

## Implementation

### Movement spectators

`Map::getMovementSpectators` scans one expanded rectangle for adjacent moves,
then removes the two extra diagonal corners. This matters: simply using a
larger rectangle would notify creatures outside both original viewports.
The result preserves the old pointer-sorted union and type partitions.
Teleports/non-adjacent moves/floor changes retain the original two-query path.
BlackTek's reference move path uses a broader symmetric expanded rectangle;
the port deliberately uses TFS's exact union instead of copying that geometry.

The snapshot owns its creature references through packet dispatch and creature
callbacks. It is not retained asynchronously. Tile post-notification queries
are deliberately not replaced by a stale pre-callback snapshot.

### Local monster tile lookups

The ring stores only a position key and a non-owning `Tile*`. The owning map and
dispatcher-only mutation discipline bound pointer lifetime. A map-layout
revision invalidates cached missing/removed/recreated slots. Removal advances
the revision both before callbacks and immediately before releasing the tile:
a callback can otherwise repopulate a cache between those two moments.

Ground, tile flags, creatures, instances, fields, field permissions and monster
state are read live. There is no cached "allowed" result. Potential movement
still performs the existing visible-creature check and real `Tile::queryAdd`.
Ordinary blocking items are not treated as unconditional blockers because
pushable-item permissions can allow them.

This is intentionally narrower than BlackTek's walkability bitset: it trades
some possible speedup for substantially simpler dynamic invalidation.
It accelerates adjacent monster walk decisions, not every A* tile query.

### Diagnostics

The existing opt-in performance manager now reports spectator queries, scanned
leaves/candidates/results, movement events/player candidates, walk decisions,
tile-cache hits/misses, early rejects and final query calls. There are fixed
counters, no unbounded event maps, and no allocation per counted candidate.
`Monster::canWalkTo` has a matching timing scope. These diagnostics are disabled
by default. Spectator/movement counts are candidates, not a claim that every
candidate received a packet.

## Changed files

| File | Purpose |
| --- | --- |
| `src/map.cpp`, `src/map.h` | Exact movement union; tile-layout invalidation; scan/event counters. |
| `src/spectators.h` | Erase/filter and shared sort/unique normalization without changing snapshot ownership. |
| `src/monster.cpp`, `src/monster.h` | Lazy local tile ring and live fast rejects; final dynamic validation retained. |
| `src/performance_metrics.cpp`, `src/performance_metrics.h` | Fixed movement/walk counters and timing scope in the existing profiler. |
| `src/tests/test_map_spectators.cpp` | Differential old/new union, boundaries, floors, teleport and owning snapshot checks. |
| `src/tests/test_monster_walk_cache.cpp` | Real production walk checks for blockers, fields, PZ, instances, removal/recreation and step overlap. |
| `src/tests/test_performance_metrics.cpp` | Counters do no work while disabled and count correctly while enabled. |
| `src/benchs/bench_movement_spectators.cpp` | Execute two-query and exact-union paths on the same synthetic world. |
| `src/benchs/bench_monster_walk.cpp` | Execute pre-cache and current walk decisions on identical clear/blocked tiles. |

The existing CMake benchmark/test discovery includes the new files. No new
production translation unit or Windows project entry is necessary.

## Safety and validation

- Clean baseline Release build and updated Release build completed with GCC
  13.3, C++23, Lua 5.5, unity builds and native optimization in WSL Ubuntu 24.04.
- Eighteen focused CTest executables passed: movement, monster idle/target
  state, creature checks/lifetime, pathfinding, spectators/quadtree/map cache,
  conditions, item fields/lifetime/registry, protocol pipeline and performance
  counters, including six new walk-cache cases.
- Windows MSVC `/Zs /std:c++latest` accepted the modified production sources and
  regression sources. This is syntax validation, not a linked Windows build or
  native live-load test.
- Debug ASan + UBSan builds of the new walk-cache and map spectator tests passed
  with halt-on-error and leak detection enabled. External libraries were not
  rebuilt with sanitizers; this is focused coverage, not a whole-server proof.
- Valgrind reported zero errors and zero definitely/indirectly/possibly lost
  bytes in both test executables. At exit, 424 bytes (walk) and 40,808 bytes
  (spectators) were still reachable; reachable allocations are not reported as
  lost memory.
- No callback/scheduler/async-buffer ownership was changed. No new ownership
  cycle, borrowed span crossing a callback boundary or encrypted-buffer sharing
  was introduced. A future parallel map mutation design must revisit the raw
  tile cache and its dispatcher-only revision contract.

The focused tests are not a substitute for the entire repository suite or a
long-running production soak. Summon/master/removal behavior is covered by the
existing target-state tests; not every live summon/GM/push scenario was tested
interactively. No serializer changed, so existing packet pipeline tests remain
the protocol safeguard rather than a claim of new serializer byte coverage.

## Isolated operation benchmarks

These are operation costs, **not total server CPU or player capacity**. Five
repetitions were run without compilation in parallel. TFS was Release; the
installed Google Benchmark library emitted its DEBUG-library warning. Results
should be reproduced on deployment hardware.

| Spectator union population | Old median CPU | New median CPU | Reduction |
| --- | ---: | ---: | ---: |
| 300 | 16.25 us | 8.78 us | 46% |
| 600 | 39.66 us | 17.00 us | 57% |
| 1,000 | 81.36 us | 29.65 us | 64% |

| Walk fixture | Old median per check | New median per check | Reduction |
| --- | ---: | ---: | ---: |
| 300 monsters, clear tiles | 62.7 ns | 55.7 ns | 11% |
| 1,000 monsters, clear tiles | 72.0 ns | 58.7 ns | 18% |
| 300 monsters, static blockers | 42.7 ns | 31.9 ns | 25% |
| 1,000 monsters, static blockers | 43.7 ns | 31.7 ns | 28% |

Walk timings divide each iteration by four cardinal checks per monster. They
measure stationary warmed lookups; dynamic movement/invalidation correctness
is tested separately, and these timings do not establish chase performance.

## Matched StressBot methodology

Measurements use an isolated copy of the local fixture, not the live database.
Every run restores the same snapshot and launches a fresh server. Synthetic
test characters have fixed level 300 and 50,000 health/mana; they are not GOD
characters. This is not a realistic character-balance or production-map claim.
The map and Demon spawn input are fixed across sides, including failed spawn
positions; actual monster populations are recorded rather than assuming every
spawn entry succeeded.

The same private StressBot build, item metadata, seed `180252`, 150 ms login
ramp, two network threads and modes are used on both sides. Reconnect is off.
Only the isolated test configuration removes per-IP admission limits and
raises connection-rate admission for the ramp; production config is unchanged.
REALISTIC uses its existing 500–1,000 ms cadence and rest/combat weights;
TORTURE uses its existing 175–225 ms cadence. No gameplay work is dropped to
improve charts. Client and server run on the same i5-10300H laptop (4 cores,
8 logical CPUs), so these are exploratory local measurements, not dedicated
server capacity figures.

Initial pilot runs were rejected for incomplete admission/deaths, compilation
overlap, and StressBot parsing errors. The parser had incorrect speech-class
IDs: monster speech omitted its position and channel-orange loot omitted its
channel ID. Relogin-window handling also needed the fork's payload-free format.
The corrected private parser was checked with a loot-message-plus-ping
regression and used identically for the final before/after matrix. The server
protocol was not changed to accommodate the benchmark.

CPU/RSS are sampled per second from the server process. CPU is reported in
one-core units (100% = one logical CPU); divide by eight for this machine's
total logical-capacity percentage. It includes networking as well as game
work. Game/reactor **CPU** alone is N/A: scope elapsed time is not exclusive
CPU attribution. Server report timestamps are attached externally; complete
five-second windows after ramp + 15-second warmup are selected. Every side uses
the final six complete report windows (approximately 30 seconds), without
selecting favorable outcomes. CPU/RSS samples are restricted to that interval.
The bot RNG is seeded; the server RNG is not. Actual monster counts and AI
outcomes can vary despite identical inputs. New counters also add some probe
overhead on the after side, so one instrumented pair is exploratory evidence.

Global queue p95/p99 are N/A because printed window quantiles cannot be merged
into a run-wide percentile. Report the worst five-second p95/p99 separately,
not their average. These histogram estimates are bucket upper bounds and can
exceed the exact observed maximum. Queue mean is weighted by sample count; queue maximum and
backlog maximum refer to selected windows. Rates use those same windows.
Wire metrics count all protocol traffic, including status/authentication, not
only combat; recipient fan-out refers to candidates unless explicitly stated.

### Final matrix

Before is `04854c381a337eaeab0a8982d0bec1ae5d54536b`; after is
`6292c0bbd7b8f358a421d78505eb3f19d58a3f34`. Each pair below is **before -> after**.
There was one run per side/scenario, not a statistically established improvement.
Every REALISTIC/TORTURE row retained its requested player count throughout the
eligible interval, with zero parser errors, unknown opcodes or disconnects.
Both LOGIN_ONLY runs are **invalid as 600-player comparisons**: some idle
characters died/disconnected. Their observed numbers are included, not counted
as evidence of a gain or regression.

| Scenario | Minimum players | Disconnects | Server CPU, one-core % | Mean RSS, MiB | Selected report seconds |
| --- | ---: | ---: | ---: | ---: | ---: |
| LOGIN_ONLY 600 (invalid) | 598 -> 597 | 3 -> 4 | 10.55 -> 14.89 | 249.31 -> 250.44 | 30.010 -> 30.121 |
| REALISTIC 300 | 300 -> 300 | 0 -> 0 | 14.24 -> 13.75 | 211.86 -> 212.05 | 30.017 -> 30.004 |
| REALISTIC 600 | 600 -> 600 | 0 -> 0 | 32.50 -> 33.47 | 273.01 -> 270.69 | 30.010 -> 30.018 |
| REALISTIC 1,000 | 1,000 -> 1,000 | 0 -> 0 | 62.22 -> 55.26 | 367.85 -> 364.10 | 30.011 -> 30.016 |
| TORTURE 1,000 | 1,000 -> 1,000 | 0 -> 0 | 142.09 -> 96.27 | 375.48 -> 370.54 | 30.016 -> 30.020 |

Game/reactor exclusive CPU and global queue p95/p99: **N/A for every row**.
The following percentile columns are the worst individual five-second window,
not the global percentile. All latency columns are milliseconds.

| Scenario | Queue mean | Worst-window p95 | Worst-window p99 | Queue exact max | Backlog max | Single callback max |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| LOGIN_ONLY 600 (invalid) | 0.631 -> 0.985 | 4.194 -> 8.389 | 16.777 -> 33.554 | 21.381 -> 29.028 | 41 -> 47 | 21.601 -> 23.532 |
| REALISTIC 300 | 0.295 -> 0.265 | 2.097 -> 2.097 | 4.194 -> 4.194 | 17.853 -> 17.608 | 62 -> 63 | 17.257 -> 17.462 |
| REALISTIC 600 | 0.516 -> 0.461 | 4.194 -> 4.194 | 33.554 -> 16.777 | 29.469 -> 18.272 | 72 -> 75 | 23.513 -> 13.788 |
| REALISTIC 1,000 | 1.111 -> 0.736 | 16.777 -> 8.389 | 33.554 -> 16.777 | 43.130 -> 27.387 | 87 -> 83 | 38.152 -> 27.683 |
| TORTURE 1,000 | 6.238 -> 1.476 | 67.109 -> 33.554 | 134.218 -> 67.109 | 99.619 -> 56.910 | 495 -> 315 | 71.408 -> 37.962 |

Lower latency must be considered together with useful work:

| Scenario | Path requests/s | Spectator queries/s | Movement calls/s | Successful weapon executions/s | Wire messages/s | Wire bytes/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| LOGIN_ONLY 600 (invalid) | 40.85 -> 40.40 | 310.70 -> 302.35 | 15.49 -> 19.16 | 0 -> 0 | 3,282.34 -> 3,461.41 | 972,098.97 -> 1,025,478.24 |
| REALISTIC 300 | 49.87 -> 46.89 | 547.02 -> 512.83 | 36.51 -> 37.36 | 29.62 -> 30.33 | 4,346.47 -> 4,267.26 | 701,283.41 -> 634,190.17 |
| REALISTIC 600 | 47.92 -> 48.14 | 662.05 -> 621.96 | 49.05 -> 50.57 | 47.68 -> 40.71 | 10,422.39 -> 10,541.61 | 1,475,041.65 -> 1,472,380.17 |
| REALISTIC 1,000 | 47.78 -> 45.11 | 774.68 -> 671.21 | 72.17 -> 67.63 | 40.29 -> 38.85 | 16,846.46 -> 17,776.92 | 1,902,895.67 -> 2,036,102.35 |
| TORTURE 1,000 | 62.33 -> 61.99 | 1,506.86 -> 1,249.90 | 236.94 -> 258.36 | 88.35 -> 50.97 | 29,980.98 -> 27,939.67 | 4,097,051.11 -> 2,629,638.31 |

Movement calls are `Map::moveCreature` scope invocations, not the number of
client movement commands or a count of completed client acknowledgements.
Path node/tile work and actual server populations show workload divergence:

| Scenario | Path nodes/s | Path tiles/s | Monsters at boot, before/after | Monsters at end, before/after |
| --- | ---: | ---: | ---: | ---: |
| LOGIN_ONLY 600 (invalid) | 604.50 -> 339.36 | 2,828.22 -> 1,650.61 | 316 / 315 | 339 / 337 |
| REALISTIC 300 | 488.92 -> 649.55 | 2,309.89 -> 3,064.06 | 314 / 316 | 365 / 363 |
| REALISTIC 600 | 470.51 -> 468.82 | 2,218.69 -> 2,227.30 | 314 / 315 | 353 / 361 |
| REALISTIC 1,000 | 952.32 -> 458.02 | 4,422.05 -> 2,158.28 | 315 / 314 | 358 / 385 |
| TORTURE 1,000 | 972.41 -> 1,028.15 | 4,574.73 -> 4,878.71 | 315 / 315 | 598 / 726 |

The 647 spawn entries did not create 647 monsters: overlapping/invalid spawn
positions failed on both sides. Summoning and differing encounters changed
population later. This fixture therefore does **not** prove performance with
1,000 independently active monsters.

Candidate fan-out per event is **mean / maximum**, with before -> after pairs.
Visibility, known-creature state and actual packet delivery remain separate.

| Scenario | Health candidates | Effect candidates | Distance candidates | Text candidates |
| --- | ---: | ---: | ---: | ---: |
| LOGIN_ONLY 600 (invalid) | 201.50 / 274 -> 214.33 / 275 | 227.52 / 284 -> 236.30 / 286 | 202.52 / 274 -> 215.04 / 274 | 204.94 / 275 -> 217.20 / 275 |
| REALISTIC 300 | 91.89 / 133 -> 83.22 / 132 | 100.59 / 142 -> 88.61 / 142 | 98.90 / 134 -> 83.58 / 130 | 92.77 / 133 -> 84.42 / 133 |
| REALISTIC 600 | 152.25 / 230 -> 161.87 / 237 | 159.04 / 236 -> 172.05 / 244 | 144.05 / 223 -> 159.95 / 237 | 153.55 / 230 -> 162.95 / 238 |
| REALISTIC 1,000 | 192.05 / 303 -> 205.14 / 318 | 193.56 / 313 -> 214.92 / 330 | 213.67 / 301 -> 202.03 / 319 | 194.04 / 306 -> 206.73 / 320 |
| TORTURE 1,000 | 271.02 / 532 -> 189.26 / 512 | 227.82 / 536 -> 155.55 / 520 | 185.52 / 527 -> 178.00 / 511 | 273.68 / 532 -> 194.90 / 516 |

New after-only adjacent walk counters are shown below; comparable before
counters are **N/A**, not zero. The queryAdd count excludes A* and other callers.
Early rejects do not include every unsuccessful walk decision.

| Scenario | Walk checks/s | Tile-cache hits/s | Tile-cache misses/s | Early rejects/s | Final queryAdd/s |
| --- | ---: | ---: | ---: | ---: | ---: |
| LOGIN_ONLY 600 (invalid) | 72.67 | 59.56 | 13.11 | 27.22 | 18.69 |
| REALISTIC 300 | 60.53 | 44.83 | 15.70 | 20.76 | 18.90 |
| REALISTIC 600 | 70.29 | 58.40 | 11.89 | 25.32 | 15.62 |
| REALISTIC 1,000 | 52.84 | 41.41 | 11.43 | 20.99 | 9.49 |
| TORTURE 1,000 | 47.20 | 35.48 | 11.73 | 21.55 | 9.26 |

For example, REALISTIC-600 had 83.1% tile-cache hits and 36.0% unconditional
early rejects. Its instrumented walk scope consumed only about 0.061 ms/s;
it was not the dominant game cost in this fixture. Walk-state rebuild counts,
spectator player-only splits/per-query scan maxima, think-slot scheduling drift,
follow-request reason splits, and aligned output-pool hit/miss/peak metrics were
not collected in this matrix (**N/A**). These are remaining attribution gaps,
not values inferred from other counters.

RSS rose within the selected short window on both sides. REALISTIC-1,000 ran
361.37 -> 370.47 MiB before and 359.18 -> 365.42 MiB after; TORTURE-1,000 ran
375.33 -> 375.53 MiB before and 365.16 -> 371.59 MiB after. There is no large
mean-RSS regression here, but neither these short intervals nor the focused
leak checks demonstrate hours-long memory stability.

### Acceptance assessment

- REALISTIC-300: nearly unchanged CPU and tail latency; one pair cannot
  establish a meaningful end-to-end improvement.
- REALISTIC-600: queue mean/max improved, but CPU increased by about 3% and
  successful weapons fell by about 15%. This is a mixed result.
- REALISTIC-1,000: CPU fell by about 11% and queue mean by about 34%; wire
  traffic rose, while successful weapons fell by about 4% and movement calls
  by about 6%. Promising, but encounters/path work differed.
- TORTURE-1,000: CPU fell by about 32% and queue mean by about 76%, but
  successful weapons fell by about 42% and wire bytes by about 36%. Lower
  combat/fan-out work explains part of the reduction. Do **not** call the
  latency/CPU difference a like-for-like 32% server-capacity improvement.
- LOGIN_ONLY-600: invalid full-admission comparison; rerun with a safe idle
  fixture before using this row for acceptance.

The operation-level changes passed the focused correctness/lifetime checks
and remain research prototypes in PR #318. This single paired matrix does
**not satisfy production performance acceptance** or establish universal
throughput gains. Repeat the runs with better encounter control before
promoting these changes; do not merge this experiment into main on this evidence.

## Interpretation and remaining work

The valid baseline REALISTIC-600 selected windows illustrate why this cannot
be solved by treating A* as the only bottleneck:

| Selected scope | Inclusive elapsed cost per second |
| --- | ---: |
| Health/combat processing (`Game::combatChangeHealth`) | 50.1 ms/s |
| Movement and its notifications (`Map::moveCreature`) | 30.4 ms/s |
| Connection enqueue/locking (`Connection::enqueue`) | 26.4 ms/s |
| Periodic creature checks (`Game::checkCreatures`) | 25.7 ms/s |
| Per-connection crypto/framing (`Protocol::cryptoFrame`) | 13.9 ms/s |
| Spectator collection (`Map::getSpectators`) | 6.9 ms/s |
| Monster think (`Monster::onThink`) | 2.5 ms/s |
| A* (`Map::getPathMatching`) | 0.3 ms/s |

These scopes overlap and mix game/network threads; **do not add them together
or interpret them as exclusive CPU percentages**. They are measured elapsed
costs in this particular fixture, not universal rankings. Health callbacks
alone were 1.5 ms/s; the wider health processing scope must not be mislabeled
as entirely Lua time. That run had about 47.9 path requests/s and 662.0 spectator
queries/s. Reducing repeated movement scans is useful, but cannot remove the
cost of all combat, packet fan-out, per-recipient writes and queueing.

The higher-pressure baseline TORTURE-1,000 selected windows show the same
pattern more strongly:

| Selected scope | Before inclusive ms/s | After inclusive ms/s |
| --- | ---: | ---: |
| Health/combat processing | 229.2 | 87.0 |
| Movement and its notifications | 142.6 | 88.7 |
| Connection enqueue/locking | 125.7 | 75.0 |
| Periodic creature checks | 75.4 | 48.7 |
| Per-connection crypto/framing | 50.0 | 36.5 |
| Spectator collection | 36.1 | 17.7 |
| Monster think | 7.6 | 5.2 |
| A* | 1.1 | 1.0 |

The largest periodic creature-check callback fell from 71.4 to 38.0 ms, while
backlog still reached 315 and queue maximum 56.9 ms after the changes. Printed
reactor reports in the final TORTURE windows had zero expired/dropped tasks;
the implementation did not raise budgets or discard work. The much lower
after-side combat rate/fan-out means the cost table is attribution evidence,
not an isolated causal estimate of this patch's savings.

In this fixture, the measured lag is consistent with long game callbacks and
large per-event combat/movement fan-out, not expensive A* or raw scheduler
sorting alone. BlackTek's cheaper operations may help, but a matched BlackTek
run is still required to explain its claimed overall capacity advantage.

The existing bounded serializers were retained. Remaining larger packet
writers need opcode-specific allocation/serialization profiles and exact
byte tests before another port; capacity-initialization counters alone do not
measure CPU spent zeroing buffers. Likewise, incremental target pruning still
does linear weak-reference validation, but a new target index is not justified
by exclusive CPU measurements from this experiment.

The proved improvement is less repeated work per adjacent movement event and
per warmed local walk decision. It is not evidence that XTEA removal or
multithreaded pathfinding explains another server's results.

The BlackTek hypothesis is therefore partially supported at operation level,
but its overall advantage remains unproven without a matched BlackTek run.
Next experiments should separate client load-generation limits, repeat the
matched runs, timestamp/merge actual queue histograms, and profile exclusive
CPU before larger spatial-index or serializer changes. Short RSS observations
cannot establish memory stability over hours.

### Repeating focused checks

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DBUILD_BENCHMARKING=ON
cmake --build build --target tfs test_monster_walk_cache test_map_spectators bench_monster_walk bench_movement_spectators
ctest --test-dir build -R '^(test_monster_walk_cache|test_map_spectators)$' --output-on-failure
build/src/benchs/bench_monster_walk --benchmark_min_time=0.15s --benchmark_repetitions=5
build/src/benchs/bench_movement_spectators --benchmark_min_time=0.25s --benchmark_repetitions=5
```

Use the repository's normal dependency setup. Raw benchmark artifacts,
private configs/SQL/world copies, credentials and generated binaries are not
included in the commits. Existing user runtime XML changes are left untouched.
