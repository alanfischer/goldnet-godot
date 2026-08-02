extends Node3D
## Replicated entity for case_replication_modes: one property per Godot replication mode.
##
## Not a case itself (run.sh only globs `case_*.gd`) — it's the fixture the case builds its
## entities from. A real script rather than an inline assembly because goldnet reads slot
## values through `get_indexed`, so the properties have to genuinely exist on the node.
##
##   always_val   REPLICATION_MODE_ALWAYS      property_get_sync()  == true
##   change_val   REPLICATION_MODE_ON_CHANGE   property_get_watch() == true
##   spawn_val    REPLICATION_MODE_NEVER       neither: spawn-only, must NOT stream
##
## The three flags are what goldnet dispatches on, and Godot sets exactly one of
## sync/watch per mode — hence a fixture that carries all three at once.

var always_val: int = 0
var change_val: int = 0
var spawn_val: int = 0
