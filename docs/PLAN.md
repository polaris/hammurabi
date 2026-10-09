# Hammurabi: Implementation Plan

Hammurabi is a distributed key-value store built on the Raft consensus algorithm, written in C++23.

This plan is meant to be worked through from top to bottom. Every step is small. Every step ends with a
project that builds and tests that pass.

---

## Contents

1. [How to use this plan](#1-how-to-use-this-plan)
2. [What we are building](#2-what-we-are-building)
3. [Words we use](#3-words-we-use)
4. [What we keep from the old code, and what we learn from it](#4-what-we-keep-from-the-old-code-and-what-we-learn-from-it)
5. [Design decisions](#5-design-decisions)
6. [Toolchain, minimum versions and C++ features](#6-toolchain-minimum-versions-and-c-features)
7. [The steps](#7-the-steps)
   - [Phase 0: Clean slate and tooling](#phase-0-clean-slate-and-tooling)
   - [Phase 1: Building blocks](#phase-1-building-blocks)
   - [Phase 2: Leader election](#phase-2-leader-election)
   - [Phase 3: The simulation](#phase-3-the-simulation)
   - [Phase 4: Log replication](#phase-4-log-replication)
   - [Phase 5: Replica, client sessions, linearizability](#phase-5-replica-client-sessions-linearizability)
   - [Phase 6: Persistence](#phase-6-persistence)
   - [Phase 7: The real network](#phase-7-the-real-network)
   - [Phase 8: Fast linearizable reads (ReadIndex)](#phase-8-fast-linearizable-reads-readindex)
   - [Phase 9: Finish](#phase-9-finish)
8. [Appendix A: Raft rules and where they live](#appendix-a-raft-rules-and-where-they-live)
9. [Appendix B: Ideas beyond this plan](#appendix-b-ideas-beyond-this-plan)

---

## 1. How to use this plan

- The steps are grouped into **phases**:
  - Phases 0–1 lay the foundations.
  - Phases 2–4 build Raft itself.
  - Phase 5 connects the key-value store.
  - Phase 6 makes the data survive restarts.
  - Phase 7 adds the real network.
  - Phase 8 adds fast reads.
  - Phase 9 polishes the project.
- Each step has the same parts:
  - **Goal:** what is true after the step.
  - **Build:** what to write.
  - **Why this way:** the reasoning behind the design.
  - **C++ notes:** what a language feature does here. These appear only where a feature is used for the first time.
  - **Check:** how to verify the step.
- You write the code. The plan gives interface sketches. For a family of similar functions, it shows one worked
  example and you write the rest. Harder ideas are explained with concrete examples.
- From step 0.2 on, every step ends with `scripts/check.sh` passing (see [6.5](#65-the-standard-check)).
  Steps may name extra checks.
- Commit after every step. Use the same message style as the existing history: `feat:`, `fix:`, `test:`,
  `chore:`, `docs:`.

---

## 2. What we are building

```
                         TCP (client protocol)
  hammurabi-cli ────────────────────────────────┐
                                                ▼
        ┌──────────────── hammurabi-server (node 1) ────────────────┐
        │  host            Asio: sockets, timer, coroutines         │
        │   └─ replica     Raft node + key-value state machine      │
        │        ├─ raft::node          pure logic, no I/O          │
        │        └─ kv::state_machine   the actual key-value data   │
        │  file_storage    term, vote and log on disk               │
        └──────────────▲──────────────────────────────▲─────────────┘
                 UDP   │      (Raft messages)         │  UDP
                   node 2                          node 3
```

**A write, from start to finish** (`hammurabi-cli put color blue`):

1. The CLI connects to any server over TCP and sends the request.
2. Suppose that server is a follower. It answers "not leader, try 127.0.0.1:8002". The CLI reconnects there.
3. The leader appends the command to its log and writes it to disk. Then it sends the new log entry to the
   followers over UDP.
4. Each follower writes the entry to disk and confirms.
5. As soon as a **majority** of servers (the leader included) has the entry on disk, the entry is
   **committed**: it can never be lost again.
6. Every server applies committed entries in log order to its key-value map. All maps therefore stay identical.
7. The leader answers the CLI: `ok`.

**A read** works like a write in phase 5: it goes through the log. In phase 8 we add a faster path that skips
the log but is still correct.

**Scope**

| Included | Not included |
|---|---|
| Leader election and log replication (Raft paper §5) | Snapshots / log compaction (the log grows forever) |
| Persistence of term, vote and log on disk | Changing cluster membership at runtime |
| Key-value state machine: `get`, `put`, `delete`, `cas` | Pre-vote and check-quorum (liveness optimizations, see Appendix B) |
| Client sessions, so each write runs at most once | Leader leases |
| Linearizable reads: first through the log, then ReadIndex | Authentication / encryption |
| Server, CLI, deterministic simulation tests, CI | Windows |

---

## 3. Words we use

Technical terms are also explained where they first appear. This table is for looking them up later.

| Term | Meaning |
|---|---|
| **Node** / **server** | One running `hammurabi-server` process. A **cluster** is a fixed group of nodes, usually 3 or 5. |
| **Consensus** | All nodes agree on the same sequence of commands, even when some nodes crash or messages get lost. |
| **Majority** / **quorum** | More than half of all nodes: 2 of 3, 3 of 5, 3 of 4. Any two majorities share at least one node. Raft's safety rests on this fact. |
| **Term** | A number that counts elections. Each term has at most one leader. Terms only ever increase. |
| **Leader / follower / candidate** | The three **roles** a node can have. The leader handles all client requests. Followers copy the leader. A candidate is trying to become leader. |
| **Log** | A numbered list of **entries**. Each entry holds a command and the term in which the leader created it. Numbering (the **index**) starts at 1. |
| **Committed** | An entry is committed once it is stored on a majority. From then on Raft guarantees it stays in every future leader's log. |
| **State machine** | The thing the log drives: here, the key-value map. **Applying** an entry means executing its command on the map. |
| **Heartbeat** | An empty `AppendEntries` message the leader sends regularly to say "I'm still here". |
| **Election timeout** | How long a follower waits without hearing from a leader before it starts an election. It is chosen randomly to avoid ties. |
| **Persistent / volatile state** | Persistent state must survive a crash, so it lives on disk. Volatile state may be lost on a crash and is rebuilt. |
| **fsync** | An operating-system call that forces data from memory caches onto the physical disk. Only after `fsync` returns is the data safe. |
| **Linearizable** | Every operation appears to happen at one instant between its start and its end, as if there were one single copy of the data. Example: once a `put` has returned, every later `get` sees it. |
| **Idempotent** | Doing it twice has the same effect as doing it once. `get` is idempotent; `cas` and `put` are not (see [5.8](#58-client-sessions-making-retries-safe)). |
| **Sans-I/O** | A design where the core logic does no input/output itself (no sockets, no clock, no files). It receives events and returns what should be done. |
| **Deterministic simulation** | Running the whole cluster in one test process, with a fake network and a fake clock controlled by a seeded random generator. The same seed gives exactly the same run. |
| **Tick** | One unit of logical time. The real server calls `tick()` every 10 ms; a test calls it whenever it wants. |
| **Invariant** | A property that must hold at every moment, for example "at most one leader per term". Tests check invariants continuously. |
| **Datagram** | A single UDP packet. It may be lost, duplicated or reordered. |
| **Coroutine** | A function that can pause (`co_await`) and later resume where it left off. Asio uses coroutines to write asynchronous network code that reads like ordinary sequential code. |
| **Sanitizer** | A compiler mode that adds runtime checks. ASan finds memory errors; UBSan finds undefined behavior. |
| **Static analysis** | Finding bugs by reading code without running it. Here: compiler warnings and clang-tidy. |
| **Fuzzing** | Feeding random, mutated inputs into a function to find crashes. |

---

## 4. What we keep from the old code, and what we learn from it

### Keep

| Part | Why |
|---|---|
| `CMakeLists.txt`, `cmake/HammurabiTargetOptions.cmake` | Warnings, sanitizers and clang-tidy are set per target and `PRIVATE`. This is clean, modern CMake. |
| `conanfile.txt`, `requirements.txt` | Pinned tool versions and a simple Conan setup. Protobuf goes away; CLI11 is added. |
| `.clang-format`, `.clang-tidy` | Good base. We may tune checks when real code produces noise. |
| `.github/workflows/ci.yml` | Good base. It is extended in step 0.2. |
| `LICENSE`, `README.md` | The README is rewritten at the end. |

### Rewrite

Everything under `hammurabi/`, `include/`, `server/`, `proto/` and `tests/`. Before deleting, we tag the old
state as `legacy-2019`, so the README can link to "before".

The old code is still valuable as a list of mistakes the new design must make impossible:

| Problem in the old code | How the new design prevents it |
|---|---|
| `set_state<follower>()` destroys the current state object and then reads its member `server_` (use-after-free, `raft.cpp:282`). | Roles are values in a `std::variant` owned by the node. Changing role assigns a new value; no object deletes itself. |
| Votes from older terms are counted; duplicated replies count twice (`raft.cpp:310`). | Votes are a `std::set<node_id>`, and replies with a different term are ignored. |
| Majority is computed from `peers_.size()`; it is wrong for even cluster sizes (`raft.cpp:328`). | One function, `majority() = members.size() / 2 + 1`, unit-tested for sizes 1–7. |
| Votes are granted without checking whether the candidate's log is up to date (§5.4.1). | Explicit rule in step 2.2, with table-driven tests. |
| Index arithmetic mixes a dummy entry at index 0 with 1-based indices; out-of-bounds read. | One convention: index `i` lives at `log_[i - 1]`, and `term_at(0) == 0`. Small helpers do all index math. |
| Replies carry no sender, so the leader cannot track followers. | Every message travels in an `envelope{from, to, message}`. |
| Raft logic is tangled with Asio, real time, random devices, files and `std::cout`. | The sans-I/O core (section 5.1) is testable without any of these. |
| `connector::send` copies at most 1024 bytes but sends `length` bytes (buffer over-read). | The codec builds exactly-sized buffers; sizes are checked on both sides; the decoders are fuzzed. |
| The state file is overwritten in place and errors are swallowed. | Atomic write via temp file + `fsync` + `rename`; I/O errors stop the server (section 5.7). |
| `kv_store` stores `std::any`, which cannot be serialized and so cannot be replicated. | Keys and values are strings; commands are encoded with our own format. |

---

## 5. Design decisions

### 5.1 A Raft core without I/O ("sans-I/O")

The heart of the project is the class `raft::node`. It has **no** sockets, **no** clock, **no** threads and
**no** files. It knows only four kinds of input:

```cpp
node.tick();                         // one unit of time has passed
node.receive(envelope);              // a message from another node arrived
node.propose(command_bytes);         // a client wants something appended to the log
node.request_read(read_id);          // a client wants a linearizable read (phase 8)
```

All results are collected in an **output**, which whoever drives the node picks up afterwards:

```cpp
raft::output out = node.take_output();
// out.messages   : messages to send to other nodes
// out.committed  : newly committed log entries, in order, to apply to the state machine
// out.reads      : reads that may now be answered (phase 8)
```

The only side effect the node performs itself is writing its persistent state through a `storage` interface
(section 5.7). That interface is also replaceable: tests use an in-memory version.

**Why?** A test can drive the node completely, and it controls every single event:

```cpp
raft::memory_storage storage;
raft::node node{config_for_three_nodes(/*self=*/1), storage};
tick_until_election(node);                                    // the test decides when time passes
auto out = node.take_output();
REQUIRE(out.messages.size() == 2);                            // one RequestVote to each peer
CHECK(std::holds_alternative<raft::request_vote>(out.messages[0].msg));
```

There is no sleeping, no flaky timing and no ports. The same design is used by production Raft libraries
such as etcd/raft and TigerBeetle. It makes the **deterministic simulation** in phase 3 possible: many
nodes, a fake network and a fake clock in one process, all reproducible from one random seed.

The real server (phase 7) is a thin shell around the core. It turns UDP datagrams into `receive()` calls,
a 10 ms timer into `tick()` calls, and output messages into UDP datagrams.

### 5.2 Logical time: ticks

The node does not know real time. It counts ticks:

| Setting | Ticks | Real time at 10 ms per tick |
|---|---|---|
| Election timeout | random in [15, 30) | 150–300 ms (the range suggested in the paper) |
| Heartbeat interval | 5 | 50 ms |

**Why ticks instead of `std::chrono` time points?** A tick is the smallest possible "clock interface". The
simulation advances time by calling `tick()`; the server calls it from a timer. We do not need to inject a
clock object, and there is no way for real time to leak into a test.

Randomness, needed for election timeouts, comes from a `std::mt19937_64` seeded from `config`. The
server seeds it from `std::random_device`; the simulation seeds it from the test seed.

### 5.3 Libraries and their layering

The code is split into small libraries. Each CMake target links only what it may use, so the **compiler**
enforces the layering. If `hammurabi_raft` included an Asio header, the build would fail, because Asio is not
on its include path.

```
                 hammurabi_wire        bytes ↔ integers/strings (no dependencies)
                  ▲           ▲
     hammurabi_raft            hammurabi_kv      Raft core + message codec  |  KV state machine,
          ▲      ▲                  ▲              commands, client protocol
          │      └──── hammurabi_replica          raft::node + kv::state_machine, pending client requests
          │                     ▲
   hammurabi_storage            │                 file_storage (POSIX files)
          ▲                     │
          └──── hammurabi_net ──┘                 Asio: UDP transport, TCP client service, host, client
                    ▲
        hammurabi-server, hammurabi-cli          executables (CLI11)

   Tests:  unit_tests        → wire, raft, kv, replica, storage
           sim_tests         → raft, kv, replica  (no Asio, no files!)
           integration_tests → net
```

Directory layout:

```
include/hammurabi/{wire,raft,kv,replica,storage,net}/*.h   public headers
src/{wire,raft,kv,replica,storage,net}/*.cpp               implementation
apps/server/main.cpp, apps/cli/main.cpp
tests/unit/, tests/sim/, tests/integration/, fuzz/
scripts/check.sh, scripts/cluster.sh, scripts/smoke_test.sh
docs/PLAN.md, docs/ARCHITECTURE.md
```

Namespaces follow the libraries: `hammurabi::wire`, `hammurabi::raft`, `hammurabi::kv`, and so on.

### 5.4 One thread per server

Each server runs everything on one thread, in one `asio::io_context`. There are no mutexes and no data races.
Raft does very little work per message; one core is plenty, and the disk (`fsync`) is the real limit.

One pitfall remains, and the plan points it out where it matters. While a coroutine is paused at
`co_await`, other code runs on the same thread and can change state. For example, the node may stop being
leader. After every `co_await`, code must re-check anything it read before.

Because there is only one thread, ThreadSanitizer (TSan) would find nothing. CI runs ASan and UBSan.

### 5.5 Our own wire format

Messages are encoded by hand with a tiny codec (`hammurabi_wire`) instead of Protobuf:

- Integers are fixed-size and big-endian ("network byte order"). This means `u8`, `u32` and `u64`.
- Strings and byte arrays are written as a `u32` length followed by the bytes.
- Every datagram starts with a version byte and a message type byte.

**Why?** Protobuf brings abseil with it, makes the build slow and hides what is on the wire. Our format is
about 200 lines that you fully understand. It teaches `std::span`, `std::byteswap` and `std::expected`, and it
is a perfect target for fuzzing (step 9.1). The version byte lets us change the format later.

### 5.6 UDP between servers, TCP for clients

**Raft over UDP.** Raft was designed for networks that lose, duplicate, delay and reorder messages:

- Lost messages are covered because the leader repeats `AppendEntries` with every heartbeat until a follower
  confirms, and candidates retry elections.
- Duplicates and reordering are harmless because every message carries its term. Replies say *which* index
  they confirm (`match_index`) instead of just "success". Followers never delete entries that match.

UDP therefore needs no extra reliability layer, and it keeps the transport simple: one socket, no
connections.

Limits we accept:

- **Datagram size.** A datagram larger than the network's packet size (MTU, about 1500 bytes on Ethernet)
  gets split into fragments. If any fragment is lost, the whole datagram is lost. We use a default maximum
  of **1400 bytes** per datagram (configurable). The leader packs as many log entries into an
  `AppendEntries` as fit. For this reason, a single command (key + value) is limited to **1 KiB**.
- **No authentication.** A datagram is accepted only if its source address matches the configured address of
  the node it claims to come from. That guards against misconfiguration, not against attackers (see scope).

**Clients over TCP.** A client sends one request and waits for exactly one answer, and values may be larger
than one datagram. TCP gives us ordered, reliable byte streams. We cut that stream into messages with a
**length prefix** (a `u32` length followed by that many bytes). Frames above 64 KiB are rejected before
anything is allocated.

### 5.7 Persistence: why Raft needs it, and how we do it

The Raft paper (Figure 2) splits each server's state in two:

- **Persistent:** `currentTerm`, `votedFor` and `log[]`. These must be on stable storage **before** the server
  answers any message.
- **Volatile:** `commitIndex`, `lastApplied`, and the leader's `nextIndex[]` and `matchIndex[]`. These may be
  lost and are rebuilt.

**What goes wrong without persistence.** Imagine a server that loses its memory on restart. Two examples
show why this breaks Raft:

*Example 1: two leaders in the same term.* Nodes A, B, C.

1. In term 5, A votes for B. B gets votes from A and B, which is a majority, and becomes leader of term 5.
2. A crashes and restarts. It has forgotten that it voted; it believes it is in term 0.
3. C's election timer fires. C enters term 5 too and asks A for a vote. A sees "term 5, I have not voted"
   and says yes.
4. C has votes from A and C, a majority, and is also leader of term 5.

Two leaders in one term break *Election Safety*. They can overwrite each other's entries.

*Example 2: a committed entry disappears.* Nodes A (leader), B, C.

1. A replicates entry `x` to B. A and B are a majority, so `x` is committed and the client is told "ok".
2. B restarts and forgets `x`. Then A crashes.
3. B and C hold an election. Neither has `x`, and they form a majority, so the new leader's log lacks `x`
   forever.

The client was told "ok" for a write that no longer exists.

The general rule is: **a server that restarts without its persistent state is a brand-new server that
pretends to be an old one.** Raft can only add new servers safely through membership changes, which are
out of scope. So we persist.

**What we persist, and when ("persist before you speak").**

| Data | Written when | Before what |
|---|---|---|
| `term` and `voted_for` (together: the **hard state**) | They change: new term seen, vote given, election started | Any message is sent |
| Log entries | The leader accepts a proposal, or a follower accepts `AppendEntries` | The leader counts itself in the majority / the follower replies "success" |
| Log truncation | A follower finds conflicting entries | The follower replies |

Inside the core, this ordering comes for free. The node calls `storage` synchronously while it handles an
event. Messages only leave the node later through `take_output()`. When the host sends them, the data is
already on disk.

**What we do not persist, and why that is safe.**

- **Commit index:** after a restart it starts at 0. The node learns it again from the current leader, or, if it
  becomes leader itself, by committing a new entry (see the no-op entry in step 4.3).
- **The key-value map and the client sessions:** after a restart they start empty. As the commit index
  grows again, the node hands out all committed entries from index 1 and the replica applies them again.
  Applying is deterministic: the same entries in the same order give the same map. This is called
  **replaying the log**.

**How.** The core talks to a small interface:

```cpp
class storage {
public:
    virtual ~storage() = default;
    [[nodiscard]] virtual persistent_state load() = 0;            // at startup
    virtual void save_hard_state(const hard_state& state) = 0;    // term + vote
    virtual void append(std::span<const log_entry> entries) = 0;  // after the current last entry
    virtual void truncate(log_index first_removed) = 0;           // remove this entry and all after it
};
```

There are two implementations:

- `raft::memory_storage` is used in tests. In the simulation, the storage object **outlives** the node object.
  When a test "crashes" a node, the node object is destroyed and its volatile state is lost. On "restart", a
  new node loads from the same storage object. This models a perfect disk, and it lets us test restarts
  thousands of times per second.
- `storage::file_storage` is used by the server (phase 6). It uses two files per node:
  - `hard_state` is replaced atomically: write a temp file, `fsync`, `rename`, `fsync` the directory.
  - `log` is append-only. Each record carries a length and a CRC32 checksum, so a record that was only
    half-written during a crash (a **torn write**) is detected and cut off at startup.

**Error policy.** If a write or `fsync` fails, the server terminates. After a failed `fsync` we do not know
what is on disk. Continuing could break the promises above. PostgreSQL learned this the hard way in 2018
("fsyncgate").

**macOS note.** On macOS, `fsync` only hands data to the drive, which may still cache it. Real durability
needs `fcntl(fd, F_FULLFSYNC)`. `file_storage` uses it on macOS.

**What remains out of scope.**

- Without snapshots, the log and the time to replay it on restart grow forever. This is fine for a demo; the
  README says so.
- **Deleting a node's data directory turns it into a new member.** That is unsafe without membership changes.
  The README warns about it.

### 5.8 Client sessions: making retries safe

**The problem.** A client cannot always know whether its request was executed. Consider `cas` (compare and
swap: "set `balance` to 50, but only if it is currently 100"):

1. The client sends `cas balance 100→50` to leader A.
2. A commits the entry and applies it, so `balance` is now 50. Then A crashes **before** it sends the reply.
3. The client times out. It cannot tell whether the request was lost before or after execution, so it
   retries at the new leader B.
4. B appends the same `cas` again. When applied, `balance` is 50, not 100, so the `cas` fails.
5. The client is told "cas failed", although its operation actually succeeded.

`put` has the same problem in a sneakier form:

1. Client 1 sends `put x 1`. It commits, but the reply is lost.
2. Client 2 sends `put x 2` and gets "ok".
3. Client 1 retries `put x 1`, and it executes again.
4. Now `x` is 1. Client 2's completed write was silently undone. No real-time order of the three operations
   explains what clients observe, so the system is **not linearizable**.

**The solution (Raft dissertation §6.3).** The state machine remembers, per client, the last request it
executed and the answer it gave. A retried request is answered from memory instead of being executed again.

1. **Register.** A client first sends `register`. This goes through the log like any write. Its answer is a
   **client id**: the log index of the `register` entry. Every node agrees on it, and it is unique without any
   randomness.
2. **Number requests.** The client numbers its requests 1, 2, 3, … (the **sequence number**). It has at most
   one request in flight and retries it with the **same** number until it gets an answer.
3. **Deduplicate.** When the state machine applies `(client_id, seq, op)`:
   - If `seq` is the next expected number, it executes `op` and stores `(seq, result)` for this client.
   - If `seq` is the number it has already executed, it returns the stored result and does not execute again.
   - If the client id is unknown, it answers `session_expired` and does not execute.

Because this logic runs inside the deterministic state machine, all nodes make the same decision, including
after a log replay.

**Limiting memory.** Every CLI call registers a new client, so sessions must be evicted eventually.
Eviction is deterministic: when there are more than 10 000 sessions, the one whose last use has the oldest
log index is removed. A client whose session was evicted gets `session_expired`. It never gets a double
execution. The CLI then reports "outcome unknown".

**Why not random client ids instead of registering?** A client would pick a random id and start at `seq = 1`.
If its session were evicted while it was still retrying, the retry would look like a brand-new client and
execute again. Registration makes that case detectable. It costs one extra log round trip per CLI call.

`get` is idempotent and needs no session.

### 5.9 Linearizable reads

**Why the naive approach is wrong.** "The leader answers `get` from its own map" fails like this:

1. Five nodes. A is leader of term 2.
2. A network split isolates A (with B) from C, D, E. C, D and E elect C as leader of term 3.
3. A client writes `x = 2` through C and gets "ok".
4. Another client asks A for `x`. A still believes it is leader and answers with the old value: a **stale read**.

The write had already completed, so the read must see it.

**Version 1 (phase 5): reads go through the log.**

- `get` is appended to the log like a write and answered when it is applied.
- An isolated old leader cannot commit anything, because it has no majority. So it can never answer.
- This is simple and obviously correct. The cost is one log entry and one disk write per read.

**Version 2 (phase 8): ReadIndex (dissertation §6.4).** The leader proves that it is still leader *right now*,
without writing anything:

1. Remember the current commit index as the **read index**.
2. Send a heartbeat round. Once a majority answers in the current term, no newer leader can exist yet: a
   newer leader would need votes from a majority, and those voters would have rejected our term.
3. Wait until the state machine has applied up to the read index.
4. Answer from the local map.

In the example above, A's heartbeat round only reaches B. Two of five is no majority, so the read is never
answered and the client tries another server. Leader leases, an even faster variant, rely on bounded clock
drift and are out of scope.

### 5.10 Error handling

| Situation | Mechanism | Example |
|---|---|---|
| Expected failure the caller must handle | `std::expected<T, E>` | Decoding bytes from the network; `propose` on a follower; parsing `--node` options |
| Value that may be absent | `std::optional<T>` | `voted_for`, the leader hint |
| Unrecoverable error | Exception, caught in `main`, which prints the error and exits | Disk I/O failure; cannot bind a port |
| Programming error (a broken invariant) | `assert` (active in Debug builds, which tests use) | Two leaders in the same term inside one node |

### 5.11 Determinism rules

These rules make the simulation reproducible with the same seed on every platform:

- Do not use `std::uniform_int_distribution` and friends. Their output differs between libstdc++ and libc++
  for the same seed. A tiny helper (`lo + rng() % (hi - lo)`) is enough; its bias is negligible for our use.
- Do not iterate over `std::unordered_map`/`std::unordered_set`, because their order depends on the standard
  library. Use `std::map`, `std::set` or `std::vector`.
- Never read real time or `std::random_device` in `raft`, `kv`, `replica` or the simulation.

### 5.12 Code style

- Names are `snake_case` like the standard library. Member variables end in `_`.
- Prefer the rule of zero: no hand-written destructors or copy/move operations unless a class owns a raw
  resource (only the file descriptor wrapper in step 6.3 does).
- Use `[[nodiscard]]` where ignoring a result is a bug (`propose`, `decode_*`, `take_output`).
- Build message structs with **designated initializers**: `request_vote{.term = 3, .last_log_index = 7, ...}`.
  The call site then shows which number is which. This matters a lot when a struct has three
  `std::uint64_t` fields in a row.
- Formatting is decided by clang-format; CI rejects unformatted code.

---

## 6. Toolchain, minimum versions and C++ features

### 6.1 Minimum versions

| Tool | Minimum | Reason |
|---|---|---|
| GCC | **14** | `std::print` and `std::ranges::to` arrived in libstdc++ 14. |
| Clang on Linux (with libstdc++ 14) | **19** | Clang 18 and older report an older value for the `__cpp_concepts` feature macro, so libstdc++ hides `std::expected` from them. |
| Apple Clang | **17** (Xcode 16.3+) | The oldest version we test in CI. It has `std::print` and `std::expected` in libc++. |
| CMake | **3.23** | The presets that Conan generates use the `include` field, which needs CMake 3.23 (the current file says 3.21). |
| Conan | 2.x, pinned in `requirements.txt` | |
| Python | 3.10+ | Only for the tools virtual environment. |

CI builds with exactly these minimums: GCC 14 and Clang 19 on Ubuntu 24.04, Apple Clang on `macos-latest`.

### 6.2 Dependencies (Conan)

| Package | Used by | Purpose |
|---|---|---|
| `asio` (standalone, no Boost) | `hammurabi_net` | UDP/TCP sockets, timers, coroutines, signals |
| `cli11` | the two executables | Command-line options and subcommands |
| `catch2` (v3) | tests | Test framework |

Protobuf is removed. When adding CLI11, take the current version from ConanCenter (`conan search cli11 -r conancenter`).

### 6.3 C++ features: what we use, and why there

**C++23**

| Feature | Where | Purpose |
|---|---|---|
| `std::expected<T, E>` | Decoders, `propose`, option parsing | Return either a value or an error **as a value**. The caller must look at it; failures are visible in the signature, and there is no hidden exception path for errors that are normal (garbage from the network). |
| `std::byteswap` | `wire::writer/reader` | Convert integers between the machine's byte order and big-endian in one standard call, with no hand-written shifting. |
| `std::to_underlying` | Message type tags | Turn an `enum class` value into its integer for encoding, without a `static_cast` that silently breaks if the enum's type changes. |
| `std::print` / `std::println` | Logging, CLI output | Type-safe formatting checked at compile time, written straight to a stream. Shorter and safer than `iostream`, with no `printf` format bugs. |
| `std::ranges::to` | Leader's commit calculation, peer lists | Turn a range pipeline (e.g. "all match indices") into a `std::vector` in one expression. |
| Monadic `std::optional` (`transform`, `or_else`) | Leader hint → client address | Transform "maybe a value" without nested `if`s. |

**C++20** (used throughout, explained where first used): coroutines (`co_await`, with Asio), concepts
(`std::unsigned_integral` in the codec), `std::span`, `std::bit_cast`, `std::endian`, designated initializers,
defaulted `operator==`, `std::format`/`std::formatter`, ranges algorithms.

**C++17** (assumed knowledge, briefly explained): `std::variant` + `std::visit`, `std::optional`, structured
bindings, `std::filesystem`, `[[nodiscard]]`.

**Deliberately not used**

| Feature | Why not |
|---|---|
| Modules / `import std;` (C++20/23) | Apple Clang does not support `import std;`, and CMake support is still experimental. Headers are fine. |
| `std::generator` (C++23) | Not available in libc++. |
| `std::flat_map` (C++23) | Needs GCC 15; `std::map` is fine at our sizes. |
| `std::move_only_function` (C++23) | Not available in the libc++ of Apple Clang 17. Where we need move-only callbacks, Asio's `any_completion_handler` does the job. |
| Deducing `this` (C++23) | We have no place where it makes the code simpler. |
| Strong types for term/index | Considered. Plain `std::uint64_t` aliases plus designated initializers prevent most mix-ups without extra templates. |

### 6.4 Quality gates

| Gate | Where | What it catches |
|---|---|---|
| Warnings `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion`, as errors in CI | Every build | Sloppy conversions, shadowing, unused values |
| clang-tidy (`bugprone-*`, `performance-*`, `modernize-*`, `cppcoreguidelines-*`) | Separate CI job | Common bug patterns, old-style code |
| clang-format | CI job + `check.sh` | Inconsistent formatting |
| ASan + UBSan | CI job | Memory errors, undefined behavior |
| Deterministic simulation with invariant checks | `sim_tests` | Raft safety bugs under faults |
| Linearizability checker | `sim_tests` | Client-visible consistency bugs |
| libFuzzer on all decoders | CI job (Linux, Clang) | Crashes on malformed network input |
| Smoke test with real processes | CI job | Wiring mistakes between server, CLI and script |

### 6.5 The standard check

From step 0.2 on, `scripts/check.sh` runs everything that must pass after each step:

```sh
#!/usr/bin/env bash
set -euo pipefail
git ls-files '*.cpp' '*.h' | xargs clang-format --dry-run --Werror
cmake --build --preset conan-debug
ctest --preset conan-debug --output-on-failure --no-tests=error
```

Run a subset of tests by Catch2 tag (tags become CTest labels, see step 0.2):

```sh
ctest --preset conan-debug -L election --output-on-failure
```

A sanitizer build re-configures the same build directory:

```sh
cmake --preset conan-debug -DHAMMURABI_SANITIZERS=address,undefined && scripts/check.sh
cmake --preset conan-debug -DHAMMURABI_SANITIZERS=                     # back to normal
```

---

## 7. The steps

### Phase 0: Clean slate and tooling

#### Step 0.1: Tag the old code and clear the slate

**Goal:** The old code is gone; the new skeleton builds two executables and one test binary.

**Build:**
- `git tag legacy-2019 && git push --tags`.
- Delete `hammurabi/`, `include/hammurabi/`, `server/`, `proto/` and `tests/tests.cpp`.
- `conanfile.txt`: remove `protobuf` (from both `[requires]` and `[tool_requires]`) and add `cli11`.
- Root `CMakeLists.txt`: drop `find_package(Protobuf)`, add `find_package(CLI11)`, and set
  `cmake_minimum_required(VERSION 3.23)`.
- Create `apps/server/main.cpp` and `apps/cli/main.cpp`. Each uses CLI11 to support `--help` and
  `--version`, and prints the version with `std::println`.
- Create `tests/unit/` with one test file containing a trivial test, built into `unit_tests`.
- Remove the MSVC branches from `HammurabiTargetOptions.cmake`, since we support only Linux and macOS.

**Why this way:** Starting with executables that do almost nothing proves the whole toolchain works before
any real code exists: Conan, CMake, CLI11, Catch2 and `std::println` on every compiler. If Apple Clang or
GCC 14 has a problem with `std::print`, we find out now, not in phase 7.

**C++ notes:** `std::println("hammurabi-server {}", version)` formats like `std::format`, but writes directly
to `stdout` and adds a newline. The format string is checked at **compile time**: a wrong number of
arguments is a compile error, not a runtime crash.

**Check:**
```sh
conan install . --build=missing -s build_type=Debug -s compiler.cppstd=23 -c tools.cmake.cmaketoolchain:generator=Ninja
cmake --preset conan-debug && cmake --build --preset conan-debug && ctest --preset conan-debug
./build/Debug/apps/server/hammurabi-server --version
```

#### Step 0.2: Tooling: one check script, compiler matrix, clang-tidy job

**Goal:** One command checks everything locally, and CI checks all supported compilers.

**Build:**
- Add `scripts/check.sh` (section 6.5) and make it executable.
- `tests/CMakeLists.txt`: use `catch_discover_tests(unit_tests ADD_TAGS_AS_LABELS)`, so Catch2 tags like
  `[codec]` become CTest labels.
- CI (`.github/workflows/ci.yml`):
  - Build matrix: GCC 14 (Ubuntu 24.04), Clang 19 (Ubuntu 24.04, installed via `apt`; use the apt.llvm.org
    script if the package is missing), Apple Clang (macOS).
  - Pass `-DHAMMURABI_WARNINGS_AS_ERRORS=ON` everywhere.
  - Keep the ASan+UBSan job.
  - Add a `clang-tidy` job: Clang 19 with `-DHAMMURABI_CLANG_TIDY=ON`.
  - Align `actions/setup-python` versions; align clang-tidy and clang-format versions in `requirements.txt`.
- Update the README build instructions (CMake 3.23, `scripts/check.sh`).

**Why this way:** A gate that runs from the first real line of code costs nothing. A gate added at the end
finds 200 warnings at once. clang-tidy gets its own job because it is slow and needs Clang's view of the
code: clang-tidy is a Clang tool, and GCC-specific flags confuse it.

**Check:** `scripts/check.sh` passes locally; all CI jobs are green on a pull request.

---

### Phase 1: Building blocks

#### Step 1.1: Byte writer and reader (`hammurabi_wire`)

**Goal:** Integers, booleans, strings and byte arrays can be turned into bytes and back.

**Build:** `include/hammurabi/wire/codec.h`, `src/wire/codec.cpp`:

```cpp
namespace hammurabi::wire {

enum class decode_error { truncated, trailing_bytes, invalid_value, too_large };

class writer {
public:
    void u8(std::uint8_t value) { put(value); }
    void u32(std::uint32_t value) { put(value); }
    void u64(std::uint64_t value) { put(value); }
    void boolean(bool value) { u8(value ? 1 : 0); }
    void bytes(std::span<const std::byte> data);  // u32 length, then the data
    void string(std::string_view text);           // same layout as bytes
    [[nodiscard]] std::size_t size() const { return buffer_.size(); }
    [[nodiscard]] std::vector<std::byte> take() && { return std::move(buffer_); }

private:
    template<std::unsigned_integral T>
    void put(T value) {
        if constexpr (std::endian::native == std::endian::little) {
            value = std::byteswap(value);  // the wire is big-endian
        }
        const auto raw = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
        buffer_.insert(buffer_.end(), raw.begin(), raw.end());
    }

    std::vector<std::byte> buffer_;
};

class reader {
public:
    explicit reader(std::span<const std::byte> input) : input_{input} {}
    std::uint8_t u8() { return get<std::uint8_t>(); }
    std::uint32_t u32() { return get<std::uint32_t>(); }
    std::uint64_t u64() { return get<std::uint64_t>(); }
    bool boolean();                  // 0 or 1; anything else is invalid_value
    std::vector<std::byte> bytes();
    std::string string();
    [[nodiscard]] std::size_t remaining() const { return input_.size(); }
    void fail(decode_error error);   // the first error "sticks"
    // Success only if every read succeeded and all input was consumed.
    [[nodiscard]] std::expected<void, decode_error> finish() const;

private:
    template<std::unsigned_integral T>
    T get();                         // you write this one: the mirror image of writer::put

    std::span<const std::byte> input_;
    std::optional<decode_error> error_;
};

} // namespace hammurabi::wire
```

**Why this way:** The reader uses a **sticky error**. When a read fails, it remembers the first error and all
later reads return 0 or an empty value. Decoding a struct is then a straight list of reads with a single
check at the end, instead of an `if` after every field. `bytes()` and `string()` check the announced length
against `remaining()` **before** allocating. Otherwise a malicious length of 4 GB would make us allocate
4 GB. The fuzzer in step 9.1 checks exactly this.

**C++ notes:**
- `std::byteswap` (C++23) reverses the bytes of an integer. Together with `std::endian::native` (C++20), the
  compiler decides at compile time whether swapping is needed. On big-endian machines the `if constexpr`
  branch disappears.
- `std::bit_cast` (C++20) reinterprets the bits of a value as another type, here an array of bytes. It is
  the safe replacement for `reinterpret_cast` or `memcpy` tricks.
- `std::unsigned_integral` (a C++20 **concept**) restricts the template to unsigned integer types. Calling
  `put(3.5)` is a readable compile error.
- `std::expected<void, decode_error>` (C++23) means "either nothing went wrong, or here is the error".
- `std::span<const std::byte>` (C++20) is a non-owning view of contiguous bytes: a pointer plus a length that
  always stay together.

**Check:** Unit tests tagged `[codec]`:
- Round trip for each type, including 0 and max values.
- A known encoding (`u32(1)` is `00 00 00 01`), which proves big-endian.
- A truncated input gives `truncated`.
- Extra bytes give `trailing_bytes`.
- A string with a too-large length gives `truncated` without a huge allocation.

`scripts/check.sh`, then `ctest --preset conan-debug -L codec`.

#### Step 1.2: Raft types and messages (`hammurabi_raft`, no logic yet)

**Goal:** All Raft data types exist and can be compared in tests.

**Build:** `include/hammurabi/raft/types.h`, `messages.h`:

```cpp
namespace hammurabi::raft {

using node_id = std::uint32_t;
using term_t = std::uint64_t;
using log_index = std::uint64_t;  // first entry has index 1; index 0 means "before the first entry"

enum class entry_kind : std::uint8_t { noop = 1, command = 2 };

struct log_entry {
    term_t term = 0;
    entry_kind kind = entry_kind::command;
    std::vector<std::byte> data;  // the encoded kv command; empty for noop
    bool operator==(const log_entry&) const = default;
};

struct request_vote {
    term_t term = 0;
    log_index last_log_index = 0;
    term_t last_log_term = 0;
    bool operator==(const request_vote&) const = default;
};
struct request_vote_reply { term_t term = 0; bool granted = false; /* operator== */ };
struct append_entries {
    term_t term = 0;
    log_index prev_log_index = 0;
    term_t prev_log_term = 0;
    std::vector<log_entry> entries;
    log_index leader_commit = 0;
    /* operator== */
};
struct append_entries_reply {
    term_t term = 0;
    bool success = false;
    log_index match_index = 0;     // on success: last index known to match the leader
    log_index last_log_index = 0;  // on failure: helps the leader skip back faster
    /* operator== */
};

using message = std::variant<request_vote, request_vote_reply, append_entries, append_entries_reply>;

struct envelope {
    node_id from = 0;
    node_id to = 0;
    message msg;
    bool operator==(const envelope&) const = default;
};

} // namespace hammurabi::raft
```

**Why this way:**
- Messages are plain structs (aggregates) inside a `std::variant`. A message is data, not behavior, and the
  variant lists *all* message kinds in one place. When we forget to handle a kind, `std::visit` fails to
  compile.
- `append_entries_reply` carries explicit indices instead of only `success`. Over UDP, replies can arrive
  late, twice or out of order. "Success for index 7" stays correct in any order; a bare "success" does not.
- We use 64-bit terms and indices, so overflow is not a concern.

**C++ notes:**
- `bool operator==(const T&) const = default;` (C++20) generates member-wise comparison. Tests can write
  `CHECK(out.messages[0] == expected)`.
- A `std::variant<A, B, C>` holds exactly one of A, B or C, and knows which one. It replaces the old class
  hierarchy with virtual functions for things that are just data.

**Check:** `scripts/check.sh`. A few unit tests construct messages with designated initializers and compare them.

#### Step 1.3: Encoding Raft messages

**Goal:** Every `envelope` can be turned into a datagram and back.

**Build:** `include/hammurabi/raft/codec.h`, `src/raft/codec.cpp`:

```cpp
inline constexpr std::uint8_t wire_version = 1;
enum class message_type : std::uint8_t { request_vote = 1, request_vote_reply = 2, append_entries = 3, append_entries_reply = 4 };

[[nodiscard]] std::vector<std::byte> encode(const envelope& env);
[[nodiscard]] std::expected<envelope, wire::decode_error> decode_envelope(std::span<const std::byte> datagram);
[[nodiscard]] std::size_t encoded_size(const log_entry& entry);  // used for batching in step 4.4
```

The datagram layout is `[version u8][type u8][from u32][to u32][payload]`. Here is one message fully
worked out; the other three follow the same pattern:

```cpp
namespace {
void write(wire::writer& w, const request_vote& m) {
    w.u64(m.term);
    w.u64(m.last_log_index);
    w.u64(m.last_log_term);
}

request_vote read_request_vote(wire::reader& r) {
    return request_vote{.term = r.u64(), .last_log_index = r.u64(), .last_log_term = r.u64()};
}

constexpr message_type type_of(const request_vote&) { return message_type::request_vote; }
// ... type_of for the other three
} // namespace

std::vector<std::byte> encode(const envelope& env) {
    wire::writer w;
    w.u8(wire_version);
    w.u8(std::to_underlying(std::visit([](const auto& m) { return type_of(m); }, env.msg)));
    w.u32(env.from);
    w.u32(env.to);
    std::visit([&w](const auto& m) { write(w, m); }, env.msg);
    return std::move(w).take();
}
```

`decode_envelope` reads the header, then uses a `switch` on the type to call the right `read_*` function. It
returns an error for an unknown version or type, and finally calls `r.finish()`. For the entry list, check
`count` against `r.remaining()` before calling `reserve`, for the same reason as in step 1.1.

**Why this way:** Overloaded free functions (`write(w, request_vote)`, `write(w, append_entries)`, …) together
with `std::visit` and a generic lambda give us dispatch without any `if`/`switch` chain on the encoding side.
If a new message type is added and `write` is missing, the program does not compile.

**C++ notes:**
- `std::to_underlying` (C++23) converts `message_type::request_vote` to the `std::uint8_t` value `1`.
- **Evaluation order:** in `request_vote{.term = r.u64(), .last_log_index = r.u64(), ...}`, C++ guarantees
  that the initializers in braces run **from left to right**. That is why reading fields this way is correct.
  Function arguments, as in `f(r.u64(), r.u64())`, have **no** guaranteed order. Never decode like that.

**Check:** `[codec]` tests:
- Round trip for each message type, including `append_entries` with 0, 1 and several entries.
- Every possible truncation of a valid datagram (loop over lengths 0…n−1) returns an error and does not crash.
- An unknown type or version returns `invalid_value`.

#### Step 1.4: KV commands and the state machine (`hammurabi_kv`, no sessions yet)

**Goal:** A key-value map that executes `get`, `put`, `remove` and `cas` and can encode them.

**Build:** `include/hammurabi/kv/command.h`, `state_machine.h`:

```cpp
namespace hammurabi::kv {

struct get    { std::string key; };
struct put    { std::string key; std::string value; };
struct remove { std::string key; };
struct cas    { std::string key; std::optional<std::string> expected; std::string desired; };
using operation = std::variant<get, put, remove, cas>;   // each struct also gets a defaulted operator==

enum class status : std::uint8_t { ok = 1, not_found = 2, cas_failed = 3 };
struct result { status code = status::ok; std::optional<std::string> value; };

class state_machine {
public:
    result apply(const operation& op);                // phase 5.2 changes this to take a command
    [[nodiscard]] result read(const get& op) const;   // used by ReadIndex in phase 8
private:
    result execute(const get& op) const;
    result execute(const put& op);
    result execute(const remove& op);
    result execute(const cas& op);
    std::map<std::string, std::string> data_;
};

[[nodiscard]] std::vector<std::byte> encode(const operation& op);
[[nodiscard]] std::expected<operation, wire::decode_error> decode_operation(std::span<const std::byte> bytes);

} // namespace hammurabi::kv
```

One method worked out:

```cpp
result state_machine::apply(const operation& op) {
    return std::visit([this](const auto& o) { return execute(o); }, op);
}

result state_machine::execute(const put& op) {
    data_.insert_or_assign(op.key, op.value);
    return {.code = status::ok, .value = std::nullopt};
}
```

Semantics of the others:
- `get` returns `ok` with the value, or `not_found`.
- `remove` returns `ok`, or `not_found` if the key was absent.
- `cas` succeeds if the current value equals `expected`. An `expected` of `nullopt` means "only if the key is
  absent". On failure, `cas` returns `cas_failed` together with the current value, so the client sees why.

**Why this way:**
- Keys and values are strings: they can be encoded, compared and replicated. That is what a replicated state
  machine needs, unlike the old `std::any`.
- `std::map` keeps iteration order deterministic (section 5.11).
- There is no mutex, because one thread owns the state machine (section 5.4).
- The state machine knows nothing about Raft. It is just a map with commands, easy to test on its own.

**Check:** `[kv]` tests: every operation including the edge cases (cas on a missing key, cas with `nullopt`
on an existing key, remove twice), plus an encode/decode round trip for each operation.

---

### Phase 2: Leader election

#### Step 2.1: Node skeleton, storage interface, single-node election

**Goal:** A `raft::node` exists; a one-node cluster elects itself leader; a three-node member sends
`RequestVote` after its election timeout.

**Build:**

```cpp
namespace hammurabi::raft {

struct config {
    node_id self = 0;
    std::vector<node_id> members;           // all nodes, including self
    int election_timeout_ticks = 15;        // actual timeout is random in [t, 2t)
    int heartbeat_interval_ticks = 5;
    std::size_t max_append_bytes = 1300;    // fits in one 1400-byte datagram (step 4.4)
    std::uint64_t random_seed = 0;
};

struct hard_state {
    term_t term = 0;
    std::optional<node_id> voted_for;
    bool operator==(const hard_state&) const = default;
};
struct persistent_state { hard_state hard; std::vector<log_entry> log; };

class storage { /* see section 5.7 */ };

class memory_storage final : public storage {
public:
    memory_storage() = default;
    explicit memory_storage(persistent_state initial);  // tests can start nodes with a prepared log
    // ... overrides
private:
    persistent_state state_;
};

enum class role { follower, candidate, leader };
struct not_leader { std::optional<node_id> leader_hint; };

struct output {
    std::vector<envelope> messages;
    std::vector<std::pair<log_index, log_entry>> committed;
};

class node {
public:
    node(config cfg, storage& store);  // loads persistent state from store

    void tick();
    void receive(const envelope& env);
    [[nodiscard]] std::expected<log_index, not_leader> propose(std::vector<std::byte> command);  // step 4.1
    [[nodiscard]] output take_output();

    [[nodiscard]] role current_role() const;
    [[nodiscard]] term_t current_term() const;
    [[nodiscard]] std::optional<node_id> leader_hint() const;
    [[nodiscard]] log_index commit_index() const;
    [[nodiscard]] std::span<const log_entry> log() const;

private:
    struct follower_state  { std::optional<node_id> leader; };
    struct candidate_state { std::set<node_id> votes; };
    struct leader_state    { std::map<node_id, log_index> next_index; std::map<node_id, log_index> match_index; };

    void become_follower(term_t term, std::optional<node_id> leader);
    void become_candidate();
    void become_leader();
    void reset_election_timer();         // picks a new random timeout
    void send(node_id to, message msg);  // appends to out_.messages
    [[nodiscard]] std::size_t majority() const { return cfg_.members.size() / 2 + 1; }
    [[nodiscard]] log_index last_log_index() const { return log_.size(); }
    [[nodiscard]] term_t term_at(log_index index) const;  // 0 for index 0

    config cfg_;
    storage& storage_;
    std::mt19937_64 rng_;
    hard_state hard_;              // persistent
    std::vector<log_entry> log_;   // persistent; entry with index i lives at log_[i - 1]
    log_index commit_index_ = 0;   // volatile
    log_index handed_out_ = 0;     // volatile; "lastApplied" from the paper (see step 4.3)
    std::variant<follower_state, candidate_state, leader_state> role_;
    int election_elapsed_ = 0;
    int election_timeout_ = 0;
    int heartbeat_elapsed_ = 0;
    output out_;
};

} // namespace hammurabi::raft
```

`tick()` in full:

```cpp
void node::tick() {
    if (std::holds_alternative<leader_state>(role_)) {
        if (++heartbeat_elapsed_ >= cfg_.heartbeat_interval_ticks) {
            heartbeat_elapsed_ = 0;
            // step 2.3: send heartbeats
        }
    } else if (++election_elapsed_ >= election_timeout_) {
        become_candidate();
    }
}
```

`become_candidate()` does the following:
- Increment the term, vote for itself, and **persist** (`storage_.save_hard_state`).
- Set `role_ = candidate_state{.votes = {cfg_.self}}` and reset the election timer.
- Send `request_vote` to every other member.
- If its own vote is already a majority (a one-node cluster), call `become_leader()` immediately.

**Why this way:**
- The node keeps its own copy of the log in memory (`log_`) and uses `storage` only as a "write-through
  journal". Reads never touch the disk. Without snapshots the whole log is in memory anyway.
- Roles are a `std::variant` of small structs. Each role keeps exactly the data it needs: a candidate's
  `votes` cannot exist while the node is leader. Switching role is a plain assignment
  (`role_ = leader_state{...}`). Nothing deletes itself, so the use-after-free from the old code is impossible.
- `storage` is a classic abstract class with virtual functions. Choosing the implementation at runtime is
  exactly what we need (memory in tests, files in the server). The cost of a virtual call is irrelevant next
  to an `fsync`.

**C++ notes:**
- `std::holds_alternative<T>(v)` asks a variant "do you currently hold a T?".
- `std::get_if<T>(&v)` returns a pointer to the T, or `nullptr`. It is the safe way to "use leader data if I am leader".

**Check:** `[election]` tests:
- A one-node cluster is leader after at most 30 ticks.
- In a three-node config, a node emits exactly two `request_vote` envelopes after its timeout.
- The term was persisted: `memory_storage` has `term == 1, voted_for == self`.
- The timeout is random but reproducible: same seed, same tick count; different seeds give different counts.

#### Step 2.2: Voting

**Goal:** A node grants or refuses votes exactly as Figure 2 and §5.4.1 say.

**Build:** First the rule that applies to **every** message, in one central place:

```cpp
void node::receive(const envelope& env) {
    const term_t msg_term = std::visit([](const auto& m) { return m.term; }, env.msg);
    if (msg_term > hard_.term) {
        become_follower(msg_term, std::nullopt);  // Figure 2, "Rules for All Servers"
    }
    std::visit([&](const auto& m) { handle(env.from, m); }, env.msg);
}
```

Then the vote handler in full, because every line matters:

```cpp
void node::handle(node_id from, const request_vote& m) {
    // §5.4.1: only vote for a candidate whose log is at least as up to date as ours.
    // "Up to date" compares the last term first, then the last index.
    const bool log_ok = std::pair{m.last_log_term, m.last_log_index} >=
                        std::pair{term_at(last_log_index()), last_log_index()};
    const bool can_vote = !hard_.voted_for || *hard_.voted_for == from;  // "== from": a duplicate request
    const bool grant = m.term == hard_.term && can_vote && log_ok;
    if (grant) {
        hard_.voted_for = from;
        storage_.save_hard_state(hard_);   // persist BEFORE the reply leaves (section 5.7)
        reset_election_timer();            // don't start a competing election right after voting
    }
    send(from, request_vote_reply{.term = hard_.term, .granted = grant});
}
```

**Why this way:**
- Requests with an older term are refused automatically: after `receive` has handled newer terms,
  `m.term == hard_.term` is false for older ones.
- A leader or candidate already voted for itself in this term, so `can_vote` is false for others. No special
  cases per role are needed.
- `*hard_.voted_for == from` lets a candidate whose reply was lost ask again. With UDP, this happens.

**Example for the up-to-date rule.** Our last entry is (term 3, index 7).

| Candidate's last entry | Grant? | Why |
|---|---|---|
| (term 4, index 2) | yes | a higher last term wins, even with a shorter log |
| (term 3, index 7) | yes | equal |
| (term 3, index 6) | no | same term, shorter log |
| (term 2, index 9) | no | lower last term, even with a longer log |

**C++ notes:** `std::pair` compares lexicographically: first by `.first`, and only if equal by `.second`.
That is exactly the rule from the paper, in one line.

**Check:** `[election]` table-driven tests using `memory_storage` with prepared logs and terms. Cover the
cases from the table, plus: already voted for another node, same node asks again, older term, newer term
(the node becomes follower and can vote).

#### Step 2.3: Winning, heartbeats, stepping down

**Goal:** Three nodes connected by hand elect exactly one leader, which sends heartbeats; higher terms make
leaders and candidates step down.

**Build:**
- `handle(request_vote_reply)`: if candidate and `m.term == hard_.term` and `m.granted`, insert `from` into
  `votes`. If `votes.size() >= majority()`, call `become_leader()`.
- `become_leader()`: set up `leader_state` (next index = last index + 1, match index = 0 for every peer) and
  send heartbeats immediately.
- Heartbeat: `append_entries` with no entries. `prev_log_index`/`prev_log_term` are taken from `next_index`.
- `handle(append_entries)` (simple version; phase 4 adds the log part):
  - If `m.term < hard_.term`, reply `success = false`.
  - Otherwise call `become_follower(m.term, from)` (a candidate in the same term steps down; everyone
    resets the election timer) and reply `success = true`.
- `handle(append_entries_reply)`: ignore for now (phase 4).
- A small test helper `deliver_all(nodes)` that moves messages from each node's output to the receivers
  until nothing is left.

**Why this way:** Stepping down on a higher term happens in `receive()` for all message types at once,
written once. The old code repeated that rule in about ten places. A leader that receives `append_entries`
for its *own* term would mean two leaders in one term; it gets an `assert`, because it can only be a bug.

**Check:** `[election]` tests with three nodes and `deliver_all`:
- Exactly one leader.
- Followers know the leader (`leader_hint()`).
- Heartbeats keep followers from starting elections over 1000 ticks.
- A node that receives a message with a higher term steps down.
- The "duplicate vote" scenario: a reply delivered twice is still one vote.

---

### Phase 3: The simulation

#### Step 3.1: Simulated clock, network and cluster

**Goal:** A test can run a whole cluster for thousands of ticks in milliseconds, deterministically.

**Build:** `tests/sim/` as a test-only library `hammurabi_sim`:

```cpp
namespace hammurabi::sim {

using tick_t = std::uint64_t;

template<typename Packet>   // Packet has a .to field; reused for client traffic in step 5.3
class network {
public:
    explicit network(std::uint64_t seed);
    void send(Packet packet, tick_t now);              // applies loss, delay, duplication
    [[nodiscard]] std::vector<Packet> deliver(tick_t now);  // packets due by now, in a fixed order
    // fault controls: step 3.2
private:
    struct in_flight { tick_t due; std::uint64_t seq; Packet packet; };  // seq breaks ties deterministically
    std::vector<in_flight> queue_;
    std::mt19937_64 rng_;
    std::uint64_t next_seq_ = 0;
};

class cluster {
public:
    cluster(std::size_t size, std::uint64_t seed);
    void step();                                       // one tick for the whole world
    void run_for(tick_t ticks);
    bool run_until(const std::function<bool()>& condition, tick_t limit);
    [[nodiscard]] std::optional<raft::node_id> leader() const;  // the leader with the highest term, if any
    raft::node& node(raft::node_id id);
    network<raft::envelope>& net();
    [[nodiscard]] tick_t now() const;
private:
    tick_t now_ = 0;
    network<raft::envelope> net_;
    std::map<raft::node_id, std::unique_ptr<raft::memory_storage>> storages_;  // survive crashes (phase 6)
    std::map<raft::node_id, std::unique_ptr<raft::node>> nodes_;
};

} // namespace hammurabi::sim
```

`step()` does the same thing in the same order every time:
1. Deliver all due packets to their nodes, then collect each node's output into the network.
2. Tick each node (in id order), and collect output again.
3. Check the invariants (step 3.2).

Also add `std::formatter` specializations for the Raft messages, so `std::format("{}", env)` prints e.g.
`1→2 AppendEntries{term=3, prev=5/2, entries=2, commit=4}`. The simulation keeps the last 200 events. On a
test failure they are printed via Catch2's `UNSCOPED_INFO`, so you can read what happened.

**Why this way:**
- All randomness in one test comes from one seed. A failing seed can be replayed exactly, as often as needed.
  This is what turns a "flaky distributed bug" into an ordinary debugging session.
- The network is a class template because in phase 5 the same logic carries client requests.

**C++ notes:** A `std::formatter<T>` specialization (C++20) teaches `std::format`/`std::print` how to print our
own types. It is the modern replacement for `operator<<`.

**Check:** `[sim]` tests:
- Clusters of 1, 3, 5 and 7 nodes elect a leader within 300 ticks.
- Two runs with the same seed produce the same leader at the same tick.

#### Step 3.2: Faults and the first invariant

**Goal:** The simulated network can lose, delay, duplicate and partition. After every step, the simulation
checks that there is never more than one leader per term.

**Build:**
- Fault controls on `network`, using integer probabilities per mille to keep them deterministic across
  platforms (section 5.11):

```cpp
void set_loss(int per_mille);
void set_duplication(int per_mille);
void set_delay(tick_t min, tick_t max);
void partition(std::vector<std::set<raft::node_id>> groups);  // packets only flow inside a group
void isolate(raft::node_id id);
void heal();
```
  (`partition` requires `Packet` to have `.from` as well.)
- An `invariant_checker` that remembers, for each term, which node was leader. If a second node is leader
  in a term that already has one, the test fails with the trace (**Election Safety**).
- **Chaos test:** for seeds 1…50 (via Catch2 `GENERATE`), run 5 000 ticks while random faults are switched on
  and off. Then heal everything and require that a leader exists within 500 ticks. Use `CAPTURE(seed)`, so a
  failure tells you which seed to replay. An environment variable `HAMMURABI_SEED` runs a single seed.

**Why this way:** Single scenario tests ("isolate the leader") check what we thought of. Random faults with
a checker running all the time find what we did not think of. Checking after **every** step catches a
violation at the moment it happens, not 3 000 ticks later.

**Check:** `[sim]` tests:
- Isolate the leader: a new leader appears in a higher term.
- Heal: the old leader steps down.
- A 2/3 split in a 5-node cluster: only the majority side elects a leader.
- The chaos test passes for all 50 seeds.

`ctest --preset conan-debug -L sim`.

---

### Phase 4: Log replication

#### Step 4.1: Leader appends; follower checks consistency

**Goal:** `propose()` adds an entry on the leader; followers accept it only if their log matches the leader's
up to that point.

**Build:**
- `propose(data)`: if not leader, return `std::unexpected{not_leader{leader_hint()}}`. Otherwise:
  1. Create `log_entry{.term = hard_.term, .kind = entry_kind::command, .data = std::move(data)}`.
  2. Call `storage_.append` and push the entry onto `log_`.
  3. Send `append_entries` to every peer and return the new index.
- `send_append_entries(peer)`: send the entries from `next_index[peer]` to the end, with `prev_log_index =
  next_index − 1` and `prev_log_term = term_at(prev_log_index)`. Heartbeats call the same function and
  therefore **re-send** unconfirmed entries automatically.
- `handle(append_entries)`, the most important function in Raft, shown in full:

```cpp
void node::handle(node_id from, const append_entries& m) {
    if (m.term < hard_.term) {
        send(from, append_entries_reply{.term = hard_.term, .success = false, .match_index = 0,
                                        .last_log_index = last_log_index()});
        return;
    }
    become_follower(m.term, from);  // same term: only resets the timer and remembers the leader

    // 1. Consistency check: do we have the entry the new entries follow?
    if (m.prev_log_index > last_log_index() || term_at(m.prev_log_index) != m.prev_log_term) {
        send(from, append_entries_reply{.term = hard_.term, .success = false, .match_index = 0,
                                        .last_log_index = last_log_index()});
        return;
    }

    // 2. Skip entries we already have. Stop at the first new or conflicting one.
    auto first_new = m.entries.begin();
    log_index index = m.prev_log_index + 1;
    while (first_new != m.entries.end() && index <= last_log_index() && term_at(index) == first_new->term) {
        ++first_new;
        ++index;
    }
    if (first_new != m.entries.end()) {
        if (index <= last_log_index()) {
            truncate_log_from(index);  // conflict: drop it and everything after (persisted)
        }
        append_to_log(std::span{first_new, m.entries.end()});  // persisted, then added to log_
    }

    // 3. Commit what the leader has committed, but only as far as we have verified (step 4.3).
    const log_index last_verified = m.prev_log_index + m.entries.size();
    advance_commit_index(std::min(m.leader_commit, last_verified));

    send(from, append_entries_reply{.term = hard_.term, .success = true, .match_index = last_verified,
                                    .last_log_index = last_log_index()});
}
```

**Why "skip, then truncate only on conflict"? An example with reordering.** The leader sends message M1 with
entries 5–6, and shortly after, M2 with entries 5–7. UDP delivers M2 first: our log now ends at 7, and we told
the leader "match 7". Then M1 arrives late.

- A naive implementation that "replaces everything after `prev_log_index`" would delete entry 7, which we
  already confirmed. The leader might already count it as committed. That is a safety violation.
- Our loop sees that 5 and 6 match, finds nothing new, and deletes nothing. The reply says "match 6"; the
  leader takes the maximum it has seen, so 7 stays confirmed.

**Why `min(leader_commit, last_verified)`?** Example: our log has entries 5–6 left over from an old term that
the current leader never had. The leader sends `prev = 3`, one entry (index 4), and `leader_commit = 6`.
We verified only up to index 4. Our 5–6 may be different from the leader's 5–6. Committing them would
apply entries that may be wrong. Using `min(6, 4) = 4` is correct.

**Check:** `[replication]` unit tests built from **Figure 7 of the paper**. Prepare follower logs (a)–(f) with
`memory_storage` and send the leader's `append_entries`. Check the result:
- reject (missing entries),
- accept and truncate (conflicting entries),
- accept without truncating (the stale-message case above).

#### Step 4.2: Leader tracks followers; backtracking

**Goal:** The leader learns how far each follower is and repairs followers that diverge.

**Build:** `handle(append_entries_reply)`:

```cpp
void node::handle(node_id from, const append_entries_reply& m) {
    auto* leader = std::get_if<leader_state>(&role_);
    if (leader == nullptr || m.term != hard_.term) {
        return;  // stale reply from another term, or we are no longer leader
    }
    if (m.success) {
        leader->match_index[from] = std::max(leader->match_index[from], m.match_index);
        leader->next_index[from] = std::max(leader->next_index[from], m.match_index + 1);
        maybe_advance_commit();  // step 4.3
    } else {
        // Go back, but jump straight to the follower's end if its log is short,
        // and never below what we already know matches.
        auto& next = leader->next_index[from];
        next = std::max(leader->match_index[from] + 1, std::min(next - 1, m.last_log_index + 1));
        send_append_entries(from);
    }
}
```

**Why this way:** `std::max` makes every update **monotonic**, meaning values only grow. An old or duplicated
reply therefore cannot move the leader backwards. The `last_log_index` hint skips many round trips when a
follower is far behind. A follower that missed 1 000 entries is repaired in one step instead of 1 000.

**Check:** `[replication]` tests:
- A follower that is far behind catches up in a few rounds.
- A follower with conflicting entries ends with exactly the leader's log.
- Duplicated and reordered replies leave `next_index`/`match_index` correct.

#### Step 4.3: Commit index and the no-op entry

**Goal:** Entries become committed when they are on a majority, with the special rule from §5.4.2, and
committed entries are handed out to the host.

**Build:**

```cpp
void node::maybe_advance_commit() {
    const auto& leader = std::get<leader_state>(role_);
    auto matched = leader.match_index | std::views::values | std::ranges::to<std::vector>();
    matched.push_back(last_log_index());                 // the leader counts itself
    std::ranges::sort(matched, std::greater{});
    const log_index candidate = matched[majority() - 1];  // stored on at least a majority
    if (candidate > commit_index_ && term_at(candidate) == hard_.term) {
        commit_index_ = candidate;                        // §5.4.2: count only current-term entries
    }
}
```

*Example:* five nodes, leader in term 3.
- Match indices: leader 7, peers 7, 5, 5, 2. Sorted descending: 7, 7, 5, 5, 2.
- `majority() = 3`, so `matched[2] = 5`: index 5 is stored on at least three nodes.
- If entry 5 has term 3, the commit index becomes 5.

`take_output()` moves all entries from `handed_out_ + 1` up to `commit_index_` into `out.committed` and
advances `handed_out_`. `become_leader()` now appends a **no-op entry** (`entry_kind::noop`) in the new term.

**Why the term check and the no-op? Figure 8 of the paper, step by step** (five nodes S1–S5):

1. Term 2: S1 is leader and replicates entry `i=2` to S2 only, then crashes.
2. Term 3: S5 becomes leader (votes from S3, S4, S5) and writes a different entry at `i=2`. It reaches no
   one else and crashes.
3. Term 4: S1 is leader again and copies its old entry `i=2` (term 2) to S3. Now S1, S2 and S3 have it, a
   majority. **If S1 counted this as committed**, it might tell a client "ok"…
4. …and then crash. S5 can win term 5 (its last term, 3, beats the others' 2). S5 overwrites `i=2`
   everywhere. The "committed" entry is gone.

Rule: a leader only commits an entry **of its own term** by counting replicas. Earlier entries become
committed indirectly, because the log matching property guarantees everything before a committed entry is
committed too. The no-op entry gives a new leader a current-term entry right away. Without it, entries from
earlier terms would stay uncommitted until the next client write. Phase 8 also relies on it.

**C++ notes:** `leader.match_index | std::views::values | std::ranges::to<std::vector>()` reads "take the
values of the map and collect them into a vector". `std::ranges::to` (C++23) is the missing piece that
turns a lazy view back into a container.

**Check:** `[replication]` tests:
- A proposal on a 3-node cluster is committed on all nodes and appears in `committed` exactly once.
- **Figure 8:** prepare the logs of situation 3 with `memory_storage`. Assert that index 2 does **not**
  become committed while only term-2 entries are on the majority, and that it does once the term-4 no-op is.
- A 1-node cluster commits immediately.

#### Step 4.4: Batching and the datagram size limit

**Goal:** `append_entries` messages never exceed `max_append_bytes`, and a long backlog is sent in several batches.

**Build:** `send_append_entries` adds entries while `encoded_size` (step 1.3) keeps the total within the
budget. It always sends at least one entry. `propose` rejects commands larger than the per-command limit.

**Why this way:** A datagram that gets too big is fragmented or dropped (section 5.6). Since every heartbeat
re-sends from `next_index`, batches continue automatically: when one batch is confirmed, the next heartbeat
or reply sends the next one.

**Check:** `[replication]`:
- After 1 000 proposals with a lagging follower, no encoded message is larger than the limit, and the
  follower catches up.
- An oversized proposal is rejected.

#### Step 4.5: Replication invariants and chaos

**Goal:** The simulation checks all of Raft's safety properties continuously while clients propose under faults.

**Build:** Extend `invariant_checker`:
- **Log Matching:** for any two nodes, if both have an entry with the same index and term, all earlier
  entries are identical. Checking only the highest common index pair is enough, because the property is
  inductive.
- **State Machine Safety:** record every committed entry handed out by any node, by index. Any later handout
  for the same index must be identical.
- **Leader Completeness:** when a node becomes leader, its log contains every entry ever handed out as committed.
- Chaos test: like step 3.2, plus random `propose()` calls on whatever node is leader.

**Why this way:** These are the five safety properties from Figure 3 of the paper, together with Election
Safety from step 3.2 and "leader append-only", which holds by construction. With continuous checking under
random faults, a passing test suite is strong evidence of correctness, not just "it worked once".

**Check:** `ctest --preset conan-debug -L sim`. All seeds pass. Also run once with sanitizers (section 6.5).

---

### Phase 5: Replica, client sessions, linearizability

#### Step 5.1: The replica: Raft plus the KV state machine (reads through the log)

**Goal:** A sans-I/O `replica` accepts client requests, runs them through Raft and returns answers.

**Build:** `hammurabi_kv` gets the client protocol types; `hammurabi_replica` gets the glue:

```cpp
namespace hammurabi::kv {
struct request { operation op; };   // step 5.2 adds client_id and sequence
struct not_leader { std::optional<std::string> leader_address; };  // client address of the leader, if known
enum class rejection : std::uint8_t { too_large = 1, malformed = 2, session_expired = 3 };
using response = std::variant<result, not_leader, rejection>;
// + encode/decode for request and response
}

namespace hammurabi {
using request_id = std::uint64_t;   // chosen by the host; identifies the waiting client

struct replica_output {
    std::vector<raft::envelope> messages;
    std::vector<std::pair<request_id, kv::response>> responses;
};

class replica {
public:
    replica(raft::config cfg, raft::storage& store, std::map<raft::node_id, std::string> client_addresses);
    void tick();
    void receive(const raft::envelope& env);
    void submit(request_id id, const kv::request& req);  // the answer comes later, via take_output()
    [[nodiscard]] replica_output take_output();
    [[nodiscard]] const raft::node& node() const;
private:
    struct pending { raft::term_t term; request_id id; };
    raft::node node_;
    kv::state_machine machine_;
    std::map<raft::log_index, pending> pending_;   // which client waits for which log index
    std::map<raft::node_id, std::string> client_addresses_;
    replica_output out_;
};
}
```

`take_output()` drives everything:
1. Take the node's output.
2. Apply every committed entry in order, skipping no-ops.
3. If a request is pending at that index **with the same term**, answer it with the result.
4. If the term differs, our entry was overwritten, so answer `not_leader`.
5. If the node is no longer leader, answer every remaining pending request with `not_leader`. The client
   retries elsewhere; with sessions (next step) that is safe.

**Why this way:**
- All answers flow through one channel (`responses`), including the immediate `not_leader` rejections. The
  host (phase 7) and the simulation (step 5.3) use the replica the same way. The simulation therefore tests
  the **real** request path, not a test-only copy.
- The term check in `pending` matters. Index 12 may now hold a different command from a newer leader, and
  answering with *its* result would be wrong.

**C++ notes:** `node.leader_hint().transform([&](raft::node_id id) { return client_addresses_.at(id); })` turns
"maybe a leader id" into "maybe an address" without an `if`. This is the monadic interface of `std::optional`
(C++23).

**Check:** `[replica]` unit tests with three replicas and `deliver_all`:
- `put` then `get` returns the value.
- A request to a follower answers `not_leader` with the right address.
- A pending request is answered with `not_leader` when the leader steps down.

#### Step 5.2: Client sessions (each write runs at most once)

**Goal:** Implement section 5.8: registering, sequence numbers, deduplication and deterministic eviction.

**Build:**
- `kv::operation` gains `register_client`.
- `kv::request` becomes `{ std::uint64_t client_id; std::uint64_t sequence; operation op; }`. `client_id = 0`
  is allowed only for `get` and `register_client`.
- The log stores a `kv::command { client_id, sequence, op }`.
- `state_machine::apply(raft::log_index index, const command& cmd)`:

```cpp
struct session { std::uint64_t last_sequence = 0; result last_result; raft::log_index last_used = 0; };
std::map<std::uint64_t, session> sessions_;   // keyed by client id

// apply():
//   register_client           → create session (client id = index); return it in result.value
//   client_id == 0 (get only) → execute, no session
//   unknown client id         → status::session_expired (never execute!)
//   seq == last_sequence      → return last_result (a retry; do not execute again)
//   seq == last_sequence + 1  → execute, store result, last_used = index
//   anything else             → reject (protocol violation)
//   then: if sessions_.size() > max_sessions → erase the one with the smallest last_used
```

**Why this way:** See section 5.8. The decision lives inside the state machine, so it is part of the
replicated, deterministic state: every node, including one replaying its log after a restart, decides the
same.

**Check:** `[kv]` and `[replica]` tests:
- The same `(client, seq)` applied twice executes once and returns the same result twice.
- **The `cas` example from section 5.8** in a replica test: commit a `cas`, drop the response, retry at the
  new leader → the client gets "ok", not "cas failed".
- Eviction removes the least recently used session; an evicted client gets `session_expired`.

#### Step 5.3: Simulated clients and recorded histories

**Goal:** The simulation runs clients that behave like the real CLI (register, retry, follow leader hints)
and records what every operation saw and when.

**Build:**
- `sim::cluster` now holds `replica`s instead of bare nodes.
- A second `network<client_packet>` carries requests and responses, so they too can be lost, delayed and
  duplicated.
- `sim::client` is a small state machine:
  1. Register.
  2. Then repeatedly pick a random operation on a few keys, send it, wait up to N ticks, and retry (same
     sequence number) at the hinted leader or the next node.
  3. Give up after M ticks and record the outcome as **unknown**.
- Every operation is recorded as `{client, operation, result or unknown, call_tick, return_tick}`.

**Why this way:** "Unknown" is a real outcome in distributed systems: the client cannot know whether a timed-out
write took effect. The checker in the next step must handle it. Recording real tick times lets the checker
know which operations overlapped in time.

**Check:** `[sim]` test: with 3 clients and no faults, every operation completes and the final values agree on
all nodes.

#### Step 5.4: The linearizability checker

**Goal:** A function that decides whether a recorded history could have come from a single, correct copy of
the data.

**Build:** `tests/sim/linearizability.{h,cpp}`:

```cpp
struct recorded_op {
    int client;
    kv::operation op;
    std::optional<kv::result> result;  // nullopt = outcome unknown
    tick_t call;
    tick_t ret;                        // max value if unknown
};
[[nodiscard]] bool is_linearizable(std::span<const recorded_op> history);
```

**How it works (Wing & Gong search, with memoization):**
1. Split the history by key. Keys do not affect each other, so each key is checked alone; this keeps the
   search small.
2. For one key, the model is `std::optional<std::string>` (absent, or a value). Search for an order of the
   operations that:
   - (a) respects real time: if A returned before B was called, A comes first;
   - (b) when replayed on the model, produces exactly the recorded results.
3. At each step, the candidates are operations that are **minimal**: no other remaining operation returned
   before they were called. Try each candidate on the model. If its result matches (an unknown result matches
   anything), recurse. Otherwise try the next candidate.
4. Remember `(set of done operations, model state)` pairs that already failed, so the same dead end is
   never searched twice.
5. Operations with unknown outcome may also be left out entirely. A timed-out write may simply never have
   happened.

**Example.** Ticks are in brackets `[call, return]`:

| Client | Operation | Result | Time |
|---|---|---|---|
| 1 | `put x 1` | ok | [0, 10] |
| 2 | `get x` | `1` | [2, 5] |

This is linearizable: the put can "take effect" at tick 3, inside both intervals, before the get. Change the
get to `[12, 13]` returning `not_found`: the put had already returned at 10, so any order must put it first,
and the get must see `1`. Not linearizable.

**Why this way:** Invariants check Raft's internals. The linearizability checker checks what **clients**
experience, which is the promise we actually make. It is the same idea as Jepsen's Knossos and Porcupine,
in small.

**Check:** `[linearizability]` unit tests of the checker itself, with hand-written histories:
- Both examples above.
- A `cas` race.
- Unknown outcomes that must, or must not, be assumed to have happened.
- A history with a lost update must be rejected.

#### Step 5.5: Chaos tests with linearizability

**Goal:** Under random faults, recorded client histories are always linearizable.

**Build:** Combine steps 3.2, 4.5 and 5.3. Several clients, random faults, then heal and let all clients
finish, then run `is_linearizable` on the whole history. Keep the number of operations per key small (a few
dozen) so the check stays fast.

**Why this way:** This is the strongest single test in the project. A deliberately broken variant shows that
the test has teeth. Temporarily answer `get` from the leader's local map without going through the log (the
naive read from section 5.9), and watch the test catch it within a few seeds. Then revert.

**Check:** `ctest --preset conan-debug -L chaos`. All seeds green. Record in the commit message how long the
suite takes.

---

### Phase 6: Persistence

#### Step 6.1: Crash and restart in the simulation

**Goal:** Nodes can crash and restart in the simulation, and all invariants and linearizability still hold.

**Build:**
- `cluster::crash(id)` destroys the replica (all volatile state is gone) and drops its in-flight packets.
- `cluster::restart(id)` creates a new replica from the **same** `memory_storage` (section 5.7).
- The chaos tests now also crash and restart random nodes, at most a minority at the same time, so a
  majority stays up.

**Why this way:** The core already persists through `storage` since step 2.1. Here we prove that the node
recovers correctly from what it persisted:
- the term and vote are respected,
- the log is intact,
- the commit index is re-learned,
- the state machine is rebuilt by replay, and so are the sessions.

**Check:** `[sim]`:
- Restarting a follower: it catches up.
- Restarting the leader: a new leader is elected, and the old one rejoins as follower.
- Restarting **all** nodes: committed data is still there, and the state machines are rebuilt.

`-L chaos` is green.

#### Step 6.2: Showing why persistence matters

**Goal:** A test that demonstrates Example 1 from section 5.7, so the reason for persistence is executable,
not just prose.

**Build:** A test that restarts a node with a **fresh, empty** `memory_storage` at exactly the right moment,
using a hand-scripted message sequence:
1. A votes for B in term 5.
2. A restarts empty.
3. C asks A for a vote in term 5.

The test asserts that the Election Safety checker **detects** two leaders in term 5.

**Why this way:** It documents the reason in code, and it proves the invariant checker would catch the bug.
A checker that never fires might simply be broken.

**Check:** `[sim]` test passes: it expects the violation.

#### Step 6.3: File storage, part 1: hard state and RAII file handles

**Goal:** `term` and `voted_for` survive process restarts on disk, written atomically.

**Build:** `hammurabi_storage`:

```cpp
namespace hammurabi::storage {

// Owns one POSIX file descriptor. Move-only: exactly one owner closes it.
class file {
public:
    static file open(const std::filesystem::path& path, int flags, mode_t mode = 0644);  // throws std::system_error
    file(file&& other) noexcept;
    file& operator=(file&& other) noexcept;
    file(const file&) = delete;
    file& operator=(const file&) = delete;
    ~file();                                   // closes if still owned
    void write_all(std::span<const std::byte> data);
    void sync();                               // fsync, or fcntl(F_FULLFSYNC) on macOS
    void truncate(std::uint64_t size);
    [[nodiscard]] std::uint64_t size() const;
private:
    explicit file(int fd) : fd_{fd} {}
    int fd_ = -1;
};

class file_storage final : public raft::storage {
public:
    explicit file_storage(std::filesystem::path directory);  // creates the directory if needed
    // ... overrides
};

} // namespace hammurabi::storage
```

`save_hard_state` does the following:
1. Encode `[magic][version][term][has_vote][vote][crc32]`.
2. Write it to `hard_state.tmp`, then `sync()`.
3. `std::filesystem::rename` it to `hard_state`.
4. Open the directory and `sync()` it, so the rename itself is durable.

CRC32 is implemented in `hammurabi_wire` with a lookup table computed at compile time.

**Why this way:**
- **Atomic replace.** `rename` either fully happens or does not happen at all. After a crash we find either the
  old or the new `hard_state`, never a half-written one. Syncing the directory makes the rename durable.
- **std::fstream cannot `fsync`.** For durability we need the POSIX calls (`open`, `write`, `fsync`,
  `ftruncate`), so we wrap them in a small class.
- **RAII** ("resource acquisition is initialization"): the destructor closes the descriptor, so it is
  closed exactly once on every path, including exceptions. This is the one class in the project that needs
  the rule of five.
- **Errors throw `std::system_error`** with `errno`. The server stops (section 5.7).

**C++ notes:**
- `constexpr` functions (C++20 allows loops and local arrays in them) can build the CRC table at compile time:
  `constexpr auto crc_table = make_crc_table();`.
- Move-only types: deleting the copy operations and implementing the move operations models unique
  ownership, like `std::unique_ptr` does for memory.

**Check:** `[storage]` tests in a fresh temporary directory per test (`std::filesystem::temp_directory_path`
plus a unique name):
- Save and reload.
- A corrupt `hard_state` (a flipped byte) is detected via the CRC and throws.
- A leftover `hard_state.tmp` is ignored.
- `file` closes its descriptor (check via a moved-from object and a reopened path).

#### Step 6.4: File storage, part 2: the log file

**Goal:** Log entries are appended durably, truncated on conflict, and torn writes at the end are cut off at startup.

**Build:**
- The `log` file is a sequence of records `[u32 length][u32 crc32][payload = encoded log_entry]`.
- `file_storage` keeps the file offset of every record in memory, so `truncate(index)` is one `ftruncate` to
  the start of that record, plus a sync.
- `append` writes all records of the batch, then syncs **once**.
- `load()` reads records until the end. At the first incomplete record or bad CRC, it truncates the file
  there and logs a warning.

**Why it is safe to cut off a torn tail.** A record is only half-written if the process crashed during
`append`, which is before the `sync` returned. Before the sync returns, nobody was told about this entry:
no reply was sent and no vote was cast. Dropping it is the same as if the crash had happened one moment
earlier.

**Check:** `[storage]` tests:
- Append, reload, compare.
- Truncate in the middle, append again, reload.
- Append garbage bytes or half a record to the file: reload drops exactly the broken tail.
- A full node round trip: a `raft::node` on `file_storage` votes, appends, is destroyed, and is created
  again from the same directory with the same state.

---

### Phase 7: The real network

#### Step 7.1: UDP transport with coroutines

**Goal:** Two endpoints on localhost exchange Raft envelopes over real UDP.

**Build:** `hammurabi_net`, `udp_transport`:

```cpp
class udp_transport {
public:
    udp_transport(asio::any_io_executor executor, const asio::ip::udp::endpoint& bind_to);
    [[nodiscard]] asio::ip::udp::endpoint local_endpoint() const;  // after binding to port 0
    void send(const raft::envelope& env, const asio::ip::udp::endpoint& to);
    asio::awaitable<std::pair<raft::envelope, asio::ip::udp::endpoint>> receive();  // skips undecodable datagrams
private:
    asio::ip::udp::socket socket_;
};
```

`receive()` written out:

```cpp
asio::awaitable<std::pair<raft::envelope, asio::ip::udp::endpoint>> udp_transport::receive() {
    std::array<std::byte, 2048> buffer{};
    asio::ip::udp::endpoint sender;
    for (;;) {
        const std::size_t n = co_await socket_.async_receive_from(asio::buffer(buffer), sender, asio::use_awaitable);
        if (auto env = raft::decode_envelope(std::span{buffer}.first(n))) {
            co_return std::pair{std::move(*env), sender};
        }
        // malformed datagram: log at debug level and keep waiting
    }
}
```

`send` encodes into a `std::shared_ptr<std::vector<std::byte>>` and calls `async_send_to` with a completion
handler that captures the pointer, so the buffer lives until the send has finished. Send errors are logged
and ignored, because Raft treats them like lost packets.

**Why this way:**
- **Coroutines vs. callbacks.** With callbacks, the receive loop is a function that restarts itself from its
  own completion handler, as in the old `connector::receive`. Control flow is spread over several functions,
  and lifetimes are hard to follow. With a coroutine, the loop is a plain `for (;;)`. `co_await` suspends the
  function until data arrives, and the thread does other work meanwhile.
- **Binding to port 0** makes the OS choose a free port. Tests never collide on ports.

**C++ notes:**
- A function returning `asio::awaitable<T>` is a coroutine. Inside it, `co_await op(..., asio::use_awaitable)`
  starts an asynchronous operation and suspends until it completes; errors arrive as `std::system_error`
  exceptions.
- `co_return` returns the result.
- `asio::co_spawn(executor, coroutine(), asio::detached)` starts a coroutine.

**Check:** `[integration]` test: two transports on `127.0.0.1:0`. Send each message type and receive it
equal. Garbage sent with a plain socket is skipped. Run the `io_context` with `run_for(1s)` as a safety
timeout.

#### Step 7.2: The host and `hammurabi-server`

**Goal:** Real server processes form a cluster and elect a leader over UDP, with data on disk.

**Build:**
- `net::host` owns `file_storage`, `replica` and `udp_transport`, and runs two coroutines:

```cpp
asio::awaitable<void> host::tick_loop() {
    asio::steady_timer timer{executor_};
    auto next = std::chrono::steady_clock::now();
    for (;;) {
        next += tick_duration;              // 10 ms; fixed rate, no drift
        timer.expires_at(next);
        co_await timer.async_wait(asio::use_awaitable);
        replica_.tick();
        flush();
    }
}

asio::awaitable<void> host::receive_loop() {
    for (;;) {
        auto [env, sender] = co_await transport_.receive();
        if (!is_configured_address(env.from, sender) || env.to != self_) continue;  // section 5.6
        replica_.receive(env);
        flush();
    }
}
```
- `flush()` takes the replica's output, sends all messages and hands responses to waiting clients
  (step 7.3).
- Members are declared in the order **storage, then replica**, because the replica holds a reference to the
  storage. C++ constructs members in declaration order and destroys them in reverse.
- `apps/server/main.cpp`:
  - CLI11 options: `--id 2 --data-dir data/2 --node 1=127.0.0.1:7001:8001 --node 2=… --node 3=…`, where each
    node is `id=host:raft_port:client_port`.
  - `--node` is parsed into `std::expected<node_address, std::string>` using `std::from_chars`.
  - `asio::signal_set` for SIGINT/SIGTERM stops the `io_context`.
  - A top-level `try`/`catch` prints the error and exits with code 1.
- A small logging helper `log::info(fmt, args...)` uses `std::println(stderr, ...)` with a timestamp and the
  node id. The host logs role changes by comparing the role before and after each event; the core itself
  never logs.

**Why this way:**
- The host contains no Raft logic, only plumbing, so it can stay short (about 150 lines).
- A fixed-rate timer (`next += tick`) does not drift when a tick runs late.

**C++ notes:** `std::format_string<Args...>` as the parameter type of the logging helper keeps compile-time
checking of format strings when we wrap `std::println`.

**Check:**
- `[integration]` test: three hosts in **one** `io_context`, each with its own temp data directory and port
  0. Bind all sockets first, then build the address table. A leader exists within 2 s.
- Manually: start three servers in three terminals and watch one become leader. Stop it with Ctrl-C and
  watch a new one take over.

#### Step 7.3: The client protocol over TCP

**Goal:** Clients can send requests over TCP; the server answers when the replica has an answer.

**Build:**
- Frame helpers: `co_await read_frame(socket)` reads a `u32` length, rejects more than 64 KiB, then reads
  the payload. `co_await write_frame(socket, bytes)` does the reverse.
- `host::accept_loop()` accepts connections and `co_spawn`s `serve_client(std::move(socket))`:

```cpp
asio::awaitable<void> host::serve_client(asio::ip::tcp::socket socket) {
    try {
        for (;;) {
            auto frame = co_await read_frame(socket);
            auto request = kv::decode_request(frame);
            kv::response response = request ? co_await submit(*request)
                                            : kv::response{kv::rejection::malformed};
            co_await write_frame(socket, kv::encode(response));
        }
    } catch (const std::system_error&) {
        // client disconnected or sent garbage: just end this coroutine
    }
}
```
- `submit` is the interesting part. It must **wait** until `flush()` finds the response, possibly many ticks
  later. We build our own awaitable operation with `asio::async_initiate`:

```cpp
template<typename CompletionToken>
auto host::async_submit(const kv::request& request, CompletionToken&& token) {
    return asio::async_initiate<CompletionToken, void(kv::response)>(
        [this, &request](auto handler) {
            const request_id id = next_request_id_++;
            waiting_.emplace(id, std::move(handler));  // std::map<request_id, asio::any_completion_handler<void(kv::response)>>
            replica_.submit(id, request);
            flush();
        },
        token);
}
// submit(request) == async_submit(request, asio::use_awaitable)
```
- In `flush()`, for every `(id, response)`, remove the handler from `waiting_` and **post** it:
  `asio::post(executor_, [h = std::move(handler), r = std::move(response)]() mutable { std::move(h)(std::move(r)); })`.

**Why this way:**
- `async_initiate` is Asio's standard way to write your own asynchronous operation. It works with coroutines,
  callbacks or futures; the caller chooses through the completion token.
- **Why post instead of calling the handler directly?** Calling it would resume the client's coroutine in
  the middle of `flush()`. That coroutine might call `submit` again, which calls `flush()` again, while
  we are still iterating over the first output. Posting runs the handler **after** `flush()` has finished.
  This is the "re-check after resume" pitfall from section 5.4, solved at the source.
- A request whose client has disconnected is still answered internally. The resumed coroutine fails to write
  and ends. Nothing leaks except during a long network split without a new leader, which is bounded by the
  number of requests made in that time.

**Check:** `[integration]` test with three hosts:
- Send a raw `put` frame to the leader → `ok`.
- To a follower → `not_leader` with the leader's address.
- A frame announcing 1 GB is rejected without allocating.

#### Step 7.4: `hammurabi-cli`

**Goal:** A command-line client that finds the leader, retries safely and reports clear results.

**Build:**
- `net::client` (reused by integration tests):

```cpp
class client {
public:
    client(asio::any_io_executor executor, std::vector<std::string> servers, std::chrono::milliseconds overall_timeout);
    asio::awaitable<std::optional<kv::result>> execute(kv::operation op);  // nullopt = outcome unknown
};
```
  `execute` works like this:
  1. Register first, except for `get`.
  2. Then try servers: the leader hint first, otherwise round-robin. Each connect, write and read has a
     per-attempt timeout via `asio::cancel_after(500ms)`.
  3. Retry with the **same** sequence number until the overall deadline.
- `apps/cli/main.cpp`: CLI11 subcommands `get KEY`, `put KEY VALUE`, `delete KEY`,
  `cas KEY [--expect VALUE | --expect-absent] NEW`. The global `--servers 127.0.0.1:8001,127.0.0.1:8002`
  defaults to the local script cluster.
- Exit codes:

| Code | Meaning |
|---|---|
| 0 | success |
| 1 | executed, but negative: `not_found`, `cas_failed` |
| 2 | unavailable or **outcome unknown** |
| CLI11's own codes | usage errors |

**Why this way:**
- "Outcome unknown" is reported honestly instead of guessing. Exit code 2 lets scripts handle it.
- `asio::cancel_after` attaches a timeout to a single operation. Without it, a coroutine would need a second
  timer racing the operation.

**Check:** `[integration]` test runs `client::execute` against three in-process hosts: put/get/delete/cas,
including stopping the leader host in the middle (the client must succeed via the new leader). Manually: three
servers plus `hammurabi-cli put a 1 && hammurabi-cli get a`.

#### Step 7.5: Cluster script and smoke test

**Goal:** Start a local cluster of any size with one command; CI runs an end-to-end smoke test with real processes.

**Build:**
- `scripts/cluster.sh`. It must be compatible with **bash 3.2**, because macOS still ships it; that means no
  associative arrays.

```
scripts/cluster.sh start N     # N nodes; raft ports 7001…, client ports 8001…; data in run/data/<id>; logs in run/<id>.log
scripts/cluster.sh stop        # stop all (PIDs from run/<id>.pid)
scripts/cluster.sh status      # running nodes and their ports
scripts/cluster.sh kill ID     # stop one node (simulate a crash)
scripts/cluster.sh restart ID  # start it again with the same data directory
scripts/cluster.sh servers     # prints the --servers value for the CLI
scripts/cluster.sh wipe        # stop and delete run/ (only for the whole cluster!)
```
- `scripts/smoke_test.sh`:
  1. Start 3 nodes and write values.
  2. Kill the leader, write and read again.
  3. Restart the old leader and check that it serves the data.
  4. Stop the cluster.
- Add a CI job that runs the smoke test after the build.

**Why this way:** The integration tests run in one process. The smoke test is the only place where real
processes, real signals and the real CLI meet. Only `wipe` deletes data directories, and only for the
whole cluster at once. Deleting the data of a single node would make it a "new member" (section 5.7).

**Check:** `scripts/cluster.sh start 5`, then a few CLI commands, `kill` the leader, more commands,
`restart`, `stop`. `scripts/smoke_test.sh` passes locally and in CI.

---

### Phase 8: Fast linearizable reads (ReadIndex)

#### Step 8.1: ReadIndex in the Raft core

**Goal:** The leader can confirm a read without writing a log entry (section 5.9).

**Build:**
- New API: `[[nodiscard]] std::expected<void, not_leader> node::request_read(read_id id)`.
  `output` gains `std::vector<read_id> reads_ready` and `std::vector<read_id> reads_failed`.
- `append_entries` gains `std::uint64_t round`; `append_entries_reply` echoes it.
- Leader logic:
  1. A read waits until the leader has committed an entry in its current term. The no-op from step 4.3
     makes this quick. Before that, the leader's commit index may be behind its predecessor's.
  2. Then the read records `read_index = commit_index_` and `round = ++current_round_`, and the leader sends
     a heartbeat round right away.
  3. For each peer, the leader remembers the highest round it echoed with `reply.term == current term`.
     Success or failure does not matter: the reply proves the peer accepted us as leader of this term.
  4. A read is **confirmed** when 1 (the leader itself) plus the number of peers with an acknowledged
     round ≥ its round reaches a majority.
  5. A confirmed read is **ready** once `handed_out_ >= read_index`. `take_output()` lists `reads_ready`
     **after** `committed`, so the replica applies the entries first. This ordering is a documented part
     of the contract.
  6. Stepping down moves all waiting reads to `reads_failed`.

**Example.** Five nodes, leader A in term 4, commit index 20.
- A read arrives. `read_index = 20`, `round = 7`, heartbeats are sent.
- Replies from B (round 7) and C (round 7) arrive: A + B + C = 3 of 5 is a majority, so the read is confirmed.
- A's state machine has applied up to 20, so the read is ready.

Had A been cut off with only B, the read would never be confirmed, which is exactly what we want.

**Why the "round" number?** A reply only proves leadership at the time the peer **sent it**. A reply to a
heartbeat sent *before* the read arrived proves nothing about the moment after the read arrived. The round
number links each reply to a specific heartbeat. Only rounds ≥ the read's round count.

**Check:** `[read_index]` tests:
- A one-node cluster: the read is ready immediately.
- Three nodes: not ready before a majority echoes the round; ready after.
- Not ready before the no-op is committed.
- In the simulation: an isolated old leader never makes a read ready.
- A leader that steps down fails its waiting reads.

#### Step 8.2: Use ReadIndex in the replica

**Goal:** `get` no longer writes to the log, and the system stays linearizable.

**Build:**
- In `replica::submit`, `get` calls `node.request_read(id)` and keeps `id → key` in a map.
- On `reads_ready`, the replica answers with `machine_.read(get)`. On `reads_failed`, it answers `not_leader`.
- The CLI no longer registers for `get`.

**Check:**
- `[replica]` test: 100 gets do not change the log length.
- `-L chaos`: linearizability holds with ReadIndex reads.
- Repeat the "naive read" experiment from step 5.5. It must fail, while the ReadIndex version passes.

---

### Phase 9: Finish

#### Step 9.1: Fuzzing the decoders

**Goal:** No input from the network can crash the server.

**Build:**
- `fuzz/decode_fuzzer.cpp` defines `LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)`. It passes
  the bytes to `raft::decode_envelope`, `kv::decode_request`, `kv::decode_response` and `kv::decode_operation`.
  When decoding succeeds, it encodes the result again and checks that the bytes are equal.
- The CMake option `HAMMURABI_FUZZ` (Clang only) builds it with `-fsanitize=fuzzer,address,undefined`.
- A CI job (Linux, Clang 19) runs it for 60 seconds, starting from a small corpus of valid messages
  produced by a unit test.

**Why this way:** The decoders are the only code that touches untrusted input. A fuzzer combined with ASan
finds out-of-bounds reads and giant allocations within seconds; the old `connector` would not have survived
it. Apple Clang does not ship libFuzzer, so this runs on Linux only.

**Check:** `cmake --preset conan-debug -DHAMMURABI_FUZZ=ON`, build, then
`./build/Debug/fuzz/decode_fuzzer -max_total_time=60`. No crash. The CI job is green.

#### Step 9.2: Documentation

**Goal:** A reviewer understands the project in five minutes and can run it in ten.

**Build:**
- `README.md`:
  - What it is.
  - The feature list and the scope table.
  - Quick start: build, `scripts/cluster.sh start 3`, CLI examples.
  - How to run the tests.
  - The testing strategy (simulation, invariants, linearizability, fuzzing).
  - Known limitations: no snapshots, no membership changes, do not delete a single node's data.
  - A link to `legacy-2019`.
- `docs/ARCHITECTURE.md` (2–3 pages):
  - The layer diagram from section 5.3.
  - The life of a write and of a read.
  - Sans-I/O and why.
  - Persistence and file format.
  - Sessions.
  - ReadIndex.
  - Key decisions with one-line reasons (taken from section 5 of this plan).

**Check:** A colleague, or a fresh clone, follows the README without help. `scripts/check.sh` still passes.

#### Step 9.3: Final review

**Goal:** Every rule of Figure 2 is implemented, tested and findable.

**Build:**
- Walk through Appendix A and fill in the test names.
- Run the full CI matrix, a sanitizer build and a long chaos run (`HAMMURABI_SEEDS=1000`).
- Fix any remaining clang-tidy suppressions or justify them in a comment.

**Check:** All CI jobs green. The 1000-seed chaos run passes.

---

## Appendix A: Raft rules and where they live

| Rule (paper, Figure 2 unless noted) | Step | Function |
|---|---|---|
| Any message with higher term → update term, become follower | 2.2 | `node::receive` |
| Follower: election timeout → become candidate | 2.1 | `node::tick` |
| Candidate: increment term, vote for self, reset timer, send RequestVote | 2.1 | `become_candidate` |
| Candidate: majority of votes → leader | 2.3 | `handle(request_vote_reply)` |
| Candidate: AppendEntries from new leader → follower | 2.3 | `handle(append_entries)` |
| RequestVote: reply false if term < currentTerm | 2.2 | `handle(request_vote)` |
| RequestVote: grant if votedFor is null/candidate and log up to date (§5.4.1) | 2.2 | `handle(request_vote)` |
| Persist currentTerm, votedFor, log before responding | 2.1, 2.2, 4.1, 6.3–6.4 | `storage` calls |
| Leader: heartbeats to prevent elections | 2.3 | `tick`, `send_append_entries` |
| Leader: append client command, respond after applied | 4.1, 5.1 | `propose`, `replica` |
| Leader: send entries from nextIndex; on failure decrement nextIndex | 4.1, 4.2 | `send_append_entries`, `handle(append_entries_reply)` |
| Leader: commit N if majority matchIndex ≥ N and log[N].term == currentTerm (§5.4.2) | 4.3 | `maybe_advance_commit` |
| AppendEntries: reply false if term < currentTerm | 4.1 | `handle(append_entries)` |
| AppendEntries: reply false if log lacks prevLogIndex/prevLogTerm | 4.1 | `handle(append_entries)` |
| AppendEntries: delete conflicting entries and all after | 4.1 | `handle(append_entries)` |
| AppendEntries: append new entries | 4.1 | `handle(append_entries)` |
| AppendEntries: commitIndex = min(leaderCommit, index of last new entry) | 4.1 | `handle(append_entries)` |
| All servers: apply entries up to commitIndex in order | 4.3, 5.1 | `take_output`, `replica` |
| Client sessions / at-most-once execution (§8, dissertation §6.3) | 5.2 | `state_machine::apply` |
| Linearizable reads (§8, dissertation §6.4) | 5.1, 8.1 | log reads, `request_read` |
| No-op entry at the start of each term (§8) | 4.3 | `become_leader` |

## Appendix B: Ideas beyond this plan

| Idea | What it would add |
|---|---|
| Pre-vote (dissertation §9.6) | A node that was cut off would no longer disrupt a healthy leader with a higher term when it returns. This is a liveness improvement only; safety does not change. |
| Check-quorum | A leader that cannot reach a majority steps down by itself, so clients find the real leader faster. |
| Snapshots / log compaction | Bounded log size and fast restarts. Needs a snapshot format for the state machine and an `InstallSnapshot` message. |
| Membership changes | Adding and removing nodes at runtime (joint consensus or single-server changes). |
| Leader leases | Even faster reads, at the cost of assuming bounded clock drift. |
| Code coverage report in CI | `llvm-cov`/`gcovr` to show which branches the simulation reaches. |
| Benchmarks | Throughput and latency with `hammurabi-cli` in a loop, or a small load generator. |
