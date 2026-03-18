# a network playground

playground to apply network application.

<br>

## goal endpoint pattern

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
<br>

---

## bench test with [`wrk`](https://github.com/wg/wrk)

t: 6
c: 100
d: 10s
path: `/{lang+framework-if_any}`
content: text/plain

| lang                    | latency avg   | latency stdev | latency max     | latency +/- stdev  |
| :--                     | :--           | :--           | :--             | :--                |
| C    (STL)              | 265.59us      | 535.88us      |   5.63ms        |  91.51%            |
| C++  (STL)              | 262.75us      | 527.58us      |   5.56ms        |  91.49%            |
| C++  (Drogon Framework) | 263.10us      | 460.84us      |   5.21ms        |  90.54%            |
| Go   (STL)              | 422.83us      | 498.64us      |   6.89ms        |  85.32%            |
| Rust (STL)              | 239.34us      | 497.83us      |   9.89ms        |  92.60%            |
| Zig  (STL)              | 359.85us      | 747.72us      |  14.89ms        |  90.04%            |
| Zig  (STL) async        | 242.50us      | 384.36us      |   5.89ms        |  93.99%            |

| lang                    | req/sec avg   | req/sec stdev | req/sec max     | req/sec +/- stdev  |
| :--                     | :--           | :--           | :--             | :--                |
| C    (STL)              |  80.07k       |   5.44k       |  93.02k         |  69.26%            |
| C++  (STL)              |  81.26k       |   5.78k       | 118.84k         |  76.62%            |
| C++  (Drogon Framework) |  87.70k       |   4.19k       | 101.60k         |  71.74%            |
| Go   (STL)              |  49.83k       |   3.30k       |  58.66k         |  68.00%            |
| Rust (STL)              |  81.77k       |   7.27k       | 141.37k         |  79.30%            |
| Zig  (STL)              |  81.85k       |   3.79k       |  98.88k         |  80.30%            |
| Zig  (STL) async        |  65.84k       |   7.91k       |  84.21k         |  81.35%            |

| lang                    | requests stats                              | requests/sec | transfer/sec |
| :--                     | :--                                         | :--          | :--          |
| C    (STL)              | 4,819,780 requests in 10.10s, 422.88MB read | 477,214.78   |  41.87MB     |
| C++  (STL)              | 4,875,039 requests in 10.10s, 427.73MB read | 482,700.22   |  42.35MB     |
| C++  (Drogon Framework) | 5,278,544 requests in 10.10s, 714.83MB read | 522,643.02   |  70.78MB     |
| Go   (STL)              | 2,935,576 requests in 10.10s, 294.16MB read | 293,531.26   |  29.39MB     |
| Rust (STL)              | 4,897,314 requests in 10.10s, 429.68MB read | 484,917.72   |  42.55MB     |
| Zig  (STL)              | 4,903,795 requests in 10.10s, 430.25MB read | 485,539.59   |  42.60MB     |
| Zig  (STL) async        | 3,969,874 requests in 10.10s, 150.01MB read | 393,048.85   |  15.74MB     |

<br>

---

###### end of readme

