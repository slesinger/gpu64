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
		fi
		if simout=$( python3 "$SIM" "$prg" --demo $mode $frames $ppm 2>&1 ); then
			[ $verbose -eq 1 ] && echo "$simout"
		else
			echo "SIM FAIL  $n ${mode:-(vblank)}"
			echo "$simout"
			bad=1
			fail=1
		fi
	done

	if [ $bad -eq 0 ]; then
		size=$(stat -c%s "$prg")
		printf 'ok  %-10s %5d bytes  ->  out/%s.ppm\n' "$n" "$size" "$n"
	fi
done

exit $fail
