#!/usr/bin/env bash
# Build the _logvcore Python extension.
# Compiles each .cpp → .o separately so ccache can cache the expensive
# pybind11 template expansion in bindings.o independently from the fast
# core objects.  Unchanged objects are instant ccache hits.
#
# Key speed trick: bindings.cpp uses -O1 (not -O2).  pybind11 template
# instantiation is 5-10× slower at -O2 and the binding glue doesn't
# benefit from heavy optimization.  Core logic keeps -O2.
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

PY=$(python3 -c "import sys; print(sys.executable)")
PB=$($PY -c "import pybind11; print(pybind11.get_include())")
PYINC=$($PY -c "import sysconfig; print(sysconfig.get_path('include'))")
EXT=$($PY -c "import sysconfig; print(sysconfig.get_config_var('EXT_SUFFIX'))")
OUT="../_logvcore${EXT}"

echo "pybind11 : $PB"
echo "python   : $PYINC"
echo "output   : $OUT"

if command -v ccache &>/dev/null; then
    CXX="ccache g++"
else
    CXX="g++"
fi
COMMON="-std=c++17 -fPIC -fvisibility=hidden -Iinclude"
OBJDIR="obj"
mkdir -p "$OBJDIR"

# ---------- Compile each translation unit ----------
# Core objects (-O2, no pybind11 headers — fast):
$CXX $COMMON -O2 -c src/log_parser.cpp -o "$OBJDIR/log_parser.o" &
$CXX $COMMON -O2 -c src/log_filter.cpp -o "$OBJDIR/log_filter.o" &

# Bindings (-O1: pybind11 templates are 5-10x slower at -O2,
# and the binding glue doesn't benefit from heavy optimization):
$CXX $COMMON -O1 -I"$PB" -I"$PYINC" \
    -c src/bindings.cpp -o "$OBJDIR/bindings.o" &
wait

# ---------- Link (always fast) ----------
$CXX -shared \
    "$OBJDIR/log_parser.o" \
    "$OBJDIR/log_filter.o" \
    "$OBJDIR/bindings.o" \
    -o "$OUT"

echo "Built: $OUT"
python3 -c "import sys; sys.path.insert(0,'..'); import _logvcore; e=_logvcore.parse_line('Mar 17 12:00:00 host cloud-connection-service[123]: <6>[INFO](tid=1) test message'); print('SMOKE TEST OK:', repr(e))"
