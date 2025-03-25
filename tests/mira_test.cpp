#include "../include/bptree/mira_page_cache.h"
#include "../include/bptree/tree.h"
#include <iostream>
#include <vector>
#include <stdexcept>
#include <random>
#include <chrono>
#include <iomanip>

// Function to measure execution time
template <typename Func>
double measure_time(Func func) {
    auto start = std::chrono::high_resolution_clock::now();
    func();
    auto end = std::chrono::high_resolution_clock::now();
    
    std::chrono::duration<double, std::milli> duration = end - start;
    return duration.count();
}

int main() {
    try {
        std::cout << "Testing Mira-style Page Cache Implementation\n";
        std::cout << "=============================================\n\n";
        
        // Create the Mira page cache with 1024 pages in hot tier and 3072 in cold tier
        bptree::MiraPageCache page_cache("./tmp/mira_tree.heap", true, 1024, 3072);
        std::cout << "Mira page cache created with:\n";
        std::cout << "  - Hot cache: 1024 pages\n";
        std::cout << "  - Cold cache: 3072 pages\n";
        std::cout << "  - Page size: " << page_cache.get_page_size() << " bytes\n\n";
        
        // Initialize B+Tree with the Mira page cache
        bptree::BTree<256, int, int> tree(&page_cache);
        std::cout << "B+Tree initialized with Mira page cache\n\n";
        
        // Random number generator for workload
        std::random_device rd;
        std::mt19937 gen(rd());
        
        // Insert a significant number of key-value pairs
        const int NUM_INSERTS = 200000;
        std::cout << "Inserting " << NUM_INSERTS << " key-value pairs...\n";
        
        double insert_time = measure_time([&]() {
            for (int i = 0; i < NUM_INSERTS; i++) {
                tree.insert(i, i * 100);
                
                // Print progress
                if ((i+1) % 10000 == 0) {
                    std::cout << "  " << (i+1) << " insertions completed\n";
                }
            }
        });
        
        std::cout << "Insertions completed in " << insert_time << " ms\n";
        std::cout << "Average time per insertion: " << insert_time / NUM_INSERTS << " ms\n\n";
        
        // Print initial cache statistics
        std::cout << "Cache statistics after insertions:\n";
        page_cache.print_stats();
        std::cout << "\n";
        
        // Reset statistics for the next test
        page_cache.reset_stats();
        
        // Perform point lookups with skewed distribution (80% of lookups on 20% of data)
        const int NUM_LOOKUPS = 50000;
        std::cout << "Performing " << NUM_LOOKUPS << " point lookups with skewed distribution...\n";
        
        // Create skewed distribution (80% of lookups on 20% of data)
        const int HOT_DATA_SIZE = NUM_INSERTS / 5;  // 20% of data
        std::uniform_int_distribution<> hot_dist(0, HOT_DATA_SIZE - 1);
        std::uniform_int_distribution<> cold_dist(HOT_DATA_SIZE, NUM_INSERTS - 1);
        std::bernoulli_distribution hot_selector(0.8);  // 80% chance for hot data
        
        std::vector<int> values;
        
        double lookup_time = measure_time([&]() {
            for (int i = 0; i < NUM_LOOKUPS; i++) {
                int key;
                if (hot_selector(gen)) {
                    // 80% of lookups: get from hot data
                    key = hot_dist(gen);
                } else {
                    // 20% of lookups: get from cold data
                    key = cold_dist(gen);
                }
                
                values.clear();
                tree.get_value(key, values);
                
                // Print progress
                if ((i+1) % 10000 == 0) {
                    std::cout << "  " << (i+1) << " lookups completed\n";
                }
            }
        });
        
        std::cout << "Lookups completed in " << lookup_time << " ms\n";
        std::cout << "Average time per lookup: " << lookup_time / NUM_LOOKUPS << " ms\n\n";
        
        // Print cache statistics after the lookups
        std::cout << "Cache statistics after lookups:\n";
        page_cache.print_stats();
        std::cout << "\n";
        
        // Reset statistics for range scan test
        page_cache.reset_stats();
        
        // Test range scan performance
        const int NUM_RANGE_SCANS = 100;
        const int RANGE_SIZE = 1000;
        std::cout << "Performing " << NUM_RANGE_SCANS << " range scans (each with " << RANGE_SIZE << " elements)...\n";
        
        std::uniform_int_distribution<> range_start_dist(0, NUM_INSERTS - RANGE_SIZE - 1);
        
        double range_scan_time = measure_time([&]() {
            for (int i = 0; i < NUM_RANGE_SCANS; i++) {
                int start_key = range_start_dist(gen);
                int count = 0;
                
                for (auto it = tree.begin(start_key); it != tree.end() && count < RANGE_SIZE; ++it) {
                    count++;
                }
                
                // Print progress
                if ((i+1) % 20 == 0) {
                    std::cout << "  " << (i+1) << " range scans completed\n";
                }
            }
        });
        
        std::cout << "Range scans completed in " << range_scan_time << " ms\n";
        std::cout << "Average time per range scan: " << range_scan_time / NUM_RANGE_SCANS << " ms\n\n";
        
        // Print cache statistics after range scans
        std::cout << "Cache statistics after range scans:\n";
        page_cache.print_stats();
        std::cout << "\n";
        
        // Test mixed workload
        page_cache.reset_stats();
        
        const int NUM_MIXED_OPS = 50000;
        std::cout << "Performing " << NUM_MIXED_OPS << " mixed operations (70% lookups, 20% inserts, 10% scans)...\n";
        
        std::uniform_int_distribution<> mixed_op_dist(1, 100);
        std::uniform_int_distribution<> insert_key_dist(NUM_INSERTS, NUM_INSERTS + NUM_MIXED_OPS);
        std::uniform_int_distribution<> scan_len_dist(10, 100);
        
        double mixed_time = measure_time([&]() {
            for (int i = 0; i < NUM_MIXED_OPS; i++) {
                int op = mixed_op_dist(gen);
                
                if (op <= 70) {
                    // 70% chance: point lookup with skewed distribution
                    int key;
                    if (hot_selector(gen)) {
                        key = hot_dist(gen);
                    } else {
                        key = cold_dist(gen);
                    }
                    
                    values.clear();
                    tree.get_value(key, values);
                } 
                else if (op <= 90) {
                    // 20% chance: insert new key
                    int key = insert_key_dist(gen);
                    tree.insert(key, key * 100);
                } 
                else {
                    // 10% chance: small range scan
                    int start_key = range_start_dist(gen);
                    int scan_length = scan_len_dist(gen);
                    int count = 0;
                    
                    for (auto it = tree.begin(start_key); it != tree.end() && count < scan_length; ++it) {
                        count++;
                    }
                }
                
                // Print progress
                if ((i+1) % 10000 == 0) {
                    std::cout << "  " << (i+1) << " mixed operations completed\n";
                }
            }
        });
        
        std::cout << "Mixed workload completed in " << mixed_time << " ms\n";
        std::cout << "Average time per operation: " << mixed_time / NUM_MIXED_OPS << " ms\n\n";
        
        // Print final cache statistics
        std::cout << "Cache statistics after mixed workload:\n";
        page_cache.print_stats();
        std::cout << "\n";
        
        // Flush all pages and close
        std::cout << "Flushing all pages to disk...\n";
        page_cache.flush_all_pages();
        
        std::cout << "\nTest completed successfully!\n";
        std::cout << "=========================\n";
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}