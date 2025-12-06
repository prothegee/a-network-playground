# a network playground

playground to apply network application.

programming language implementations are:
- [?] c
- [?] cc (c++)
- [?] cc_drogon
- [?] go
- [?] go_fiber
- [?] rust
- [?] rust_ohkami

__*IMPORTANT*:__

- `{lang-framework}` mean:
    - it has lang with/without framework
    - says it doesn't use any framework and just only use stl in go, so:
        - `/go/foo`
    - if it using fiber, it should be
        - `/go-fiber/foo`

- check [goal endpoint pattern](#goal endpoint pattern), each section has it own purpose

- each framework should be able:
    - initialize plugin/s before main framework run
    - has middleware
    - c.r.u.d. operation (with postgresql)
    - c.r.u.d. session operation (redis)
    - message queue (redis)
    - simple chat app with websocket
    - consume topic with websocket (kafka)

<br>

## goal endpoint pattern

- `/{lang-framework}`:
    - text/plain
    - content: `home`

- `/{lang-framework}/json`:
    - application/json
    - content:
        ```json
        {
            "decimal": 3.14,
            "string": "string",
            "round": 69,
            "boolean": true
        }
        ```

<br>

---

###### end of readme

