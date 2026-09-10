class_name SyncSpec
extends RefCounted

## One table saying what a MultiplayerSynchronizer replicates — the properties, and per property
## goldnet's per-slot hints — from which the SceneReplicationConfig and goldnet's metas are BOTH
## derived. The fourth addon helper, alongside InterpolationBuffer, PredictedBody and ServerClock,
## and like them pure GDScript: it needs no built extension and is inert without goldnet installed.
##
## Why it exists: goldnet's per-slot hints ride node metas (`gn_quant`, `gn_push`, `gn_priority`)
## keyed by a property's LEAF name, because SceneReplicationConfig has nowhere to put per-property
## metadata (see README, "What we wish Godot had" #5). Written by hand those metas are a second
## list to keep aligned with the config, and the failure is quiet in both directions: a name that
## matches no slot is ignored, so the hint simply never applies. Deriving both from one table
## removes the alignment problem rather than documenting it.
##
##   var sync := SyncSpec.build("NetSync", {
##       "NetInterp:net_pos":   {"quant": "vec3_half", "push": true},
##       "NetInterp:net_stamp": {"quant": "time_delta", "push": true},
##       "NetInterp:net_anim":  {},   # replicated, full precision, polled every tick
##   })
##   node.add_child(sync)
##
## Per-property options, all optional:
##   "quant"  String  lossy encoding hint — see QUANT_HINTS and the README's Usage section.
##   "push"   bool    the game promises to mark_dirty() every write to this property, so goldnet
##                    may skip reading it on a tick nobody marked. An undeclared property is
##                    polled every tick, which is why forgetting one costs a read, not a desync.
##   "mode"   int     SceneReplicationConfig.ReplicationMode. Defaults to ALWAYS, the engine's own
##                    default: goldnet skips a slot equal to the peer's acked baseline, so an idle
##                    ALWAYS slot is free, and ON_CHANGE never re-sends the baseline a peer needs
##                    when an entity becomes visible late.
##   "spawn"  bool    the config's spawn flag. Left at the engine's default when absent.
##
## Sync-wide options (the third argument):
##   "priority" float  goldnet's `gn_priority` entity weight for the snapshot budget.
##
## For a synchronizer BAKED into a .tscn — which stock SceneMultiplayer requires, since a
## runtime-built one gets no replication-ID handshake; goldnet pairs either, by scene path — the
## scene owns the config, so declare the same table and call apply() instead. It stamps the metas
## and reports any drift between the table and the config the scene actually shipped:
##
##   SyncSpec.apply($NetSync, SPEC)
##
## Everything here is static; the class is never instantiated.

## Quantization hints `gn_quant` understands. Anything else is a typo goldnet would silently
## ignore, so validate() reports it.
const QUANT_HINTS := ["angle8", "angle16", "half", "vec3_half", "time_delta"]

const _PROP_KEYS := ["quant", "push", "mode", "spawn"]
const _SPEC_KEYS := ["priority"]


## Build a synchronizer from the table: config, metas and name in one call.
static func build(sync_name: String, decl: Dictionary, opts: Dictionary = {}) -> MultiplayerSynchronizer:
	var sync := MultiplayerSynchronizer.new()
	sync.name = sync_name
	sync.replication_config = config(decl)
	apply(sync, decl, opts)
	return sync


## Stamp the goldnet metas onto an existing synchronizer whose config came from somewhere else
## (a .tscn), checking the table against that config first. Also the tail of build().
static func apply(sync: MultiplayerSynchronizer, decl: Dictionary, opts: Dictionary = {}) -> void:
	for problem in validate(decl, sync.replication_config, opts):
		push_error("SyncSpec (%s): %s" % [sync.name, problem])
	# Always set, even when empty: an empty gn_quant/gn_push means the same as an absent one
	# (all-auto / poll everything), and a sync that always carries both describes itself when
	# something is inspecting it.
	sync.set_meta("gn_quant", quant_hints(decl))
	sync.set_meta("gn_push", pushed(decl))
	if opts.has("priority"):
		sync.set_meta("gn_priority", float(opts["priority"]))


## The SceneReplicationConfig the table describes.
static func config(decl: Dictionary) -> SceneReplicationConfig:
	var cfg := SceneReplicationConfig.new()
	for key in decl:
		var path := NodePath(key)
		var o := _opts(decl, key)
		cfg.add_property(path)
		cfg.property_set_replication_mode(path,
			int(o.get("mode", SceneReplicationConfig.REPLICATION_MODE_ALWAYS)))
		if o.has("spawn"):
			cfg.property_set_spawn(path, bool(o["spawn"]))
	return cfg


## The `gn_quant` meta: leaf name -> hint, for the properties that declared one.
static func quant_hints(decl: Dictionary) -> Dictionary:
	var out := {}
	for key in decl:
		var o := _opts(decl, key)
		if o.has("quant"):
			out[_leaf(key)] = String(o["quant"])
	return out


## The `gn_push` meta: the leaf names the game promises to announce with mark_dirty().
static func pushed(decl: Dictionary) -> Array:
	var out := []
	for key in decl:
		if bool(_opts(decl, key).get("push", false)):
			out.append(_leaf(key))
	return out


## Everything wrong with a table, as human-readable lines — empty when it is sound. build() and
## apply() push_error each line; returning them rather than only logging keeps the checks
## testable, and lets a caller assert on its own declarations.
##
## Pass the config to also check the table against it, which is the whole point of apply(): the
## metas are keyed by leaf name and a name matching no slot is ignored in silence, so a table
## that has drifted from a baked config disables the hints it looks like it is setting.
static func validate(decl: Dictionary, cfg: SceneReplicationConfig = null,
		opts: Dictionary = {}) -> PackedStringArray:
	var problems := PackedStringArray()
	var leaf_owners := {}  # leaf -> first path that claimed it
	for key in decl:
		var path := NodePath(key)
		if path.get_subname_count() == 0:
			problems.append("'%s' names no property (a path is \"Child:prop\" or \".:prop\")" % key)
			continue
		if typeof(decl[key]) != TYPE_DICTIONARY:
			problems.append("'%s' maps to %s, not a Dictionary of options" % [key, type_string(typeof(decl[key]))])
			continue
		var o: Dictionary = decl[key]
		for k in o:
			if not (k in _PROP_KEYS):
				problems.append("'%s' has unknown option '%s' (want one of %s)" % [key, k, _PROP_KEYS])
		if o.has("quant") and not (String(o["quant"]) in QUANT_HINTS):
			problems.append("'%s' has unknown quant hint '%s' (want one of %s)"
				% [key, o["quant"], QUANT_HINTS])
		if o.has("push") and typeof(o["push"]) != TYPE_BOOL:
			problems.append("'%s' has non-bool push" % key)
		if o.has("mode") and not (int(o.get("mode", -1)) in [
				SceneReplicationConfig.REPLICATION_MODE_NEVER,
				SceneReplicationConfig.REPLICATION_MODE_ALWAYS,
				SceneReplicationConfig.REPLICATION_MODE_ON_CHANGE]):
			problems.append("'%s' has mode %s, not a SceneReplicationConfig.ReplicationMode" % [key, o["mode"]])
		# Two properties sharing a leaf cannot be hinted apart: one meta entry covers both. Only
		# a problem when a hint is actually involved — otherwise the leaf is never looked up.
		var leaf := _leaf(key)
		if leaf_owners.has(leaf):
			if o.has("quant") or o.has("push") or _opts(decl, leaf_owners[leaf]).has("quant") \
					or _opts(decl, leaf_owners[leaf]).has("push"):
				problems.append("'%s' and '%s' share the leaf name '%s', which the metas key by, so a hint on one applies to both"
					% [leaf_owners[leaf], key, leaf])
		else:
			leaf_owners[leaf] = key
	for k in opts:
		if not (k in _SPEC_KEYS):
			problems.append("unknown sync option '%s' (want one of %s)" % [k, _SPEC_KEYS])
	if cfg != null:
		var declared := {}
		for key in decl:
			declared[String(NodePath(key))] = true
		var in_config := {}
		for p in cfg.get_properties():
			in_config[String(p)] = true
			if not declared.has(String(p)):
				problems.append("config replicates '%s', which the table does not declare (its hints, if any, are ignored)" % p)
		for key in declared:
			if not in_config.has(key):
				problems.append("table declares '%s', which the config does not replicate" % key)
	return problems


static func _opts(decl: Dictionary, key: Variant) -> Dictionary:
	var v: Variant = decl.get(key)
	return v if typeof(v) == TYPE_DICTIONARY else {}


## The name the metas key by: the last subname of the property path.
static func _leaf(key: Variant) -> String:
	var path := NodePath(key)
	var n := path.get_subname_count()
	return String(path.get_subname(n - 1)) if n > 0 else String(key)
