#include <cstdint>
#include <print>
#include <sstream>
#include <string>

#define ADDRESS_IP "0.0.0.0"
#define ADDRESS_PORT 9002

// --------------------------------------------------------- //

static const char *HTTP_CONTENT_TYPE_HINT = "Content-Type";
static const char *HTTP_CONTENT_LENGTH_HINT = "Content-Length";
static const char *HTTP_CONNECTION_HINT = "Connection";

static const char *HTTP_CONTENT_TYPE_HINT_APP_TEXT = "text/plain";
static const char *HTTP_CONTENT_TYPE_HINT_APP_JSON = "application/json";

static const char *HTTP_RESP_MESSAGE_METHOD_NOT_ALLOWED = "Method Not Allowed";
static const char *HTTP_RESP_MESSAGE_INTERNAL_SERVER_ERROR = "Internal Server Error";

// --------------------------------------------------------- //

struct TypeRespJson {
	double decimal;
	std::string string;
	int32_t round;
	bool boolean;

	std::string to_json() const {
		std::ostringstream oss;

		oss << "{"
			<< "\"decimal\":" << decimal << ","
			<< "\"string\":" << string << ","
			<< "\"round\":" << round << ","
			<< "\"round\":" << (boolean ? "true" : "false")
			<< "}";

		return oss.str();
	}
}; // struct TypeRespJson;

// --------------------------------------------------------- //

class HttpResponse {
public:
	std::string build() const {
		std::ostringstream oss;

		//

		return oss.str();
	};
}; // class HttpResponse

// --------------------------------------------------------- //

int main() {
    std::print("backend_cc: run on {}:{}\n", ADDRESS_IP, ADDRESS_PORT);

    // TBD

    return 0;
}

