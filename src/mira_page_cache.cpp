#include "../include/bptree/mira_page_cache.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <chrono>
#include <cmath>

namespace bptree {

MiraPageCache::MiraPageCache(std::string_view filename, bool create,
                             size_t hot_cache_size, size_t cold_cache_size,
                             double promotion_threshold, size_t page_size)
    : heap_file(std::make_unique<HeapFile>(filename, create, page_size)),
      hot_cache_size(hot_cache_size),
      cold_cache_size(cold_cache_size),
      promotion_threshold(promotion_threshold),
      admission_probability(0.1),
      force_miss_probability(0.0),
      page_size(page_size),
      start_time(std::chrono::steady_clock::now())
{
    // Initialize random engine for admission policy
    std::random_device rd;
    rng = std::mt19937(rd());
    dist = std::uniform_real_distribution<double>(0.0, 1.0);
    
    // Initialize statistics
    stats.reset();
}

MiraPageCache::~MiraPageCache() {
    try {
        flush_all_pages();
    } catch (std::exception& e) {
        std::cerr << "Error during destruction: " << e.what() << std::endl;
    }
}

uint64_t MiraPageCache::get_current_time() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        now - start_time).count();
}

Page* MiraPageCache::new_page(boost::upgrade_lock<Page>& lock) {
    // We need to create a new page
    std::lock_guard<std::mutex> file_guard(file_mutex);
    std::lock_guard<std::mutex> hot_guard(hot_mutex);
    std::lock_guard<std::mutex> cold_guard(cold_mutex);
    
    // Allocate a new page in the file
    PageID new_id = heap_file->new_page();
    
    // Create a new Page object
    std::unique_ptr<Page> page_ptr = std::make_unique<Page>(new_id, page_size);
    Page* result_page = page_ptr.get();
    
    // Create the lock now before we move the page
    lock = boost::upgrade_lock<Page>(*result_page);
    
    // Create the MiraPage wrapper with the page
    auto mira_page = std::make_unique<MiraPage>(std::move(page_ptr), get_current_time());
    
    // Always place new pages in the hot cache for simplicity
    // First check if we need to make room
    if (hot_cache.size() >= hot_cache_size) {
        PageID victim_id;
        if (evict_from_hot_cache_internal(victim_id)) {
            stats.evictions++;
        }
    }
    
    // Add to hot cache
    hot_cache.push_front(std::move(mira_page));
    hot_map[new_id] = hot_cache.begin();
    
    // Pin the page
    pin_counts[new_id] = 1;  // Set directly to 1
    result_page->pin();
    stats.inserts++;
    
    return result_page;
}

Page* MiraPageCache::fetch_page(PageID id, boost::upgrade_lock<Page>& lock) {
    // First try with just the hot cache lock
    {
        std::lock_guard<std::mutex> hot_guard(hot_mutex);
        
        auto hot_it = hot_map.find(id);
        if (hot_it != hot_map.end()) {
            stats.hits++;
            
            // Found in hot cache
            Page* page = hot_it->second->get()->get_page();
            
            // Create the lock on the page
            lock = boost::upgrade_lock<Page>(*page);
            
            // Pin the page and update stats
            pin_counts[id]++;
            page->pin();
            
            // Update access statistics
            hot_it->second->get()->increment_access_count();
            hot_it->second->get()->update_access_time(get_current_time());
            
            // Move to front of hot cache (most recently used)
            hot_cache.splice(hot_cache.begin(), hot_cache, hot_it->second);
            
            return page;
        }
    }
    
    // Then try with just the cold cache lock
    {
        std::lock_guard<std::mutex> cold_guard(cold_mutex);
        
        auto cold_it = cold_map.find(id);
        if (cold_it != cold_map.end()) {
            stats.hits++;
            
            // Found in cold cache
            Page* page = cold_it->second->get()->get_page();
            
            // Create the lock on the page
            lock = boost::upgrade_lock<Page>(*page);
            
            // Pin the page and update stats
            pin_counts[id]++;
            page->pin();
            
            // Update access statistics
            cold_it->second->get()->increment_access_count();
            cold_it->second->get()->update_access_time(get_current_time());
            
            // Move to front of cold cache (most recently used)
            cold_cache.splice(cold_cache.begin(), cold_cache, cold_it->second);
            
            return page;
        }
    }
    
    // Not in cache, try to read from disk
    try {
        // Need all locks in consistent order for safety
        std::lock_guard<std::mutex> file_guard(file_mutex);
        std::lock_guard<std::mutex> hot_guard(hot_mutex);
        std::lock_guard<std::mutex> cold_guard(cold_mutex);
        
        stats.misses++;
        
        // Create a new Page object
        std::unique_ptr<Page> page_ptr = std::make_unique<Page>(id, page_size);
        Page* result_page = page_ptr.get();
        
        // Create the lock now before we move the page
        lock = boost::upgrade_lock<Page>(*result_page);
        
        // Try to read the page from disk - might throw if page doesn't exist
        boost::upgrade_to_unique_lock<Page> ulock(lock);
        try {
            heap_file->read_page(result_page, ulock);
        } catch (IOException& e) {
            // Page doesn't exist - for special case of page ID 1, 
            // initialize it as a new page if we're creating a new file
            if (id == 1) {
                // Just initialize the page as empty, B-tree will handle the rest
            } else {
                // For other pages, rethrow the exception
                throw;
            }
        }
        
        // Create the MiraPage wrapper with the page
        auto mira_page = std::make_unique<MiraPage>(std::move(page_ptr), get_current_time());
        
        // Determine cache placement - always to cold cache unless it's page 1
        if (id == 1 || dist(rng) < admission_probability) {
            // Add to hot cache
            // First check if we need to make room
            if (hot_cache.size() >= hot_cache_size) {
                PageID victim_id;
                if (evict_from_hot_cache_internal(victim_id)) {
                    stats.evictions++;
                }
            }
            
            // Add to hot cache
            hot_cache.push_front(std::move(mira_page));
            hot_map[id] = hot_cache.begin();
        } else {
            // Add to cold cache
            // First check if we need to make room
            if (cold_cache.size() >= cold_cache_size) {
                PageID victim_id;
                if (evict_from_cold_cache_internal(victim_id)) {
                    stats.evictions++;
                }
            }
            
            // Add to cold cache
            cold_cache.push_front(std::move(mira_page));
            cold_map[id] = cold_cache.begin();
        }
        
        // Pin the page
        pin_counts[id] = 1;  // Set directly to 1
        result_page->pin();
        
        return result_page;
    } catch (IOException& e) {
        // Page doesn't exist in disk
        return nullptr;
    } catch (std::exception& e) {
        std::cerr << "Error in fetch_page: " << e.what() << std::endl;
        return nullptr;
    }
}

void MiraPageCache::pin_page(Page* page, boost::upgrade_lock<Page>& lock) {
    if (!page) return;
    
    PageID pid = page->get_id();
    
    // Need both locks since we don't know which cache the page is in
    std::lock_guard<std::mutex> hot_guard(hot_mutex);
    std::lock_guard<std::mutex> cold_guard(cold_mutex);
    
    // Increment pin count
    pin_counts[pid]++;
    page->pin();
}

void MiraPageCache::unpin_page(Page* page, bool dirty, boost::upgrade_lock<Page>& lock) {
    if (!page) return;
    
    // Set dirty flag if needed
    if (dirty) {
        page->set_dirty(true);
    }
    
    PageID pid = page->get_id();
    
    // Need both locks
    std::lock_guard<std::mutex> hot_guard(hot_mutex);
    std::lock_guard<std::mutex> cold_guard(cold_mutex);
    
    // Decrease pin count
    auto it = pin_counts.find(pid);
    if (it != pin_counts.end() && it->second > 0) {
        it->second--;
        page->unpin();
    }
}

void MiraPageCache::flush_page(Page* page, boost::upgrade_lock<Page>& lock) {
    if (!page || !page->is_dirty()) return;
    
    try {
        std::lock_guard<std::mutex> file_guard(file_mutex);
        heap_file->write_page(page, lock);
        page->set_dirty(false);
        stats.flushes++;
    } catch (std::exception& e) {
        std::cerr << "Error flushing page: " << e.what() << std::endl;
    }
}

void MiraPageCache::flush_all_pages() {
    // We'll use a simple approach: copy the pointers to pages we need to flush
    // then release all locks before actually flushing
    std::vector<Page*> pages_to_flush;
    
    // Collect dirty pages from hot cache
    {
        std::lock_guard<std::mutex> hot_guard(hot_mutex);
        
        for (const auto& mira_page_ptr : hot_cache) {
            Page* page = mira_page_ptr->get_page();
            if (page && page->is_dirty()) {
                pages_to_flush.push_back(page);
            }
        }
    }
    
    // Collect dirty pages from cold cache
    {
        std::lock_guard<std::mutex> cold_guard(cold_mutex);
        
        for (const auto& mira_page_ptr : cold_cache) {
            Page* page = mira_page_ptr->get_page();
            if (page && page->is_dirty()) {
                pages_to_flush.push_back(page);
            }
        }
    }
    
    // Now flush each page individually
    for (Page* page : pages_to_flush) {
        try {
            auto lock = boost::upgrade_lock<Page>(*page);
            flush_page(page, lock);
        } catch (std::exception& e) {
            // Log error but continue with other pages
            std::cerr << "Error during flush_all_pages: " << e.what() << std::endl;
        }
    }
}

void MiraPageCache::update_page_heat(MiraPage* mira_page) {
    // Update heat based on access frequency and recency
    uint64_t current_time = get_current_time();
    uint64_t time_diff = current_time - mira_page->get_access_time();
    
    // Avoid division by zero
    time_diff = std::max(time_diff, (uint64_t)1);
    
    // Heat formula: access_count / log(time_diff)
    double new_heat = mira_page->get_access_count() / std::log(time_diff + 1);
    mira_page->update_heat(new_heat);
}

MiraPage* MiraPageCache::find_in_hot_cache(PageID id) {
    auto it = hot_map.find(id);
    if (it != hot_map.end()) {
        return it->second->get();
    }
    return nullptr;
}

MiraPage* MiraPageCache::find_in_cold_cache(PageID id) {
    auto it = cold_map.find(id);
    if (it != cold_map.end()) {
        return it->second->get();
    }
    return nullptr;
}

// Original API methods delegating to internal implementations
void MiraPageCache::insert_to_hot_cache(std::unique_ptr<MiraPage> mira_page) {
    std::lock_guard<std::mutex> guard(hot_mutex);
    insert_to_hot_cache_internal(std::move(mira_page));
}

void MiraPageCache::insert_to_cold_cache(std::unique_ptr<MiraPage> mira_page) {
    std::lock_guard<std::mutex> guard(cold_mutex);
    insert_to_cold_cache_internal(std::move(mira_page));
}

bool MiraPageCache::evict_from_hot_cache(PageID& victim_id) {
    std::lock_guard<std::mutex> guard(hot_mutex);
    return evict_from_hot_cache_internal(victim_id);
}

bool MiraPageCache::evict_from_cold_cache(PageID& victim_id) {
    std::lock_guard<std::mutex> guard(cold_mutex);
    return evict_from_cold_cache_internal(victim_id);
}

void MiraPageCache::promote_to_hot_cache(std::unique_ptr<MiraPage> mira_page) {
    // Acquire both locks in correct order
    std::lock_guard<std::mutex> hot_guard(hot_mutex);
    std::lock_guard<std::mutex> cold_guard(cold_mutex);
    
    // Check if hot cache is full
    if (hot_cache.size() >= hot_cache_size) {
        PageID victim_id;
        if (evict_from_hot_cache_internal(victim_id)) {
            stats.evictions++;
        }
    }
    
    // Now insert to hot cache
    PageID id = mira_page->get_page()->get_id();
    hot_cache.push_front(std::move(mira_page));
    hot_map[id] = hot_cache.begin();
    stats.promotes++;
}

void MiraPageCache::maybe_demote_from_hot_cache() {
    // Lock both caches in correct order
    std::lock_guard<std::mutex> hot_guard(hot_mutex);
    std::lock_guard<std::mutex> cold_guard(cold_mutex);
    
    // Find a low heat page to demote to cold cache
    double min_heat = promotion_threshold;
    auto min_it = hot_cache.end();
    
    for (auto it = hot_cache.begin(); it != hot_cache.end(); ++it) {
        if ((*it)->get_heat() < min_heat) {
            PageID pid = (*it)->get_page()->get_id();
            auto pin_it = pin_counts.find(pid);
            
            // Only consider unpinned pages
            if (pin_it == pin_counts.end() || pin_it->second == 0) {
                min_heat = (*it)->get_heat();
                min_it = it;
            }
        }
    }
    
    // If found a page to demote
    if (min_it != hot_cache.end()) {
        PageID id = (*min_it)->get_page()->get_id();
        
        // Move to cold cache - first check if we need to make room
        if (cold_cache.size() >= cold_cache_size) {
            PageID victim_id;
            if (evict_from_cold_cache_internal(victim_id)) {
                stats.evictions++;
            }
        }
        
        // Now move the page
        std::unique_ptr<MiraPage> page_to_demote = std::move(*min_it);
        hot_map.erase(id);
        hot_cache.erase(min_it);
        
        // Add to cold cache
        cold_cache.push_front(std::move(page_to_demote));
        cold_map[id] = cold_cache.begin();
        stats.demotes++;
    }
}

// Internal implementations

void MiraPageCache::insert_to_hot_cache_internal(std::unique_ptr<MiraPage> mira_page) {
    // First, check if we need to make space
    if (hot_cache.size() >= hot_cache_size) {
        PageID victim_id;
        if (evict_from_hot_cache_internal(victim_id)) {
            stats.evictions++;
        }
    }
    
    // Now add the new page to hot cache
    PageID id = mira_page->get_page()->get_id();
    hot_cache.push_front(std::move(mira_page));
    hot_map[id] = hot_cache.begin();
}

void MiraPageCache::insert_to_cold_cache_internal(std::unique_ptr<MiraPage> mira_page) {
    // First, check if we need to make space
    if (cold_cache.size() >= cold_cache_size) {
        PageID victim_id;
        if (evict_from_cold_cache_internal(victim_id)) {
            stats.evictions++;
        }
    }
    
    // Now add the new page to cold cache
    PageID id = mira_page->get_page()->get_id();
    cold_cache.push_front(std::move(mira_page));
    cold_map[id] = cold_cache.begin();
}

bool MiraPageCache::evict_from_hot_cache_internal(PageID& victim_id) {
    // Find an unpinned page to evict from the hot cache
    for (auto it = hot_cache.rbegin(); it != hot_cache.rend(); ++it) {
        PageID pid = (*it)->get_page()->get_id();
        auto pin_it = pin_counts.find(pid);
        
        // Check if page is unpinned
        if (pin_it == pin_counts.end() || pin_it->second == 0) {
            // Can evict this page
            victim_id = pid;
            
            // Mark for later flushing if dirty
            if ((*it)->get_page()->is_dirty()) {
                (*it)->get_page()->set_dirty(true);
            }
            
            // Remove from hot cache
            auto map_it = hot_map.find(victim_id);
            if (map_it != hot_map.end()) {
                hot_map.erase(map_it);
            }
            
            // Convert reverse_iterator to normal iterator and erase
            auto base_it = --(it.base());
            hot_cache.erase(base_it);
            
            return true;
        }
    }
    
    // Could not find a page to evict
    return false;
}

bool MiraPageCache::evict_from_cold_cache_internal(PageID& victim_id) {
    // Find an unpinned page to evict from the cold cache
    for (auto it = cold_cache.rbegin(); it != cold_cache.rend(); ++it) {
        PageID pid = (*it)->get_page()->get_id();
        auto pin_it = pin_counts.find(pid);
        
        // Check if page is unpinned
        if (pin_it == pin_counts.end() || pin_it->second == 0) {
            // Can evict this page
            victim_id = pid;
            
            // Mark for later flushing if dirty
            if ((*it)->get_page()->is_dirty()) {
                (*it)->get_page()->set_dirty(true);
            }
            
            // Remove from cold cache
            auto map_it = cold_map.find(victim_id);
            if (map_it != cold_map.end()) {
                cold_map.erase(map_it);
            }
            
            // Convert reverse_iterator to normal iterator and erase
            auto base_it = --(it.base());
            cold_cache.erase(base_it);
            
            return true;
        }
    }
    
    // Could not find a page to evict
    return false;
}

void MiraPageCache::evict_pages_under_pressure(size_t num_pages_to_free) {
    size_t evicted_hot = 0;
    size_t evicted_cold = 0;
    
    // First try to evict from cold cache
    for (auto it = cold_cache.rbegin(); it != cold_cache.rend() && evicted_cold < num_pages_to_free/2;) {
        PageID pid = (*it)->get_page()->get_id();
        auto pin_it = pin_counts.find(pid);
        
        if (pin_it == pin_counts.end() || pin_it->second == 0) {
            // Safe to evict
            if ((*it)->get_page()->is_dirty()) {
                // Mark for later flushing
                (*it)->get_page()->set_dirty(true);
            }
            
            // Remove from cold cache
            auto map_it = cold_map.find(pid);
            if (map_it != cold_map.end()) {
                cold_map.erase(map_it);
            }
            
            // Convert reverse_iterator to normal iterator and erase
            auto base_it = --(it.base());
            cold_cache.erase(base_it);
            
            // Reset the iterator
            it = cold_cache.rbegin();
            
            evicted_cold++;
            stats.evictions++;
        } else {
            ++it;
        }
    }
    
    // Then try to evict from hot cache if needed
    for (auto it = hot_cache.rbegin(); it != hot_cache.rend() && evicted_hot < (num_pages_to_free - evicted_cold);) {
        PageID pid = (*it)->get_page()->get_id();
        auto pin_it = pin_counts.find(pid);
        
        if (pin_it == pin_counts.end() || pin_it->second == 0) {
            // Safe to evict
            if ((*it)->get_page()->is_dirty()) {
                // Mark for later flushing
                (*it)->get_page()->set_dirty(true);
            }
            
            // Remove from hot cache
            auto map_it = hot_map.find(pid);
            if (map_it != hot_map.end()) {
                hot_map.erase(map_it);
            }
            
            // Convert reverse_iterator to normal iterator and erase
            auto base_it = --(it.base());
            hot_cache.erase(base_it);
            
            // Reset the iterator
            it = hot_cache.rbegin();
            
            evicted_hot++;
            stats.evictions++;
        } else {
            ++it;
        }
    }
    
    std::cout << "  [Memory pressure] Evicted " << evicted_hot << " hot pages and " 
              << evicted_cold << " cold pages" << std::endl;
}

} // namespace bptree