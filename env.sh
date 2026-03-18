function _build_backend_c() {
    echo "backend_c";
    gcc -o ./backend_c.o ./backend_c/main.c \
        -std=c23 \
        -D_GNU_SOURCE \
        -Wall \
        -Wextra \
        -O3 \
        -flto \
        -pthread \
        -march=native;
}

# --------------------------------------------------------- #
# --------------------------------------------------------- #

function _build_backend_cc() {
    echo "backend_cc";
    g++ -o ./backend_cc.o ./backend_cc/main.cc \
        -std=c++23 \
        -Wall \
        -Wextra \
        -O3 \
        -flto \
        -pthread \
        -march=native;
}

function _build_backend_cc_drogon() {
	echo "backend_cc_drogon";
    g++ backend_cc_drogon/main.cc -o ./backend_cc_drogon.o \
	-std=c++23 -Wall -Wextra -O3 -pthread -march=native \
	-I/usr/include -I/usr/local/include -I/mnt/256a1/include \
	-L/usr/lib -L/usr/local/lib -L/mnt/256a1/lib \
	-ldrogon -ltrantor -ljsoncpp -luuid -lz -lbrotlienc -lbrotlidec \
	-lssl -lcrypto -lpq -lmysqlclient -lsqlite3 -lhiredis -lyaml-cpp \
	-lpthread -ldl -lcares \
	;
}

# --------------------------------------------------------- #
# --------------------------------------------------------- #

function _build_backend_rs() {
    echo "backend_rs";
    rustc ./backend_rs/src/main.rs -o ./backend_rs.o \
      -C opt-level=z \
      -C lto=fat \
      -C codegen-units=1 \
      -C panic=abort \
      -C target-cpu=native \
      -C strip=debuginfo;
    # opt-level: z,3
    # lto: fat,true
    # codegen-units: 1
    # panic: abort
    # target-cpu: native
    # strip: debuginfo
}

# --------------------------------------------------------- #
# --------------------------------------------------------- #

function _build_backend_zig() {
    echo "backend_zig";
    zig build-exe backend_zig/main.zig --name backend_zig.o;
}
function _build_backend_zig_async() {
    echo "backend_zig_async";
    zig build-exe backend_zig/main_async.zig --name backend_zig_async.o;
}

# --------------------------------------------------------- #
# --------------------------------------------------------- #

function _build_backend_go() {
    echo "backend_go";
	go build -o ./backend_go.o ./backend_go;
}

# --------------------------------------------------------- #
# --------------------------------------------------------- #

function _build_backend_all() {
    _build_backend_c;
    _build_backend_cc;
    _build_backend_cc_drogon;
    _build_backend_rs;
    _build_backend_zig;
    _build_backend_zig_async;
    _build_backend_go;
}

