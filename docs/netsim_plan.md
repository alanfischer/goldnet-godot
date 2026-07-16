# Network-condition simulation in goldnet — implementation plan

> Move latency / loss / spike simulation **out of the consuming game and into the
> transport**, where a reusable netcode module should own it. First consumer:
> WizardWars, which today carries a 185-line `net_latency_sim.gd` plus ~59 call-site
> wrappers that this change deletes.
>
> **Reordering is explicitly out of scope** (dropped). It was the only *receive-side*
> artifact; without it, all simulation is **send-side**, which is what makes this a net
> simplification rather than a relocation.

## 1. Goal

Give `GoldNetMultiplayer` a built-in, configurable network-condition simulator covering
**latency**, **packet loss**, and **WiFi latency spikes**, applied at the two send funnels
the transport already owns:

- `GoldNetMultiplayer::_rpc(peer, object, method, args)` — the single chokepoint every
  game `@rpc` call passes through (currently a pure pass-through: `return inner->rpc(...)`).
- The snapshot send path in `_server_tick` (which already runs `loss_percent` / `dbg_loss`).

WizardWars then deletes `net_latency_sim.gd`, drops the `sim` member and its ~59 `queue_*`
wrappers (each collapses to a direct call), and rewrites the `/sim_*` admin commands as thin
forwarders to the new goldnet config.

Non-goals:
- **No reordering.** Removed from the game today; not reimplemented.
- **No receive-side hook.** All simulation is send-side (see §4).
- No new behavior on the **stock** `SceneMultiplayer` transport — sim only exists when
  goldnet is installed (see §6, caveat 1).

## 2. Why send-side only is sufficient (and why dropping reorder unlocks it)

The game's current sim applies latency as *half on send + half on recv* and drops packets on
both send and recv. The recv side existed for exactly one reason: **reorder**, which had to
bypass the receive-ordering guard so a later packet could overtake an earlier one. Everything
else (latency, loss, spike) is expressible purely on the sender:

- **Latency** — delay the outgoing packet; the receiver runs its handler immediately on arrival.
  Applied once per leg at the sender, total RTT = client-send-latency + server-send-latency.
- **Loss** — drop the outgoing packet before it reaches the wire.
- **Spike** — a time-windowed latency burst on the sender.

So with reorder gone, goldnet needs **no** receive interception. Every WizardWars
`sim.queue_*_recv(func(): body)` wrapper — 45 of the 59 — becomes just `body`, because the
latency those wrappers used to add is now applied once at the *sender's* goldnet funnel.

## 3. goldnet side (C++) — the work

### 3.1 A `GoldNetSim` holder
Add a small struct/class member on `GoldNetMultiplayer` (or inline fields, matching the
existing `dbg_loss` style):

```
int   latency_min_ms   = 0;
int   latency_max_ms   = 0;
int   loss_percent      = 0;   // already exists as dbg_loss — fold in
int   spike_ms          = 0;
float spike_interval_s  = 10.0;
float spike_duration_s  = 0.2;
```

Plus a **send delay queue** (`Vector<PendingSend>` of `{ fire_at_ms, kind, payload }`) and the
spike state machine (`_spike_active`, `_spike_timer`, `_spike_elapsed`) — a direct C++ port of
`net_latency_sim.gd`'s `_update_spike` / `_get_half_delay_ms` (minus the `>> 1` half-split;
see §4) and its ordered `_delay_queue`. Preserve the **per-stream ordering cursors**
(`_last_unreliable_fire_at`, `_last_reliable_fire_at`) so packets released late still fire in
send order.

### 3.2 Pump the queue
goldnet already polls every frame (`_poll` → `inner->poll()`, ~line 1200). Drain the delay
queue there: fire every entry whose `fire_at_ms <= now`, advance the spike timer by the poll
delta. (goldnet must track its own frame delta or use `Time::get_ticks_msec()` deltas — the
GDScript version was driven by `sim.flush(delta)` from `_process`.)

### 3.3 Intercept `_rpc`
Replace the pass-through at `goldnet_multiplayer.cpp:1235`:

```
Error GoldNetMultiplayer::_rpc(peer, object, method, args) {
    TransferMode mode = _resolve_transfer_mode(object, method);   // see §4
    if (mode == UNRELIABLE && _sim_drops())        return OK;      // simulated loss
    int d = _sim_delay_ms();
    if (d <= 0) return inner->rpc(peer, object, method, args);
    _queue_send(d, /* deferred */ [peer,object,method,args]{ inner->rpc(...); });
    return OK;
}
```

Deferring an `inner->rpc` from the queue: store the four args in the `PendingSend` and replay
`inner->rpc(...)` when it fires. `_rpc` returns `OK` optimistically (the game can't observe the
few-ms deferral, same contract the GDScript wrapper had).

### 3.4 Fold loss + latency + spike into the snapshot send
`_server_tick` already gates snapshot emission on `dbg_loss` (line 954). Route that same send
through the delay queue for latency/spike, and keep the loss check. One code path for both
funnels.

### 3.5 Config surface (bind + env)
Bind properties mirroring `loss_percent`'s existing binding (lines 621–643):
`latency_min_ms`, `latency_max_ms`, `spike_ms`, `spike_interval_s`, `spike_duration_s`, plus a
`sim_reset()` method. Extend the existing `GOLDNET_LOSS` env-var read (line 227) with
`GOLDNET_LATENCY` (`min,max` or `fixed`) and `GOLDNET_SPIKE` (`ms,interval,duration`) so a
**headless server** can simulate its own send leg without an admin console (see §6, caveat 2).

## 4. Two design points to decide up front

**(a) Reliable-vs-unreliable detection for loss.** Loss must only drop **unreliable** RPCs —
dropping a reliable one before `inner->rpc` means ENet never retransmits it, corrupting state.
`_rpc` doesn't receive the transfer mode directly; it's in the sender's rpc config for the
method. `_resolve_transfer_mode` queries the node's rpc config (`get_rpc_config` /
`get_node_rpc_config` → the method's `transfer_mode`) once and caches per (object, method).
- *Recommended:* implement the lookup — it preserves today's ability to `/sim_loss`-drop the
  input-command stream, which is exactly what stress-tests the input-redundancy ring
  (`INPUT_REDUNDANCY`, `main.gd`).
- *Fallback if the lookup proves awkward:* apply loss **only** to the snapshot stream (already
  there) and to RPCs never — simpler, but `/sim_loss` no longer drops input RPCs (a testing
  regression). Latency/spike still apply to everything.

**(b) Latency is per-leg at the sender, not split send+recv.** Today `/sim_latency` on a single
client simulated *both* directions (half on its send, half on its recv). Send-side-only means a
client's config affects only client→server; the server→client leg needs the **server** endpoint
configured (via the `GOLDNET_*` env vars from §3.5, or an admin path). This is more realistic
(each leg's latency lives at its own sender) but changes the single-knob test ergonomics —
call it out in the `/sim_*` help text.

## 5. WizardWars side (GDScript) — the simplification

1. **Delete** `src/systems/net_latency_sim.gd` (185 lines).
2. **`network_manager.gd`:** remove `const NetLatencySim` (line 4), `var sim` (line 11),
   `sim.flush(delta)` (line 763), `sim.reset()` (line 307). Fix the stale CLI-flag comment
   (line 10) while here.
3. **Unwrap all 59 `sim.queue_*` call sites** → direct calls:
   - `sim.queue_reliable_recv(func(): X)` (37×), `sim.queue_unreliable_recv(func(): X)` (8×),
     `sim.queue_reliable_send(func(): X)` (12×), `sim.queue_unreliable_send(func(): X)` (2×)
     all become `X`. Single-line closures are a trivial substitution; the multi-line
     `func():`-block sites (e.g. 575, 849, 1061, 1175, 1228, 1345, 1533, 1651, 2069, …)
     unwrap to the block body at the same indentation. This also removes the per-snapshot
     closure allocation the earlier `/simplify` pass flagged in `_on_goldnet_server_time`.
4. **`client_command_system.gd`:** rewrite `/sim_latency`, `/sim_loss`, `/sim_spike` to set the
   goldnet properties on `multiplayer` (guarded by `NetworkManager.is_goldnet()`; reply
   "network simulation requires the goldnet transport" otherwise). **Delete `/sim_reorder`** and
   `net_latency_sim`'s reorder fields entirely.

## 6. Caveats (unchanged from the discussion, restated for the record)

1. **Stock transport loses all sim.** The interception points only exist under goldnet. If a
   consumer still tests netcode on stock `SceneMultiplayer`, that path goes dark. **Resolved for
   WizardWars: goldnet is the default transport**, so there is no stock path to keep sim on —
   this caveat does not apply here. (Still worth noting for other/future goldnet consumers that
   run stock.)
2. **Headless-server sim needs env vars.** No admin console on `--headless`; the `GOLDNET_*` env
   vars (§3.5) are how the server configures its own send leg.
3. **Complexity relocates, doesn't vanish.** The queue/spike/reliability logic still exists — in
   goldnet C++ instead of GDScript. Total LOC ~flat; *WizardWars* shrinks by ~185 lines + 59
   wrappers, and the sim becomes reusable by every goldnet consumer.
4. **C++ is less hackable than the GDScript original.** Tuning knobs move behind a rebuild.

## 7. Suggested sequencing

1. goldnet: latency + spike on both funnels, bound config + env vars, queue pump. (Loss already
   exists on snapshots.) Rebuild all dylib variants; full relaunch.
2. goldnet: `_resolve_transfer_mode` + loss on unreliable `_rpc` (design point 4a).
3. WizardWars: unwrap call sites, delete `net_latency_sim.gd`, rewrite `/sim_*`, drop reorder.
4. Verify (headless `--host` + a bot): `/sim_loss` still recovers via the input ring;
   `GOLDNET_LATENCY` on the server produces smooth interpolation on the client; spike exercises
   the jitter buffer. Bump the submodule pointer in the parent repo.

## 8. Verdict

Go. goldnet **is** WizardWars' default transport, so the one precondition is met and the
stock-path concern is moot. Dropping reorder is what keeps it clean: send-side-only, no receive
hook, 59 wrappers deleted. The remaining cost is the C++ port (design point 4a, transfer-mode
resolution) — bounded and worth it.
