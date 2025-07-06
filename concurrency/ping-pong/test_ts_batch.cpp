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
#include <array>

// Convert TSC ticks to nanoseconds
double tsc_to_ns(uint64_t tsc_ticks, double tsc_freq) {
    return (double)tsc_ticks / tsc_freq * 1e9;
}


// Tap point structures from the design document
struct CompactTapRecord {
    uint32_t packed_data;            // timestamp(22) + duration_or_input_ref(9) + event_type(1)
    
    // Bit layout optimized for correlation:
    // Bits 31-10: Relative timestamp (22 bits * 100ns = 419ms range at 100ns precision)
    // Bits 9-1:   For INPUT: Duration (9 bits * 100ns = 51.1μs max)
    //             For OUTPUT: Input reference (9 bits = 0-255 input index, 256-511 special flags)
    // Bit 0:      Event type (0=input, 1=output)
    
    // Helper methods
    uint32_t get_relative_timestamp() const { return (packed_data >> 10) & 0x3FFFFF; }
    uint16_t get_duration_or_ref() const { return (packed_data >> 1) & 0x1FF; }
    bool is_output_event() const { return packed_data & 1; }
    
    void set_input_event(uint32_t rel_ts, uint16_t duration) {
        packed_data = (rel_ts << 10) | (duration << 1) | 0;
    }
    
    void set_output_event(uint32_t rel_ts, uint16_t input_ref) {
        packed_data = (rel_ts << 10) | (input_ref << 1) | 1;
    }
};

constexpr int kMaxTapEvents = 120; // 512 bytes, or 8 cache lines

struct BatchTapEvent {
    // Minimal header (32 bytes)
    uint64_t base_timestamp_ns;      // Base timestamp for all relative calculations
    uint64_t batch_id;               // Unique batch identifier
    uint64_t base_sequence_number;   // Base sequence number for this batch
    uint8_t  stage_id;               // Stage ID for all events in this batch  
    uint8_t  algo_engine_id;         // Algo engine ID for all events in this batch
    uint16_t record_count;           // Number of valid records (0-120)
    
    // Fixed-size record array
    std::array<CompactTapRecord, kMaxTapEvents> records;
};

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

// Simple tap point implementation
class TapPoint {
private:
    static constexpr int BATCH_SIZE = 64;
    std::vector<BatchTapEvent> batches;
    BatchTapEvent current_batch;
    uint64_t batch_counter = 0;
    uint64_t sequence_counter = 0;
    double tsc_frequency{};
    
public:
    TapPoint(uint8_t stage_id, uint8_t algo_engine_id, double tsc_frequency): tsc_frequency(tsc_frequency) {
        current_batch.stage_id = stage_id;
        current_batch.algo_engine_id = algo_engine_id;
        current_batch.record_count = 0;
        current_batch.batch_id = 0;
        current_batch.base_sequence_number = 0;
        current_batch.base_timestamp_ns = 0;
        batches.reserve(1000); // Pre-allocate for testing
    }
    
    // Tap input event (lightweight TSC)
    uint16_t tap_input_start(uint64_t event_id) {
        uint64_t now_tsc = tsc_to_ns(rdtsc_serialized(), tsc_frequency);
        uint16_t input_ref = current_batch.record_count;
        
        if (current_batch.record_count == 0) {
            current_batch.base_timestamp_ns = now_tsc;
            current_batch.batch_id = batch_counter++;
            current_batch.base_sequence_number = sequence_counter;
        }
        
        if (current_batch.record_count < kMaxTapEvents) {
            uint32_t rel_ts = (uint32_t)((now_tsc - current_batch.base_timestamp_ns) / 100); // 100ns precision
            current_batch.records[current_batch.record_count].set_input_event(rel_ts & 0x3FFFFF, 0);
            current_batch.record_count++;
        }
        
        sequence_counter++;
        return input_ref;
    }
    
    // Tap input event end (reliable TSC)
    void tap_input_end(uint64_t event_id, uint16_t input_index) {
        uint64_t now_tsc =  tsc_to_ns(rdtsc_serialized(), tsc_frequency);
        
        if (input_index < current_batch.record_count) {
            uint32_t rel_ts = (uint32_t)((now_tsc - current_batch.base_timestamp_ns) / 100);
            uint32_t start_ts = current_batch.records[input_index].get_relative_timestamp();
            uint16_t duration = std::min(511, (int)((rel_ts - start_ts) & 0x1FF));
            
            // Update the input record with duration
            current_batch.records[input_index].set_input_event(start_ts, duration);
        }
    }
    
    // Tap output event (lightweight TSC)
    void tap_output(uint64_t event_id, uint16_t input_ref) {
        uint64_t now_tsc =  tsc_to_ns(rdtsc_plain(), tsc_frequency);
        
        if (current_batch.record_count < kMaxTapEvents) {
            uint32_t rel_ts = (uint32_t)((now_tsc - current_batch.base_timestamp_ns) / 100);
            current_batch.records[current_batch.record_count].set_output_event(rel_ts & 0x3FFFFF, input_ref);
            current_batch.record_count++;
        }
    }
    
    // Flush current batch
    void flush_batch() {
        if (current_batch.record_count > 0) {
            batches.push_back(current_batch);
            current_batch.record_count = 0;
        }
    }
    
    // Get tap overhead statistics
    void analyze_tap_overhead(const std::string& stage_name, double tsc_freq) {
        std::cout << "\n=== " << stage_name << " Tap Point Analysis ===" << std::endl;
        std::cout << "Batches collected: " << batches.size() << std::endl;
        
        int total_records = 0;
        int input_records = 0;
        int output_records = 0;
        std::vector<double> processing_times_ns;
        std::vector<double> inter_event_times_ns;
        uint64_t last_event_time = 0;
        
        for (const auto& batch : batches) {
            total_records += batch.record_count;
            
            for (int i = 0; i < batch.record_count; i++) {
                const auto& record = batch.records[i];
                uint64_t event_time_ns = batch.base_timestamp_ns + (record.get_relative_timestamp() * 100);
                
                if (record.is_output_event()) {
                    output_records++;
                } else {
                    input_records++;
                    
                    // Calculate processing time for input events
                    uint16_t duration_100ns = record.get_duration_or_ref();
                    if (duration_100ns > 0) {
                        double processing_time_ns = duration_100ns * 100.0;
                        processing_times_ns.push_back(processing_time_ns);
                    }
                }
                
                // Calculate inter-event timing
                if (last_event_time > 0) {
                    double inter_event_ns = (double)(event_time_ns - last_event_time) / tsc_freq * 1e9;
                    if (inter_event_ns > 0 && inter_event_ns < 1000000) { // Filter outliers > 1ms
                        inter_event_times_ns.push_back(inter_event_ns);
                    }
                }
                last_event_time = event_time_ns;
            }
        }
        
        std::cout << "Total records: " << total_records << std::endl;
        std::cout << "Input records: " << input_records << std::endl;
        std::cout << "Output records: " << output_records << std::endl;
        std::cout << "Batch efficiency: " << std::fixed << std::setprecision(2) 
                  << (100.0 * total_records / (batches.size() * kMaxTapEvents)) << "%" << std::endl;
        
        // Analyze processing times
        if (!processing_times_ns.empty()) {
            std::sort(processing_times_ns.begin(), processing_times_ns.end());
            size_t n = processing_times_ns.size();
            
            double sum = 0;
            for (double val : processing_times_ns) {
                sum += val;
            }
            
            std::cout << "\nProcessing Time Statistics (ns):" << std::endl;
            std::cout << "  Events with timing: " << n << std::endl;
            std::cout << "  Min: " << std::fixed << std::setprecision(1) << processing_times_ns[0] << std::endl;
            std::cout << "  Median: " << processing_times_ns[n/2] << std::endl;
            std::cout << "  Average: " << (sum / n) << std::endl;
            std::cout << "  P95: " << processing_times_ns[std::min(n-1, (size_t)(n * 0.95))] << std::endl;
            std::cout << "  P99: " << processing_times_ns[std::min(n-1, (size_t)(n * 0.99))] << std::endl;
            std::cout << "  Max: " << processing_times_ns[n-1] << std::endl;
        }
        
        // Analyze inter-event timing
        if (!inter_event_times_ns.empty()) {
            std::sort(inter_event_times_ns.begin(), inter_event_times_ns.end());
            size_t n = inter_event_times_ns.size();
            
            double sum = 0;
            for (double val : inter_event_times_ns) {
                sum += val;
            }
            
            std::cout << "\nInter-Event Timing Statistics (ns):" << std::endl;
            std::cout << "  Valid intervals: " << n << std::endl;
            std::cout << "  Min: " << std::fixed << std::setprecision(1) << inter_event_times_ns[0] << std::endl;
            std::cout << "  Median: " << inter_event_times_ns[n/2] << std::endl;
            std::cout << "  Average: " << (sum / n) << std::endl;
            std::cout << "  P95: " << inter_event_times_ns[std::min(n-1, (size_t)(n * 0.95))] << std::endl;
            std::cout << "  P99: " << inter_event_times_ns[std::min(n-1, (size_t)(n * 0.99))] << std::endl;
            std::cout << "  Max: " << inter_event_times_ns[n-1] << std::endl;
            
            // Calculate event rate
            if (sum > 0) {
                double avg_interval_ns = sum / n;
                double events_per_sec = 1e9 / avg_interval_ns;
                std::cout << "  Event rate: " << std::fixed << std::setprecision(0) << events_per_sec << " events/sec" << std::endl;
            }
        }
        
        // Memory usage analysis
        size_t total_memory = batches.size() * sizeof(BatchTapEvent);
        std::cout << "\nMemory Usage:" << std::endl;
        std::cout << "  Total tap memory: " << total_memory << " bytes (" 
                  << std::fixed << std::setprecision(2) << (total_memory / 1024.0) << " KB)" << std::endl;
        std::cout << "  Bytes per event: " << std::fixed << std::setprecision(1) 
                  << (total_records > 0 ? (double)total_memory / total_records : 0) << std::endl;
    }
};

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


// Set thread affinity to specific CPU core
bool set_thread_affinity(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
}

// Ping-pong test between two cores WITH tap points
class PingPongTestWithTaps {
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
    
    // Tap points for each core
    std::unique_ptr<TapPoint> ping_tap;
    std::unique_ptr<TapPoint> pong_tap;
    
public:
    PingPongTestWithTaps(int ping_core, int pong_core, int num_samples, TimestampMode mode = TimestampMode::LFENCE_BOTH_SIDES) 
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

          // Initialize tap points
        ping_tap = std::make_unique<TapPoint>(1, 1, tsc_frequency); // stage_id=1, algo_engine_id=1
        pong_tap = std::make_unique<TapPoint>(2, 1, tsc_frequency); // stage_id=2, algo_engine_id=1
      

        std::cout << "Tap points: ENABLED" << std::endl;
        
    }
    
    void run_test() {
        std::cout << "Starting ping-pong test with tap points between cores " << ping_core 
                  << " and " << pong_core << std::endl;
        
        // Start pong thread first
        std::thread pong_thread(&PingPongTestWithTaps::pong_worker, this);
        
        // Small delay to ensure pong thread is ready
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        
        // Run ping worker on current thread
        ping_worker();
        
        // Signal stop and join
        stop_test.store(true, std::memory_order_release);
        pong_thread.join();
        
        // Flush tap points
        ping_tap->flush_batch();
        pong_tap->flush_batch();
        
        analyze_results();
        
        // Analyze tap overhead
        ping_tap->analyze_tap_overhead("PING STAGE", tsc_frequency);
        pong_tap->analyze_tap_overhead("PONG STAGE", tsc_frequency);
        
        // Calculate tap point overhead estimate
        calculate_tap_overhead_estimate();
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
            
            // TAP: Input event start
            uint16_t input_index = ping_tap->tap_input_start(i);
            
            // Use sequence number to avoid old responses
            uint64_t sequence = i + 1;
            
            // Send ping with sequence number
            sample.send_tsc = read_timestamp();
            ping_request.store(sequence, std::memory_order_release);
            
            // TAP: Output event
            ping_tap->tap_output(i, input_index);
            
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
            
            // TAP: Input event end
            ping_tap->tap_input_end(i, input_index);
            
            samples.push_back(sample);

            // Flush batch periodically
            if (i % 64 == 63) {
                ping_tap->flush_batch();
            }
        }
    }
    
    void pong_worker() {
        if (!set_thread_affinity(pong_core)) {
            std::cerr << "Failed to set pong thread affinity to core " << pong_core << std::endl;
            return;
        }
        
        std::cout << "Pong thread running on core " << pong_core << std::endl;
        
        uint64_t last_request = 0;
        int event_counter = 0;
        
        // Add variables to measure tap overhead
        std::vector<double> tap_overhead_normal_ns;
        std::vector<double> tap_overhead_flush_ns;
        
        while (!stop_test.load(std::memory_order_acquire)) {
            
            // Wait for new ping request
            uint64_t current_request = ping_request.load(std::memory_order_acquire);
            auto v = read_timestamp();
            if (current_request > last_request) {
                // Start measuring tap overhead
                uint64_t tap_start = rdtsc_serialized();
                
                // TAP: Input event start
                uint16_t input_index = pong_tap->tap_input_start(event_counter);
            
                // TAP: Output event
                pong_tap->tap_output(event_counter, input_index);
                
                // Respond immediately with the same sequence number                
                pong_timestamp.store(v, std::memory_order_release);
                pong_response.store(current_request, std::memory_order_release);
                last_request = current_request;
                
                event_counter++;

                 // TAP: Input event end
                pong_tap->tap_input_end(event_counter, input_index);
                
                // Flush batch periodically and measure separately
                if (event_counter % 64 == 0) {
                    pong_tap->flush_batch();
                    
                    // End measuring tap overhead (with flush)
                    uint64_t tap_end = rdtsc_serialized();
                    double tap_time_ns = tsc_to_ns(tap_end - tap_start, tsc_frequency);
                    if (tap_time_ns > 0 && tap_time_ns < 50000) { // Filter outliers > 50μs
                        tap_overhead_flush_ns.push_back(tap_time_ns);
                    }
                } else {
                    // End measuring tap overhead (normal case)
                    uint64_t tap_end = rdtsc_serialized();
                    double tap_time_ns = tsc_to_ns(tap_end - tap_start, tsc_frequency);
                    if (tap_time_ns > 0 && tap_time_ns < 50000) { // Filter outliers > 50μs
                        tap_overhead_normal_ns.push_back(tap_time_ns);
                    }
                }
            }
        }
        
        // Analyze pong tap overhead at the end
        if (!tap_overhead_normal_ns.empty()) {
            std::sort(tap_overhead_normal_ns.begin(), tap_overhead_normal_ns.end());
            size_t n = tap_overhead_normal_ns.size();
            double sum = 0;
            for (double val : tap_overhead_normal_ns) sum += val;
            
            std::cout << "\n=== Pong Worker Tap Overhead (Normal) ===" << std::endl;
            std::cout << "Samples: " << n << std::endl;
            std::cout << "Min: " << std::fixed << std::setprecision(1) << tap_overhead_normal_ns[0] << " ns" << std::endl;
            std::cout << "Median: " << tap_overhead_normal_ns[n/2] << " ns" << std::endl;
            std::cout << "Average: " << (sum / n) << " ns" << std::endl;
            std::cout << "P95: " << tap_overhead_normal_ns[std::min(n-1, (size_t)(n * 0.95))] << " ns" << std::endl;
            std::cout << "P99: " << tap_overhead_normal_ns[std::min(n-1, (size_t)(n * 0.99))] << " ns" << std::endl;
            std::cout << "Max: " << tap_overhead_normal_ns[n-1] << " ns" << std::endl;
        }
        
        if (!tap_overhead_flush_ns.empty()) {
            std::sort(tap_overhead_flush_ns.begin(), tap_overhead_flush_ns.end());
            size_t n = tap_overhead_flush_ns.size();
            double sum = 0;
            for (double val : tap_overhead_flush_ns) sum += val;
            
            std::cout << "\n=== Pong Worker Tap Overhead (With Flush) ===" << std::endl;
            std::cout << "Samples: " << n << std::endl;
            std::cout << "Min: " << std::fixed << std::setprecision(1) << tap_overhead_flush_ns[0] << " ns" << std::endl;
            std::cout << "Median: " << tap_overhead_flush_ns[n/2] << " ns" << std::endl;
            std::cout << "Average: " << (sum / n) << " ns" << std::endl;
            std::cout << "P95: " << tap_overhead_flush_ns[std::min(n-1, (size_t)(n * 0.95))] << " ns" << std::endl;
            std::cout << "P99: " << tap_overhead_flush_ns[std::min(n-1, (size_t)(n * 0.99))] << " ns" << std::endl;
            std::cout << "Max: " << tap_overhead_flush_ns[n-1] << " ns" << std::endl;
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
        std::cout << "\n=== Results WITH TAP POINTS (" << mode_names[static_cast<int>(mode)] << ") ===" << std::endl;
        std::cout << "Cores: " << ping_core << " <-> " << pong_core << std::endl;
        std::cout << "Total samples: " << samples.size() << std::endl;
        std::cout << "Valid samples: " << valid_count << std::endl;
        std::cout << "Negative latencies: " << negative_count << " (" 
                  << (100.0 * negative_count / samples.size()) << "%)" << std::endl;
        std::cout << "Filtered samples: " << (samples.size() - valid_count - negative_count) << std::endl;
        
        // Estimate one-way latency (assuming symmetric)
        std::cout << "\nEstimated one-way latency WITH TAPS (ns):" << std::endl;
        std::cout << "  Min: " << std::fixed << std::setprecision(1) << min_val << std::endl;
        std::cout << "  Median: " << median << std::endl;
        std::cout << "  Average: " << avg << std::endl;
        std::cout << "  Std Dev: " << stddev << std::endl;
        std::cout << "  P99: " << p99 << std::endl;
        std::cout << "  P99.9: " << p999 << std::endl;
        std::cout << "  Max: " << max_val << std::endl;
        
    }
    
    // Estimate tap point overhead by measuring tap operations
    void calculate_tap_overhead_estimate() {
        std::cout << "\n=== Tap Point Overhead Measurement ===" << std::endl;
        
        const int test_iterations = 10000;
        std::vector<double> tap_times_ns;
        
        // Measure time for tap operations
        for (int i = 0; i < test_iterations; i++) {
            uint64_t start = rdtsc_serialized();
            
            // Simulate tap sequence: input_start + output + input_end
            uint64_t dummy_tsc1 = rdtsc_plain();      // input_start
            uint64_t dummy_tsc2 = rdtsc_plain();      // output  
            uint64_t dummy_tsc3 = rdtsc_serialized(); // input_end
            
            uint64_t end = rdtsc_serialized();
            
            double tap_time_ns = tsc_to_ns(end - start, tsc_frequency);
            if (tap_time_ns > 0 && tap_time_ns < 10000) { // Filter outliers
                tap_times_ns.push_back(tap_time_ns);
            }
        }
        
        if (!tap_times_ns.empty()) {
            std::sort(tap_times_ns.begin(), tap_times_ns.end());
            size_t n = tap_times_ns.size();
            
            double sum = 0;
            for (double val : tap_times_ns) {
                sum += val;
            }
            
            std::cout << "Tap sequence timing (3 TSC reads + processing):" << std::endl;
            std::cout << "  Samples: " << n << std::endl;
            std::cout << "  Min: " << std::fixed << std::setprecision(1) << tap_times_ns[0] << " ns" << std::endl;
            std::cout << "  Median: " << tap_times_ns[n/2] << " ns" << std::endl;
            std::cout << "  Average: " << (sum / n) << " ns" << std::endl;
            std::cout << "  P95: " << tap_times_ns[std::min(n-1, (size_t)(n * 0.95))] << " ns" << std::endl;
            std::cout << "  P99: " << tap_times_ns[std::min(n-1, (size_t)(n * 0.99))] << " ns" << std::endl;
            std::cout << "  Max: " << tap_times_ns[n-1] << " ns" << std::endl;
        }
        
        // Measure individual TSC call overhead
        std::vector<double> tsc_plain_times;
        std::vector<double> tsc_serialized_times;
        
        for (int i = 0; i < test_iterations; i++) {
            // Measure plain TSC
            uint64_t start1 = rdtsc_serialized();
            uint64_t dummy1 = rdtsc_plain();
            uint64_t end1 = rdtsc_serialized();
            
            double plain_time = tsc_to_ns(end1 - start1, tsc_frequency);
            if (plain_time > 0 && plain_time < 1000) {
                tsc_plain_times.push_back(plain_time);
            }
            
            // Measure serialized TSC
            uint64_t start2 = rdtsc_serialized();
            uint64_t dummy2 = rdtsc_serialized();
            uint64_t end2 = rdtsc_serialized();
            
            double serialized_time = tsc_to_ns(end2 - start2, tsc_frequency);
            if (serialized_time > 0 && serialized_time < 1000) {
                tsc_serialized_times.push_back(serialized_time);
            }
        }
        
        auto print_tsc_stats = [](const std::vector<double>& times, const std::string& name) {
            if (times.empty()) return;
            std::vector<double> sorted_times = times;
            std::sort(sorted_times.begin(), sorted_times.end());
            size_t n = sorted_times.size();
            double sum = 0;
            for (double val : sorted_times) sum += val;
            
            std::cout << name << " timing:" << std::endl;
            std::cout << "  Average: " << std::fixed << std::setprecision(1) << (sum / n) << " ns" << std::endl;
            std::cout << "  Median: " << sorted_times[n/2] << " ns" << std::endl;
            std::cout << "  P95: " << sorted_times[std::min(n-1, (size_t)(n * 0.95))] << " ns" << std::endl;
        };
        
        std::cout << "\nIndividual TSC Call Overhead:" << std::endl;
        print_tsc_stats(tsc_plain_times, "Plain RDTSC");
        print_tsc_stats(tsc_serialized_times, "Serialized RDTSC");
    }
};

int main(int argc, char* argv[]) {
    // Default parameters
    int ping_core = 0;
    int pong_core = 1;
    int num_samples = 100000;
    int mode_int = 1; // Default to LFENCE_BOTH_SIDES
    
    std::cout << "TSC Ping-Pong Latency Test WITH TAP POINTS" << std::endl;
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
        PingPongTestWithTaps::TimestampMode mode = static_cast<PingPongTestWithTaps::TimestampMode>(mode_int);
        PingPongTestWithTaps test(ping_core, pong_core, num_samples, mode);
        test.run_test();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}