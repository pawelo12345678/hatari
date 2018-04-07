#!/bin/sh

if [ $# -ne 3 -o "$1" = "-h" -o "$1" = "--help" ]; then
	echo "Usage: $0 <hatari> b|w <machine>"
	exit 1;
fi

hatari=$1
if [ ! -x "$hatari" ]; then
	echo "First parameter must point to valid hatari executable."
	exit 1;
fi;

width=$2
machine=$3
basedir=$(dirname $0)
testdir=$(mktemp -d)

case "$machine" in
 st)		reffile="st_$width.txt" ;;
 megast)	reffile="mst4_$width.txt" ;;
 ste)		reffile="ste_$width.txt" ;;
 megaste)	reffile="mste4_$width.txt" ;;
 tt)		reffile="tt_$width.txt" ;;
 falcon)	reffile="fal_n_$width.txt" ;;
 *)		echo "Unsupported machine type: $machine"; exit 1;;
esac

export SDL_VIDEODRIVER=dummy
export SDL_AUDIODRIVER=dummy

HOME="$testdir" $hatari --log-level fatal --sound off --machine $machine \
	--tos none --fast-forward on -d $testdir "$basedir/buserr_$width.prg" \
	2> $testdir/stderr.txt > $testdir/stdout.txt
exitstat=$?
if [ $exitstat -ne 0 ]; then
	echo "Running hatari failed. Status=${exitstat}, output in ${testdir}"
	exit 1
fi

grep -v '^(' "$basedir/results/$reffile" > "$testdir/expected.txt"

if [ "$width" = "b" ]; then
	tester_output="$testdir/BUSERR_B.TXT"
else
	tester_output="$testdir/BUSERR_W.TXT"
fi

if ! diff -q "$testdir/expected.txt" "$tester_output"; then
	echo "Test FAILED, output differs:"
	diff -u "$testdir/expected.txt" "$tester_output"
	rm -rf "$testdir"
	exit 1
fi

echo "Test PASSED."
rm -rf "$testdir"
exit 0
