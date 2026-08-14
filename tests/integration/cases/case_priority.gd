extends "res://test_case.gd"
## Priority-ordered overflow (`gn_priority`) and the per-peer bandwidth budget.
##
## When more entities change than the budget covers, `_server_tick` sorts the candidates by
## (ticks waited) x (gn_priority) and writes the highest first, so a game can say "the
## player matters more than the corpse" without goldnet knowing what either of those is.
##
## Two things have to hold at once, and they pull against each other:
##
##   1. The weight actually biases the budget — a weighted entity updates far more often
##      than an unweighted one. Without this, gn_priority is decorative.
##   2. The weight does NOT starve anyone permanently — every unweighted entity still gets
##      its turn, because the ticks-waited factor grows until it outranks the weight. This
##      is the half a naive "sort by priority" implementation gets wrong, and it fails
##      silently: the game looks fine until the one entity you didn't look at is frozen.
##
## Budget pressure is created rather than waited for: `bandwidth_bps` is set low enough that
## only ~3 entities fit per snapshot, so 21 entities changing every frame overflow it
## permanently. That also exercises the rate budget itself, including that a tight one
## still makes forward progress instead of stalling the stream.
##
## Timeline (server drives state, client asserts):
##
##   t=0.1        spawn 1 weighted + FILLERS unweighted entities
##   t<COUNT_FROM every entity moves every frame; stream settles, all seen at least once
##   t=3..7       client counts how many distinct values it receives per entity
##   t≥JUDGE_AT   weighted entity updated >= RATIO x the busiest filler   → assertion 1
##                every filler updated at least once                     → assertion 2
##
## SCOPE: the ratio assertion is deliberately loose (>= 3x against an expected ~8x). The
## exact split depends on how many entities happen to fit in a packet, which depends on the
## encoded width of a Vector3 — pinning it tighter would make this a test of the encoder's
## byte count, which `quant` and the codec unit tests already own.

## The weighted entity is Ent0; everything else is filler.
const FILLERS := 20
const PRIORITY := 50.0

## ~39 B of entity body per snapshot at a 33 ms tick — room for about three entities.
const BANDWIDTH_BPS := 1200

const COUNT_FROM := 3.0
const COUNT_TO := 7.0
const JUDGE_AT := 7.0
## Expected separation is ~8x (the weighted entity every tick; each filler every ~10).
const RATIO := 3

var _seen := {}    # entity name -> last position observed
var _counts := {}  # entity name -> distinct values observed in the counting window
## Whether _seen has been primed with the positions entities already held when the window
## opened. Load-bearing: counting "position differs from what I recorded" against an EMPTY
## _seen scores every entity a free update on the window's first frame, which makes the
## no-starvation assertion below vacuous — it can never see a zero. Caught by mutation
## (score by weight alone starves the fillers, and this case passed anyway until priming).
var _primed := false


func make_entity(data: Variant) -> Node:
	var n: Node = super.make_entity(data)
	if int(data) == 0:
		# Read once, on this entity's first tick — set it before the sync enters the tree.
		n.get_node("Sync").set_meta("gn_priority", PRIORITY)
	return n


func setup(is_server: bool) -> void:
	timeout_s = 20.0
	if is_server:
		# Squeeze the entity-delta body so overflow is the steady state, not a rare burst.
		goldnet().set_bandwidth_bps(BANDWIDTH_BPS)


func server_step(t: float) -> void:
	if not spawn_once(t, FILLERS + 1):
		return
	var ents: Dictionary = main.entities()
	# Everything changes every frame, so the budget is always oversubscribed and the sort
	# is always deciding who gets through.
	for i in FILLERS + 1:
		var e: Node3D = ents.get("Ent%d" % i)
		if e != null:
			e.position = Vector3(float(i) + 1.0, 0.0, t * 10.0)


func client_step(t: float) -> void:
	var ents: Dictionary = main.entities()

	if t >= COUNT_FROM and t < COUNT_TO:
		if not _primed:
			for name in ents:
				_seen[name] = ents[name].position
			_primed = true
		else:
			for name in ents:
				var e: Node3D = ents[name]
				if _seen.get(name) != e.position:
					_seen[name] = e.position
					_counts[name] = int(_counts.get(name, 0)) + 1

	if t < JUDGE_AT:
		return

	if not check_eq(ents.size(), FILLERS + 1, "all %d entities arrived" % (FILLERS + 1)):
		finish()
		return

	var weighted := int(_counts.get("Ent0", 0))
	var busiest_filler := 0
	var idle_fillers := 0
	for i in range(1, FILLERS + 1):
		var c := int(_counts.get("Ent%d" % i, 0))
		busiest_filler = maxi(busiest_filler, c)
		if c == 0:
			idle_fillers += 1
	print("[client] weighted=%d busiest filler=%d idle fillers=%d"
		% [weighted, busiest_filler, idle_fillers])

	# 1. The weight biases the budget...
	check(weighted >= busiest_filler * RATIO,
		"gn_priority biased the budget (weighted %d >= %dx busiest filler %d — if this fails "
		% [weighted, RATIO, busiest_filler] + "the weight isn't reaching the overflow sort)")
	# ...2. but the ticks-waited factor still gets everyone a turn. A weighted entity that
	# starved the rest would sail through assertion 1 and fail here, which is the point of
	# asserting both.
	check_eq(idle_fillers, 0,
		"no filler was starved by the weighted entity (%d of %d never updated)"
			% [idle_fillers, FILLERS])
	finish()
