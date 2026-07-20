extends RefCounted
## Base for the GDScript helper suites.
##
## Deliberately no `class_name`, for the same reason tests/integration/test_case.gd has
## none: global class names resolve from a cache the editor writes during an import pass,
## which a bare `--headless --path` run doesn't perform. Suites `extends "res://suite.gd"`
## by path, so this runs from a clean checkout with no editor step.
##
## The classes under test declare `class_name` (they're a public API), so they must be
## loaded BY PATH here too — `preload("res://addons/goldnet/...")` rather than the bare
## global identifier, which wouldn't resolve.
##
## Suites override `run()` and call the assertions below. Assertions accumulate rather than
## abort, so one run reports everything wrong instead of only the first failure.

var _failures: Array[String] = []
var _checks: int = 0


## Override with the suite's tests.
func run() -> void:
	pass


## Human-readable suite name, used in the report. Defaults to the script's filename.
func suite_name() -> String:
	return (get_script() as GDScript).resource_path.get_file().get_basename()


# --- assertions ---

func check(cond: bool, msg: String) -> bool:
	_checks += 1
	if not cond:
		_failures.append(msg)
	return cond


func check_eq(got: Variant, want: Variant, msg: String) -> bool:
	return check(got == want, "%s (got %s, want %s)" % [msg, got, want])


## Float comparison with an absolute tolerance.
func check_near(got: float, want: float, tol: float, msg: String) -> bool:
	return check(absf(got - want) <= tol,
		"%s (got %f, want %f, tol %f)" % [msg, got, want, tol])


## Vector3 comparison with an absolute tolerance on the distance.
func check_near_v(got: Vector3, want: Vector3, tol: float, msg: String) -> bool:
	return check(got.distance_to(want) <= tol,
		"%s (got %.4v, want %.4v, tol %f)" % [msg, got, want, tol])


func fail(msg: String) -> void:
	_checks += 1
	_failures.append(msg)


# --- harness plumbing ---

func failures() -> Array[String]:
	return _failures


func check_count() -> int:
	return _checks
