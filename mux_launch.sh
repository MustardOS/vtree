#!/bin/sh
# HELP: vTree Gold
# ICON: dingux
# GRID: vTree

. /opt/muos/script/var/func.sh

APP_BIN="vtree"
SETUP_APP "$APP_BIN" ""

# -----------------------------------------------------------------------------

VTREE_DIR="$1"
cd "$VTREE_DIR" || exit

./"$APP_BIN" --logfile="${VTREE_DIR}/vtree.log"