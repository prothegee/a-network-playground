#include <drogon/drogon.h>

#define ADDRESS_IP "0.0.0.0"
#define ADDRESS_PORT 9003

int main() {
	printf("backend_cc_drogon: run on %s:%d\n", ADDRESS_IP, ADDRESS_PORT);

	drogon::app().addListener(ADDRESS_IP, ADDRESS_PORT);


	drogon::app().run();

	return 0;
}

