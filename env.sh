

# --------------------------------------------------------- #
# --------------------------------------------------------- #

function _build_backend_cc() {
    g++ -o ./backend_cc.o ./backend_cc/main.cc \
        -std=c++23 \
        -O3 \
        -flto \
        -pthread \
        -march=native;
}

function _build_backend_cc_drogon() {
	g++ backend_cc_drogon/main.cc -o ./backend_cc_drogon.o \
	-std=c++23 -Wall -Wextra -O3 \
	-I/usr/include -I/usr/local/include -I/home/pr/include \
	-L/usr/lib -L/usr/local/lib -L/home/pr/lib \
	-ldrogon -ltrantor -ljsoncpp -luuid -lz -lbrotlienc -lbrotlidec \
	-lssl -lcrypto -lpq -lmysqlclient -lsqlite3 -lhiredis -lyaml-cpp \
	-lpthread -ldl -lcares \
	;
}

# --------------------------------------------------------- #
# --------------------------------------------------------- #

function _build_backend_rs() {
    rustc ./backend_rs/src/main.rs -o ./backend_rs.o \
      -C opt-level=3 \
      -C lto=true \
      -C codegen-units=1 \
      -C panic=abort \
      -C target-cpu=native;
}

# --------------------------------------------------------- #
# --------------------------------------------------------- #

function _build_backend_go() {
	go build -o ./backend_go.o ./backend_go;
}

# --------------------------------------------------------- #
# --------------------------------------------------------- #

function _build_backend_all() {
    _build_backend_cc;
    _build_backend_cc_drogon;
    _build_backend_go;
    _build_backend_rs;
}
