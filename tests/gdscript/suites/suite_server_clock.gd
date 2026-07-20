extends "res://suite.gd"
## ServerClock — the client-side server-time estimator.
##
## Unlike the other two helpers this one reads the wall clock directly
## (`Time.get_ticks_msec()`), so tests can't hand it a synthetic timeline. Two consequences
## shape everything below:
##
##   * To feed a chosen candidate offset C, we sample `Time.get_ticks_msec() + C`. The
##     clock re-reads the wall clock a moment later, so every measured value carries a
##     millisecond or two of slop — hence SLOP on the absolute assertions.
##   * sample() rejects non-increasing server times (they're reordered, and their apparent
##     one-way delay is fake). A *lower* candidate therefore requires real elapsed time
##     between samples, which is what `_settle()` is for. That's not a testing artifact:
##     in production the candidate only drops because local time advanced.
##
## Where an assertion can be made independent of the wall clock, it is — the correction
## tests below assert on the RATIO between successive steps rather than absolute positions,
## which removes the slop from the result entirely.

const Clock := preload("res://addons/goldnet/server_clock.gd")

## Tolerance for anything derived from a wall-clock read.
const SLOP := 12.0


# --- helpers ---

## Feed one observation whose candidate offset is `candidate_ms`.
func _feed(clock, candidate_ms: float) -> void:
	clock.sample(int(Time.get_ticks_msec() + candidate_ms))


## The clock's current applied offset: what now() reports, minus local time.
func _offset(clock) -> float:
	return clock.now_ms() - float(Time.get_ticks_msec())


## Burn real milliseconds. Needed whenever a test wants to feed a LOWER candidate than the
## previous one (server time must still increase) or to age samples out of the window.
func _settle(ms: int) -> void:
	OS.delay_msec(ms)


## Run `frames` corrections at a fixed frame time.
func _advance(clock, frames: int, delta: float = 1.0 / 60.0) -> void:
	for _i in frames:
		clock.advance(delta)


func run() -> void:
	_test_pre_sync_fallback()
	_test_first_sample_initializes()
	_test_now_is_seconds()
	_test_advance_before_sample_is_inert()
	_test_stale_sample_ignored()
	_test_min_delay_filter_picks_the_best_candidate()
	_test_slew_is_capped()
	_test_correction_is_proportional()
	_test_large_gap_snaps()
	_test_window_ages_out_stale_samples()
	_test_reset()


# --- pre-sync ---

func _test_pre_sync_fallback() -> void:
	var c := Clock.new()
	check(not c.is_synced(), "a fresh clock is not synced")
	# Before any observation, now() falls back to local time so callers get a usable value
	# rather than zero (which would put render_time decades in the past).
	check_near(_offset(c), 0.0, SLOP, "pre-sync now_ms() falls back to local time")


func _test_first_sample_initializes() -> void:
	var c := Clock.new()
	_feed(c, 250.0)
	check(c.is_synced(), "one observation is enough to be synced")
	# The applied offset is initialized outright rather than slewed in from zero, so now()
	# is usable on the very first frame after joining.
	check_near(_offset(c), 250.0, SLOP, "the first sample initializes the applied offset")


func _test_now_is_seconds() -> void:
	var c := Clock.new()
	_feed(c, 1000.0)
	check_near(c.now(), c.now_ms() / 1000.0, 0.001, "now() is now_ms() in seconds")


func _test_advance_before_sample_is_inert() -> void:
	# advance() runs every frame, including before the first snapshot arrives. It must not
	# drift the offset away from the local-time fallback.
	var c := Clock.new()
	_advance(c, 100)
	check(not c.is_synced(), "advancing without samples does not mark the clock synced")
	check_near(_offset(c), 0.0, SLOP, "advancing without samples leaves the fallback alone")


# --- sample filtering ---

func _test_stale_sample_ignored() -> void:
	# A reordered observation's apparent one-way delay is fiction — it looks like a huge
	# positive delay and would drag the estimate down if it were trusted.
	#
	# SCOPE — established by mutation: deleting the `server_time_ms <= _last_server_ms`
	# guard leaves this test passing, and that appears to be structural rather than a hole
	# in the test. A reordered sample arrives later (higher local time) carrying an older
	# server time, so its candidate is strictly LOWER than a live one's — and the min-delay
	# filter already discards it by taking the maximum. The explicit guard saves the work
	# and keeps the window clean; the filter is what actually protects the estimate. This
	# test pins the observable outcome, which is what the caller depends on.
	var c := Clock.new()
	_feed(c, 200.0)
	c.sample(int(Time.get_ticks_msec()) - 5000)  # far older server time
	_advance(c, 400)
	check_near(_offset(c), 200.0, SLOP, "a reordered observation does not move the estimate")


func _test_min_delay_filter_picks_the_best_candidate() -> void:
	# Each candidate is (true_offset − one_way_delay), so the LARGEST is the one that
	# travelled fastest and is closest to the truth. Averaging instead would bias the clock
	# by the mean network delay and make it jitter with the link.
	var c := Clock.new()
	c.sample_window_ms = 10000.0  # keep every sample below inside the window

	# Candidates chosen so server time still increases across the 30 ms gaps.
	_feed(c, 100.0)
	_settle(30); _feed(c, 80.0)
	_settle(30); _feed(c, 140.0)   # the best sample — lowest delay
	_settle(30); _feed(c, 95.0)

	_advance(c, 2000)
	check_near(_offset(c), 140.0, SLOP,
		"the estimate converges on the best (largest) candidate, not the mean or the newest")


# --- correction dynamics ---

func _test_slew_is_capped() -> void:
	# The cap bounds how fast the applied offset chases the target. Every ms/s of slew is a
	# ms/s of render_time dilation, visible as moving entities drifting against the camera.
	var c := Clock.new()
	c.sample_window_ms = 10000.0
	c.correction_rate = 1000.0     # proportional term would close the whole gap in one step
	c.max_slew_ms_per_sec = 20.0
	c.snap_threshold_ms = 10000.0  # keep this test out of the snap path

	_feed(c, 100.0)
	_settle(30); _feed(c, 190.0)   # ~90 ms gap to close

	var before := _offset(c)
	c.advance(0.1)                 # cap allows 20 ms/s * 0.1 s = 2 ms
	var moved := _offset(c) - before
	# Independent of the wall clock: both reads are offsets, so the slop cancels.
	check_near(moved, 2.0, 0.1,
		"one frame moves at most max_slew_ms_per_sec * delta, however large the gap")


func _test_correction_is_proportional() -> void:
	# Under the cap, each frame closes a fixed FRACTION of the remaining error, so
	# successive steps shrink geometrically. That's what low-passes the target's small
	# step-downs into sub-ms ripple instead of stepping the render timeline.
	var c := Clock.new()
	c.sample_window_ms = 10000.0
	c.correction_rate = 6.0
	c.max_slew_ms_per_sec = 100000.0  # effectively uncapped: isolate the proportional term
	c.snap_threshold_ms = 10000.0

	_feed(c, 100.0)
	_settle(30); _feed(c, 190.0)

	# delta = 0.1 s → each frame closes 6.0 * 0.1 = 60% of what's left.
	var o0 := _offset(c)
	c.advance(0.1)
	var o1 := _offset(c)
	c.advance(0.1)
	var o2 := _offset(c)

	var step1 := o1 - o0
	var step2 := o2 - o1
	check(step1 > 0.0, "the first correction moves toward the target")
	# Asserting the RATIO makes this independent of the actual gap, and therefore of the
	# wall-clock slop in the candidates.
	check_near(step2 / step1, 0.4, 0.05,
		"each frame closes 60%% of the remaining error, so steps shrink by 0.4x")


func _test_large_gap_snaps() -> void:
	# First sync after a join, or an alt-tab hitch: the gap is far too large to slew away
	# (at 20 ms/s a 600 ms error would take 30 seconds), so it's taken in one step.
	var c := Clock.new()
	c.sample_window_ms = 10000.0
	c.snap_threshold_ms = 150.0

	_feed(c, 100.0)
	_settle(30); _feed(c, 800.0)  # ~700 ms gap, way past the threshold

	c.advance(1.0 / 60.0)
	check_near(_offset(c), 800.0, SLOP, "a gap beyond snap_threshold_ms is taken in one step")


func _test_window_ages_out_stale_samples() -> void:
	# The window length is how fast a REAL downward clock shift is tracked: the estimate can
	# only fall once the stale low-delay samples holding it up age out. Without this the
	# min-delay filter would latch onto one lucky early sample forever.
	var c := Clock.new()
	c.sample_window_ms = 50.0
	c.snap_threshold_ms = 10000.0  # force the slew path so this tests aging, not snapping

	# The settle has to exceed the candidate DROP as well as the window: server time must
	# still increase, so falling from 200 to 100 needs more than 100 ms of real elapsed
	# time or the newer sample is rejected as reordered.
	_feed(c, 200.0)               # a very good sample...
	_settle(120)                  # ...aged well past the 50 ms window
	_feed(c, 100.0)
	_settle(10); _feed(c, 105.0)

	_advance(c, 4000)
	check_near(_offset(c), 105.0, SLOP,
		"once the best sample ages out of the window the estimate follows the newer ones down")


func _test_reset() -> void:
	# Session end / reconnect: old offsets describe a different server process.
	var c := Clock.new()
	_feed(c, 500.0)
	check(c.is_synced(), "synced before reset")
	c.reset()
	check(not c.is_synced(), "reset() clears the synced flag")
	check_near(_offset(c), 0.0, SLOP, "reset() returns now_ms() to the local-time fallback")

	# The stale-sample guard must reset too, or the first observation of the new session
	# (whose server time may be lower than the old one's) would be rejected as reordered.
	_feed(c, 40.0)
	check(c.is_synced(), "a new session's first sample is accepted after reset()")
	check_near(_offset(c), 40.0, SLOP, "reset() lets the estimate re-initialize from scratch")
