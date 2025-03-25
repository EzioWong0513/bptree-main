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
    flush_all_pages();
}

uint64_t MiraPageCache::get_current_time() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        now - start_time).count();
}

Page* MiraPageCache::new_page(boost::upgrade_lock<Page>& lock) {
    std::lock_guard<std::mutex> guard(file_mutex);
    
    // Check if we need to free space in cache
    if (size() >= hot_cache_size + cold_cache_size) {
        // Try to evict some pages
        evict_pages_under_pressure(10);
    }
    
    // Allocate a new page in the file
    PageID new_id = heap_file->new_page();
    
    // Create a new MiraPage for this PageID
    auto mira_page = create_new_mira_page(new_id, lock);
    
    // Pin the page (increases pin count)
    pin_page(mira_page->get_page(), lock);
    stats.inserts++;
    
    return mira_page->get_page();
}

Page* MiraPageCache::fetch_page(PageID id, boost::upgrade_lock<Page>& lock) {
    // Optional: simulate a cache miss with some probability
    if (force_miss_probability > 0.0) {
        if (dist(rng) < force_miss_probability) {
            stats.misses++;
            
            // Skip cache lookup, force reading from disk
            try {
                std::lock_guard<std::mutex> file_guard(file_mutex);
                auto mira_page = create_new_mira_page(id, lock);
                
                if (!mira_page) {
                    return nullptr;
                }
                
                boost::upgrade_to_unique_lock<Page> ulock(lock);
                heap_file->read_page(mira_page->get_page(), ulock);
                
                // Pin the page after reading
                pin_page(mira_page->get_page(), lock);
                
                return mira_page->get_page();
            } catch (IOException& e) {
                std::cerr << "Error reading page: " << e.what() << std::endl;
                return nullptr;
            }
        }
    }

    MiraPage* mira_page = nullptr;
    
    // First check the hot cache
    mira_page = find_in_hot_cache(id);
    if (mira_page) {
        stats.hits++;
        lock = boost::upgrade_lock<Page>(*mira_page->get_page());
        pin_page(mira_page->get_page(), lock);
        
        // Update access statistics
        mira_page->increment_access_count();
        mira_page->update_access_time(get_current_time());
        update_page_heat(mira_page);
        
        return mira_page->get_page();
    }
    
    // Then check the cold cache
    mira_page = find_in_cold_cache(id);
    if (mira_page) {
        stats.hits++;
        lock = boost::upgrade_lock<Page>(*mira_page->get_page());
        pin_page(mira_page->get_page(), lock);
        
        // Update access statistics
        mira_page->increment_access_count();
        mira_page->update_access_time(get_current_time());
        update_page_heat(mira_page);
        
        // Potentially promote to hot cache based on heat
        if (mira_page->get_heat() > promotion_threshold) {
            // Use a separate lock scope to handle promotion
            {
                std::lock_guard<std::mutex> hot_guard(hot_mutex);
                std::lock_guard<std::mutex> cold_guard(cold_mutex);
                
                auto it = cold_map.find(id);
                if (it != cold_map.end()) {
                    std::unique_ptr<MiraPage> page_to_promote = std::move(*it->second);
                    cold_cache.erase(it->second);
                    cold_map.erase(it);
                    
                    promote_to_hot_cache(std::move(page_to_promote));
                    stats.promotes++;
                }
            }
        }
        
        return mira_page->get_page();
    }
    
    // Page not in cache, load from disk
    stats.misses++;
    
    // Create a new MiraPage
    try {
        std::lock_guard<std::mutex> file_guard(file_mutex);
        mira_page = create_new_mira_page(id, lock);
        
        if (!mira_page) {
            // If page creation failed, return null
            return nullptr;
        }
        
        boost::upgrade_to_unique_lock<Page> ulock(lock);
        heap_file->read_page(mira_page->get_page(), ulock);
        
        // Pin the page after reading
        pin_page(mira_page->get_page(), lock);
        
        return mira_page->get_page();
    } catch (IOException& e) {
        std::cerr << "Error reading page: " << e.what() << std::endl;
        return nullptr;
    }
}

void MiraPageCache::pin_page(Page* page, boost::upgrade_lock<Page>& lock) {
    if (!page) return;
    
    PageID pid = page->get_id();
    
    // First increment the pin count
    page->pin();
    
    // Then update our tracking
    {
        std::lock_guard<std::mutex> guard(hot_mutex);
        std::lock_guard<std::mutex> cold_guard(cold_mutex); 
        
        pin_counts[pid]++;
        
        // Remove from LRU lists if necessary
        auto hot_it = hot_map.find(pid);
        if (hot_it != hot_map.end()) {
            if (pin_counts[pid] == 1) {
                // First pin, move to front of hot cache (most recently used position)
                hot_cache.splice(hot_cache.begin(), hot_cache, hot_it->second);
            }
        }
        
        auto cold_it = cold_map.find(pid);
        if (cold_it != cold_map.end()) {
            if (pin_counts[pid] == 1) {
                // First pin, move to front of cold cache
                cold_cache.splice(cold_cache.begin(), cold_cache, cold_it->second);
            }
        }
    }
}

void MiraPageCache::unpin_page(Page* page, bool dirty, boost::upgrade_lock<Page>& lock) {
    if (!page) return;
    
    // Set dirty flag if needed
    if (dirty) {
        page->set_dirty(true);
    }
    
    PageID pid = page->get_id();
    
    // Decrease pin count
    {
        std::lock_guard<std::mutex> hot_guard(hot_mutex);
        std::lock_guard<std::mutex> cold_guard(cold_mutex);
        
        auto it = pin_counts.find(pid);
        if (it != pin_counts.end() && it->second > 0) {
            it->second--;
            page->unpin();
        }
    }
    
    // If dirty and unpinned, consider flushing
    if (page->is_dirty() && pin_counts[pid] == 0) {
        // Use a separate scope for flushing to avoid holding locks too long
        flush_page(page, lock);
    }
}

void MiraPageCache::flush_page(Page* page, boost::upgrade_lock<Page>& lock) {
    if (page->is_dirty()) {
        std::lock_guard<std::mutex> file_guard(file_mutex);
        heap_file->write_page(page, lock);
        page->set_dirty(false);
        stats.flushes++;
    }
}

void MiraPageCache::flush_all_pages() {
    // Flush hot cache
    {
        std::lock_guard<std::mutex> guard(hot_mutex);
        for (auto& mira_page_ptr : hot_cache) {
            auto page = mira_page_ptr->get_page();
            auto lock = boost::upgrade_lock<Page>(*page);
            if (page->is_dirty()) {
                flush_page(page, lock);
            }
        }
    }
    
    // Flush cold cache
    {
        std::lock_guard<std::mutex> guard(cold_mutex);
        for (auto& mira_page_ptr : cold_cache) {
            auto page = mira_page_ptr->get_page();
            auto lock = boost::upgrade_lock<Page>(*page);
            if (page->is_dirty()) {
                flush_page(page, lock);
            }
        }
    }
}

MiraPage* MiraPageCache::create_new_mira_page(PageID id, boost::upgrade_lock<Page>& lock) {
    // Create a new Page object
    auto page = std::make_unique<Page>(id, page_size);
    lock = boost::upgrade_lock<Page>(*page);
    
    // Create MiraPage wrapper with initial settings
    auto mira_page = std::make_unique<MiraPage>(std::move(page), get_current_time());
    auto* result_page = mira_page->get_page();
    
    // Determine if it should go to hot or cold cache
    // Use admission policy based on random probability
    if (dist(rng) < admission_probability) {
        // Goes to hot cache directly (small chance)
        std::lock_guard<std::mutex> guard(hot_mutex);
        insert_to_hot_cache(std::move(mira_page));
    } else {
        // Goes to cold cache first (most likely)
        std::lock_guard<std::mutex> guard(cold_mutex);
        insert_to_cold_cache(std::move(mira_page));
    }
    
    // Find and return the newly added page
    MiraPage* result = nullptr;
    
    if ((result = find_in_hot_cache(id)) != nullptr) {
        return result;
    } else if ((result = find_in_cold_cache(id)) != nullptr) {
        return result;
    }
    
    std::cerr << "Error: Newly created page not found in cache!" << std::endl;
    return nullptr;
}

void MiraPageCache::update_page_heat(MiraPage* mira_page) {
    // Update heat based on access frequency and recency
    // Higher heat = more important to keep in cache
    uint64_t current_time = get_current_time();
    uint64_t time_diff = current_time - mira_page->get_access_time();
    
    // Avoid division by zero
    time_diff = std::max(time_diff, (uint64_t)1);
    
    // Heat formula: access_count / log(time_diff)
    double new_heat = mira_page->get_access_count() / std::log(time_diff + 1);
    mira_page->update_heat(new_heat);
}

MiraPage* MiraPageCache::find_in_hot_cache(PageID id) {
    std::lock_guard<std::mutex> guard(hot_mutex);
    
    auto it = hot_map.find(id);
    if (it != hot_map.end()) {
        // Move to front of list (LRU policy)
        hot_cache.splice(hot_cache.begin(), hot_cache, it->second);
        return it->second->get();
    }
    
    return nullptr;
}

MiraPage* MiraPageCache::find_in_cold_cache(PageID id) {
    std::lock_guard<std::mutex> guard(cold_mutex);
    
    auto it = cold_map.find(id);
    if (it != cold_map.end()) {
        // Move to front of list (LRU policy)
        cold_cache.splice(cold_cache.begin(), cold_cache, it->second);
        return it->second->get();
    }
    
    return nullptr;
}

void MiraPageCache::insert_to_hot_cache(std::unique_ptr<MiraPage> mira_page) {
    // First, check if we need to make space
    if (hot_cache.size() >= hot_cache_size) {
        // Evict a page from hot cache
        PageID victim_id;
        if (evict_from_hot_cache(victim_id)) {
            stats.evictions++;
        }
    }
    
    // Now add the new page to hot cache
    PageID id = mira_page->get_page()->get_id();
    hot_cache.push_front(std::move(mira_page));
    hot_map[id] = hot_cache.begin();
}

void MiraPageCache::insert_to_cold_cache(std::unique_ptr<MiraPage> mira_page) {
    // First, check if we need to make space
    if (cold_cache.size() >= cold_cache_size) {
        // Evict a page from cold cache
        PageID victim_id;
        if (evict_from_cold_cache(victim_id)) {
            stats.evictions++;
        }
    }
    
    // Now add the new page to cold cache
    PageID id = mira_page->get_page()->get_id();
    cold_cache.push_front(std::move(mira_page));
    cold_map[id] = cold_cache.begin();
}

bool MiraPageCache::evict_from_hot_cache(PageID& victim_id) {
    // Find an unpinned page to evict from the hot cache
    auto it = hot_cache.rbegin();
    while (it != hot_cache.rend()) {
        PageID pid = (*it)->get_page()->get_id();
        auto pin_it = pin_counts.find(pid);
        
        // Check if page is unpinned
        if (pin_it == pin_counts.end() || pin_it->second == 0) {
            // Can evict this page
            victim_id = pid;
            
            // Write to disk if dirty
            if ((*it)->get_page()->is_dirty()) {
                auto lock = boost::upgrade_lock<Page>(*(*it)->get_page());
                std::lock_guard<std::mutex> file_guard(file_mutex);
                heap_file->write_page((*it)->get_page(), lock);
                (*it)->get_page()->set_dirty(false);
                stats.flushes++;
            }
            
            // We need to hold onto this page temporarily
            std::unique_ptr<MiraPage> temp_page;
            
            // Check if it should be demoted to cold cache
            if ((*it)->get_heat() < promotion_threshold) {
                // Move to cold cache - make a copy instead of moving the original yet
                temp_page = std::move(*it);
                
                // Remove from hot cache first
                auto map_it = hot_map.find(victim_id);
                if (map_it != hot_map.end()) {
                    hot_map.erase(map_it);
                }
                
                // Remove from list - need to convert reverse_iterator to normal iterator
                auto base_it = --(it.base());
                hot_cache.erase(base_it);
                
                // Now insert to cold cache
                std::lock_guard<std::mutex> cold_guard(cold_mutex);
                insert_to_cold_cache(std::move(temp_page));
                stats.demotes++;
                
                return true;
            }
            
            // If not demoting, simply remove from hot cache
            auto map_it = hot_map.find(victim_id);
            if (map_it != hot_map.end()) {
                hot_map.erase(map_it);
            }
            
            // Erase the list element - need to convert reverse_iterator to normal iterator
            auto base_it = --(it.base());
            hot_cache.erase(base_it);
            
            return true;
        }
        
        ++it;
    }
    
    // Could not find a page to evict
    return false;
}

bool MiraPageCache::evict_from_cold_cache(PageID& victim_id) {
    // Find an unpinned page to evict from the cold cache
    auto it = cold_cache.rbegin();
    while (it != cold_cache.rend()) {
        PageID pid = (*it)->get_page()->get_id();
        auto pin_it = pin_counts.find(pid);
        
        // Check if page is unpinned
        if (pin_it == pin_counts.end() || pin_it->second == 0) {
            // Can evict this page
            victim_id = pid;
            
            // Write to disk if dirty
            if ((*it)->get_page()->is_dirty()) {
                auto lock = boost::upgrade_lock<Page>(*(*it)->get_page());
                std::lock_guard<std::mutex> file_guard(file_mutex);
                heap_file->write_page((*it)->get_page(), lock);
                (*it)->get_page()->set_dirty(false);
                stats.flushes++;
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
        
        ++it;
    }
    
    // Could not find a page to evict
    return false;
}

void MiraPageCache::promote_to_hot_cache(std::unique_ptr<MiraPage> mira_page) {
    // Check if hot cache is full
    if (hot_cache.size() >= hot_cache_size) {
        // Try to demote a page from hot cache to make room
        maybe_demote_from_hot_cache();
    }
    
    // If still full, need to evict
    if (hot_cache.size() >= hot_cache_size) {
        PageID victim_id;
        if (evict_from_hot_cache(victim_id)) {
            stats.evictions++;
        }
    }
    
    // Now insert to hot cache
    PageID id = mira_page->get_page()->get_id();
    hot_cache.push_front(std::move(mira_page));
    hot_map[id] = hot_cache.begin();
}

void MiraPageCache::maybe_demote_from_hot_cache() {
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
        
        // Move to cold cache
        std::lock_guard<std::mutex> cold_guard(cold_mutex);
        insert_to_cold_cache(std::move(*min_it));
        
        // Remove from hot cache
        hot_map.erase(id);
        hot_cache.erase(min_it);
        
        stats.demotes++;
    }
}

void MiraPageCache::evict_pages_under_pressure(size_t num_pages_to_free) {
    size_t evicted_hot = 0;
    size_t evicted_cold = 0;
    
    // First try to evict from cold cache
    {
        std::lock_guard<std::mutex> cold_guard(cold_mutex);
        
        auto it = cold_cache.rbegin();  // Start from LRU end
        while (it != cold_cache.rend() && evicted_cold < num_pages_to_free/2) {
            PageID pid = (*it)->get_page()->get_id();
            auto pin_it = pin_counts.find(pid);
            
            if (pin_it == pin_counts.end() || pin_it->second == 0) {
                // Safe to evict
                if ((*it)->get_page()->is_dirty()) {
                    auto lock = boost::upgrade_lock<Page>(*(*it)->get_page());
                    std::lock_guard<std::mutex> file_guard(file_mutex);
                    heap_file->write_page((*it)->get_page(), lock);
                    (*it)->get_page()->set_dirty(false);
                    stats.flushes++;
                }
                
                auto map_it = cold_map.find(pid);
                if (map_it != cold_map.end()) {
                    cold_map.erase(map_it);
                }
                
                auto base_it = --(it.base());
                cold_cache.erase(base_it);
                
                evicted_cold++;
                stats.evictions++;
                
                // Need to restart iteration since we modified the container
                it = cold_cache.rbegin();
                continue;
            }
            
            ++it;
        }
    }
    
    // Then try to evict from hot cache if needed
    if (evicted_cold < num_pages_to_free/2) {
        std::lock_guard<std::mutex> hot_guard(hot_mutex);
        
        auto it = hot_cache.rbegin();  // Start from LRU end
        while (it != hot_cache.rend() && evicted_hot < (num_pages_to_free - evicted_cold)) {
            PageID pid = (*it)->get_page()->get_id();
            auto pin_it = pin_counts.find(pid);
            
            if (pin_it == pin_counts.end() || pin_it->second == 0) {
                // Safe to evict
                if ((*it)->get_page()->is_dirty()) {
                    auto lock = boost::upgrade_lock<Page>(*(*it)->get_page());
                    std::lock_guard<std::mutex> file_guard(file_mutex);
                    heap_file->write_page((*it)->get_page(), lock);
                    (*it)->get_page()->set_dirty(false);
                    stats.flushes++;
                }
                
                auto map_it = hot_map.find(pid);
                if (map_it != hot_map.end()) {
                    hot_map.erase(map_it);
                }
                
                auto base_it = --(it.base());
                hot_cache.erase(base_it);
                
                evicted_hot++;
                stats.evictions++;
                
                // Need to restart iteration since we modified the container
                it = hot_cache.rbegin();
                continue;
            }
            
            ++it;
        }
    }
    
    std::cout << "  [Memory pressure] Evicted " << evicted_hot << " hot pages and " 
              << evicted_cold << " cold pages" << std::endl;
}

} // namespace bptree