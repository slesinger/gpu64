#!/usr/bin/env bash
#
# demos.sh - assemble the gpu64 demonstration programs and check them on a PC.
#
# For each Source/Demos/gpu64_demo_*.a:
#   1. assemble it with 64tass into the matching .prg
#   2. run it under tools/prgsim, twice: once modelling a display with a
#      frame clock and once modelling one without, because a demo that only
#      works on the first is broken on hardware nobody can predict. The
#      first run is 400 frames -- see the comment on --stop-after below
#   3. render what the HDMI output was showing when it finished, into
#      Source/Demos/out/<name>.ppm
#
# A class-1 demo is handled differently at step 3. The model does not
# rasterise class 1 -- see tools/prgsim/gpu64class1.py -- so there is no page
# for --ppm to photograph. Instead the run writes a frame stream, and
# tools/hostsim/scenesim renders EVERY frame of it through the firmware's own
# gpu64_3dSceneRender(). out/<name>.ppm is the last of those, and
# out/<name>.c1/ holds the stream, its blobs and the per-frame PPMs.
#
# The demos are not the conformance suite: they assert nothing and reach no
# verdict. What this script establishes is that each one assembles, runs to
# completion in both display cases, and produces a picture -- which is the
# part that would otherwise cost bench time to find out. Look at the PPMs
# before deploying; that is the whole point of rendering them.
#
# Note that 64tass is invoked WITHOUT --nostart: that option strips the
# 2-byte load address and produces a PRG that loads to the wrong place and
# silently never runs.
#
# Usage:
#   tools/demos.sh                # assemble + run + render everything
#   tools/demos.sh hello rotate   # just these
#   tools/demos.sh -v hello       # and print the C64 screen it produced

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEMODIR="$REPO_ROOT/Source/Demos"
OUTDIR="$DEMODIR/out"
SIM="$REPO_ROOT/tools/prgsim/runsim.py"
SCENESIM="$REPO_ROOT/tools/hostsim/scenesim"

verbose=0
if [ "${1:-}" = "-v" ]; then verbose=1; shift; fi

if [ $# -gt 0 ]; then
	names=("$@")
else
	names=()
	for f in "$DEMODIR"/gpu64_demo_*.a; do
		b="$(basename "$f" .a)"
		names+=("${b#gpu64_demo_}")
	done
fi

mkdir -p "$OUTDIR"

fail=0
for n in "${names[@]}"; do
	src="$DEMODIR/gpu64_demo_$n.a"
	prg="$DEMODIR/gpu64_demo_$n.prg"
	if [ ! -f "$src" ]; then
		echo "no such demo: $n"
		fail=1
		continue
	fi

	if ! out=$( cd "$DEMODIR" && 64tass --cbm-prg -o "$prg" "$src" 2>&1 ); then
		echo "ASSEMBLE FAIL  $n"
		echo "$out" | grep -v '^$' | tail -20
		fail=1
		continue
	fi

	# Per-demo extra arguments. A demo steered by the keyboard reads every
	# key as released without --key, so it stands still for four hundred
	# frames and proves almost nothing; the schedule below walks it around
	# its room, looks up, and fires.
	extra=()
	c1=0
	case "$n" in
	quake3d)
		c1=1
		extra=(--key=W:20-260 --key=A:120-200 --key=D:300-420
		       --key=E:430-500 --key=W:520-760 --key=F1:640-700
		       --key=F7:780-800 --key=SPACE:820-900 --key=S:950-1150
		       --key=Q:1000-1080 --key=F3:1200-1260)
		;;
	esac

	bad=0
	for mode in "" "--no-vblank"; do
		ppm=""
		frames="--stop-after=60"
		if [ -z "$mode" ]; then
			ppm="--ppm=$OUTDIR/$n.ppm"
			# Several hundred frames, not the handful the simulator
			# used to run: a defect whose onset was frame 98 got
			# through a 96-frame check once already on this project,
			# and the raycast demo looked clean here for the same
			# reason. The no-vblank pass only has to prove the demo
			# does not hang, so it stays short.
			frames="--stop-after=400"
			if [ $c1 -eq 1 ]; then
				# One commit per frame clock tick, so the
				# stop-poll count that buys 400 rendered
				# frames is the same 400 -- but the class-1
				# demos poll STOP once per attempted commit,
				# and a run worth looking at wants more than
				# the first few seconds of it.
				frames="--stop-after=1500"
				rm -rf "$OUTDIR/$n.c1"
				mkdir -p "$OUTDIR/$n.c1"
				ppm="--c1-stream=$OUTDIR/$n.c1/frames.txt"
			fi
		fi
		if simout=$( python3 "$SIM" "$prg" --demo $mode $frames $ppm "${extra[@]+"${extra[@]}"}" 2>&1 ); then
			[ $verbose -eq 1 ] && echo "$simout"
		else
			echo "SIM FAIL  $n ${mode:-(vblank)}"
			echo "$simout"
			bad=1
			fail=1
		fi
	done

	# The picture, for a class-1 demo, comes out of the firmware renderer
	# rather than the model: every frame of the stream, so a defect that
	# only shows up once the player has walked somewhere is visible here
	# and not only at the bench.
	if [ $bad -eq 0 ] && [ $c1 -eq 1 ]; then
		if [ ! -x "$SCENESIM" ]; then
			echo "SCENESIM MISSING  $n  (make -C tools/hostsim)"
			bad=1
			fail=1
		elif ! scout=$( "$SCENESIM" "$OUTDIR/$n.c1/frames.txt" \
				 "$OUTDIR/$n.c1" --ppm-every=25 2>&1 ); then
			echo "RENDER FAIL  $n"
			echo "$scout"
			bad=1
			fail=1
		else
			[ $verbose -eq 1 ] && echo "$scout"
			last=$(ls "$OUTDIR/$n.c1"/frame*.ppm 2>/dev/null | tail -1)
			[ -n "$last" ] && cp "$last" "$OUTDIR/$n.ppm"
		fi
	fi

	if [ $bad -eq 0 ]; then
		size=$(stat -c%s "$prg")
		printf 'ok  %-10s %5d bytes  ->  out/%s.ppm\n' "$n" "$size" "$n"
	fi
done

exit $fail
