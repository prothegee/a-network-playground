clear;
set -e;
mkdir -p logs public;

source env.sh;

# backend_c
gcc -o ./backend_c.o ./backend_c/main.c -std=c23 -g;

# --------------------------------------------------------- #

# backend_cc
g++ -o ./backend_cc.o ./backend_cc/main.cc -std=c++23 -g; #-O3;

# backend_cc_drogon
_build_backend_cc_drogon;

# --------------------------------------------------------- #

# backend_go
go build -o ./backend_go.o ./backend_go; #-ldflags "-s -w";

# --------------------------------------------------------- #

# backend_rs
# todo

# --------------------------------------------------------- #

# backend_zig
# todo

