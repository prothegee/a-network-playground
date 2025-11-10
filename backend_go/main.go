package main

import (
	"io"
	"fmt"
	"log"
	"net/http"
)

// --------------------------------------------------------- //

const HTTP_CONTENT_TYPE_HINT = "Content-Type"

const HTTP_CONTENT_TYPE_HINT_APP_TEXT = "text/plain"
const HTTP_CONTENT_TYPE_HINT_APP_HTML = "application/html"
const HTTP_CONTENT_TYPE_HINT_APP_JSON = "application/json"

// --------------------------------------------------------- //

// json grpc to cpp

// json grpc from cpp

// json hello

const HomeHandlerPath string = "/"
func HomeHandler(w http.ResponseWriter, r* http.Request) {
	var body = "home"

	w.Header().Set(HTTP_CONTENT_TYPE_HINT, HTTP_CONTENT_TYPE_HINT_APP_TEXT)

	io.WriteString(w, body)
}

// --------------------------------------------------------- //

func registerHandler() {
	http.HandleFunc(HomeHandlerPath, HomeHandler)
}

// --------------------------------------------------------- //

func main() {
	var address, port = "0.0.0.0", ":9003"

	fmt.Printf("main.go: run on %s%s\n", address, port)

	registerHandler()

	log.Fatal(http.ListenAndServe(port, nil))
}

