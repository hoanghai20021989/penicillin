#include "spmc_queue.h" // Your header file
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <random>
#include <atomic>
#include <iomanip>
#include <algorithm>
#include <numeric>
#include <cstring>
#include <sched.h>
#include <unistd.h>
#include <sys/syscall.h>

// High-resolution timer
class Timer
{
public:
    static uint64_t now_ns()
    {
        return std::chrono::high_resolution_clock::now().time_since_epoch().count();
    }

    static uint64_t rdtsc()
    {
        uint32_t lo, hi;
        __asm__ __volatile__(
            "rdtsc\n\t"
            : "=a"(lo), "=d"(hi)
            :
            : "memory");
        return ((uint64_t)hi << 32) | lo;
    }
};

// CPU affinity helper functions
class CPUAffinity
{
public:
    static bool set_affinity(int cpu_id)
    {
        if (cpu_id < 0)
            return true; // No affinity requested

        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu_id, &cpuset);

        pid_t tid = syscall(SYS_gettid);
        int result = sched_setaffinity(tid, sizeof(cpu_set_t), &cpuset);

        if (result == 0)
        {
            std::cout << "Thread " << std::this_thread::get_id()
                      << " bound to CPU " << cpu_id << std::endl;
        }
        else
        {
            std::cerr << "Failed to set CPU affinity to " << cpu_id
                      << " for thread " << std::this_thread::get_id() << std::endl;
        }

        return result == 0;
    }

    static bool set_realtime_priority(int priority = 99)
    {
        struct sched_param param;
        param.sched_priority = priority;

        int result = sched_setscheduler(0, SCHED_FIFO, &param);
        if (result == 0)
        {
            std::cout << "Thread " << std::this_thread::get_id()
                      << " set to RT priority " << priority << std::endl;
        }
        else
        {
            std::cerr << "Failed to set RT priority (may need sudo)" << std::endl;
        }

        return result == 0;
    }

    static bool set_nice_priority(int nice_value = -20)
    {
        int result = nice(nice_value);
        if (result != -1)
        {
            std::cout << "Thread " << std::this_thread::get_id()
                      << " set nice value to " << nice_value << std::endl;
        }
        return result != -1;
    }

    static int get_current_cpu()
    {
        return sched_getcpu();
    }

    static void print_thread_info(const std::string &thread_name)
    {
        std::cout << thread_name << " running on CPU " << get_current_cpu()
                  << ", TID: " << syscall(SYS_gettid) << std::endl;
    }

    // CPU-based delay functions (no sleep)
    static void cpu_pause(uint32_t cycles = 1)
    {
        for (uint32_t i = 0; i < cycles; ++i)
        {
            __builtin_ia32_pause();
        }
    }

    static void cpu_delay_ns(uint64_t nanoseconds)
    {
        uint64_t start = Timer::rdtsc();
        // Rough estimate: modern CPUs ~3GHz, so ~3 cycles per nanosecond
        uint64_t cycles = nanoseconds * 3;
        while ((Timer::rdtsc() - start) < cycles)
        {
            __builtin_ia32_pause();
        }
    }

    static void cpu_delay_cycles(uint64_t cycles)
    {
        uint64_t start = Timer::rdtsc();
        while ((Timer::rdtsc() - start) < cycles)
        {
            __builtin_ia32_pause();
        }
    }

    // Variable intensity spinning
    static void adaptive_pause(uint32_t backoff_level)
    {
        if (backoff_level == 0)
            return;

        if (backoff_level < 10)
        {
            // Light spinning with pause
            cpu_pause(backoff_level);
        }
        else if (backoff_level < 100)
        {
            // Medium spinning
            cpu_delay_cycles(backoff_level * 10);
        }
        else
        {
            // Heavy spinning (but still no sleep)
            cpu_delay_cycles(backoff_level * 100);
        }
    }
};

// Performance test configuration
struct PerfConfig
{
    size_t num_readers = 1;
    size_t num_messages = 1000000;
    size_t message_size = 128;
    std::chrono::seconds test_duration{1};
    bool use_fixed_size = true;
    size_t min_message_size = 64;
    size_t max_message_size = 1024;

    // CPU affinity settings (-1 means no affinity)
    int writer_cpu = -1;
    std::vector<int> reader_cpus = {}; // Empty means no affinity, or specify per-reader
    bool isolate_threads = false;      // If true, set RT priority and nice values

    // Delay/throttling settings (0 = no delay)
    uint32_t writer_delay_cycles = 0;         // CPU cycles delay between writes
    uint32_t reader_delay_cycles = 0;         // CPU cycles delay between reads
    uint32_t writer_throttle_every = 0;       // Add delay every N writes (0 = no throttling)
    uint32_t reader_backoff_threshold = 1000; // Start backing off after N torn reads

    // Notification settings
    uint32_t notification_batch_size = 1;   // Send notification every N messages
    bool drop_notifications_on_full = true; // Drop notifications instead of blocking
};

// Test message structure
struct TestMessage
{
    uint64_t sequence_id;
    uint64_t timestamp_ns;
    uint64_t thread_id;
    uint32_t data_size;
    char data[]; // Variable size data
};

// Performance metrics
struct PerfMetrics
{
    std::atomic<uint64_t> messages_written{0};
    std::atomic<uint64_t> messages_read{0};
    std::atomic<uint64_t> bytes_written{0};
    std::atomic<uint64_t> bytes_read{0};
    std::atomic<uint64_t> torn_reads{0};
    std::atomic<uint64_t> notifications_sent{0};
    std::atomic<uint64_t> notifications_dropped{0};

    std::vector<uint64_t> write_latencies;
    std::vector<std::vector<uint64_t>> read_latencies;
    std::vector<std::vector<uint64_t>> end_to_end_latencies;

    // Larger notification queue (lock-free single-producer, multiple-consumer)
    static constexpr size_t kNotificationQueueSize = 16384; // Much larger queue
    std::atomic<uint64_t> notification_queue[kNotificationQueueSize];
    std::atomic<uint32_t> notification_write_idx{0};
    std::atomic<uint32_t> notification_read_idx{0};

    std::chrono::high_resolution_clock::time_point start_time;
    std::chrono::high_resolution_clock::time_point end_time;

    void reset()
    {
        messages_written = 0;
        messages_read = 0;
        bytes_written = 0;
        bytes_read = 0;
        torn_reads = 0;
        notifications_sent = 0;
        notifications_dropped = 0;
        write_latencies.clear();
        for (auto &v : read_latencies)
            v.clear();
        for (auto &v : end_to_end_latencies)
            v.clear();
        read_latencies.reserve(1000000);
        end_to_end_latencies.reserve(1000000);
        notification_write_idx = 0;
        notification_read_idx = 0;

        // Clear notification queue
        for (auto &notification : notification_queue)
        {
            notification.store(0);
        }
    }

    // Producer (writer) pushes notifications
    bool push_notification(uint64_t notification)
    {
        uint32_t write_idx = notification_write_idx.load(std::memory_order_relaxed);
        uint32_t next_write_idx = (write_idx + 1) % kNotificationQueueSize;

        if (next_write_idx == notification_read_idx.load(std::memory_order_acquire))
        {
            notifications_dropped.fetch_add(1);
            return false; // Queue full
        }

        notification_queue[write_idx].store(notification, std::memory_order_release);
        notification_write_idx.store(next_write_idx, std::memory_order_release);
        notifications_sent.fetch_add(1);
        return true;
    }

    // Consumer (reader) pops notifications
    bool pop_notification(uint64_t &notification)
    {
        uint32_t read_idx = notification_read_idx.load(std::memory_order_relaxed);
        if (read_idx == notification_write_idx.load(std::memory_order_acquire))
        {
            return false; // Queue empty
        }

        notification = notification_queue[read_idx].load(std::memory_order_acquire);
        notification_read_idx.store((read_idx + 1) % kNotificationQueueSize, std::memory_order_release);
        return true;
    }

    // Get queue utilization percentage
    double get_queue_utilization() const
    {
        uint32_t write_idx = notification_write_idx.load();
        uint32_t read_idx = notification_read_idx.load();
        uint32_t used = (write_idx >= read_idx) ? (write_idx - read_idx) : (kNotificationQueueSize - read_idx + write_idx);
        return (double)used / kNotificationQueueSize * 100.0;
    }
};

// Test data generator
class TestDataGenerator
{
private:
    std::mt19937 rng_;
    std::uniform_int_distribution<size_t> size_dist_;

public:
    TestDataGenerator(size_t min_size, size_t max_size)
        : rng_(std::random_device{}()), size_dist_(min_size, max_size) {}

    size_t next_size()
    {
        return size_dist_(rng_);
    }

    void fill_data(char *data, size_t size)
    {
        std::uniform_int_distribution<uint8_t> byte_dist(0, 255);
        for (size_t i = 0; i < size; ++i)
        {
            data[i] = static_cast<char>(byte_dist(rng_));
        }
    }
};

// SPMC Performance Test Suite
template <size_t BlockSize, uint16_t NumBlocks>
class SPMCPerfTest
{
private:
    using DataStore = DataStoreT<BlockSize, NumBlocks>;
    using DataReader = DataReaderT<BlockSize, NumBlocks>;

    DataStore store_;
    PerfMetrics metrics_;
    PerfConfig config_;

public:
    SPMCPerfTest(const PerfConfig &config) : config_(config) {}

    // Writer thread function
    void writer_thread()
    {
        // Set CPU affinity and priority for writer
        if (config_.writer_cpu >= 0)
        {
            CPUAffinity::set_affinity(config_.writer_cpu);
        }

        if (config_.isolate_threads)
        {
            CPUAffinity::set_realtime_priority(99);
            CPUAffinity::set_nice_priority(-20);
        }

        CPUAffinity::print_thread_info("Writer");

        TestDataGenerator gen(config_.min_message_size, config_.max_message_size);
        uint64_t sequence_id = 0;
        uint32_t current_block_idx = 0;

        auto start_time = std::chrono::high_resolution_clock::now();
        auto end_time = start_time + config_.test_duration;

        // Write header
        TestMessage msg;
        while (std::chrono::high_resolution_clock::now() < end_time)
        {
            size_t message_size = config_.use_fixed_size ? config_.message_size : gen.next_size();
            size_t total_size = sizeof(TestMessage) + message_size;

            // Create notification BEFORE writing data (but only every N messages)
            uint64_t message_timestamp = Timer::now_ns();

            auto notification = store_.CreateReaderNotification();
            metrics_.push_notification(notification);

            auto write_start = Timer::now_ns();

            msg.sequence_id = sequence_id++;
            msg.timestamp_ns = message_timestamp; // Use pre-determined timestamp
            msg.thread_id = std::hash<std::thread::id>{}(std::this_thread::get_id());
            msg.data_size = static_cast<uint32_t>(message_size);
            // Now write the data
            size_t written{};
            store_.Put(notification, total_size, [&](auto payload_span, size_t block_size, size_t offset)
                       {
                        auto remain_sz{block_size};
                        size_t write_sz{};
                        if (written<sizeof(TestMessage)) {
                            write_sz = std::min(sizeof(TestMessage), block_size);
                            std::memcpy(payload_span.data(), &msg, write_sz);
                            written += write_sz;
                            remain_sz-=write_sz;
                        }
                        if (remain_sz) {
                            gen.fill_data(reinterpret_cast<char*>(payload_span.data()) + write_sz,
                                    remain_sz);
                                    written+=remain_sz;
                        } });

            auto write_end = Timer::now_ns();

            // Estimate next block position (simplified)
            uint32_t blocks_needed = (total_size + DataStore::kPayloadSize - 1) / DataStore::kPayloadSize;
            current_block_idx = (current_block_idx + blocks_needed) % NumBlocks;

            metrics_.messages_written.fetch_add(1, std::memory_order_relaxed);
            metrics_.bytes_written.fetch_add(written, std::memory_order_relaxed);

            // Record latency (sampling to avoid overwhelming the vectors)
            if (sequence_id % 1000 == 0)
            {
                metrics_.write_latencies.push_back(write_end - write_start);
            }

            // Add CPU-based delays without sleeping
            if (config_.writer_delay_cycles > 0)
            {
                CPUAffinity::cpu_delay_cycles(config_.writer_delay_cycles);
            }

            // Throttle every N writes
            if (config_.writer_throttle_every > 0 && (sequence_id % config_.writer_throttle_every) == 0)
            {
                CPUAffinity::cpu_delay_cycles(config_.writer_delay_cycles * 10);
            }
        }
    }

    // Reader thread function
    void reader_thread(int reader_id)
    {
        // Set CPU affinity and priority for reader
        int reader_cpu = -1;
        if (!config_.reader_cpus.empty())
        {
            if (reader_id < static_cast<int>(config_.reader_cpus.size()))
            {
                reader_cpu = config_.reader_cpus[reader_id];
            }
            else
            {
                // Wrap around if more readers than specified CPUs
                reader_cpu = config_.reader_cpus[reader_id % config_.reader_cpus.size()];
            }
        }

        if (reader_cpu >= 0)
        {
            CPUAffinity::set_affinity(reader_cpu);
        }

        if (config_.isolate_threads)
        {
            CPUAffinity::set_realtime_priority(98 - reader_id); // Slightly lower than writer
            CPUAffinity::set_nice_priority(-19);
        }

        CPUAffinity::print_thread_info("Reader-" + std::to_string(reader_id));

        uint64_t expected_sequence = 0;
        uint64_t out_of_order_count = 0;
        uint64_t successful_reads = 0;
        uint64_t torn_reads_local = 0;
        uint64_t empty_reads = 0;
        uint64_t notifications_processed = 0;
        uint32_t backoff_level = 0;

        auto start_time = std::chrono::high_resolution_clock::now();
        auto end_time = start_time + config_.test_duration;

        uint64_t written{};
        TestMessage msg_header;
        std::vector<uint8_t> message_buffer;
        message_buffer.reserve(30000);
        while (std::chrono::high_resolution_clock::now() < end_time)
        {
            // Wait for notification from writer
            uint64_t notification;
            if (!metrics_.pop_notification(notification))
            {
                // No notifications available, light pause
                CPUAffinity::cpu_pause(10);
                empty_reads++;
                continue;
            }

            notifications_processed++;

            // Create reader from notification
            auto reader = DataReader::create_from_notification(store_.GetBlocksPtr(), notification);

            written = {};
            auto read_start = Timer::now_ns();

            bool success = reader.template SpinRead<false, false>(
                [&](auto payload, bool is_last)
                {
                    size_t remain_sz{DataStore::kPayloadSize};
                    if (written < sizeof(TestMessage))
                    {
                        size_t write_sz = std::min(sizeof(TestMessage) - written, DataStore::kPayloadSize);
                        std::memcpy(reinterpret_cast<char *>(&msg_header), payload.data(), write_sz);
                        remain_sz -= write_sz;
                        written += write_sz;
                        if (written == sizeof(TestMessage))
                        {
                            message_buffer.resize(msg_header.data_size);
                        }
                    }
                    auto message_written = msg_header.data_size - (written - sizeof(TestMessage));
                    remain_sz = std::min(message_written, remain_sz);
                    if (remain_sz)
                    {
                        std::memcpy(message_buffer.data() + message_written, payload.data(), remain_sz);
                        written += remain_sz;
                    }
                    if (is_last)
                    {
                        auto read_end = Timer::now_ns();
                        auto end_to_end = read_end - msg_header.timestamp_ns;

                        metrics_.messages_read.fetch_add(1, std::memory_order_relaxed);
                        metrics_.bytes_read.fetch_add(written, std::memory_order_relaxed);
                        successful_reads++;

                        // Reset backoff on successful read
                        backoff_level = 0;

                        // Check sequence ordering (relaxed for multi-reader)
                        if (msg_header.sequence_id < expected_sequence)
                        {
                            out_of_order_count++;
                        }
                        else
                        {
                            expected_sequence = msg_header.sequence_id + 1;
                        }

                        // Record latencies (sampling)
                        if (msg_header.sequence_id % 1000 == 0)
                        {
                            metrics_.read_latencies[reader_id].push_back(read_end - read_start);
                            metrics_.end_to_end_latencies[reader_id].push_back(end_to_end);
                        }
                    }
                });

            if (!success)
            {
                torn_reads_local++;
                metrics_.torn_reads.fetch_add(1, std::memory_order_relaxed);

                // Increase backoff level for adaptive pausing
                if (torn_reads_local > config_.reader_backoff_threshold)
                {
                    backoff_level = std::min(backoff_level + 1, 1000U);
                }
            }
        }

        // Print per-reader statistics
        std::cout << "Reader-" << reader_id << " stats: "
                  << "Successful: " << successful_reads
                  << ", Torn: " << torn_reads_local
                  << ", Empty: " << empty_reads
                  << ", Notifications: " << notifications_processed
                  << ", Max backoff: " << backoff_level << std::endl;
    }

    // Run the performance test
    void run_test()
    {
        std::cout << "Running SPMC Performance Test\n";
        std::cout << "Block Size: " << BlockSize << " bytes\n";
        std::cout << "Num Blocks: " << NumBlocks << "\n";
        std::cout << "Num Readers: " << config_.num_readers << "\n";
        std::cout << "Message Size: " << config_.message_size << " bytes\n";
        std::cout << "Test Duration: " << config_.test_duration.count() << " seconds\n";
        std::cout << "Stride: " << DataStore::DataBlock::kStride << "\n";
        std::cout << "Fixed Sz Msg: " << config_.use_fixed_size << "\n";
        std::cout << "Writer CPU: " << (config_.writer_cpu >= 0 ? std::to_string(config_.writer_cpu) : "any") << "\n";
        std::cout << "Reader CPUs: ";
        if (config_.reader_cpus.empty())
        {
            std::cout << "any";
        }
        else
        {
            for (size_t i = 0; i < config_.reader_cpus.size(); ++i)
            {
                if (i > 0)
                    std::cout << ",";
                std::cout << config_.reader_cpus[i];
            }
        }
        std::cout << "\n";
        std::cout << "RT Priority: " << (config_.isolate_threads ? "enabled" : "disabled") << "\n";
        std::cout << "----------------------------------------\n";

        metrics_.reset();
        metrics_.start_time = std::chrono::high_resolution_clock::now();

        // Start reader threads
        std::vector<std::thread> readers;
        for (size_t i = 0; i < config_.num_readers; ++i)
        {
            metrics_.read_latencies.push_back({});
            metrics_.end_to_end_latencies.push_back({});
            readers.emplace_back(&SPMCPerfTest::reader_thread, this, i);
        }

        // Start writer thread
        std::thread writer(&SPMCPerfTest::writer_thread, this);

        // Wait for all threads to complete
        writer.join();
        for (auto &reader : readers)
        {
            reader.join();
        }

        metrics_.end_time = std::chrono::high_resolution_clock::now();

        // Print results
        print_results();
    }

private:
    void print_results()
    {
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            metrics_.end_time - metrics_.start_time);

        double duration_sec = duration.count() / 1000.0;
        double write_throughput = metrics_.messages_written.load() / duration_sec;
        double read_throughput = metrics_.messages_read.load() / duration_sec;
        double write_bandwidth = (metrics_.bytes_written.load() / duration_sec) / (1024 * 1024);
        double read_bandwidth = (metrics_.bytes_read.load() / duration_sec) / (1024 * 1024);

        std::cout << "\n=== PERFORMANCE RESULTS ===\n";
        std::cout << "Duration: " << std::fixed << std::setprecision(3) << duration_sec << " seconds\n";
        std::cout << "Messages Written: " << metrics_.messages_written.load() << "\n";
        std::cout << "Messages Read: " << metrics_.messages_read.load() << "\n";
        std::cout << "Notifications Sent: " << metrics_.notifications_sent.load() << "\n";
        std::cout << "Notifications Dropped: " << metrics_.notifications_dropped.load() << "\n";
        std::cout << "Queue Utilization: " << std::fixed << std::setprecision(1)
                  << metrics_.get_queue_utilization() << "%\n";
        std::cout << "Torn Reads: " << metrics_.torn_reads.load() << "\n";
        std::cout << "\nThroughput:\n";
        std::cout << "  Write: " << std::fixed << std::setprecision(0) << write_throughput << " msg/sec\n";
        std::cout << "  Read:  " << std::fixed << std::setprecision(0) << read_throughput << " msg/sec\n";
        std::cout << "\nBandwidth:\n";
        std::cout << "  Write: " << std::fixed << std::setprecision(2) << write_bandwidth << " MB/sec\n";
        std::cout << "  Read:  " << std::fixed << std::setprecision(2) << read_bandwidth << " MB/sec\n";

        // Latency statistics
        if (!metrics_.write_latencies.empty())
        {
            print_latency_stats("Write Latency", metrics_.write_latencies);
        }
        for (size_t i = 0; i < metrics_.read_latencies.size(); i++)
        {
            if (!metrics_.read_latencies[i].empty())
            {
                print_latency_stats("Read Latency", metrics_.read_latencies[i]);
            }
            if (!metrics_.end_to_end_latencies[i].empty())
            {
                print_latency_stats("End-to-End Latency", metrics_.end_to_end_latencies[i]);
            }
        }
    }

    void print_latency_stats(const std::string &name, std::vector<uint64_t> &latencies)
    {
        if (latencies.empty())
            return;

        std::sort(latencies.begin(), latencies.end());

        auto mean = std::accumulate(latencies.begin(), latencies.end(), 0ULL) / latencies.size();
        auto p50 = latencies[latencies.size() * 50 / 100];
        auto p95 = latencies[latencies.size() * 95 / 100];
        auto p99 = latencies[latencies.size() * 99 / 100];
        auto p999 = latencies[latencies.size() * 999 / 1000];

        std::cout << "\n"
                  << name << " (nanoseconds):\n";
        std::cout << "  Mean:  " << mean << " ns\n";
        std::cout << "  P50:   " << p50 << " ns\n";
        std::cout << "  P95:   " << p95 << " ns\n";
        std::cout << "  P99:   " << p99 << " ns\n";
        std::cout << "  P99.9: " << p999 << " ns\n";
        std::cout << "  Max:   " << latencies.back() << " ns\n";
    }
};

// Main test runner
int main()
{
    std::cout << "SPMC Queue Performance Test Suite\n";
    std::cout << "==================================\n\n";

    // Test configurations with CPU affinity and throttling
    auto test_dur = std::chrono::seconds(5);
    std::vector<PerfConfig> configs;

    for (bool use_fixed_size : {true, false})
    {
        for (size_t msg_sz_payload_mult : {1, 2, 4, 8})
        {
            auto msg_sz = msg_sz_payload_mult * 56;
            std::vector<PerfConfig> tmp_configs = {
                // Basic test - no affinity, batch notifications every 10 messages
                {1, 1000000, msg_sz, test_dur, use_fixed_size, 64, 1024, 7, {3, 4, 5, 6}, true, 1, 0, 0, 1000, 1, false},

                // Throttled writer test - slow down writer, notify every 5 messages
                {2, 1000000, msg_sz, test_dur, use_fixed_size, 64, 1024, 7, {3, 4, 5, 6}, true, 1, 0, 0, 1000, 1, false},

                // Writer on core 7, readers on cores 0-3, with minimal notifications
                {4, 1000000, msg_sz, test_dur, use_fixed_size, 64, 1024, 7, {3, 4, 5, 6}, true, 1, 0, 0, 1000, 1, false},
            };
            for (auto cfg : tmp_configs)
            {
                configs.push_back(cfg);
            }
        }
    }

    // Test different block sizes to demonstrate stride effectiveness
    for (const auto &config : configs)
    {
        std::cout << "\n"
                  << std::string(60, '=') << "\n";
        std::cout << "Config: " << config.num_readers << " readers, "
                  << config.message_size << "B messages, "
                  << config.test_duration.count() << "s duration\n";
        std::cout << std::string(60, '=') << "\n";

        // Test with 64-byte blocks (stride 1)
        std::cout << "\nTesting 64-byte blocks (stride 1):\n";
        SPMCPerfTest<64, 4096> test_64(config);
        test_64.run_test();

        // Test with 128-byte blocks (stride 1)
        std::cout << "\nTesting 128-byte blocks (stride 1):\n";
        SPMCPerfTest<128, 4096> test_128(config);
        test_128.run_test();

        // Test with 256-byte blocks (stride 1)
        std::cout << "\nTesting 256-byte blocks (stride 1):\n";
        SPMCPerfTest<256, 2048> test_256(config);
        test_256.run_test();

        // Test with 512-byte blocks (stride 1)
        std::cout << "\nTesting 512-byte blocks (stride 1):\n";
        SPMCPerfTest<512, 2048> test_512(config);
        test_512.run_test();

        // Test with 1024-byte blocks (stride 1)
        std::cout << "\nTesting 1024-byte blocks (stride 1):\n";
        SPMCPerfTest<1024, 2048> test_1024(config);
        test_1024.run_test();

        std::cout << "\n"
                  << std::string(60, '=') << "\n";
    }

    return 0;
}