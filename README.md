# Failure Containment Fabric Network (FCFN) 1.0.0

FCFN is an infrastructure runtime that answers one operational question, deterministically and truthfully:

> Given authoritative failure evidence and dependency topology, what **minimum governed scope** must be contained **now** to prevent propagation, which healthy resources must remain **protected**, and when may containment **expand, contract, or be released**?

FCFN owns **bounded containment intent** and nothing else. It computes and records what must be contained, under which generations, with which authority, and it tracks whether that intent has been applied. It never diagnoses failures, never plans repairs, never routes traffic, and never enforces isolation itself.

* Portable C++20, CMake, zero third-party runtime dependencies.
* Library plus runtime plus tools, all installable as a CMake package.
* Durable state is versioned, integrity-checked, and conservatively recovered.
* Every externally visible answer carries an explicit claim strength; nothing is rounded up to success.

---

## 1. Systems boundary

**Owned by FCFN**

* The containment decision: the minimum set of resources to contain, with an explicit reason and witness for every member.
* Authority binding: coordinator epoch, process incarnation, policy generation, topology generation, evidence revision.
* Authorization as a separate, bounded, revocable lease between detection and decision.
* The apply protocol state machine: intent, acknowledgement, verification, and the explicit states in between.
* Generation-bound transitions (expand, contract, release) and their fences.
* Durable lineage: definitions, policy, evidence records, plans, transitions, attempts, fences.

**Explicitly not owned by FCFN**

* Failure classification and diagnosis. FCFN consumes authoritative failure evidence; it does not decide what a failure means.
* Repair or remediation planning.
* Traffic engineering, rerouting, or drain orchestration.
* Enforcement of isolation. FCFN emits intent; an external enforcement plane applies it and reports what it observed.
* Cryptographic authentication of peers. See the trust boundary in section 9.

FCFN integrates through typed inputs and outputs, evidence with generations, and explicit authority boundaries. It does not absorb adjacent responsibilities to make demonstrations easier.

---

## 2. Authority and generation model

Containment is legal only when the exact inputs that produced it are still current. FCFN therefore binds every decision to an **authority vector**:

| Field | Meaning |
| --- | --- |
| coordinator epoch | Monotone counter advanced by every restart (new incarnation). |
| boot identity | Random 64-bit boot id plus process id; identifies one process incarnation. |
| policy generation | Revision of the durable containment policy. |
| topology generation | Revision of the durable topology definition. |
| evidence revision and digest | Exact evidence vector revision and its canonical digest. |
| issued sequence | The durable sequence the vector was minted at. |

The rules FCFN enforces, without exception:

* Matching identifiers are **not** matching generations. A vector whose boot id differs is **fenced**, even if every other field matches.
* Persistence is **not** liveness. A restarted coordinator restores definitions and lineage, never freshness, leases, or effect.
* Observation is **not** authority. A detection record never authorizes containment.
* Eligibility is **not** authorization. An authorization lease gates planning and apply submission.
* Authorization is **not** application. Handing intent to the enforcement plane changes nothing by itself.
* Acknowledgement is **not** verified effect. Only an effect verification whose observed boundary digest equals the intent digest, bound to the same attempt and the same incarnation, moves an attempt to VERIFIED_APPLIED.

Every authority-bearing dependency is generation-bound, so any change revokes what it authorized: an authorization whose bound vector no longer matches current state is fenced on first use and refuses every later request.

---

## 3. The containment problem: class MCC-1

FCFN solves exactly one problem class, stated precisely in include/fcfn/engine/cut.hpp.

**Input.** A validated propagation graph G over nodes V; each edge is PROVEN, UNKNOWN, or REFUTED. The conservative graph G+ contains PROVEN and UNKNOWN edges (UNKNOWN is never assumed absent). A non-empty set F of failure sources, a set P of protected obligations, per-node eligibility and containment weight, and policy bounds.

**Decision variable.** A containment set C, the governed scope to contain now.

**Hard constraints.**

1. Every member of C is eligible for containment.
2. Every eligible failure source is in C when the policy requires source inclusion.
3. No protected obligation is in C unless the policy explicitly authorizes protected inclusion.
4. No duplicates; member count and total weight stay inside the configured bounds.
5. In G+ with C removed there is no path from any source in F to any obligation in P.

**Objective**, minimised lexicographically and totally ordered:

1. number of protected obligations inside the boundary,
2. total containment weight,
3. cardinality,
4. canonical member signature (deterministic tie-break).

The optimum is unique. Output is independent of insertion, discovery, and container order: topology nodes are canonically sorted, edges are canonically sorted, and the tie-break is a canonical identity comparison.

**Why the answer is trustworthy**

* The exact solver is complete: it either proves optimality by exhaustive search, proves infeasibility, or reports that its budget ended. It never presents a bounded search as a proof.
* Branching takes a canonical shortest residual path and branches on the first member of that path that belongs to the cut, which enumerates every feasible set exactly once.
* Pruning uses a valid lower bound built from vertex-disjoint residual paths.
* Every returned boundary is re-verified by an independent verifier that re-checks all five hard constraints, not just disconnection.
* Infeasibility is reported only with a compact certificate (a source-to-obligation path whose nodes are all ineligible, checked by an independent verifier) or after an exhaustive search that ended early-free. A bounded search that finds nothing returns SEARCH_LIMIT_REACHED semantics: feasibility unknown, never "no solution".
* Every member carries a witness path proving it is load-bearing, plus a machine-readable inclusion reason.

A scalable deterministic heuristic handles instances above the exact-search node limit. It never claims optimality.

---

## 4. Claim strength

Every plan states its claim explicitly:

| Claim | Meaning |
| --- | --- |
| PROVEN_CONTAINMENT | Verified cut, proven optimal, evidence current, every required source contained. |
| PROVEN_FEASIBLE_NOT_MINIMAL | Verified cut exists; optimality was not proven within the budget. |
| INDETERMINATE | The cut cannot be claimed: incomplete adjacency, stale or unconfirmed evidence, conflicting evidence, or a required source that cannot be contained. |
| PROVEN_INFEASIBLE | No eligible containment exists, with a certificate or a completed exhaustive search. |
| INVALID / UNSUPPORTED | The request itself was rejected, or is not implemented. |

Supporting axes are reported separately so no information is lost: feasibility status, optimality status, evidence currency, search counters (explored nodes, pruned branches, residual paths, budget, whether the budget was exhausted), and an explicit infeasibility witness.

Degradation is deliberate and visible:

* An UNKNOWN edge does **not** degrade the claim: the conservative graph is used, so containment holds for every realisation of that edge. The plan says so and reports the unknown edge count.
* Unknown **adjacency completeness** on the propagation frontier does degrade the claim, because paths may be missing.
* Evidence not confirmed in the current boot degrades the claim until the detector re-confirms it (see restart semantics).
* Contradictory evidence for one resource is rejected and marks subsequent plans CONFLICTING until a strictly newer generation supersedes it.

---

## 5. Lifecycle and restart semantics

~~~
detection  ->  authorization  ->  plan  ->  transition  ->  apply  ->  ack  ->  verify
   (input)        (lease)        (decision)  (generation-     (intent) (observed) (verified
                                             bound)                              effect)
~~~

**Transitions.** Expand, contract, and release are decisions bound to the exact boundary generation they supersede and to a valid authorization. Contract must be a proper subset of the current boundary; expand must be a superset; release requires a proven plan with an empty boundary (no active failures). Any transition whose current boundary is not in verified effect state is refused as INDETERMINATE. Every decision, including every refusal, is recorded durably with its reason.

**Startup.** A new process incarnation always advances the coordinator epoch and takes a fresh boot identity. Then:

* every pre-restart attempt is fenced with an explicit reason;
* effect state is never restored as current: a surviving boundary becomes AMBIGUOUS and requires fresh verification under the new incarnation;
* authorizations are never restored; the number dropped is reported;
* evidence freshness is not restored: a plan computed immediately after a restart is INDETERMINATE until the detector re-confirms the evidence (same generation and digest is enough: re-confirmation is not re-detection);
* definitions (topology, policy) and lineage (plans, transitions, detections, fences) are restored.

**Re-establishing effect.** The enforcement plane re-asserts the surviving boundary by submitting an apply for a plan whose membership is identical to the current boundary. Verification of that attempt under the current epoch makes the boundary current again with VERIFIED_APPLIED. Membership never changes through this path.

---

## 6. Durable formats and recovery

Everything durable is framed: magic, format version, record type, declared length, monotone sequence, payload CRC-32C, header CRC-32C.

* Declared lengths above the bound are refused **before** allocation.
* Decoding is total and sticky: the first failure poisons the decoder.
* Impossible lengths, unknown record types, unsupported versions, and sequence regression are refused, never repaired.
* Snapshot files carry their own magic, version, length, and integrity field; trailing bytes are rejected.
* The CURRENT pointer is integrity-checked; store files without a pointer are corruption, not a fresh store.
* Transactional replacement: snapshots and the CURRENT pointer are written to a temporary sibling, flushed to stable storage, then atomically replaced.
* Ordering: append to the write-ahead log, flush durably, publish to memory, and only then snapshot.
* Recovery replays only records newer than the snapshot, requires strictly increasing sequences, and skips records the snapshot already covers.
* Exactly one damage class is repaired: a strict prefix of a record at the end of the log (a torn tail), reported with the discarded byte count and made durable immediately. A corrupted header or payload inside a complete record is refused.
* A store is opened by one incarnation at a time. The lock records the owning pid; a stale lock from a dead process is taken over, a live one refuses the second open.

---

## 7. Wire protocol

Loopback TCP with a bounded framed protocol (44-byte header: magic, version, message type, declared payload length, session id, monotone sequence, epoch, payload CRC, header CRC).

* Oversized declared payloads are refused before allocation.
* Every enum and domain value is validated.
* Truncated prefixes, corrupt frames, invalid types, replayed or regressed sequences, and trailing bytes are rejected; a protocol violation poisons the session and closes it.
* Each connection performs a token handshake and is bound to a session identity. Every request must carry that session id and the epoch advertised at handshake time. A request under another session's identity is refused; a request carrying a stale epoch is refused.
* Shutdown closes the listener and every session socket, which releases blocked accepts and reads; session threads are joined outside every lock and never by themselves.

**Trust boundary (explicit).** The session token is an opaque bearer label compared for equality. The channel is not encrypted, no cryptographic authentication is performed, and FCFN makes no secure-transport claim. Authentication, encryption, and peer identity are pluggable concerns outside this boundary; deploy FCFN on a trusted management network or tunnel it.

---

## 8. Build, install, use

Requirements: CMake 3.25+, a C++20 compiler, Ninja or any CMake generator. No third-party libraries.

~~~
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure     # no timeouts anywhere, by design
cmake --install build --prefix /path/to/prefix
~~~

Consume the installed package from an independent project:

~~~cmake
find_package(FCFN 1.0 CONFIG REQUIRED)
target_link_libraries(my_target PRIVATE fcfn::core fcfn::runtime)
~~~

Targets: fcfn::core (model, engine, store, runtime coordinator) and fcfn::runtime (framed transport, sessions, service). Executables: fcfn_coordinator, fcfn_client, fcfnctl.

CMake options: FCFN_BUILD_TESTS, FCFN_BUILD_APPS, FCFN_WARNINGS_AS_ERRORS (default ON, /W4 /WX on MSVC), FCFN_ENABLE_ASAN, FCFN_ENABLE_UBSAN, FCFN_ENABLE_STATIC_ANALYSIS.

---

## 9. Tools and input formats

**fcfn_coordinator** runs the runtime:

~~~
fcfn_coordinator --token <token> --store <dir> [--port <n>] [--topology <file>] [--policy <file>]
                 [--lease-millis <n>] [--no-current-boot-confirmation]
                 [--crash-point <before_commit|after_commit_before_ack|after_ack>] [--crash-after <n>]
~~~

It prints its startup report, its listening port, and READY, then serves until shutdown. The crash-point options exist for crash-recovery proofs: the process announces CRASH-POINT, blocks, and is hard-killed by the supervising test.

**fcfn_client** is a real session driven by a scenario script:

~~~
fcfn_client --port <n> --token <token> --scenario <file>
# commands: describe, topology <file>, policy <file>, evidence,
#           detect <resource> <present|absent|unknown> <generation> [digest=<hex32>],
#           authorize, plan, transition <expand|contract|release>,
#           apply, ack [accepted|rejected],
#           verify <applied|released|not_applied|partially_applied|unknown>,
#           boundary, attempts, checkpoint, shutdown, expect <status-token>
~~~

**fcfnctl** performs offline work and store inspection:

~~~
fcfnctl plan --topology <file> [--evidence <file>] [--policy <file>] [--out <plan.bin>] [--json-out <plan.json>]
fcfnctl verify --topology <file> --plan <plan.bin> [--evidence <file>] [--policy <file>]
fcfnctl store-inspect --root <dir> [--repair]
fcfnctl version
~~~

An offline plan is a planning aid: it carries no authority and says so in its explanation. Only a running coordinator can authorize containment.

**Topology file**

~~~
generation 1
node edge-a containable=1 weight=4 protected=0 completeness=complete
edge edge-a spine-1 evidence=proven generation=1
~~~

**Evidence file**

~~~
edge-a present 1
edge-b unknown 2 digest=0123456789abcdeffedcba9876543210
~~~

**Policy file**

~~~
generation 2
require_failure_source_inclusion 1
allow_protected_inclusion 0
max_boundary_weight 1000000
max_boundary_members 512
exact_search_budget 200000
heuristic_budget 200000
exact_instance_node_limit 4096
~~~

**Examples.** examples/downstream_consumer is an independent project that consumes the installed package, computes a plan, verifies it with the public verifier, and drives a durable runtime through one containment decision.

---

## 10. Evidence matrix: REAL, SYNTHETIC, UNSUPPORTED

| Area | Status | Notes |
| --- | --- | --- |
| Dependency graphs, evidence, plans, transitions | **SYNTHETIC** | Every fixture is generated deterministically from an explicit seed. No physical fabric is involved. |
| Multiprocess runtime, sockets, hard kills, restarts | **REAL** | Independent OS processes over real loopback sockets; TerminateProcess at three durable boundaries; restart on the same store. |
| Durable store, torn tails, corruption refusal, snapshots | **REAL** | Real files, real truncation and byte corruption, real process restarts. |
| Sanitizer coverage | see section 12 | Reported as run or UNSUPPORTED with the missing component. |
| Static analysis | see section 12 | Reported as run or UNSUPPORTED with the missing component. |
| Switch ASIC, NIC, RDMA/RoCE, DPU/SmartNIC, NVLink, InfiniBand, multi-node | **UNSUPPORTED** | Not exercised on hardware. FCFN does not claim any physical fabric behaviour, and no stub or metadata is presented as hardware validation. |

---

## 11. Concurrency and ownership

FCFN has no callback API, so no user code can run while a lock is held; observers poll for results instead. The rules and the audit that verifies them are in docs/CONCURRENCY_AUDIT.md:

* one mutex per runtime guards all mutable state; session handling never takes it while holding the session table lock and vice versa;
* socket teardown closes the handle first so blocked reads and accepts return;
* shutdown joins threads outside every lock and never joins the calling thread;
* store instances are single-writer and additionally protected by an incarnation lock;
* no lock is re-entered, and no helper is called with a lock held that could re-enter state.

---

## 12. Tests and validation

Suites live under tests/ and are run by CTest. There are no timeouts anywhere, in code or in the build system: a hang is a defect to diagnose, not to mask.

| Suite | Focus |
| --- | --- |
| unit | model documents, canonical round trips, lifecycle smoke, claim semantics |
| codec | encode/decode totality, truncation prefixes, corruption, bounds |
| persistence | store recovery, torn tails, corruption refusal, restart semantics |
| solver | differential comparison against an independent exhaustive reference solver, known answers, limits, certificates, adversarial graphs |
| property | invariant checks after every operation: monotonic safety, necessity soundness, determinism |
| adversarial | hostile inputs, resource exhaustion, authority forgery |
| concurrency | deterministic latches and barriers, no sleep-based luck |
| multiprocess | real processes, real sockets, hard kills, restart fencing |
| scale | completed work at multiple sizes with explicit search-limit outcomes |

Validation scripts:

~~~
pwsh scripts/validate.ps1       # full matrix: release, debug, tests, install, consumer, tools, probes
pwsh scripts/fresh_clone.ps1    # clones the committed revision and reproduces closure from it
~~~

The validation matrix builds Release and Debug, runs CTest for both, installs, configures and runs the independent consumer, runs the tools, and probes sanitizer and static-analysis capability. A capability the host lacks is reported as UNSUPPORTED, never as coverage. The fresh-clone script proves the commit, not the working tree: it clones, builds, tests, installs, and runs the consumer against that install.

---

## 13. Genuine limitations

* FCFN solves one problem class (MCC-1) as defined above. Multi-commodity containment with per-obligation service objectives, time-dependent propagation, and capacity-constrained containment are out of scope and are not approximated.
* Exact search is exponential in the worst case. Large instances fall back to the heuristic, which is honestly labelled and never claims optimality; the exact budget is a configuration parameter with an explicit bounded outcome.
* The shared-risk domain reason exists in the vocabulary and is accepted in boundaries, but no automatic shared-risk inference is implemented: a caller that knows about a shared-risk fence must express it through the topology.
* The wire protocol is unauthenticated and unencrypted by design; peer identity, key management, and transport security are out of scope.
* Evidence freshness and effect state are process-incarnation scoped: after a restart they must be re-established, which is deliberate but costs round trips.
* The store is single-writer per incarnation and uses advisory locking through a pid file; it is not a distributed consensus log.
* Only the Windows/MSVC toolchain is exercised in the recorded validation results. The code is portable C++20 with POSIX paths implemented for sockets, processes, and file flushing, but those paths have not been built or run on this host and are therefore unproven here.

---

## License
Apache License 2.0. Copyright 2026 Summon Software Labs.
