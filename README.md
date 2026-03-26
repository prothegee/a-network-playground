# a network playground

playground to apply network application.

<br>

## http goal endpoint pattern

`/{lang+framework-if_any}`
- `/{lang-framework}`:
    - text/plain
    - content: `home`

- `/{lang+framework-if_any}/json`:
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
- `/lang+framework-if_any/echo*`:
    - application/json
    - e.g.:
    ```sh
    # all params as json
    curl "http://localhost:PORT/{lang+framework-if_any}/echo?text=1&foo=bar&z=&s=123"
    # → {"text":"1","foo":"bar","z":null,"s":"123"}

    # no params
    curl "http://localhost:PORT/{lang+framework-if_any}/echo"
    # → null

    # empty value
    curl "http://localhost:PORT/{lang+framework-if_any}/echo?key="
    # → {"key":null}

    # special chars (json escaped)
    curl "http://localhost:PORT/{lang+framework-if_any}/echo?msg=hello%22world"
    # → {"msg":"hello\"world"}
    ```

<br>

---

## bench test with [`wrk`](https://github.com/wg/wrk)

**Test Configuration:**
- `t: 2` threads, `c: 100` connections, `d: 10s` duration
- Endpoint: `/{lang+framework-if_any}/json`
- Content-Type: `application/json`

### 🏆 Ranking by Throughput (Requests/sec)

| Rank | lang                    | requests/sec | transfer/sec | latency avg |
| :--: | :--                     | :--          | :--          | :--         |
| 🥇   | Rust (STL)              | **267,551**  | 39.55MB      | 190.69us    |
| 🥈   | C (STL)                 | **267,147**  | 39.49MB      | 191.02us    |
| 🥉   | C++ (STL)               | **266,129**  | 39.34MB      | 192.00us    |
| 4    | Zig (STL)               | **265,204**  | 39.20MB      | 193.01us    |
| 5    | Zig (STL) async*        | **215,607**  | **84.30MB**  | 249.87us    |
| 6    | Go (STL)                | **209,554**  | 33.77MB      | 305.40us    |
| 7    | C++ (STL) async         | **84,831**   | 12.54MB      | 1.18ms      |

> *Zig async transfers ~2x data per response (likely larger payload), so direct req/sec comparison isn't apples-to-apples.

<br>

### Latency Statistics

| lang                    | latency avg   | latency stdev | latency max     | latency +/- stdev  |
| :--                     | :--           | :--           | :--             | :--                |
| Rust (STL)              | **190.69us**  | 53.20us       | 3.70ms          | 68.20%             |
| C (STL)                 | 191.02us      | 57.82us       | 5.95ms          | 65.16%             |
| C++ (STL)               | 192.00us      | 56.59us       | 3.76ms          | 63.19%             |
| Zig (STL)               | 193.01us      | 59.20us       | 6.88ms          | 99.23%             |
| Zig (STL) async*        | 249.87us      | 261.14us      | 19.52ms         | 98.70%             |
| Go (STL)                | 305.40us      | 240.34us      | 5.67ms          | 87.91%             |
| C++ (STL) async         | 1.18ms        | 56.02us       | 4.72ms          | 83.38%             |

<br>

### Throughput Statistics (Req/Sec)

| lang                    | req/sec avg   | req/sec stdev | req/sec max     | req/sec +/- stdev  |
| :--                     | :--           | :--           | :--             | :--                |
| Rust (STL)              | **134.50k**   | 3.66k         | 143.74k         | 67.00%             |
| C (STL)                 | 134.29k       | 4.58k         | 145.87k         | 68.00%             |
| C++ (STL)               | 133.81k       | 3.93k         | 143.80k         | 65.50%             |
| Zig (STL)               | 133.30k       | 5.04k         | 144.66k         | 64.50%             |
| Zig (STL) async*        | 108.37k       | 8.02k         | 124.12k         | 77.50%             |
| Go (STL)                | 105.35k       | 1.26k         | 107.90k         | 69.50%             |
| C++ (STL) async         | 42.63k        | 326.84        | 44.21k          | 72.00%             |

<br>

### Full Request Summary

| lang                    | requests stats                              | requests/sec | transfer/sec |
| :--                     | :--                                         | :--          | :--          |
| Rust (STL)              | 2,675,983 requests in 10.00s, 395.56MB read | **267,551**  | 39.55MB      |
| C (STL)                 | 2,671,828 requests in 10.00s, 394.95MB read | 267,147      | 39.49MB      |
| C++ (STL)               | 2,661,714 requests in 10.00s, 393.45MB read | 266,129      | 39.34MB      |
| Zig (STL)               | 2,652,658 requests in 10.00s, 392.11MB read | 265,204      | 39.20MB      |
| Zig (STL) async*        | 2,156,447 requests in 10.00s, **843.18MB** read | 215,607   | **84.30MB**  |
| Go (STL)                | 2,095,972 requests in 10.00s, 337.81MB read | 209,554      | 33.77MB      |
| C++ (STL) async         | 848,366 requests in 10.00s, 125.41MB read   | 84,831       | 12.54MB      |

<br>

### Key Observations

1. **Top 4 are tightly clustered**: Rust, C, C++, and Zig (STL) all deliver ~265–267k req/sec with sub-200µs avg latency — essentially equivalent performance for this JSON endpoint.
2. **Go is ~22% slower** than the top tier but still solid at ~209k req/sec; slightly higher latency variance.
3. **Zig async** shows lower req/sec but **2× transfer volume**, suggesting it serves a larger/more complex JSON payload — throughput by *bytes/sec* is actually highest here.
4. **C++ async** has significantly higher latency (1.18ms avg) and lowest throughput — likely due to async overhead or different implementation strategy; may benefit from tuning.
5. **Latency consistency**: Most implementations keep >63% of requests within 1 stdev; Zig variants show higher `% +/- stdev`, indicating more variable tail latency.

<br>

---

###### end of readme
