#include <iostream>
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>
#include <cstring>
#include <sched.h>
#include <unistd.h>
#include <algorithm>
#include <iomanip>
#include <cmath>

// Structure to hold ping-pong measurement data
struct PingPongSample {
    int64_t send_tsc;
    int64_t recv_tsc;
    
    // Calculate round-trip latency in TSC ticks
    int64_t round_trip_latency() const { return recv_tsc - send_tsc; }
};

// Inline assembly for fully serialized rdtsc
static inline uint64_t rdtsc_serialized() {
    uint32_t lo, hi;
    __asm__ __volatile__ (
        "lfence\n\t"     // Ensure all prior instructions complete
        "rdtsc\n\t"      // Read timestamp counter
        "lfence\n\t"     // Ensure rdtsc completes before subsequent instructions
        : "=a" (lo), "=d" (hi)
        :
        : "memory"
    );
    return ((uint64_t)hi << 32) | lo;
}

// Alternative: using mfence for stronger ordering (serializes both loads and stores)
static inline uint64_t rdtsc_mfence() {
    uint32_t lo, hi;
    __asm__ __volatile__ (
        "mfence\n\t"     // Full memory barrier
        "rdtsc\n\t"      // Read timestamp counter
        "lfence\n\t"     // Ensure rdtsc completes before subsequent instructions
        : "=a" (lo), "=d" (hi)
        :
        : "memory"
    );
    return ((uint64_t)hi << 32) | lo;
}

// Plain rdtsc for comparison
static inline uint64_t rdtsc_plain() {
    uint32_t lo, hi;
    __asm__ __volatile__ (
        "rdtsc\n\t"
        : "=a" (lo), "=d" (hi)
        :
        : "memory"
    );
    return ((uint64_t)hi << 32) | lo;
}

// Get TSC frequency by measuring against steady_clock
double get_tsc_frequency() {
    const int samples = 10;
    const auto duration = std::chrono::milliseconds(100);
    
    double freq_sum = 0.0;
    
    for (int i = 0; i < samples; i++) {
        auto start_time = std::chrono::steady_clock::now();
        uint64_t start_tsc = rdtsc_serialized();
        
        std::this_thread::sleep_for(duration);
        
        uint64_t end_tsc = rdtsc_serialized();
        auto end_time = std::chrono::steady_clock::now();
        
        auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
        uint64_t tsc_diff = end_tsc - start_tsc;
        
        double frequency = (double)tsc_diff / (elapsed_ns / 1e9);
        freq_sum += frequency;
    }
    
    return freq_sum / samples;
}

// Convert TSC ticks to nanoseconds
double tsc_to_ns(uint64_t tsc_ticks, double tsc_freq) {
    return (double)tsc_ticks / tsc_freq * 1e9;
}

// Set thread affinity to specific CPU core
bool set_thread_affinity(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
}

// Ping-pong test between two cores
class PingPongTest {
public:
    // Test mode enum
    enum class TimestampMode {
        PLAIN_RDTSC,
        LFENCE_BOTH_SIDES,
        MFENCE_LFENCE
    };

private:
    static constexpr int CACHE_LINE_SIZE = 64;    
    
    // Communication protocol atomics - properly aligned
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> ping_request{0};
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> pong_response{0};
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> pong_timestamp;
    alignas(CACHE_LINE_SIZE) std::atomic_bool stop_test{false};
    
    std::vector<PingPongSample> samples;
    int ping_core;
    int pong_core;
    int num_samples;
    double tsc_frequency;
    TimestampMode mode;
    
public:
    PingPongTest(int ping_core, int pong_core, int num_samples, TimestampMode mode = TimestampMode::LFENCE_BOTH_SIDES) 
        : ping_core(ping_core), pong_core(pong_core), num_samples(num_samples), mode(mode) {
        
        // Validate inputs
        if (ping_core < 0 || pong_core < 0 || ping_core == pong_core) {
            throw std::invalid_argument("Invalid core configuration");
        }
        
        samples.reserve(num_samples);
        
        std::cout << "Measuring TSC frequency..." << std::endl;
        tsc_frequency = get_tsc_frequency();
        std::cout << "TSC frequency: " << std::fixed << std::setprecision(2) 
                  << tsc_frequency / 1e9 << " GHz" << std::endl;
        
        const char* mode_names[] = {"Plain RDTSC", "LFENCE both sides", "MFENCE+LFENCE"};
        std::cout << "Timestamp mode: " << mode_names[static_cast<int>(mode)] << std::endl;
    }
    
    void run_test() {
        std::cout << "Starting ping-pong test between cores " << ping_core 
                  << " and " << pong_core << std::endl;
        
        // Start pong thread first
        std::thread pong_thread(&PingPongTest::pong_worker, this);
        
        // Small delay to ensure pong thread is ready
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        
        // Run ping worker on current thread
        ping_worker();
        
        // Signal stop and join
        stop_test.store(true, std::memory_order_release);
        pong_thread.join();
        
        analyze_results();
    }
    
private:
    uint64_t read_timestamp() {
        switch (mode) {
            case TimestampMode::PLAIN_RDTSC:
                return rdtsc_plain();
            case TimestampMode::LFENCE_BOTH_SIDES:
                return rdtsc_serialized();
            case TimestampMode::MFENCE_LFENCE:
                return rdtsc_mfence();
            default:
                return rdtsc_serialized();
        }
    }
    
    void ping_worker() {
        if (!set_thread_affinity(ping_core)) {
            std::cerr << "Failed to set ping thread affinity to core " << ping_core << std::endl;
            return;
        }
        
        std::cout << "Ping thread running on core " << ping_core << std::endl;
        
        // Warmup - let pong thread get ready
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        
        for (int i = 0; i < num_samples && !stop_test.load(std::memory_order_acquire); i++) {
            PingPongSample sample;
            
            // Use sequence number to avoid old responses
            uint64_t sequence = i + 1;
            
            // Send ping with sequence number
            sample.send_tsc = read_timestamp();
            ping_request.store(sequence, std::memory_order_release);
            
            // Wait for correct pong response
            uint64_t response;
            do {
                response = pong_response.load(std::memory_order_acquire);
                if (stop_test.load(std::memory_order_acquire)) {
                    return;
                }
            } while (response != sequence);
            
            // Record receipt time
            sample.recv_tsc = pong_timestamp.load(std::memory_order_acquire);
            
            samples.push_back(sample);

            rdtsc_serialized();
        }
    }
    
    void pong_worker() {
        if (!set_thread_affinity(pong_core)) {
            std::cerr << "Failed to set pong thread affinity to core " << pong_core << std::endl;
            return;
        }
        
        std::cout << "Pong thread running on core " << pong_core << std::endl;
        
        uint64_t last_request = 0;
        
        while (!stop_test.load(std::memory_order_acquire)) {
            
            // Wait for new ping request
            uint64_t current_request = ping_request.load(std::memory_order_acquire);
            auto v = read_timestamp();
            if (current_request > last_request) {
                // Respond immediately with the same sequence number                
                pong_timestamp.store(v, std::memory_order_release);
                pong_response.store(current_request, std::memory_order_release);
                last_request = current_request;
            }
        }
    }
    
    void analyze_results() {
        if (samples.empty()) {
            std::cout << "No samples collected!" << std::endl;
            return;
        }
        
        std::vector<double> latencies_ns;
        int negative_count = 0;
        int valid_count = 0;
        
        for (const auto& sample : samples) {
            int64_t latency_tsc = sample.round_trip_latency();
            
            if (latency_tsc < 0) {
                negative_count++;
                continue;
            }
            
            double latency_ns = tsc_to_ns(latency_tsc, tsc_frequency);
            
            // Filter out obvious outliers (> 1ms likely indicates scheduling issues)
            if (latency_ns > 1000000.0) {
                continue;
            }
            
            latencies_ns.push_back(latency_ns);
            valid_count++;
            
        }
        
        if (latencies_ns.empty()) {
            std::cout << "No valid samples collected!" << std::endl;
            return;
        }
        
        // Sort for percentile calculations
        std::sort(latencies_ns.begin(), latencies_ns.end());
        
        // Calculate statistics
        double sum = 0;
        for (double val : latencies_ns) {
            sum += val;
        }
        
        size_t n = latencies_ns.size();
        double min_val = latencies_ns[0];
        double median = latencies_ns[n / 2];
        double avg = sum / n;
        double p99 = latencies_ns[std::min(n - 1, static_cast<size_t>(n * 0.99))];
        double p999 = latencies_ns[std::min(n - 1, static_cast<size_t>(n * 0.999))];
        double max_val = latencies_ns[n - 1];
        
        // Calculate standard deviation
        double variance = 0;
        for (double val : latencies_ns) {
            variance += (val - avg) * (val - avg);
        }
        double stddev = std::sqrt(variance / n);
        
        const char* mode_names[] = {"Plain RDTSC", "LFENCE both sides", "MFENCE+LFENCE"};
        std::cout << "\n=== Results (" << mode_names[static_cast<int>(mode)] << ") ===" << std::endl;
        std::cout << "Cores: " << ping_core << " <-> " << pong_core << std::endl;
        std::cout << "Total samples: " << samples.size() << std::endl;
        std::cout << "Valid samples: " << valid_count << std::endl;
        std::cout << "Negative latencies: " << negative_count << " (" 
                  << (100.0 * negative_count / samples.size()) << "%)" << std::endl;
        std::cout << "Filtered samples: " << (samples.size() - valid_count - negative_count) << std::endl;
        
        // Estimate one-way latency (assuming symmetric)
        std::cout << "\nEstimated one-way latency (ns):" << std::endl;
        std::cout << "  Min: " << std::fixed << std::setprecision(1) << min_val << std::endl;
        std::cout << "  Median: " << median << std::endl;
        std::cout << "  Average: " << avg << std::endl;
        std::cout << "  Std Dev: " << stddev << std::endl;
        std::cout << "  P99: " << p99 << std::endl;
        std::cout << "  P99.9: " << p999 << std::endl;
        std::cout << "  Max: " << max_val << std::endl;
        
    }
};

int main(int argc, char* argv[]) {
    // Default parameters
    int ping_core = 0;
    int pong_core = 1;
    int num_samples = 100000;
    int mode_int = 1; // Default to LFENCE_BOTH_SIDES
    
    std::cout << "TSC Ping-Pong Latency Test" << std::endl;
    std::cout << "Usage: " << argv[0] << " [ping_core] [pong_core] [num_samples] [mode]" << std::endl;
    std::cout << "Modes: 0=Plain RDTSC, 1=LFENCE both sides, 2=MFENCE+LFENCE" << std::endl;
    
    // Parse and validate command line arguments
    if (argc >= 2) {
        ping_core = std::atoi(argv[1]);
        if (ping_core < 0) {
            std::cerr << "Error: ping_core must be non-negative" << std::endl;
            return 1;
        }
    }
    
    if (argc >= 3) {
        pong_core = std::atoi(argv[2]);
        if (pong_core < 0) {
            std::cerr << "Error: pong_core must be non-negative" << std::endl;
            return 1;
        }
    }
    
    if (argc >= 4) {
        num_samples = std::atoi(argv[3]);
    }
    
    if (argc >= 5) {
        mode_int = std::atoi(argv[4]);
        if (mode_int < 0 || mode_int > 2) {
            std::cerr << "Error: mode must be 0, 1, or 2" << std::endl;
            return 1;
        }
    }
    
    if (ping_core == pong_core) {
        std::cerr << "Error: ping_core and pong_core must be different" << std::endl;
        return 1;
    }
    
    std::cout << "Config: cores " << ping_core << " <-> " << pong_core 
              << ", " << num_samples << " samples, mode " << mode_int << std::endl;
    
    // Check if we have enough CPU cores
    int num_cores = std::thread::hardware_concurrency();
    if (ping_core >= num_cores || pong_core >= num_cores) {
        std::cerr << "Error: System only has " << num_cores << " cores" << std::endl;
        return 1;
    }
    
    try {
        PingPongTest::TimestampMode mode = static_cast<PingPongTest::TimestampMode>(mode_int);
        PingPongTest test(ping_core, pong_core, num_samples, mode);
        test.run_test();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}