#include "../include/bptree/heap_page_cache.h"
#include "../include/bptree/mira_page_cache.h"
#include "../include/bptree/tree.h"
#include <iostream>
#include <iomanip>
#include <fstream>
#include <string>
#include <vector>
#include <memory>
#include <random>
#include <chrono>
#include <thread>
#include <functional>
#include <numeric>
#include <algorithm>

// Structure to store statistics from multiple runs
struct BenchmarkStats {
    std::vector<double> heap_insert_times;
    std::vector<double> mira_insert_times;
    std::vector<double> heap_lookup_times;
    std::vector<double> mira_lookup_times;
    std::vector<double> heap_range_times;
    std::vector<double> mira_range_times;
    std::vector<double> heap_mixed_times;
    std::vector<double> mira_mixed_times;
    
    // Final statistics for Mira cache
    uint64_t mira_hits = 0;
    uint64_t mira_misses = 0;
    uint64_t mira_promotions = 0;
    uint64_t mira_demotions = 0;
    uint64_t mira_evictions = 0;
};

// Function to measure execution time
template <typename Func>
double measure_time(Func func) {
    auto start = std::chrono::high_resolution_clock::now();
    func();
    auto end = std::chrono::high_resolution_clock::now();
    
    std::chrono::duration<double, std::milli> duration = end - start;
    return duration.count();
}

// Calculate statistics from a vector of timing results
double calculate_average(const std::vector<double>& times) {
    if (times.empty()) return 0.0;
    return std::accumulate(times.begin(), times.end(), 0.0) / times.size();
}

double calculate_median(std::vector<double> times) {
    if (times.empty()) return 0.0;
    std::sort(times.begin(), times.end());
    if (times.size() % 2 == 0) {
        return (times[times.size()/2 - 1] + times[times.size()/2]) / 2.0;
    } else {
        return times[times.size()/2];
    }
}

double calculate_percentile(std::vector<double> times, double percentile) {
    if (times.empty()) return 0.0;
    std::sort(times.begin(), times.end());
    int idx = static_cast<int>(times.size() * percentile / 100.0);
    return times[idx];
}

double calculate_min(const std::vector<double>& times) {
    if (times.empty()) return 0.0;
    return *std::min_element(times.begin(), times.end());
}

double calculate_max(const std::vector<double>& times) {
    if (times.empty()) return 0.0;
    return *std::max_element(times.begin(), times.end());
}

double calculate_stddev(const std::vector<double>& times) {
    if (times.size() <= 1) return 0.0;
    double avg = calculate_average(times);
    double sum_squared_diff = 0.0;
    for (double time : times) {
        double diff = time - avg;
        sum_squared_diff += diff * diff;
    }
    return std::sqrt(sum_squared_diff / (times.size() - 1));
}

// Run a single benchmark iteration
void run_benchmark_iteration(
    bptree::HeapPageCache* heap_cache, 
    bptree::MiraPageCache* mira_cache,
    BenchmarkStats& stats,
    int num_inserts,
    int num_lookups,
    int num_range_scans,
    int range_size,
    int num_mixed_ops,
    bool verbose = false) 
{
    // Create B+Trees with the caches
    bptree::BTree<256, int, int> heap_tree(heap_cache);
    bptree::BTree<256, int, int> mira_tree(mira_cache);
    
    // Create random number generators
    std::random_device rd;
    std::mt19937 gen(rd());
    
    // Insert Test
    if (verbose) {
        std::cout << "Inserting " << num_inserts << " key-value pairs...\n";
    }
    
    // HeapPageCache insert test
    if (verbose) std::cout << "  Testing HeapPageCache inserts... ";
    double heap_insert_time = measure_time([&]() {
        for (int i = 0; i < num_inserts; i++) {
            heap_tree.insert(i, i * 100);
        }
    });
    if (verbose) std::cout << "done in " << heap_insert_time << " ms\n";
    stats.heap_insert_times.push_back(heap_insert_time);
    
    // MiraPageCache insert test
    if (verbose) std::cout << "  Testing MiraPageCache inserts... ";
    double mira_insert_time = measure_time([&]() {
        for (int i = 0; i < num_inserts; i++) {
            mira_tree.insert(i, i * 100);
        }
    });
    if (verbose) std::cout << "done in " << mira_insert_time << " ms\n";
    stats.mira_insert_times.push_back(mira_insert_time);
    
    // Reset Mira stats after insertions
    mira_cache->reset_stats();
    
    // Point Lookup Test with skewed distribution
    if (verbose) {
        std::cout << "Performing " << num_lookups << " point lookups with skewed distribution...\n";
    }
    
    // Create skewed distribution (80% of lookups on 20% of data)
    const int HOT_DATA_SIZE = num_inserts / 5;  // 20% of data
    std::uniform_int_distribution<> hot_dist(0, HOT_DATA_SIZE - 1);
    std::uniform_int_distribution<> cold_dist(HOT_DATA_SIZE, num_inserts - 1);
    std::bernoulli_distribution hot_selector(0.8);  // 80% chance for hot data
    
    // For storing results
    std::vector<int> heap_values;
    std::vector<int> mira_values;
    
    // HeapPageCache lookup test
    if (verbose) std::cout << "  Testing HeapPageCache lookups... ";
    double heap_lookup_time = measure_time([&]() {
        for (int i = 0; i < num_lookups; i++) {
            int key = hot_selector(gen) ? hot_dist(gen) : cold_dist(gen);
            heap_values.clear();
            heap_tree.get_value(key, heap_values);
        }
    });
    if (verbose) std::cout << "done in " << heap_lookup_time << " ms\n";
    stats.heap_lookup_times.push_back(heap_lookup_time);
    
    // MiraPageCache lookup test
    if (verbose) std::cout << "  Testing MiraPageCache lookups... ";
    double mira_lookup_time = measure_time([&]() {
        for (int i = 0; i < num_lookups; i++) {
            int key = hot_selector(gen) ? hot_dist(gen) : cold_dist(gen);
            mira_values.clear();
            mira_tree.get_value(key, mira_values);
        }
    });
    if (verbose) std::cout << "done in " << mira_lookup_time << " ms\n";
    stats.mira_lookup_times.push_back(mira_lookup_time);
    
    // Reset Mira stats after lookups
    mira_cache->reset_stats();
    
    // Range Scan Test
    if (verbose) {
        std::cout << "Performing " << num_range_scans << " range scans (each with " << range_size << " elements)...\n";
    }
    
    std::uniform_int_distribution<> range_start_dist(0, num_inserts - range_size - 1);
    
    // HeapPageCache range scan test
    if (verbose) std::cout << "  Testing HeapPageCache range scans... ";
    double heap_range_time = measure_time([&]() {
        for (int i = 0; i < num_range_scans; i++) {
            int start_key = range_start_dist(gen);
            int count = 0;
            
            for (auto it = heap_tree.begin(start_key); it != heap_tree.end() && count < range_size; ++it) {
                count++;
            }
        }
    });
    if (verbose) std::cout << "done in " << heap_range_time << " ms\n";
    stats.heap_range_times.push_back(heap_range_time);
    
    // MiraPageCache range scan test
    if (verbose) std::cout << "  Testing MiraPageCache range scans... ";
    double mira_range_time = measure_time([&]() {
        for (int i = 0; i < num_range_scans; i++) {
            int start_key = range_start_dist(gen);
            int count = 0;
            
            for (auto it = mira_tree.begin(start_key); it != mira_tree.end() && count < range_size; ++it) {
                count++;
            }
        }
    });
    if (verbose) std::cout << "done in " << mira_range_time << " ms\n";
    stats.mira_range_times.push_back(mira_range_time);
    
    // Reset Mira stats after range scans
    mira_cache->reset_stats();
    
    // Mixed Workload Test
    if (verbose) {
        std::cout << "Performing " << num_mixed_ops << " mixed operations (70% lookups, 20% inserts, 10% scans)...\n";
    }
    
    std::uniform_int_distribution<> mixed_op_dist(1, 100);
    std::uniform_int_distribution<> insert_key_dist(num_inserts, num_inserts + num_mixed_ops);
    std::uniform_int_distribution<> scan_len_dist(10, 100);
    
    // HeapPageCache mixed workload test
    if (verbose) std::cout << "  Testing HeapPageCache mixed workload... ";
    double heap_mixed_time = measure_time([&]() {
        for (int i = 0; i < num_mixed_ops; i++) {
            int op = mixed_op_dist(gen);
            
            if (op <= 70) {
                // 70% chance: point lookup with skewed distribution
                int key = hot_selector(gen) ? hot_dist(gen) : cold_dist(gen);
                heap_values.clear();
                heap_tree.get_value(key, heap_values);
            } 
            else if (op <= 90) {
                // 20% chance: insert new key
                int key = insert_key_dist(gen);
                heap_tree.insert(key, key * 100);
            } 
            else {
                // 10% chance: small range scan
                int start_key = range_start_dist(gen);
                int scan_length = scan_len_dist(gen);
                int count = 0;
                
                for (auto it = heap_tree.begin(start_key); it != heap_tree.end() && count < scan_length; ++it) {
                    count++;
                }
            }
        }
    });
    if (verbose) std::cout << "done in " << heap_mixed_time << " ms\n";
    stats.heap_mixed_times.push_back(heap_mixed_time);
    
    // MiraPageCache mixed workload test
    if (verbose) std::cout << "  Testing MiraPageCache mixed workload... ";
    double mira_mixed_time = measure_time([&]() {
        for (int i = 0; i < num_mixed_ops; i++) {
            int op = mixed_op_dist(gen);
            
            if (op <= 70) {
                // 70% chance: point lookup with skewed distribution
                int key = hot_selector(gen) ? hot_dist(gen) : cold_dist(gen);
                mira_values.clear();
                mira_tree.get_value(key, mira_values);
            } 
            else if (op <= 90) {
                // 20% chance: insert new key
                int key = insert_key_dist(gen);
                mira_tree.insert(key, key * 100);
            } 
            else {
                // 10% chance: small range scan
                int start_key = range_start_dist(gen);
                int scan_length = scan_len_dist(gen);
                int count = 0;
                
                for (auto it = mira_tree.begin(start_key); it != mira_tree.end() && count < scan_length; ++it) {
                    count++;
                }
            }
        }
    });
    if (verbose) std::cout << "done in " << mira_mixed_time << " ms\n";
    stats.mira_mixed_times.push_back(mira_mixed_time);
    
    // Capture final Mira stats (only on the last iteration)
    stats.mira_hits = mira_cache->get_hits();
    stats.mira_misses = mira_cache->get_misses();
    stats.mira_promotions = mira_cache->get_promotions();
    stats.mira_demotions = mira_cache->get_demotions();
    stats.mira_evictions = mira_cache->get_evictions();
    
    // Flush caches at the end of each iteration
    heap_cache->flush_all_pages();
    mira_cache->flush_all_pages();
}

// Run multiple benchmark iterations and generate a report
void run_multi_benchmark(
    int num_iterations, 
    size_t cache_size, 
    size_t page_size,
    bool warm_up = true) 
{
    std::cout << "====== Page Cache Performance Comparison Benchmark ======\n";
    std::cout << "Running " << num_iterations << " iterations\n";
    std::cout << "Cache Size: " << cache_size << " pages\n";
    std::cout << "Page Size: " << page_size << " bytes\n";
    
    // Create output file for results
    std::ofstream result_file("./cache_benchmark_results.txt");
    result_file << "Page Cache Performance Benchmark Results\n";
    result_file << "======================================\n\n";
    result_file << "Configuration:\n";
    result_file << "  - Benchmark iterations: " << num_iterations << "\n";
    result_file << "  - Total cache size: " << cache_size << " pages\n";
    result_file << "  - Page size: " << page_size << " bytes\n";
    result_file << "  - Total memory usage: " << (cache_size * page_size) / (1024 * 1024) << " MB\n\n";
    
    // Parameters for each test
    const int NUM_INSERTS = 20000;  // Using smaller values for multiple iterations
    const int NUM_LOOKUPS = 10000;
    const int NUM_RANGE_SCANS = 100;
    const int RANGE_SIZE = 1000;
    const int NUM_MIXED_OPS = 10000;
    
    // Create the page caches (recreate for each iteration)
    std::cout << "\nPreparing caches for benchmark...\n";
    
    // Storage for all benchmark results
    BenchmarkStats stats;
    
    // Run a warm-up iteration first if requested (results discarded)
    if (warm_up) {
        std::cout << "Running warm-up iteration... ";
        
        auto heap_cache = std::make_unique<bptree::HeapPageCache>(
            "./tmp/heap_benchmark_warmup.heap", true, cache_size, page_size);
        auto mira_cache = std::make_unique<bptree::MiraPageCache>(
            "./tmp/mira_benchmark_warmup.heap", true, cache_size/4, cache_size*3/4);
            
        BenchmarkStats warmup_stats;
        run_benchmark_iteration(
            heap_cache.get(), mira_cache.get(), warmup_stats,
            NUM_INSERTS, NUM_LOOKUPS, NUM_RANGE_SCANS, RANGE_SIZE, NUM_MIXED_OPS, false);
            
        std::cout << "done\n";
    }
    
    // Run the actual benchmark iterations
    for (int i = 0; i < num_iterations; i++) {
        std::cout << "Running iteration " << (i+1) << "/" << num_iterations << "... ";
        
        // Create fresh caches for each iteration
        auto heap_cache = std::make_unique<bptree::HeapPageCache>(
            "./tmp/heap_benchmark_" + std::to_string(i) + ".heap", true, cache_size, page_size);
        auto mira_cache = std::make_unique<bptree::MiraPageCache>(
            "./tmp/mira_benchmark_" + std::to_string(i) + ".heap", true, cache_size/4, cache_size*3/4);
            
        // Run the benchmark
        run_benchmark_iteration(
            heap_cache.get(), mira_cache.get(), stats,
            NUM_INSERTS, NUM_LOOKUPS, NUM_RANGE_SCANS, RANGE_SIZE, NUM_MIXED_OPS, false);
            
        std::cout << "done\n";
        
        // Sleep briefly between iterations to let system stabilize
        if (i < num_iterations - 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    
    // Calculate statistics
    double heap_insert_avg = calculate_average(stats.heap_insert_times);
    double mira_insert_avg = calculate_average(stats.mira_insert_times);
    double heap_lookup_avg = calculate_average(stats.heap_lookup_times);
    double mira_lookup_avg = calculate_average(stats.mira_lookup_times);
    double heap_range_avg = calculate_average(stats.heap_range_times);
    double mira_range_avg = calculate_average(stats.mira_range_times);
    double heap_mixed_avg = calculate_average(stats.heap_mixed_times);
    double mira_mixed_avg = calculate_average(stats.mira_mixed_times);
    
    double heap_insert_median = calculate_median(stats.heap_insert_times);
    double mira_insert_median = calculate_median(stats.mira_insert_times);
    double heap_lookup_median = calculate_median(stats.heap_lookup_times);
    double mira_lookup_median = calculate_median(stats.mira_lookup_times);
    double heap_range_median = calculate_median(stats.heap_range_times);
    double mira_range_median = calculate_median(stats.mira_range_times);
    double heap_mixed_median = calculate_median(stats.heap_mixed_times);
    double mira_mixed_median = calculate_median(stats.mira_mixed_times);
    
    double heap_insert_p95 = calculate_percentile(stats.heap_insert_times, 95);
    double mira_insert_p95 = calculate_percentile(stats.mira_insert_times, 95);
    double heap_lookup_p95 = calculate_percentile(stats.heap_lookup_times, 95);
    double mira_lookup_p95 = calculate_percentile(stats.mira_lookup_times, 95);
    double heap_range_p95 = calculate_percentile(stats.heap_range_times, 95);
    double mira_range_p95 = calculate_percentile(stats.mira_range_times, 95);
    double heap_mixed_p95 = calculate_percentile(stats.heap_mixed_times, 95);
    double mira_mixed_p95 = calculate_percentile(stats.mira_mixed_times, 95);
    
    double heap_insert_stddev = calculate_stddev(stats.heap_insert_times);
    double mira_insert_stddev = calculate_stddev(stats.mira_insert_times);
    double heap_lookup_stddev = calculate_stddev(stats.heap_lookup_times);
    double mira_lookup_stddev = calculate_stddev(stats.mira_lookup_times);
    double heap_range_stddev = calculate_stddev(stats.heap_range_times);
    double mira_range_stddev = calculate_stddev(stats.mira_range_times);
    double heap_mixed_stddev = calculate_stddev(stats.heap_mixed_times);
    double mira_mixed_stddev = calculate_stddev(stats.mira_mixed_times);
    
    // Calculate percentage differences using median values (more robust than average)
    double insert_diff_pct = ((mira_insert_median - heap_insert_median) / heap_insert_median) * 100;
    double lookup_diff_pct = ((mira_lookup_median - heap_lookup_median) / heap_lookup_median) * 100;
    double range_diff_pct = ((mira_range_median - heap_range_median) / heap_range_median) * 100;
    double mixed_diff_pct = ((mira_mixed_median - heap_mixed_median) / heap_mixed_median) * 100;
    
    // Write results to file
    result_file << "\nPerformance Results (averaged over " << num_iterations << " iterations):\n";
    result_file << "--------------------------------------------------------\n";
    
    // Create a nice formatted comparison table
    result_file << std::fixed << std::setprecision(2);
    result_file << std::left << std::setw(20) << "Operation" 
               << std::right << std::setw(12) << "HeapCache" 
               << std::setw(12) << "MiraCache" 
               << std::setw(15) << "Diff (%)" 
               << std::setw(11) << "HC StdDev" 
               << std::setw(11) << "MC StdDev" << "\n";
    result_file << std::string(81, '-') << "\n";
    
    // Average results
    result_file << "AVERAGE TIMES (ms)\n";
    
    // Insert performance
    result_file << std::left << std::setw(20) << "Insert" 
               << std::right << std::setw(12) << heap_insert_avg
               << std::setw(12) << mira_insert_avg
               << std::setw(15) << ((mira_insert_avg - heap_insert_avg) / heap_insert_avg) * 100
               << std::setw(11) << heap_insert_stddev
               << std::setw(11) << mira_insert_stddev << "\n";
    
    // Point lookup performance
    result_file << std::left << std::setw(20) << "Point Lookup" 
               << std::right << std::setw(12) << heap_lookup_avg
               << std::setw(12) << mira_lookup_avg
               << std::setw(15) << ((mira_lookup_avg - heap_lookup_avg) / heap_lookup_avg) * 100
               << std::setw(11) << heap_lookup_stddev
               << std::setw(11) << mira_lookup_stddev << "\n";
    
    // Range scan performance
    result_file << std::left << std::setw(20) << "Range Scan" 
               << std::right << std::setw(12) << heap_range_avg
               << std::setw(12) << mira_range_avg
               << std::setw(15) << ((mira_range_avg - heap_range_avg) / heap_range_avg) * 100
               << std::setw(11) << heap_range_stddev
               << std::setw(11) << mira_range_stddev << "\n";
    
    // Mixed workload performance
    result_file << std::left << std::setw(20) << "Mixed Workload" 
               << std::right << std::setw(12) << heap_mixed_avg
               << std::setw(12) << mira_mixed_avg
               << std::setw(15) << ((mira_mixed_avg - heap_mixed_avg) / heap_mixed_avg) * 100
               << std::setw(11) << heap_mixed_stddev
               << std::setw(11) << mira_mixed_stddev << "\n\n";
    
    // Median results
    result_file << "MEDIAN TIMES (ms)\n";
    
    // Insert performance
    result_file << std::left << std::setw(20) << "Insert" 
               << std::right << std::setw(12) << heap_insert_median
               << std::setw(12) << mira_insert_median
               << std::setw(15) << insert_diff_pct << "\n";
    
    // Point lookup performance
    result_file << std::left << std::setw(20) << "Point Lookup" 
               << std::right << std::setw(12) << heap_lookup_median
               << std::setw(12) << mira_lookup_median
               << std::setw(15) << lookup_diff_pct << "\n";
    
    // Range scan performance
    result_file << std::left << std::setw(20) << "Range Scan" 
               << std::right << std::setw(12) << heap_range_median
               << std::setw(12) << mira_range_median
               << std::setw(15) << range_diff_pct << "\n";
    
    // Mixed workload performance
    result_file << std::left << std::setw(20) << "Mixed Workload" 
               << std::right << std::setw(12) << heap_mixed_median
               << std::setw(12) << mira_mixed_median
               << std::setw(15) << mixed_diff_pct << "\n\n";
    
    // 95th percentile results
    result_file << "95th PERCENTILE TIMES (ms)\n";
    
    // Insert performance
    result_file << std::left << std::setw(20) << "Insert" 
               << std::right << std::setw(12) << heap_insert_p95
               << std::setw(12) << mira_insert_p95
               << std::setw(15) << ((mira_insert_p95 - heap_insert_p95) / heap_insert_p95) * 100 << "\n";
    
    // Point lookup performance
    result_file << std::left << std::setw(20) << "Point Lookup" 
               << std::right << std::setw(12) << heap_lookup_p95
               << std::setw(12) << mira_lookup_p95
               << std::setw(15) << ((mira_lookup_p95 - heap_lookup_p95) / heap_lookup_p95) * 100 << "\n";
    
    // Range scan performance
    result_file << std::left << std::setw(20) << "Range Scan" 
               << std::right << std::setw(12) << heap_range_p95
               << std::setw(12) << mira_range_p95
               << std::setw(15) << ((mira_range_p95 - heap_range_p95) / heap_range_p95) * 100 << "\n";
    
    // Mixed workload performance
    result_file << std::left << std::setw(20) << "Mixed Workload" 
               << std::right << std::setw(12) << heap_mixed_p95
               << std::setw(12) << mira_mixed_p95
               << std::setw(15) << ((mira_mixed_p95 - heap_mixed_p95) / heap_mixed_p95) * 100 << "\n\n";
    
    // Mira-specific statistics from the last iteration
    result_file << "MiraPageCache Statistics (from last iteration):\n";
    result_file << "-----------------------------------------\n";
    result_file << "  - Cache hits: " << stats.mira_hits << "\n";
    result_file << "  - Cache misses: " << stats.mira_misses << "\n";
    result_file << "  - Hit ratio: " << (stats.mira_hits * 100.0) / (stats.mira_hits + stats.mira_misses) << "%\n";
    result_file << "  - Promotions: " << stats.mira_promotions << "\n";
    result_file << "  - Demotions: " << stats.mira_demotions << "\n";
    result_file << "  - Evictions: " << stats.mira_evictions << "\n\n";
    
    // Raw data for further analysis
    result_file << "\nRaw Data:\n";
    result_file << "---------\n";
    
    result_file << "HeapPageCache Insert Times (ms): ";
    for (double time : stats.heap_insert_times) {
        result_file << time << ", ";
    }
    result_file << "\n";
    
    result_file << "MiraPageCache Insert Times (ms): ";
    for (double time : stats.mira_insert_times) {
        result_file << time << ", ";
    }
    result_file << "\n";
    
    result_file << "HeapPageCache Lookup Times (ms): ";
    for (double time : stats.heap_lookup_times) {
        result_file << time << ", ";
    }
    result_file << "\n";
    
    result_file << "MiraPageCache Lookup Times (ms): ";
    for (double time : stats.mira_lookup_times) {
        result_file << time << ", ";
    }
    result_file << "\n";
    
    result_file << "HeapPageCache Range Times (ms): ";
    for (double time : stats.heap_range_times) {
        result_file << time << ", ";
    }
    result_file << "\n";
    
    result_file << "MiraPageCache Range Times (ms): ";
    for (double time : stats.mira_range_times) {
        result_file << time << ", ";
    }
    result_file << "\n";
    
    result_file << "HeapPageCache Mixed Times (ms): ";
    for (double time : stats.heap_mixed_times) {
        result_file << time << ", ";
    }
    result_file << "\n";
    
    result_file << "MiraPageCache Mixed Times (ms): ";
    for (double time : stats.mira_mixed_times) {
        result_file << time << ", ";
    }
    result_file << "\n\n";
    
    // Print conclusion based on median values (more reliable than means)
    result_file << "Conclusion:\n";
    result_file << "-----------\n";
    if (mira_lookup_median < heap_lookup_median && mira_mixed_median < heap_mixed_median) {
        result_file << "MiraPageCache outperforms HeapPageCache for both point lookups and mixed workloads, ";
        result_file << "achieving " << std::abs(lookup_diff_pct) << "% faster lookups and " 
                   << std::abs(mixed_diff_pct) << "% faster mixed operations.\n";
    } else if (mira_lookup_median < heap_lookup_median) {
        result_file << "MiraPageCache outperforms HeapPageCache for point lookups by " 
                   << std::abs(lookup_diff_pct) << "%, but is slower for other operations.\n";
    } else if (heap_lookup_median < mira_lookup_median) {
        result_file << "HeapPageCache outperforms MiraPageCache for point lookups by " 
                   << std::abs(lookup_diff_pct) << "%.\n";
    }
    
    result_file << "\nThe standard deviation of the measurements indicates ";
    if (mira_lookup_stddev < heap_lookup_stddev) {
        result_file << "MiraPageCache has more consistent performance than HeapPageCache.\n";
    } else {
        result_file << "HeapPageCache has more consistent performance than MiraPageCache.\n";
    }
    
    result_file.close();
    
    // Print summary to console
    std::cout << "\n========== Benchmark Summary ==========\n";
    std::cout << std::fixed << std::setprecision(2);
    
    std::cout << "HeapPageCache vs MiraPageCache performance comparison (median values):\n";
    std::cout << "  - Insert: " << heap_insert_median << " ms vs " << mira_insert_median 
             << " ms (" << (insert_diff_pct > 0 ? "+" : "") << insert_diff_pct << "%)\n";
    
    std::cout << "  - Point Lookup: " << heap_lookup_median << " ms vs " << mira_lookup_median 
             << " ms (" << (lookup_diff_pct > 0 ? "+" : "") << lookup_diff_pct << "%)\n";
    
    std::cout << "  - Range Scan: " << heap_range_median << " ms vs " << mira_range_median 
             << " ms (" << (range_diff_pct > 0 ? "+" : "") << range_diff_pct << "%)\n";
    
    std::cout << "  - Mixed Workload: " << heap_mixed_median << " ms vs " << mira_mixed_median 
             << " ms (" << (mixed_diff_pct > 0 ? "+" : "") << mixed_diff_pct << "%)\n";
    
    std::cout << "\nDetailed results written to: ./cache_benchmark_results.txt\n";
    std::cout << "=====================================\n";
}

int main(int argc, char* argv[]) {
    try {
        // Default values
        int num_iterations = 10;
        size_t cache_size = 4096;
        size_t page_size = 4096;
        bool warm_up = true;
        
        // Parse command line arguments if provided
        if (argc > 1) {
            num_iterations = std::stoi(argv[1]);
        }
        
        if (argc > 2) {
            cache_size = std::stoul(argv[2]);
        }
        
        if (argc > 3) {
            page_size = std::stoul(argv[3]);
        }
        
        // Run benchmark with specified iterations
        run_multi_benchmark(num_iterations, cache_size, page_size, warm_up);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}