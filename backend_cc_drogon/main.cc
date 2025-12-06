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
				.setUploadPath("./upload")
				.setMaxConnectionNumPerIP(0)
				.setThreadNum(0);

	registerHandler(drogon::app());

	drogon::app().run();

	return 0;
}

