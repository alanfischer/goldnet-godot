extends "res://suite.gd"
## SyncSpec — the one-table declaration a config and goldnet's metas are both derived from.
##
## Two things are worth pinning here. The derivation itself: the metas are keyed by LEAF name
## while the config holds full paths, and that translation is the only reason the two lists could
## ever disagree. And validate(), which exists because every way of getting the metas wrong is
## silent at runtime — goldnet ignores an unrecognised hint name and a leaf that matches no slot,
## so the symptom is a hint that simply never applied, with nothing logged.
##
## The extension is not involved: these are the structures goldnet would read, checked directly.

const Spec := preload("res://addons/goldnet/sync_spec.gd")

const DECL := {
	"NetInterp:net_pos": {"quant": "vec3_half", "push": true},
	"NetInterp:net_stamp": {"quant": "time_delta", "push": true},
	"NetInterp:net_anim": {},
	".:is_ridden": {"mode": SceneReplicationConfig.REPLICATION_MODE_ON_CHANGE},
}


func run() -> void:
	_test_config_properties()
	_test_config_modes()
	_test_spawn_flag_only_when_asked()
	_test_metas_are_leaf_keyed()
	_test_build_stamps_everything()
	_test_apply_on_a_baked_config()
	_test_valid_table_has_no_problems()
	_test_unknown_quant_hint()
	_test_unknown_option_key()
	_test_bad_path_and_bad_value()
	_test_leaf_collision()
	_test_config_drift_both_ways()
	_test_unknown_sync_option()


# --- derivation ---

func _test_config_properties() -> void:
	var cfg := Spec.config(DECL)
	var props := cfg.get_properties()
	check_eq(props.size(), 4, "one config property per declared entry")
	for key in DECL:
		check(cfg.has_property(NodePath(key)), "config replicates %s" % key)


func _test_config_modes() -> void:
	var cfg := Spec.config(DECL)
	# ALWAYS is the default a table gets by saying nothing, and it is the mode a late-visible
	# entity needs to receive its baseline at all.
	check_eq(cfg.property_get_replication_mode(^"NetInterp:net_pos"),
		SceneReplicationConfig.REPLICATION_MODE_ALWAYS, "an undeclared mode is ALWAYS")
	check_eq(cfg.property_get_replication_mode(^".:is_ridden"),
		SceneReplicationConfig.REPLICATION_MODE_ON_CHANGE, "a declared mode is honored")


func _test_spawn_flag_only_when_asked() -> void:
	# The engine's own default is left alone unless the table says otherwise, so adopting SyncSpec
	# cannot change what an existing config spawn-replicates.
	var raw := SceneReplicationConfig.new()
	raw.add_property(^"NetInterp:net_pos")
	var untouched := Spec.config({"NetInterp:net_pos": {}})
	check_eq(untouched.property_get_spawn(^"NetInterp:net_pos"),
		raw.property_get_spawn(^"NetInterp:net_pos"),
		"a table that says nothing leaves the engine's spawn default")
	var off := Spec.config({"NetInterp:net_pos": {"spawn": false}})
	var on := Spec.config({"NetInterp:net_pos": {"spawn": true}})
	check_eq(off.property_get_spawn(^"NetInterp:net_pos"), false, "spawn:false is honored")
	check_eq(on.property_get_spawn(^"NetInterp:net_pos"), true, "spawn:true is honored")


func _test_metas_are_leaf_keyed() -> void:
	# goldnet's _read_quant/_read_push match a slot by its last subname, never the full path.
	var q := Spec.quant_hints(DECL)
	check_eq(q.size(), 2, "only the properties with a hint appear in gn_quant")
	check_eq(q.get("net_pos"), "vec3_half", "gn_quant is keyed by leaf name")
	check_eq(q.get("net_stamp"), "time_delta", "every hinted leaf is present")
	var p := Spec.pushed(DECL)
	check_eq(p.size(), 2, "only push:true properties are declared")
	check("net_pos" in p and "net_stamp" in p, "gn_push holds leaf names")
	check(not ("net_anim" in p), "a property that declares nothing is polled, not pushed")


# --- build / apply ---

func _test_build_stamps_everything() -> void:
	var sync: MultiplayerSynchronizer = Spec.build("NetSync", DECL, {"priority": 4.0})
	check_eq(sync.name, &"NetSync", "the synchronizer is named")
	check_eq(sync.replication_config.get_properties().size(), 4, "it carries the derived config")
	check_eq(sync.get_meta("gn_quant"), Spec.quant_hints(DECL), "gn_quant is stamped")
	check_eq(sync.get_meta("gn_push"), Spec.pushed(DECL), "gn_push is stamped")
	check_eq(sync.get_meta("gn_priority"), 4.0, "gn_priority is stamped when asked for")
	var plain: MultiplayerSynchronizer = Spec.build("NetSync", DECL)
	check(not plain.has_meta("gn_priority"), "and absent when not")
	# Empty rather than absent: goldnet reads them the same, and a sync that always carries both
	# is self-describing.
	var bare: MultiplayerSynchronizer = Spec.build("NetSync", {"NetInterp:net_anim": {}})
	check_eq(bare.get_meta("gn_quant"), {}, "a table with no hints still stamps gn_quant")
	check_eq(bare.get_meta("gn_push"), [], "a table with no promises still stamps gn_push")
	sync.free()
	plain.free()
	bare.free()


func _test_apply_on_a_baked_config() -> void:
	# The .tscn case: the scene owns the config, apply() only stamps the metas onto it.
	var baked := MultiplayerSynchronizer.new()
	baked.name = "NetSync"
	baked.replication_config = Spec.config(DECL)
	var cfg := baked.replication_config
	Spec.apply(baked, DECL)
	check(baked.replication_config == cfg, "apply() does not replace the baked config")
	check_eq(baked.get_meta("gn_push"), Spec.pushed(DECL), "apply() stamps the metas")
	baked.free()


# --- validation ---

func _test_valid_table_has_no_problems() -> void:
	check_eq(Spec.validate(DECL).size(), 0, "a sound table reports nothing")
	check_eq(Spec.validate(DECL, Spec.config(DECL), {"priority": 2.0}).size(), 0,
		"and nothing against the config it built")


func _test_unknown_quant_hint() -> void:
	# The failure this catches: goldnet's gn_quant_from_name falls back to AUTO for a name it
	# does not know, so a typo means full precision on the wire and no complaint anywhere.
	var problems := Spec.validate({"NetInterp:net_pos": {"quant": "vec3half"}})
	check_eq(problems.size(), 1, "an unrecognised hint is one problem")
	check("vec3half" in problems[0], "the problem quotes the bad hint")


func _test_unknown_option_key() -> void:
	var problems := Spec.validate({"NetInterp:net_pos": {"quantize": "vec3_half"}})
	check_eq(problems.size(), 1, "a misspelled option key is caught, not ignored")
	check("quantize" in problems[0], "the problem names the key")


func _test_bad_path_and_bad_value() -> void:
	check_eq(Spec.validate({"NetInterp": {}}).size(), 1, "a path with no property is a problem")
	var problems := Spec.validate({"NetInterp:net_pos": "vec3_half"})
	check_eq(problems.size(), 1, "a bare hint where options belong is a problem")
	check_eq(Spec.validate({"NetInterp:net_pos": {"push": "yes"}}).size(), 1, "push must be a bool")
	check_eq(Spec.validate({"NetInterp:net_pos": {"mode": 7}}).size(), 1, "mode must be a real mode")


func _test_leaf_collision() -> void:
	# Two paths, one leaf: the metas cannot tell them apart, so a hint on either covers both.
	var problems := Spec.validate({
		"A:net_pos": {"quant": "vec3_half"},
		"B:net_pos": {},
	})
	check_eq(problems.size(), 1, "a hinted leaf collision is reported")
	check("net_pos" in problems[0], "the problem names the shared leaf")
	check_eq(Spec.validate({"A:net_pos": {}, "B:net_pos": {}}).size(), 0,
		"the same collision with no hints on either side is harmless")


func _test_config_drift_both_ways() -> void:
	var cfg := Spec.config(DECL)
	var short_table := {"NetInterp:net_pos": {"quant": "vec3_half", "push": true}}
	check_eq(Spec.validate(short_table, cfg).size(), 3,
		"every config property the table forgot is reported")
	var long_table := DECL.duplicate()
	long_table["NetInterp:net_health"] = {"push": true}
	var problems := Spec.validate(long_table, cfg)
	check_eq(problems.size(), 1, "a table entry the config lacks is reported")
	check("net_health" in problems[0], "the problem names the orphan")


func _test_unknown_sync_option() -> void:
	var problems := Spec.validate(DECL, null, {"prioritiy": 2.0})
	check_eq(problems.size(), 1, "a misspelled sync option is caught")
	check("prioritiy" in problems[0], "the problem names it")
