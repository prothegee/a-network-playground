package main

import (
	"fmt"
	"log"
	"github.com/gofiber/fiber/v2"
)

const ADDRESS = "0.0.0.0:9005"

// --------------------------------------------------------- //

const HTTP_RESP_MESSAGE_METHOD_NOT_ALLOWED = "Method Not Allowed"
const HTTP_RESP_MESSAGE_INTERNAL_SERVER_ERROR = "Internal Server Error"

// --------------------------------------------------------- //

const HomeHandlerPath string = "/go-fiber"
func HomeHandler(f *fiber.App) {
	f.All(HomeHandlerPath, func(c *fiber.Ctx) error {
		if c.Method() != "GET" {
			return c.SendString(HTTP_RESP_MESSAGE_METHOD_NOT_ALLOWED)
		}

		resp := "home"

		return c.SendString(resp)
	})
}

// --------------------------------------------------------- //

func registerHandler(f *fiber.App) {
	HomeHandler(f)
}

// --------------------------------------------------------- //

func main() {
	fmt.Printf("backend_go_fiber: run on %s\n", ADDRESS)

	app := fiber.New()

	registerHandler(app)

	log.Fatal(app.Listen(ADDRESS))
}

