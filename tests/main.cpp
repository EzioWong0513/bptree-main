#include "../include/bptree/heap_page_cache.h"
#include "../include/bptree/tree.h"
#include <iostream>                 
#include <vector>                   
#include <stdexcept>                

int main() {
    try {
        
        bptree::HeapPageCache page_cache("./tmp/tree.heap", true, 4096);  // Changed to local dir for permissions
        std::cout << "Page cache created at ./tree.heap with 4096-byte pages.\n";

        bptree::BTree<256, int, int> tree(&page_cache);
        std::cout << "B+ tree of order 256 initialized.\n";

        std::cout << "Inserting 100 key-value pairs...\n";
        for (int i = 0; i < 100; i++) {
            tree.insert(i, i * 100);  
        }
        std::cout << "Insertions completed.\n";

        std::vector<int> values;
        tree.get_value(50, values);
        std::cout << "Point search for key 50: ";
        if (!values.empty()) {
            std::cout << "Found value(s): ";
            for (int val : values) {
                std::cout << val << " ";
            }
            std::cout << "\n";
        } else {
            std::cout << "Key 50 not found.\n";
        }

        std::cout << "\nRange search (keys >= 50):\n";
        int count = 0;
        for (auto it = tree.begin(50); it != tree.end(); ++it) {
            std::cout << "Key: " << it->first << ", Value: " << it->second << "\n";
            count++;
        }
        std::cout << "Found " << count << " entries in range search.\n";

        std::cout << "\nFull tree traversal:\n";
        count = 0;
        for (auto&& p : tree) {
            std::cout << "Key: " << p.first << ", Value: " << p.second << "\n";
            count++;
        }
        std::cout << "Total entries in tree: " << count << "\n";

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}