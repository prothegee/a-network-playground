# Benchmark Analysis: Language Performance Summary

## Final Rankings (Top → Lowest) by Composite Score
*Composite = 50% Latency Score + 50% Throughput Score (normalized 0-100)*

| Rank | Language/Framework     | Latency Avg   | Throughput        | Transfer/sec  | Composite Score   |
|------|------------------------|---------------|-------------------|---------------|-------------------|
| 1    | **C++ (Drogon)**       | 263.10μs      | **522,643** req/s | **70.78 MB**  | **93.53**         |
| 2    | **Rust (STL)**         | **239.34μs**  | 484,918 req/s     | 42.55 MB      | **91.77**         |
| 3    | **C++ (STL)**          | 262.75μs      | 482,700 req/s     | 42.35 MB      | **84.90**         |
| 4    | **C (STL)**            | 265.59μs      | 477,215 req/s     | 41.87 MB      | **82.93**         |
| 5    | **Zig (STL) async**    | 242.50μs      | 393,049 req/s     | 15.74 MB      | **70.86**         |
| 6    | **Zig (STL)**          | 359.85μs      | 485,540 req/s     | 42.60 MB      | **59.06**         |
| 7    | **Go (STL)**           | 422.83μs      | 293,531 req/s     | 29.39 MB      | **0.00**          |

---

## Metric-Specific Rankings

### Lowest Latency
1. **Rust (STL)**: 239.34μs
2. **Zig async**: 242.50μs  
3. **C++ (STL)**: 262.75μs
4. **C++ (Drogon)**: 263.10μs
5. **C (STL)**: 265.59μs
6. **Zig (STL)**: 359.85μs
7. **Go (STL)**: 422.83μs

### Highest Throughput
1. **C++ (Drogon)**: 522,643 req/s
2. **Zig (STL)**: 485,540 req/s
3. **Rust (STL)**: 484,918 req/s
4. **C++ (STL)**: 482,700 req/s
5. **C (STL)**: 477,215 req/s
6. **Zig async**: 393,049 req/s
7. **Go (STL)**: 293,531 req/s

### Efficiency
1. **Rust (STL)**: 2,026 req/s/μs
2. **C++ (Drogon)**: 1,986 req/s/μs
3. **C++ (STL)**: 1,837 req/s/μs
4. **C (STL)**: 1,797 req/s/μs
5. **Zig async**: 1,621 req/s/μs
6. **Zig (STL)**: 1,349 req/s/μs
7. **Go (STL)**: 694 req/s/μs

---

## Summary Statistics (All Languages)
| Metric                                    | Average       | Best                          | Worst                     |
|-------------------------------------------|---------------|-------------------------------|---------------------------|
| **Latency Avg**                           | 293.71μs      | 239.34μs (Rust)               | 422.83μs (Go)             |
| **Throughput**                            | 448,514 req/s | 522,643 req/s (C++ Drogon)    | 293,531 req/s (Go)        |
| **Transfer Rate**                         | 40.75 MB/s    | 70.78 MB/s (C++ Drogon)       | 15.74 MB/s (Zig async)    |
| **Latency Stability** (% within ±stdev)   | 90.78%        | 93.99% (Zig async)            | 85.32% (Go)               |

---

## 🔑 Key Insights

! **C++ (Drogon)** wins overall with the best balance: highest throughput (+15% vs avg) and competitive latency, though it transfers more data (70.78 MB/s) suggesting different response size.

! **Rust (STL)** has the **lowest latency** (239.34μs) and highest efficiency, making it ideal for latency-sensitive applications.

! **C/C++/Rust/Zig** (systems languages) cluster tightly in the top 5 for throughput (~477K-485K req/s), showing comparable performance.

? **Zig (STL) async** shows excellent latency (242.50μs) but lower throughput—async overhead may not pay off for this workload.

? **Zig (STL) sync** has high latency (359.85μs) despite good throughput—investigate stdev (747.72μs) for consistency issues.

X **Go (STL)** trails significantly in both latency (+44% vs avg) and throughput (-35% vs avg) for this benchmark.

---

>  **Recommendation**: 
> - Choose **Rust STL** for ultra-low latency requirements  
> - Choose **C/C++ STL** for balanced, predictable performance
> - Choose **C++ Drogon** for maximum throughput/bandwidth workloads
> - Profile **Go** further if ecosystem benefits outweigh ~35% performance gap

*Note: All tests ran for 10.10s. Transfer/sec variations may reflect different response payload sizes.*

