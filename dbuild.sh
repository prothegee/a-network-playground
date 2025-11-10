clear;
mkdir -p log public;

# backend_c
# todo

# --------------------------------------------------------- #

# backend_cc
g++ -o ./backend_cc.o ./backend_cc/main.cc -std=c++23 -g; #-O3;

# --------------------------------------------------------- #

# backend_go
go build -o ./backend_go.o ./backend_go; #-ldflags "-s -w";

# --------------------------------------------------------- #

# backend_rs
# todo

# --------------------------------------------------------- #

# backend_zig
# todo

