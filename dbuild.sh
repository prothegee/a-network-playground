#/bin/sh
clear;
set -e;
mkdir -p logs public;

source env.sh;

# backend_c
gcc -o ./backend_c.o ./backend_c/main.c -std=c23 -g;

# --------------------------------------------------------- #

# backend_cc
_build_backend_cc;

# backend_cc_drogon
_build_backend_cc_drogon;

# --------------------------------------------------------- #

# backend_go
_build_backend_go;

# backend_go_fiber

# --------------------------------------------------------- #

# backend_rs
# todo

# --------------------------------------------------------- #

# backend_zig
# todo

