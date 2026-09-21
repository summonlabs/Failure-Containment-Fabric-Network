# FCFN concurrency and ownership audit

Scope: the whole runtime as shipped in 1.0.0. Method: deliberate inspection of every lock,
thread, socket, and lifetime boundary, plus the concurrency suite and the multiprocess suite.
A finding is recorded with its fix; nothing here is asserted without a code reference.

## 1. Ownership model

| Component | Owner | Rule |
| --- | --- | --- |
| ContainmentRuntime | callers of the library, or the coordinator service | one instance per process incarnation |
| RuntimeState | ContainmentRuntime | all access serialised by a single std::mutex |
| DurableStore | one RuntimeState | single writer; additionally protected by the store incarnation lock |
| CoordinatorServer::Impl | the server process | session table guarded by sessions_lock, thread table by threads_lock |
| Socket | the session thread that owns it, plus the server for shutdown | handle is closed with an atomic exchange, never twice |

## 2. Locks and ordering

* RuntimeState::lock is the innermost lock. It is never held while calling into the store's
  filesystem helpers beyond the documented append/flush sequence, and never held across a
  socket operation.
* Session threads take sessions_lock (to register or remove their socket) and never hold it
  while calling the runtime. The runtime lock is never taken while sessions_lock is held.
  Ordering is therefore sessions_lock then runtime lock, and never the reverse.
* threads_lock guards only the thread table and is never held while joining.
* No lock is held while invoking anything that can re-enter FCFN: FCFN has no callback API,
  no user-provided function is ever called, and observers poll for results instead.

## 3. Findings

**F1 - session table mutated while serve() iterates it (fixed).**
Session threads pushed into a shared std::vector<std::thread> that serve() iterated when
joining. A join loop concurrent with push_back is undefined behaviour.
Fix: the thread table has its own mutex; finished threads are reaped by the accept loop and
joined outside the lock; request_shutdown swaps the table out under the lock and joins the
local copy afterwards.

**F2 - request_shutdown could join the calling thread (fixed).**
The Shutdown operation runs on a session thread. Joining every thread in the table would have
deadlocked (or thrown) when it reached itself.
Fix: request_shutdown compares thread ids, detaches its own thread, and joins the rest.

**F3 - joining while holding the session lock (fixed).**
An earlier design joined session threads while holding sessions_lock, which deadlocks as soon
as a session thread tries to remove its socket under the same lock.
Fix: sockets are snapshotted under the lock, shut down outside it, and threads are joined with
no lock held.

**F4 - blocked accept() and read() after shutdown (fixed).**
Shutdown must release threads blocked in accept() and recv() without a timeout.
Fix: Listener::shutdown and Socket::shutdown exchange the handle to an invalid value with
std::atomic::exchange and then close it, so a blocked call returns immediately and a second
close is impossible. Verified by the multiprocess suite (server shutdown while sessions are
idle) and the concurrency suite.

**F5 - second incarnation opening a live store (fixed).**
Two coordinators on one store directory would interleave a single write-ahead log.
Fix: DurableStore acquires an incarnation lock (root/LOCK) holding the owning pid; a live owner
refuses the second open with ALREADY_EXISTS, a dead owner is taken over. Proven by a
multiprocess test that starts a second coordinator against a live store.

**F6 - forced-effect state derived from a stale table (fixed).**
recompute_effect_state() iterates the retained attempt table and could see an evicted attempt.
Fix: the newest attempt is never evicted, and the derivation is guarded by an explicit
reverification flag set at startup.

**F7 - read-modify-write on the effect state across calls (fixed by construction).**
Effect transitions (submit, acknowledge, verify) each take the runtime lock once, validating
and mutating inside a single critical section, so no interleaving can produce an acknowledgement
for an attempt that was fenced in between.

**F8 - moved-from handles (reviewed, no defect).**
Socket and Listener move constructors transfer a shared_ptr and leave the source invalid;
every entry point checks validity. Result<T> moves its variant; no moved-from value is read.

## 4. Explicit non-findings

* No read-lock then write-lock re-entry: FCFN uses plain std::mutex only, never a recursive or
  shared mutex.
* No lock held across a helper that re-enters state: the only nested call is runtime ->
  DurableStore, which is a leaf and does not call back.
* No callbacks retaining references to mutable state: there are no callbacks.
* No cross-object mutex order inversion: the only two-lock path is documented in section 2 and
  is taken in one direction only.
* No double close: handles are closed through a single atomic exchange.

## 5. How this is proven

* tests/concurrency uses deterministic latches and barriers (no sleeps) to interleave session
  handling, runtime calls, and shutdown.
* tests/multiprocess runs real coordinator and client processes, hard-kills the coordinator at
  three durable boundaries, and restarts it on the same store.
* The runtime never blocks on a peer without a socket event, and shutdown closes handles rather
  than waiting, so no test needs a timeout to terminate.
