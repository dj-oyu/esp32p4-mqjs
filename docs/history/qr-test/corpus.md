# QR Readability Benchmark

Host decoder: zxing-cpp. Canvas emulates the current 400x300 camera analysis image. Decode time is host-only.

| Payload | Modules | Side px | px/module | Success | p50 ms | p95 ms |
|---|---:|---:|---:|---:|---:|---:|
| combined | 65 | 140 | 1.92 | 0.0% | 3.17 | 3.17 |
| combined | 65 | 160 | 2.19 | 100.0% | 0.36 | 0.36 |
| combined | 65 | 180 | 2.47 | 100.0% | 0.36 | 0.36 |
| combined | 65 | 200 | 2.74 | 100.0% | 0.43 | 0.43 |
| combined | 65 | 220 | 3.01 | 100.0% | 0.39 | 0.39 |
| wifi | 49 | 140 | 2.46 | 100.0% | 2.13 | 2.13 |
| wifi | 49 | 160 | 2.81 | 100.0% | 0.37 | 0.37 |
| wifi | 49 | 180 | 3.16 | 100.0% | 0.36 | 0.36 |
| wifi | 49 | 200 | 3.51 | 100.0% | 0.33 | 0.33 |
| wifi | 49 | 220 | 3.86 | 100.0% | 0.37 | 0.37 |

## Clean-case floor

| Payload | Smallest clean side decoded | px/module |
|---|---:|---:|
| combined | 160 | 2.19 |
| wifi | 140 | 2.46 |
