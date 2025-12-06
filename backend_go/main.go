package main

import (
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
)

// --------------------------------------------------------- //

const HTTP_CONTENT_TYPE_HINT = "Content-Type"

const HTTP_CONTENT_TYPE_HINT_APP_TEXT = "text/plain"
const HTTP_CONTENT_TYPE_HINT_APP_JSON = "application/json"

const HTTP_RESP_MESSAGE_METHOD_NOT_ALLOWED = "Method Not Allowed"
const HTTP_RESP_MESSAGE_INTERNAL_SERVER_ERROR = "Internal Server Error"

// --------------------------------------------------------- //

type TypeRespJson struct {
	Decimal float64 `json:"decimal"`
	String string `json:"string"`
	Round int32 `json:"round"`
	Boolean bool `json:"boolean"`
}

// --------------------------------------------------------- //

// json hello
const JsonHandlerPath string = "/go/json"
func JsonHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, HTTP_RESP_MESSAGE_METHOD_NOT_ALLOWED, http.StatusMethodNotAllowed)
		return
	}

	w.Header().Set(HTTP_CONTENT_TYPE_HINT, HTTP_CONTENT_TYPE_HINT_APP_JSON)

	resp := TypeRespJson {
		Decimal: 3.14,
		String: "string",
		Round: 69,
		Boolean: true,
	}

	if err := json.NewEncoder(w).Encode(resp); err != nil {
		http.Error(w, HTTP_RESP_MESSAGE_INTERNAL_SERVER_ERROR, http.StatusInternalServerError)
	}
}

const HomeHandlerPath string = "/go"
func HomeHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, HTTP_RESP_MESSAGE_METHOD_NOT_ALLOWED, http.StatusMethodNotAllowed)
		return
	}

	resp := "home"

	w.Header().Set(HTTP_CONTENT_TYPE_HINT, HTTP_CONTENT_TYPE_HINT_APP_TEXT)

	io.WriteString(w, resp)
}

// --------------------------------------------------------- //

func registerHandler() {
	http.HandleFunc(HomeHandlerPath, HomeHandler)
	http.HandleFunc(JsonHandlerPath, JsonHandler)
}

// --------------------------------------------------------- //

func main() {
	var address, port = "0.0.0.0", ":9003"

	fmt.Printf("backend_go: run on %s%s\n", address, port)

	registerHandler()

	log.Fatal(http.ListenAndServe(port, nil))
}

