#!/bin/bash

BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

export PATH=$PATH:$BASE_DIR/ns2.34/bin:$BASE_DIR/ns2.34/tcl8.4.18/unix
export LD_LIBRARY_PATH=$BASE_DIR/ns2.34/otcl-1.13:$BASE_DIR/ns2.34/lib
export TCL_LIBRARY=$BASE_DIR/ns2.34/tcl8.4.18/library

echo "[SIRD] env loaded ✅"

