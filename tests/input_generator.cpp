#include <iostream>
#include <fstream>
#include <vector>
#include <random>
#include <chrono>

using namespace std;

enum class Distribution
{
    SEQUENTIAL,
    RANDOM,
    SKEWED,
};

void generate_data (const string &filename, size_t num_entries, Distribution dist)
{
    ofstream file(filename);
    if (!file.is_open())
    {
        cerr << "Failed to open file: " << filename << endl;
        return;
    }

    vector<int> keys(num_entries);
    vector<int> values(num_entries);

    if (dist == Distribution::SEQUENTIAL)
    {
        for (size_t i = 0; i < num_entries; i++)
        {
            keys[i] = i;
            values[i] = i * 100;
        
        }
    } else {
        mt19937 rng(chrono::system_clock::now().time_since_epoch().count());
        if (dist == Distribution::RANDOM)
        {
            uniform_int_distribution<int> dist(0, num_entries * 10);
            for (size_t i = 0; i < num_entries; i++){
                keys[i] = dist(rng);
                values[i] = keys[i] * 100;
            }
        } else if (dist == Distribution::SKEWED) {
            normal_distribution<double> dist(num_entries / 2.0, num_entries / 2.0);
            for (size_t i = 0; i < num_entries; i++) {
                keys[i] = max(0, min(static_cast<int>(num_entries * 10), static_cast<int>(dist(rng))));
                values[i] = keys[i] * 100;
            }
        }
    }

    for (size_t i = 0; i < num_entries; i++) {
        file << keys[i] << " " << values[i] << "\n";
    }

    file.close();
    cout << "Generated " << num_entries << " entries in " << filename << endl;
}

int main(int argc, char *argv[])
{
    if (argc < 4) {
        cerr << "Usage: " << argv[0] << " <output_file> <num_entries> <distribution: sequential/random/skewed>\n";
        return 1;
    }
    
    string filename = argv[1];
    size_t num_entries = stoull(argv[2]);
    string dist_type = argv[3];
    
    Distribution dist;
    if (dist_type == "sequential") {
        dist = Distribution::SEQUENTIAL;
    } else if (dist_type == "random") {
        dist = Distribution::RANDOM;
    } else if (dist_type == "skewed") {
        dist = Distribution::SKEWED;
    } else {
        cerr << "Invalid distribution type. Choose from: sequential, random, skewed\n";
        return 1;
    }

    generate_data(filename, num_entries, dist);
    return 0;
}