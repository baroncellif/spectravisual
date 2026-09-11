#!/bin/sh
# Riproduzioni dell'audit (docs/audit/A4-riproduzioni.md).
#
# Compila un harness che include le translation unit reali dell'app
# (main.c, controller.c, predfit.c) e le esegue su fixture sintetiche e sui
# cataloghi del repository in SOLA LETTURA.  Ogni scenario lavora in una
# directory temporanea propria: nessun file del repository viene scritto.
# SPCAT/SPFIT vengono lanciati solo dentro quelle directory temporanee.
#
# Uso:  sh docs/audit/repro/run_all.sh [cartella_output]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
OUT=${1:-$(mktemp -d "${TMPDIR:-/tmp}/sv-audit.XXXXXX")}
BUILD="$OUT/build"; RUNS="$OUT/runs"; FX="$OUT/fixtures"
mkdir -p "$BUILD" "$RUNS" "$FX"

python3 "$HERE/gen_fixtures.py" "$FX" "$REPO"
gcc -w -g -O0 $(sdl2-config --cflags) -I"$REPO" -o "$BUILD/harness" \
    "$HERE/harness.c" "$REPO/loader.c" "$REPO/algorithms.c" "$REPO/layout.c" \
    "$REPO/settings.c" "$REPO/ui_icons.c" "$REPO/intensity_fit.c" "$REPO/view.c" \
    "$HERE/plotgpu_stub.c" $(sdl2-config --libs) -lSDL2_ttf -lm
H="$BUILD/harness"

"$H" qnfmt "$REPO/pred.cat" "$REPO/.fit/model.cat" "$FX/cat3_303.cat" "$FX/cat4_304.cat" \
     "$FX/cat4_1404.cat" "$FX/cat5_305.cat" "$FX/cat6_306.cat" "$FX/cat_letter.cat" "$FX/cat_trim.cat" > "$RUNS/qnfmt.log" 2>&1 || true
"$H" roundtrip "$FX/cat3_303.cat"  "$RUNS/rt3"    3000.1 5980.0 5999.2867 7000.0 8000.0 9000.0 > "$RUNS/rt3.log" 2>&1 || true
"$H" roundtrip "$FX/cat4_304.cat"  "$RUNS/rt304"  3100.0 3100.3 6100.0 6100.3 > "$RUNS/rt304.log" 2>&1 || true
"$H" roundtrip "$FX/cat4_1404.cat" "$RUNS/rt1404" 2511.3375 2511.9 6033.5894 6034.0 > "$RUNS/rt1404.log" 2>&1 || true
"$H" roundtrip "$FX/cat5_305.cat"  "$RUNS/rt5"    3200.0 3200.3 6200.0 > "$RUNS/rt5.log" 2>&1 || true
"$H" roundtrip "$FX/cat6_306.cat"  "$RUNS/rt6"    3300.0 3300.3 6300.0 > "$RUNS/rt6.log" 2>&1 || true
"$H" roundtrip "$REPO/pred.cat"    "$RUNS/rtpred" 221.5761 392.7959 5999.2867 > "$RUNS/rtpred.log" 2>&1 || true
"$H" nvib "$RUNS/nvib" > "$RUNS/nvib.log" 2>&1 || true
"$H" formats "$RUNS/formats" "$REPO/assignments_backup.txt" > "$RUNS/formats.log" 2>&1 || true
"$H" stale_sel "$RUNS/stale" "$FX/cat3_303.cat" "$FX/cat4_1404.cat" > "$RUNS/stale.log" 2>&1 || true
# Gli scenari seguenti richiedono SPCAT e SPFIT (percorsi rilevati come fa l'app,
# settings.c:248-284).
"$H" predfit_mixed "$RUNS/pfmix" "$REPO/pred.cat" > "$RUNS/pfmix.log" 2>&1 || true
mkdir -p "$RUNS/restore_cwd"
"$H" restore "$RUNS/restore_data" "$RUNS/restore_cwd" "$FX/cat4_1404.cat" > "$RUNS/restore.log" 2>&1 || true
"$H" intfit "$RUNS/intfit" > "$RUNS/intfit.log" 2>&1 || true
"$H" undo "$RUNS/undo" > "$RUNS/undo.log" 2>&1 || true
"$H" sentinel "$RUNS/sent001" 0.01 > "$RUNS/sent001.log" 2>&1 || true
"$H" sentinel "$RUNS/sent05" 0.5 > "$RUNS/sent05.log" 2>&1 || true
"$H" restore_int "$RUNS/rint" "$FX/cat4_1404.cat" > "$RUNS/rint.log" 2>&1 || true
"$H" qnfmt "$RUNS/undo/.fit/model.cat" > "$RUNS/qnfmt_single_species.log" 2>&1 || true

# Secondo e terzo passaggio (B-28…B-51, scenari R-18…R-40).
mkdir -p "$RUNS/with space" "$RUNS/saveloss_cwd"
"$H" spaces "$RUNS/with space" > "$RUNS/spaces.log" 2>&1 || true
"$H" remove_active "$RUNS/remact" > "$RUNS/remove_active.log" 2>&1 || true
"$H" peakfinder > "$RUNS/peakfinder.log" 2>&1 || true
"$H" divzero "$RUNS/divzero" "$FX/cat3_303.cat" > "$RUNS/divzero.log" 2>&1 || true
"$H" fitfail "$RUNS/fitfail" > "$RUNS/fitfail.log" 2>&1 || true
"$H" mu_input "$RUNS/muinput" > "$RUNS/mu_input.log" 2>&1 || true
"$H" multidrop "$RUNS/multidrop" > "$RUNS/multidrop.log" 2>&1 || true
"$H" workdir "$RUNS/workdir" > "$RUNS/workdir.log" 2>&1 || true
"$H" uppercase "$RUNS/upper" "$FX/cat3_303.cat" > "$RUNS/uppercase.log" 2>&1 || true
"$H" param0 "$RUNS/param0" > "$RUNS/param0.log" 2>&1 || true
"$H" paramdup "$RUNS/paramdup" > "$RUNS/paramdup.log" 2>&1 || true
"$H" clicat "$RUNS/clicat" "$REPO/pred.cat" > "$RUNS/clicat.log" 2>&1 || true
"$H" saveloss "$RUNS/saveloss_data" "$RUNS/saveloss_cwd" "$FX/cat3_303.cat" > "$RUNS/saveloss.log" 2>&1 || true
"$H" exportstale "$RUNS/exportstale" > "$RUNS/exportstale.log" 2>&1 || true
"$H" descending "$RUNS/descending" > "$RUNS/descending.log" 2>&1 || true
"$H" descending2 "$RUNS/desc2" > "$RUNS/descending2.log" 2>&1 || true
"$H" speciesdel "$RUNS/speciesdel" > "$RUNS/speciesdel.log" 2>&1 || true
"$H" lineerr "$RUNS/lineerr" > "$RUNS/lineerr.log" 2>&1 || true
"$H" errcol "$RUNS/errcol" > "$RUNS/errcol.log" 2>&1 || true
"$H" baseline "$RUNS/baseline" > "$RUNS/baseline.log" 2>&1 || true
"$H" reassign "$RUNS/reassign" "$FX/cat3_303.cat" > "$RUNS/reassign.log" 2>&1 || true
"$H" perf "$RUNS/perf" > "$RUNS/perf.log" 2>&1 || true
"$H" textleak "$RUNS/textleak" > "$RUNS/textleak.log" 2>&1 || true
"$H" keyleak "$RUNS/keyleak" > "$RUNS/keyleak.log" 2>&1 || true
"$H" yrange "$RUNS/yrange" "$FX/cat3_303.cat" > "$RUNS/yrange.log" 2>&1 || true
"$H" noA "$RUNS/noA" > "$RUNS/noA.log" 2>&1 || true
"$H" noA2 "$RUNS/noA2" > "$RUNS/noA2.log" 2>&1 || true

echo "Risultati in $RUNS"
