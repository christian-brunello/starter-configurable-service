# TODO — known issues / follow-ups

Snapshot after feedback / MANUAL_OVERRIDE (variant B) + override-timeout.
Date context: 2026-09-17.

## Done recently

- [x] Remove `<feedback>` / `<manual-override-timeout>` / OVERRIDE wiring
- [x] Writer listens to `write-complete` / `write-error`
- [x] Retry same bytes up to `<write-attempts>` (default 1); no retry on `CANCELLED`
- [x] HTTP poll async + cron; Ubuntu 20.04 / libsoup2 compat
- [x] Config: transport exclusive to one reader **or** one writer; warn if unbound

## High priority

- [ ] **HTTP write: cancellable + epoch** (mirror poll). Today write passes `NULL` cancellable; dispose/reconfigure does not abort in-flight writes cleanly.
- [ ] **Async (or non-blocking) login on write path.** `do_login` / `ensure_auth` and 401 relogin are synchronous and can block the main loop.
- [ ] **Retry backoff / delay.** Immediate re-enqueue can hammer a failing endpoint; file write can stack N sync attempts on the main loop.

## Medium priority

- [x] Feedback bind role=feedback + read-sync + write ack + MANUAL_OVERRIDE + override-timeout
- [ ] **Transport dispose vs inflight write.** Pending gets `CANCELLED`; inflight HTTP is not cancelled; `finish_*` may still run on a disposed object.
- [ ] **Remove or use orphaned API:** `stcs_transport_is_syncing()`, `stcs_transport_get_write_failed()` (no consumers after feedback removal).
- [ ] **json-writer coalesce/debounce.** Each mapped key change flushes the whole document → N writes; depth-1 drops intermediates (`CANCELLED`).
- [ ] **Clarify transforms on output path.** `datapoint_push` always runs transforms; may distort commanded values if transforms are read-oriented.
- [ ] **Boot: runlevel failure handling.** Failed `runlevel_up` only logs; tick retries forever (no backoff / exit).
- [ ] **Clean shutdown.** Signal → runlevel down → drain/cancel in-flight writes.
- [ ] **Example + smoke test** for output → writer → transport write + `write-attempts`.

## Low / optional

- [ ] Double equality (`gdouble ==`) for “unchanged” datapoint emit skip
- [ ] After N write failures: only log today; optional signal/property for “misaligned” toward Starter
- [ ] Drop unused `handler_data` on `STCSBinding` if still unused
- [ ] Document `<write-attempts>` in README / example XML

## Design notes (current model)

```text
Command:
  STOutput val-changed → command datapoint → writer → transport write
  write-ok / write-error → writer ack hook → output write_state (idle/failed)
  (pending skips MANUAL_OVERRIDE on feedback mismatch)

Feedback (role=feedback):
  reader parse begin/end → syncing on linked outputs
  reader → fb datapoint → under sync: align STOutput
  runlevel < 3 (pre-STServer): startup align, no MANUAL_OVERRIDE
  runlevel >= 3 + idle mismatch: MANUAL_OVERRIDE + set_val + override-timeout
  pending / settle-delay after command: ignore feedback mismatch
  failed mismatch: align to device, clear failed, no override
```

- `<override-timeout>` on `<output>`: bare ms, or `60s` / `5m` / `1h` / `1000ms`; `0`/absent = never auto-clear.
- `settle-delay` on feedback `<bind>`: default `5s`; `0` disables; armed on each command write.
- Transport: queue depth 1; newer write replaces pending (`CANCELLED`).
- Config: `<write-attempts>` under `<writer>` (total attempts, min 1).
- Config: each transport binds to at most one reader or one writer (not both, not shared).
