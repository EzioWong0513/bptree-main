#ifndef _BPTREE_MIRA_PAGE_CACHE_H_
#define _BPTREE_MIRA_PAGE_CACHE_H_

#include "heap_file.h"
#include "page_cache.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <random>
#include <chrono>
#include <cmath>

#include <iostream>

namespace bptree {

// Statistics for monitoring cache performance
struct CacheStats {
    std::atomic<uint64_t> hits{0};
    std::atomic<uint64_t> misses{0};
    std::atomic<uint64_t> inserts{0};
    std::atomic<uint64_t> evictions{0};
    std::atomic<uint64_t> promotes{0};
    std::atomic<uint64_t> demotes{0};
    std::atomic<uint64_t> flushes{0};
    
    void reset() {
        hits = 0;
        misses = 0;
        inserts = 0;
        evictions = 0;
        promotes = 0;
        demotes = 0;
        flushes = 0;
    }
    
    void print() const {
        std::cout << "Cache Statistics:" << std::endl;
        std::cout << "  Hits: " << hits.load() << std::endl;
        std::cout << "  Misses: " << misses.load() << std::endl;
        std::cout << "  Hit ratio: " << (hits.load() * 100.0) / (hits.load() + misses.load()) << "%" << std::endl;
        std::cout << "  Inserts: " << inserts.load() << std::endl;
        std::cout << "  Evictions: " << evictions.load() << std::endl;
        std::cout << "  Promotions: " << promotes.load() << std::endl;
        std::cout << "  Demotions: " << demotes.load() << std::endl;
        std::cout << "  Flushes: " << flushes.load() << std::endl;
    }
};

// Class for a Page in the Mira cache
class MiraPage {
public:
    MiraPage(std::unique_ptr<Page> page, uint64_t access_time)
        : page(std::move(page)), 
          access_time(access_time),
          access_count(1),
          heat(1.0) {}
    
    Page* get_page() const { return page.get(); }
    
    uint64_t get_access_time() const { return access_time; }
    void update_access_time(uint64_t time) { access_time = time; }
    
    uint32_t get_access_count() const { return access_count; }
    void increment_access_count() { access_count++; }
    
    double get_heat() const { return heat; }
    void update_heat(double new_heat) { heat = new_heat; }

private:
    std::unique_ptr<Page> page;
    uint64_t access_time;  // Last access time
    uint32_t access_count; // Number of accesses
    double heat;           // Heat value for admission policy
};

// Main MiraPageCache class
class MiraPageCache : public AbstractPageCache {
public:
    MiraPageCache(std::string_view filename, bool create,
                  size_t hot_cache_size = 1024,
                  size_t cold_cache_size = 3072,
                  double promotion_threshold = 3.0,
                  size_t page_size = 4096);
    
    virtual ~MiraPageCache();

    // AbstractPageCache interface implementations
    virtual Page* new_page(boost::upgrade_lock<Page>& lock) override;
    virtual Page* fetch_page(PageID id, boost::upgrade_lock<Page>& lock) override;
    
    virtual void pin_page(Page* page, boost::upgrade_lock<Page>& lock) override;
    virtual void unpin_page(Page* page, bool dirty, boost::upgrade_lock<Page>& lock) override;
    
    virtual void flush_page(Page* page, boost::upgrade_lock<Page>& lock) override;
    virtual void flush_all_pages() override;
    
    virtual size_t size() const override { 
        return hot_cache.size() + cold_cache.size(); 
    }
    
    virtual size_t get_page_size() const override { 
        return page_size; 
    }
    
    // Additional Mira-specific methods
    void print_stats() const { stats.print(); }
    void reset_stats() { stats.reset(); }
    
    // Configure admission/eviction policies
    void set_promotion_threshold(double threshold) { promotion_threshold = threshold; }
    void set_admission_probability(double prob) { admission_probability = prob; }
    
    // Force cache misses for testing
    void set_miss_probability(double prob) { force_miss_probability = prob; }
    
    // Handle memory pressure explicitly
    void evict_pages_under_pressure(size_t num_pages_to_free);

private:
    std::unique_ptr<HeapFile> heap_file;
    size_t page_size;
    size_t hot_cache_size;
    size_t cold_cache_size;
    double promotion_threshold;
    double admission_probability;
    
    // Simulate memory constraints
    double force_miss_probability = 0.0;
    
    // Clock for timing access patterns
    std::chrono::steady_clock::time_point start_time;
    uint64_t get_current_time();
    
    // Random generator for admission policy
    std::mt19937 rng;
    std::uniform_real_distribution<double> dist;
    
    // Cache statistics
    CacheStats stats;
    
    // Mutexes for thread safety
    std::mutex hot_mutex;
    std::mutex cold_mutex;
    std::mutex file_mutex;
    
    // Hot cache (Most frequently accessed pages)
    std::list<std::unique_ptr<MiraPage>> hot_cache;
    std::unordered_map<PageID, std::list<std::unique_ptr<MiraPage>>::iterator> hot_map;
    
    // Cold cache (Less frequently accessed pages)
    std::list<std::unique_ptr<MiraPage>> cold_cache;
    std::unordered_map<PageID, std::list<std::unique_ptr<MiraPage>>::iterator> cold_map;
    
    // Pin counts for active pages
    std::unordered_map<PageID, int32_t> pin_counts;
    
    // Helper methods
    MiraPage* create_new_mira_page(PageID id, boost::upgrade_lock<Page>& lock);
    void update_page_heat(MiraPage* mira_page);
    
    // Cache management
    MiraPage* find_in_hot_cache(PageID id);
    MiraPage* find_in_cold_cache(PageID id);
    void insert_to_hot_cache(std::unique_ptr<MiraPage> mira_page);
    void insert_to_cold_cache(std::unique_ptr<MiraPage> mira_page);
    bool evict_from_hot_cache(PageID& victim_id);
    bool evict_from_cold_cache(PageID& victim_id);
    void promote_to_hot_cache(std::unique_ptr<MiraPage> mira_page);
    void maybe_demote_from_hot_cache();
};

} // namespace bptree

#endif