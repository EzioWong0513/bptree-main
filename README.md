# Page Cache Benchmark

This tool compares the performance of HeapPageCache and MiraPageCache.

## Building

```bash
make clean
make init
make
```

## Running

### Basic Run (10 iterations)

```bash
make benchmark
```

### Custom Run

```bash
./cache_benchmark [iterations] [cache_size] [page_size]

# Examples:
./cache_benchmark 100           # 100 iterations
./cache_benchmark 50 8192       # 50 iterations, 8192 pages
./cache_benchmark 20 4096 8192  # 20 iterations, 4096 pages, 8192 bytes per page
```

## Results

Results are saved to `cache_benchmark_results.txt` and include:
- Average, median, and 95th percentile times
- Performance difference percentages
- MiraCache hit/miss statistics

## Interpreting Results

- **Negative difference**: MiraPageCache is faster
- **Positive difference**: HeapPageCache is faster
- **Lower standard deviation**: More consistent performance

## Tests Performed

1. **Insert Test**: Adding keys to the B+Tree
2. **Point Lookup Test**: Retrieving individual keys (with skewed access pattern)
3. **Range Scan Test**: Retrieving ranges of consecutive keys
4. **Mixed Workload Test**: Combination of lookups (70%), inserts (20%), and scans (10%)