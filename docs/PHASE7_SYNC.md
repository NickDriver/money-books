# Money Books — Phase 7: P2P sync (design)

> Status: **design / not yet implemented.** This is the working spec for Phase 7. It supersedes
> SPEC §13 in detail; SPEC §13 stays the one-paragraph summary. Decisions here graduate into the
> SPEC decision log (D28+) once implemented.

Phase 7 lets one user run the **same book on several of their devices** (laptop + desktop, later a
phone) and keep them in sync directly, device-to-device, with **no server holding the data**. It is
the first phase that links a **Rust** staticlib (`iroh-c-ffi`) — the engine, UI, and MCP server stay
pure C.

---

## 1. Goal & MVP

**MVP (Phase 7a):** two paired devices, same book, each able to make entries offline; after a sync
both books are byte-identical in their journals and produce identical reports. LAN first, then
NAT-traversed internet via iroh relays.

Explicitly **out of scope for 7a:** real-time collaboration, partial/selective sync, encrypted
multi-user sharing, conflict *resolution* UI (there are no destructive conflicts to resolve — see §4),
mobile. Those are 7b+.

**Non-negotiables carried from the engine design:**
- The journal stays **append-only & immutable** (D13). Sync never edits or deletes an entry.
- The **single-writer engine** (D8) still owns every local mutation. Sync is a *reader of remote
  entries that re-drives the same insert path* — it does not write SQL behind the engine's back.
- Money stays integer cents; double-entry invariant (Σ postings = 0) holds per entry, so any union
  of valid entries is still valid.

---

## 2. The core problem: today's chain forks under two writers

The schema is already "sync-ready" (D20: every syncable row carries `device_id, seq, lamport`, and
`journal_entry` adds `content_hash, prev_hash`). But the **semantics** of two of those fields are
still single-writer:

- **`seq`** comes from one book-global `next_seq` counter ([store.c:234](../src/store/store.c)).
- **`prev_hash`** is the book-global `last_hash` meta — every new entry chains off whatever was last
  written, regardless of device ([journal.c:78](../src/journal/journal.c),
  [journal.c:138](../src/journal/journal.c)).

So the journal is **one linear hash chain**. The instant device A and device B each append while
offline, both chain off the same `last_hash` → two entries claim the same predecessor → the chain is
no longer linear. It's a fork, and the global `seq` numbers collide.

**This is the expensive decision Phase 7 turns on.** The columns are right; the chaining rule must
change from *one chain per book* to **one chain per device**.

---

## 3. Data model: per-device log (the retrofit)

Reframe the journal as **N independent append-only logs, one per `device_id`**, unioned into one
table. Concretely:

| field | today | Phase 7 |
|---|---|---|
| `device_id` | this book's UUID | unchanged — identifies the originating device's log |
| `seq` | book-global counter | **per-device** monotonic (1,2,3… within a device_id) |
| `lamport` | book-global, +1 each entry | unchanged role: max(seen)+1; gives a cross-device total order |
| `content_hash` | sha256 of canonical entry | unchanged — globally unique entry identity |
| `prev_hash` | book-global `last_hash` | **content_hash of `(this device_id, seq-1)`** — chains within the device's own log |

Changes required, all narrow:
1. **`mb_store_next_stamp`** → take/return a **per-device** seq. Store `next_seq:<device_id>` and
   keep the single global `lamport`. (One-device books renumber identically, so existing books and
   tests are unaffected: there's exactly one device_id and seq stays 1,2,3…)
2. **journal append** → read `last_hash:<device_id>` instead of `last_hash` for `prev_hash`; write it
   back per device. The canonical hash input already includes `device_id` and `seq`, so the
   `content_hash` formula doesn't change.
3. **A genesis rule** per device: first entry from a device has `prev_hash = NULL` (already handled by
   the `has_prev` path).

Integrity after this: each device's log is an independently verifiable hash chain; cross-device trust
comes from `content_hash` being self-certifying. Tampering with any entry breaks its own device's
chain. (We verify chains on **receive** — §5.)

> **Migration:** existing single-device books need no data migration — re-keying `last_hash` →
> `last_hash:<device_id>` and `next_seq` → `next_seq:<device_id>` is a `book_meta` rename done once on
> open (schema `user_version` bump, v6). The journal rows are already correct. Verified against a real
> v5 book: `next_seq|33` → `next_seq:<dev>|33`, `book_id` backfilled, `lamport` preserved.

### 3.1 Two findings from building 7a-0

**`seq` is shared across *all* syncable tables, not just the journal.** Every syncable insert —
`account`, `item`, `counterparty`, `invoice`, `bill`, `payment`, `allocation`, *and* `journal_entry` —
draws from the one `mb_store_next_stamp` counter. So a device's log is **one interleaved op-stream**
across all those tables, ordered by its per-device `seq`. The per-device retrofit in `next_stamp`
therefore covers every table at once, and the **version vector spans all syncable tables**:
`{device_id → max(seq) over every syncable table}`. Sync ships *all* rows (any table) with
`seq > peer.vv[device]`. Only `journal_entry` additionally carries the hash chain.

**Non-journal rows are *mutable* — the immutable-union model does not cover them.** `invoice.status`
(DRAFT→OPEN→PARTIAL→PAID→VOID), `bill.status`, and `account.is_active` change *after* creation. A pure
"union immutable logs by `(device_id, seq)`" merge — correct for the append-only journal — silently
drops a status change that happens after the peer already holds the row. **This is the central open
problem for 7a-1.** Candidate policies (decide in 7a-1):
- **Derive, don't sync** (preferred where possible): the journal is the source of truth, so recompute
  the mutable state. `REVERSED` is already handled this way in 7a-0 (applying a REVERSAL flips its
  target; status is never trusted off the wire). Invoice/bill *paid* status is likewise derivable from
  the `allocation` ledger + journal.
- **Last-writer-wins by `lamport`** for genuinely independent mutable fields with no journal backing
  (e.g. `account.is_active`, an invoice memo edit): carry a per-row `updated_lamport`, keep the higher.

7a-1 will need an `apply_remote` for each syncable table; the journal's is the hard one (done) and sets
the verification pattern. The simpler tables reduce to "insert-if-absent + LWW on mutable columns."

---

## 4. Sync protocol: version vectors over the immutable log

Because entries are immutable and content-addressed, sync is a **set union of two append-only logs** —
no CRDT, no merge conflicts on the journal itself.

**Version vector (VV):** `{ device_id → max seq present locally }`. This is the complete description
of "what I have." Computed with one `SELECT device_id, MAX(seq) FROM journal_entry GROUP BY device_id`.

**Exchange (one round trip, symmetric):**
```
A → B : HELLO  { book_id, vv_A }
B → A : HELLO  { book_id, vv_B }           # both learn the gap in one round
A → B : ENTRIES for every (dev,seq) where seq > vv_B[dev]   # what B lacks
B → A : ENTRIES for every (dev,seq) where seq > vv_A[dev]   # what A lacks
both  : verify + insert, recompute lamport, ACK { new_vv }
```
- `book_id` is a stable per-book identifier (new `book_meta.book_id`, a UUID at creation) so we never
  cross-sync two different companies. Mismatch → refuse.
- An ENTRY message carries the full entry + its postings (the canonical serialization that produced
  `content_hash`), so the receiver can **recompute the hash and verify** before inserting.
- Entries are shipped **in `seq` order per device** so each device's chain links as it lands.

**Merge / insert (receiver side, through the engine, in one `BEGIN IMMEDIATE`):**
1. Verify `content_hash` recomputes from the payload; reject entry if not.
2. Verify `prev_hash` matches the `content_hash` of `(device_id, seq-1)` already held (or NULL at
   genesis); if the predecessor is missing, buffer until it arrives (we sent in order, so it won't).
3. Insert the journal_entry + postings verbatim (preserving `device_id, seq, lamport, hashes`). This
   is a **new engine entry point** `mb_journal_apply_remote()` — it does *not* mint a new id/seq/hash;
   it replays a foreign one. Still single-writer, still one transaction.
4. Bump local `lamport = max(local, entry.lamport)` so future local entries sort after merged ones.

**Deterministic order for reporting/replay:** sort by `(lamport, device_id, seq)`. Total, stable,
identical on every device after a full sync → identical reports (this is the property the
`tests/` reconciliation already checks, now across devices).

**Idempotent & resumable:** re-running sync with equal VVs ships nothing. A dropped connection just
leaves a smaller VV; the next sync resends the tail. No partial-entry state (transaction per entry or
per batch).

---

## 5. Transport: `iroh-c-ffi` (QUIC, dial-by-key)

`iroh-c-ffi` **v0.101.0** (2026-06-15, tracks iroh 1.x) gives us authenticated, NAT-traversed QUIC
streams keyed by node public key. We use **only** the transport — not iroh-blobs/gossip/docs (Rust
only). Confirmed surface from `irohnet.h`:

```c
Endpoint_t *endpoint_default(void);
EndpointResult_t endpoint_bind(EndpointConfig_t const*, SocketAddrV4_t*, SocketAddrV6_t*, Endpoint_t* const*);
EndpointResult_t endpoint_addr(Endpoint_t* const*, EndpointAddr_t* out);        // our dialable addr
char *public_key_as_base32(PublicKey_t const*);                                 // share this to pair
EndpointResult_t endpoint_connect(Endpoint_t* const*, slice_ref_uint8_t alpn, EndpointAddr_t, Connection_t* const*);
EndpointResult_t endpoint_accept (Endpoint_t* const*, slice_ref_uint8_t alpn,  Connection_t* const*);
EndpointResult_t connection_open_bi  (Connection_t* const*, SendStream_t**, RecvStream_t**);
EndpointResult_t connection_accept_bi(Connection_t* const*, SendStream_t**, RecvStream_t**);
EndpointResult_t send_stream_write(SendStream_t**, slice_ref_uint8_t);
int64_t          recv_stream_read (RecvStream_t**, slice_mut_uint8_t);
void endpoint_free(...); void connection_close(...); void send_stream_free(...); void recv_stream_free(...);
```

- **ALPN:** `"money-books/sync/1"` — version the wire protocol in the ALPN so future revisions
  negotiate cleanly.
- **Identity:** the iroh node keypair is the **device identity**. Persist the iroh `SecretKey` in the
  per-book/registry config (NOT in the journal). The node's base32 public key is what users exchange
  to pair. (Open question §11: tie `device_id` to the iroh pubkey, or keep the existing UUID and map.)
- **Wire framing:** length-prefixed JSON messages (reuse our existing cJSON) over one bidi stream:
  `u32 len + bytes`. Each HELLO/ENTRIES/ACK is one frame. Simple, debuggable, transport-agnostic.

**Transport is abstracted** behind a tiny C interface (`mb_sync_transport`) with `connect/accept/
send_frame/recv_frame/close`. iroh is one implementation; a **loopback in-memory transport** is the
other (drives the whole protocol in `tests/` with zero network, zero Rust). The msquic fallback
(RESEARCH §5) would be a third impl if we ever drop Rust.

---

## 6. Build integration (introduces Rust)

- `iroh-c-ffi` builds to a **staticlib** via `cargo build --release` + `cargo run --features headers`
  to regenerate `irohnet.h`. Vendor a pinned commit/tag under `src/vendor/iroh-c-ffi/` (we already
  vendor C deps); commit the generated header so a Rust-less `make` of non-sync targets still works.
- **Makefile (Mac, now):** a `make sync-lib` target shells out to cargo, producing
  `build/libiroh_c_ffi.a`; the `app` and a new `synctest` target link it + `-framework Security
  -framework CoreFoundation` (QUIC/TLS needs them back). Pure-C targets (`mcp`, plain `test`) stay
  Rust-free.
- **CMake (Phase 6, D27):** the cross-platform move folds `make sync-lib` into a CMake
  `ExternalProject`/`corrosion` step. Per-OS system libs differ (Security.framework on macOS,
  bcrypt/ncrypt on Windows, none extra on Linux) — that's the only OS seam sync adds.
- Rust toolchain becomes a **build prerequisite for the desktop app only**; documented in README and
  gated so contributors not touching sync don't need it.

---

## 7. Module structure

New `src/sync/`:
- `sync.h` / `sync.c` — orchestration: compute VV, run the HELLO/ENTRIES/ACK exchange, drive
  `mb_journal_apply_remote`. Transport-agnostic (takes an `mb_sync_transport*`).
- `transport.h` — the `mb_sync_transport` vtable.
- `transport_iroh.c` — iroh-c-ffi implementation (the only file that includes `irohnet.h`; the only
  Rust-linked C). `#ifdef MB_WITH_SYNC` so the engine builds without it.
- `transport_loopback.c` — in-memory pipe transport for tests.

New engine entry point in `src/journal/`: `mb_journal_apply_remote(store*, const mb_remote_entry*)`
— verify + insert a foreign entry (§4). This is the *only* new way rows enter the journal, and it's
as guarded as the local path.

Wiring: a new `app.sync_*` shell method (start/stop endpoint, list/add peers, trigger sync, status)
and, later, MCP tools are **out of scope for 7a** (sync is device-operator action, not an LLM action —
keep it off the MCP surface for now).

---

## 8. Pairing & identity UX (7a, minimal)

1. Device A: **Settings → Sync → Enable** → app binds an iroh endpoint, shows A's base32 node key +
   a short pairing code.
2. Device B: **Add peer** → paste A's node key (+ confirm the book_id/name) → B dials A over the
   `money-books/sync/1` ALPN.
3. First connection: **mutual confirmation** on both screens (show the peer's key fingerprint + device
   label) before any entries flow — TOFU (trust-on-first-use), pin the key in the registry.
4. Thereafter: **Sync now** button (and later, auto-sync on connect). Status line: last-synced, VV
   gap, peer online/offline.

Peers are stored in the **app registry** (books.json / a new `peers` table), not the journal — peer
membership isn't accounting data.

---

## 9. VPS assist (optional, later)

iroh's public relays already give NAT traversal + hole-punching for free, so 7a needs **no server**.
A user-run VPS becomes useful only for: (a) an **always-on rendezvous** so two sometimes-offline
devices still converge (store-and-forward of ENTRIES frames — it never needs plaintext if we add
e2e encryption), and (b) self-hosting a relay instead of n0's. Defined as **Phase 7b**, opt-in,
never required.

---

## 10. Security model

- **Transport:** QUIC/TLS with peer authentication by node public key — every byte is encrypted and
  the peer is cryptographically identified by iroh.
- **Authorization:** TOFU pinning (§8) — only explicitly-paired keys may sync a given book. Unknown
  ALPN/keys are dropped at accept.
- **Integrity:** every received entry is hash-verified and chain-checked before insert (§4–5) — a
  malicious/buggy peer cannot inject a tampered or unbalanced entry (the engine re-validates Σ=0 too).
- **Privacy:** data flows **only** to paired devices; no third party (n0 relays only forward encrypted
  QUIC and learn metadata, not contents). Aligns with "engine makes no outbound network calls" — sync
  is the one explicit, user-initiated exception, clearly surfaced.
- **Threats deferred to 7b:** a compromised paired device (can inject valid-but-bogus entries — needs
  per-device revocation), at-rest encryption of the relay store.

---

## 11. Open decisions (need a call before/while building)

- **D-a `device_id` ↔ iroh key.** Make the iroh node public key *be* the `device_id` (one identity,
  simpler trust), or keep the existing UUID `device_id` and store the iroh key alongside? Leaning:
  **keep UUID, map to key** — `device_id` is already embedded in every historical hash; rebinding it
  would rewrite identity. (Recommended.)
- **D-b seq retrofit timing.** Do the per-device `seq`/`prev_hash` re-keying (§3) as a standalone
  prep commit (with its own tests, no networking) *before* any transport work? Leaning **yes** — it's
  the riskiest change and fully testable in isolation. (Recommended first step.)
- **D-c wire format.** Length-prefixed **JSON** (reuse cJSON, debuggable) vs a compact binary framing.
  Leaning **JSON** for 7a; revisit if entry volume makes it slow.
- **D-d auto-sync vs manual.** 7a ships a manual "Sync now"; auto-sync-on-connect in 7b.

---

## 12. Implementation order (each step shippable & tested)

1. **7a-0 Prep (no network) — ✅ DONE.** per-device `seq` (`mb_store_next_stamp`, covers all tables) +
   per-device hash chain (`last_hash:<device_id>`) + `book_id` + migration v6 + `mb_journal_apply_remote`
   (verify hash, chain, balance, accounts; idempotent; derives REVERSED; bumps lamport). Tests:
   `store.seq_is_per_device`, `store.has_book_id`, `journal.per_device_log`,
   `journal.apply_remote_idempotent_and_verified`, `journal.apply_remote_chain`. 92/92 green, 0 leaks,
   v5→v6 migration verified on a real book. **Surfaced the mutable-row problem — see §3.1.**
2. **7a-1 Protocol over loopback:** VV computation, HELLO/ENTRIES/ACK, merge. Full sync test using
   `transport_loopback` — two in-memory stores converge to identical reports. **Zero Rust.**
3. **7a-2 iroh transport:** vendor + pin iroh-c-ffi, `make sync-lib`, `transport_iroh.c`, LAN sync of
   two real processes. Integration test behind `MB_WITH_SYNC`.
4. **7a-3 UI/pairing:** Settings → Sync, peer registry, TOFU confirm, Sync-now + status.
5. **7b:** auto-sync, VPS rendezvous, mobile, revocation, at-rest relay encryption.

Per the project rule, **every new MCP tool gets an integration test** — sync adds no MCP tools in 7a,
but the loopback sync test is the equivalent gate for this subsystem.
