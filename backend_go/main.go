package main

import (
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"regexp"
	"strings"
)

const ADDRESS = "0.0.0.0:9004"

// --------------------------------------------------------- //

const HTTP_CONTENT_TYPE_HINT = "Content-Type"

const HTTP_CONTENT_TYPE_HINT_TEXT_PLAIN = "text/plain"
const HTTP_CONTENT_TYPE_HINT_APPLICATION_JSON = "application/json"

const HTTP_RESP_MESSAGE_NOT_FOUND = "Method Not Allowed"
const HTTP_RESP_MESSAGE_METHOD_NOT_ALLOWED = "Method Not Allowed"
const HTTP_RESP_MESSAGE_INTERNAL_SERVER_ERROR = "Internal Server Error"

// --------------------------------------------------------- //

var (
	validValuePattern = regexp.MustCompile("^[a-zA-Z0-9_-]+$")
)

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

	w.Header().Set(HTTP_CONTENT_TYPE_HINT, HTTP_CONTENT_TYPE_HINT_APPLICATION_JSON)

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

	w.Header().Set(HTTP_CONTENT_TYPE_HINT, HTTP_CONTENT_TYPE_HINT_TEXT_PLAIN)

	io.WriteString(w, resp)
}

// n/a
func DynamicHandler1(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, HTTP_RESP_MESSAGE_METHOD_NOT_ALLOWED, http.StatusMethodNotAllowed)
		return
	}

	pathParts := strings.Split(r.URL.Path, "/")

	// maybe check len of pathParts and check each pathParts index then do something, i.e.:
	/*
	if len(pathParts) != 4 || pathParts[0] != "" || pathParts[1] != "go" ... and so on ... {
		http.Error(w, "Woa, Not Found", http.StatusNotFound)
		return
	}
	*/
	// or check depth for each pathParts as in the len

	epn2 := pathParts[2]
	if epn2 == "" || !validValuePattern.MatchString(epn2) {
		http.Error(w, HTTP_RESP_MESSAGE_NOT_FOUND, http.StatusNotFound)
		return
	}

	resp := fmt.Sprintf("this is /%s/%s\n", pathParts[1], epn2)

	w.Header().Set(HTTP_CONTENT_TYPE_HINT, HTTP_CONTENT_TYPE_HINT_TEXT_PLAIN)

	io.WriteString(w, resp)
}

// --------------------------------------------------------- //

func registerHandler() {
	http.HandleFunc(HomeHandlerPath, HomeHandler)
	http.HandleFunc(JsonHandlerPath, JsonHandler)
	http.HandleFunc("/go/", func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/go" {
			HomeHandler(w, r)
			return
		}
		if r.URL.Path == "/go/json" {
			JsonHandler(w, r)
			return
		}
		if strings.HasSuffix(r.URL.Path, "") {
			DynamicHandler1(w, r)
			return
		}
	})
}

// --------------------------------------------------------- //

func main() {
	fmt.Printf("backend_go: run on %s\n", ADDRESS)

	registerHandler()

	log.Fatal(http.ListenAndServe(ADDRESS, nil))
}

