#include "drogon/HttpRequest.h"
#include "drogon/HttpResponse.h"
#include "drogon/HttpTypes.h"
#include <drogon/drogon.h>
#include <functional>

#define ADDRESS_IP "0.0.0.0"
#define ADDRESS_PORT 9003

using namespace drogon;

// --------------------------------------------------------- //

static const char *HTTP_RESP_MESSAGE_METHOD_NOT_ALLOWED = "Method Not Allowed";
// static const char *HTTP_RESP_MESSAGE_INTERNAL_SERVER_ERROR = "Internal Server Error";

// --------------------------------------------------------- //

static const char *HomeHandlerPath = "/cc-drogon";
void HomeHandler(drogon::HttpAppFramework &app) {
	app.registerHandler(HomeHandlerPath, [](const HttpRequestPtr &p_req, std::function<void(const HttpResponsePtr &)> &&callback) {
		if (p_req->getMethod() != Get) {
			HttpResponsePtr p_resp = HttpResponse::newHttpResponse();
			p_resp->setCustomStatusCode(k405MethodNotAllowed, HTTP_RESP_MESSAGE_METHOD_NOT_ALLOWED);
			callback(p_resp);
			return;
		}

		HttpResponsePtr p_resp = HttpResponse::newHttpResponse();

		char resp[5] = "home";

		p_resp->setBody(resp);

		callback(p_resp);
	});
}

// --------------------------------------------------------- //

void registerHandler(drogon::HttpAppFramework& drogonApp) {
	HomeHandler(drogonApp); 
}

// --------------------------------------------------------- //

int main() {
	printf("backend_cc_drogon: run on %s:%d\n", ADDRESS_IP, ADDRESS_PORT);

	drogon::app().addListener(ADDRESS_IP, ADDRESS_PORT)
                .setDocumentRoot("./public")
				.setUploadPath("u")
				.setMaxConnectionNumPerIP(0)
				.setThreadNum(0);

	registerHandler(drogon::app());

	drogon::app().run();

	return 0;
}

/*
IMPORTANT:
this implementation result:
➜ wrk -c 100 -t 6 -d 10s http://localhost:9003/cc-drogon
Running 10s test @ http://localhost:9003/cc-drogon
  6 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   263.10us  460.84us   5.21ms   90.54%
    Req/Sec    87.70k     4.19k  101.60k    71.74%
  5278544 requests in 10.10s, 714.83MB read
Requests/sec: 522643.02
Transfer/sec:     70.78MB
*/

