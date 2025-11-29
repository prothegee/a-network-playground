function _build_backend_cc_drogon() {
	g++ backend_cc_drogon/main.cc -o ./backend_cc_drogon.o \
	-std=c++23 -Wall -g \
	-I/usr/include -I/usr/local/include -I/home/pr/include \
	-L/usr/lib -L/usr/local/lib -L/home/pr/lib \
	-ldrogon -ltrantor -ljsoncpp -luuid -lz -lbrotlienc -lbrotlidec \
	-lssl -lcrypto -lpq -lmysqlclient -lsqlite3 -lhiredis -lyaml-cpp \
	-lpthread -ldl -lcares \
	;
}

