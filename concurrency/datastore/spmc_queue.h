#pragma once

#include <atomic>
#include <cstddef>
#include <array>
#include <memory>
#include <span>
#include <functional>
#include <limits>
#include <chrono>
#include <thread>
#include <cstring>
#include <iostream>

constexpr int kCacheLine = 64;

template <auto N>
concept PowerOfTwo = (N > 0) && ((N & (N - 1)) == 0);

template <auto N, auto MinValue>
concept AtLeast = (N >= MinValue);

template <size_t BlockSize, uint16_t NumBlocks>
concept ValidBlockConfig =
    PowerOfTwo<BlockSize> &&
    PowerOfTwo<NumBlocks> &&
    AtLeast<BlockSize, 32> &&
    ((BlockSize * NumBlocks) % kCacheLine == 0);

struct alignas(util::kCacheLine) DataHeader
{
    // Single 64-bit field encoding: [39-bit timestamp][1-bit is_last][24-bit data_size]
    // Special value: std::numeric_limits<uint64_t>::max() means data not ready

    // Bit layout constants
    static constexpr uint64_t kTimestampMask = 0xFFFFFFFE00000000ULL; // Upper 39 bits (32+7)
    static constexpr uint64_t kIsLastMask = 0x01000000ULL;            // Bit 24
    static constexpr uint64_t kDataSizeMask = 0x00FFFFFFULL;          // Lower 24 bits (16MB max)
    static constexpr int kTimestampShift = 25;
    // Maximum data size: 2^24 - 1 = 16,777,215 bytes (~16MB)
    static constexpr uint32_t kMaxDataSize = 0x00FFFFFF; // 16MB - 1 byte
    // Maximum timestamp: 2^39 - 1 = 549,755,813,887 microseconds ≈ 549,755 seconds ≈ 152 hours
    static constexpr uint64_t kMaxTimestamp = 0x7FFFFFFFFFULL; // 39 bits

    std::atomic<uint64_t> timestamp_flags_size{};
};

template <size_t kBlockSize, uint16_t kNumBlocks>
    requires ValidBlockConfig<kBlockSize, kNumBlocks>
struct alignas(kBlockSize) DataBlockT
{

    static constexpr size_t kPayloadSize = kBlockSize;

    // Stride configuration based on block size
    static constexpr size_t kStride = (kBlockSize >= kCacheLine) ? 1 : (kBlockSize >= 32) ? 2
                                                                                          : 4;

    using Payload = std::array<std::byte, kPayloadSize>;
    using PayloadView = std::span<const std::byte>;
    using WritablePayload = std::span<std::byte>;

    // Remaining space is payload
    alignas(8) Payload payload{};

    PayloadView get_payload() const { return std::as_bytes(std::span{payload}); }
    WritablePayload get_payload() { return std::as_writable_bytes(std::span{payload}); }

    /// Set block info atomically
    void set_block_info(uint64_t timestamp_us, bool is_last, uint32_t data_size)
    {
        // Ensure timestamp fits in 39 bits
        timestamp_us &= kMaxTimestamp;
        // Ensure data_size fits in 24 bits (max 16MB)
        data_size &= kMaxDataSize;

        uint64_t combined = (timestamp_us << kTimestampShift) |
                            (is_last ? kIsLastMask : 0) |
                            data_size;

        timestamp_flags_size.store(combined, std::memory_order_release);
    }
    /// Get all components at once
    struct BlockInfo
    {
        uint64_t timestamp_us;
        bool is_last;
        uint32_t data_size;
    };

    BlockInfo get_block_info() const
    {
        uint64_t value = timestamp_flags_size.load(std::memory_order_acquire);

        return {
            .timestamp_us = (value & kTimestampMask) >> kTimestampShift,
            .is_last = (value & kIsLastMask) != 0,
            .data_size = static_cast<uint32_t>(value & kDataSizeMask)};
    }

    /// Convert logical block index to physical block index using stride
    static constexpr uint32_t logical_to_physical_block_idx(uint32_t logical_idx)
    {
        if constexpr (kStride == 1)
        {
            return logical_idx;
        }
        else
        {
            uint32_t blocks_per_phase = kNumBlocks / kStride;
            uint32_t group = logical_idx / blocks_per_phase;
            uint32_t phase = logical_idx % blocks_per_phase;
            return phase * kStride + group;
        }
    }

    /// Convert physical block index to logical block index
    static constexpr uint32_t physical_to_logical_block_idx(uint32_t physical_idx)
    {
        if constexpr (kStride == 1)
        {
            return physical_idx;
        }
        else
        {
            uint32_t blocks_per_phase = kNumBlocks / kStride;
            uint32_t phase = physical_idx / kStride;
            uint32_t group = physical_idx % kStride;
            return group * blocks_per_phase + phase;
        }
    }
};
struct ReaderNotification
{
    uint64_t timestamp_us;
    uint32_t start_block_idx;
    bool reserved_flag; // Could be used for reader-specific flags

    // Bit layout constants (same as DataBlock)
    static constexpr uint64_t kTimestampMask = 0xFFFFFFFE00000000ULL; // Upper 39 bits
    static constexpr uint64_t kReservedMask = 0x01000000ULL;          // Bit 24
    static constexpr uint64_t kStartIdxMask = 0x00FFFFFFULL;          // Lower 24 bits
    static constexpr int kTimestampShift = 25;
    static constexpr uint64_t kMaxTimestamp = 0x7FFFFFFFFFULL; // 39 bits
    static constexpr uint32_t kMaxStartIdx = 0x00FFFFFF;       // 24 bits (16M blocks max)

    // Pack into 64-bit value
    uint64_t pack() const
    {
        uint64_t ts = timestamp_us & kMaxTimestamp;
        uint32_t idx = start_block_idx & kMaxStartIdx;

        return (ts << kTimestampShift) |
               (reserved_flag ? kReservedMask : 0) |
               idx;
    }

    // Unpack from 64-bit value
    static ReaderNotification unpack(uint64_t packed)
    {
        return {
            .timestamp_us = (packed & kTimestampMask) >> kTimestampShift,
            .start_block_idx = static_cast<uint32_t>(packed & kStartIdxMask),
            .reserved_flag = (packed & kReservedMask) != 0};
    }
};

// Modified DataReader to support subscription pattern
template <size_t kBlockSize, uint16_t kNumBlocks>
    requires ValidBlockConfig<kBlockSize, kNumBlocks>
class DataReaderT
{
public:
    using DataBlock = DataBlockT<kBlockSize, kNumBlocks>;
    using DataBlockArray = std::array<DataBlock, kNumBlocks>;
    using PayloadView = DataBlock::PayloadView;
    using NewBlockCallback = std::move_only_function<void(PayloadView, bool)>;

    static constexpr size_t kPayloadSize = DataBlock::kPayloadSize;
    static constexpr size_t kBlockHeaderSize = DataBlock::kBlockHeaderSize;
    static constexpr size_t kStride = DataBlock::kStride;

    ~DataReaderT() = default;
    DataReaderT(DataReaderT &&) = default;
    DataReaderT &operator=(DataReaderT &&) = default;

    // Factory method to create from packed notification
    static DataReaderT create_from_notification(const DataBlock *blocks_ptr, uint64_t packed_notification)
    {
        auto notification = ReaderNotification::unpack(packed_notification);
        return DataReaderT{blocks_ptr, notification.start_block_idx, notification.timestamp_us};
    }

    /// Spin-read block-by-block until the next full data entry
    template <bool IsPeekOnly, bool ReadTillLatest>
    bool SpinRead(NewBlockCallback &&on_new_data)
    {
        uint32_t logical_block_idx{start_block_idx_};
        uint64_t expected_ts{expected_ts_};

        while (true)
        {
            uint32_t physical_block_idx = DataBlock::logical_to_physical_block_idx(logical_block_idx);
            const auto &current_block = blocks_ptr_[physical_block_idx];

            if (auto block_info = current_block.get_block_info(); block_info.timestamp_us == expected_ts_) [[likely]]
            {
                if (block_info.is_last) [[likely]]
                {
                    auto last_payload_data_size = (block_info.data_size != 0) * (((block_info.data_size - 1) % kPayloadSize) + 1);
                    auto last_payload = current_block.get_payload();
                    on_new_data(last_payload.subspan(0, last_payload_data_size), true);
                    last_valid_block_idx_ = physical_block_idx;
                    if (auto updated_info = current_block.get_block_info(); updated_info.timestamp_us != expected_ts) [[unlikely]]
                    {
                        return false;
                    }
                    if constexpr (ReadTillLatest)
                    {
                        uint32_t next_logical_idx = (logical_block_idx + 1) % kNumBlocks;
                        uint32_t next_physical_idx = DataBlock::logical_to_physical_block_idx(next_logical_idx);
                        const auto &next_block = blocks_ptr_[next_physical_idx];
                        if (auto next_block_info = next_block.get_block_info();
                            next_block_info.timestamp_us > expected_ts) [[likely]]
                        {
                            logical_block_idx = next_logical_idx;
                            expected_ts = next_block_info.timestamp_us;
                            continue;
                        }
                    }
                    return true;
                }
                else
                {
                    on_new_data(current_block.get_payload(), false);
                    if (auto updated_info = current_block.get_block_info();
                        updated_info.timestamp_us != expected_ts) [[unlikely]]
                    {
                        return false;
                    }
                    if constexpr (IsPeekOnly)
                    {
                        return true;
                    }
                    logical_block_idx = (logical_block_idx + 1) % kNumBlocks;
                }
            }
            else if (block_info.timestamp_us > expected_ts)
            {
                return false;
            }
            __builtin_ia32_pause();
        }
    }

    /// Check if there's newer data available beyond current read position
    bool HasNext() const
    {
        uint32_t next_logical_idx = DataBlock::physical_to_logical_block_idx(last_valid_block_idx_) + 1;
        uint32_t next_physical_idx = DataBlock::logical_to_physical_block_idx(next_logical_idx % kNumBlocks);
        const auto &next_block = blocks_ptr_[next_physical_idx];
        if (auto next_block_info = next_block.get_block_info(); next_block_info.timestamp_us > expected_ts_)
        {
            return true;
        }
        return false;
    }

private:
    // Constructor for subscription pattern - takes raw pointer to blocks
    DataReaderT(const DataBlock *blocks_ptr, uint32_t start_block_idx, uint64_t expected_ts)
        : blocks_ptr_{blocks_ptr}, start_block_idx_(start_block_idx),
          last_valid_block_idx_(start_block_idx), expected_ts_{expected_ts}
    {
        // Do nothing
    }
    DataReaderT(const DataReaderT &) = delete;
    DataReaderT &operator=(const DataReaderT &) = delete;

    const DataBlock *blocks_ptr_{}; // Raw pointer instead of reference
    uint32_t start_block_idx_{};
    uint32_t last_valid_block_idx_{};
    uint64_t expected_ts_{};
};

template <size_t kBlockSize, uint16_t kNumBlocks>
    requires ValidBlockConfig<kBlockSize, kNumBlocks>
class DataStoreT
{
public:
    using DataBlock = DataBlockT<kBlockSize, kNumBlocks>;

    static constexpr size_t kPayloadSize = DataBlock::kPayloadSize;
    static constexpr size_t kStride = DataBlock::kStride;

    DataStoreT() : tsc_frequency_(GetTscFreq()), start_timestamp_us_(GetSystemTimestamp()), current_timestamp_us_{1} {} // Start with timestamp 1

    ~DataStoreT() = default;
    DataStoreT(const DataStoreT &) = delete;
    DataStoreT &operator=(const DataStoreT &) = delete;
    DataStoreT(DataStoreT &&) = delete;
    DataStoreT &operator=(DataStoreT &&) = delete;

    const DataBlock *GetBlocksPtr() const
    {
        return blocks_.data();
    }

    /// Create reader notification for a new reader
    uint64_t CreateReaderNotification()
    {
        auto entry_timestamp = GetNextTimestamp();
        ReaderNotification notification{
            .timestamp_us = entry_timestamp,
            .start_block_idx = current_logical_block_idx_,
            .reserved_flag = false};
        return notification.pack();
    }

    /// Write variable-size data using a function
    void Put(uint64_t packed_notification, size_t data_size, auto &&write_func)
    {
        auto notification = ReaderNotification::unpack(packed_notification);
        auto num_blocks_needed = CalculateBlocksNeeded(data_size);
        auto entry_timestamp = notification.timestamp_us;
        auto start_logical_idx = current_logical_block_idx_;

        // Write data to blocks
        size_t written = 0;
        for (size_t i = 0; i < num_blocks_needed; ++i)
        {
            auto logical_idx = (start_logical_idx + i) % kNumBlocks;
            auto physical_idx = DataBlock::logical_to_physical_block_idx(logical_idx);
            auto &block = blocks_[physical_idx];

            auto remaining = data_size - written;
            auto block_size = std::min(remaining, kPayloadSize);

            // Write data using user function
            write_func(block.get_payload(), block_size, written);
            written += block_size;

            // Set block metadata
            bool is_last = (i == num_blocks_needed - 1);

            block.set_block_info(entry_timestamp, is_last, data_size);
        }
        current_logical_block_idx_ = (current_logical_block_idx_ + num_blocks_needed) % kNumBlocks;
    }

    /// Write data by copying from source
    void Put(uint64_t packed_notification, const void *data, size_t size)
    {
        return Put(packed_notification, size, [data](auto payload_span, size_t block_size, size_t offset)
                   { std::memcpy(payload_span.data(), reinterpret_cast<const std::byte *>(data) + offset, block_size); });
    }

    /// Write typed data
    template <typename T>
        requires std::is_trivially_copyable_v<T> || std::is_trivially_move_assignable_v<T>
    void Put(uint64_t packed_notification, const T &data)
    {
        return Put(packed_notification, &data, sizeof(T));
    }

private:
    uint32_t CalculateBlocksNeeded(size_t data_size)
    {
        return (data_size + kPayloadSize - 1) / kPayloadSize;
    }
    uint64_t GetNextTimestamp()
    {
        // Use monotonically increasing timestamp for natural ordering
        auto system_time = GetSystemTimestamp() - start_timestamp_us_;

        // Ensure monotonic increase even if system clock doesn't advance
        if (system_time <= current_timestamp_us_)
        {
            current_timestamp_us_++;
        }
        else
        {
            current_timestamp_us_ = system_time;
        }

        return current_timestamp_us_;
    }
    inline uint64_t GetSystemTimestamp() const
    {
        return (double)rdtsc_plain() / tsc_frequency_ * 1e6;
    }

    static inline uint64_t rdtsc_plain()
    {
        uint32_t lo, hi;
        __asm__ __volatile__(
            "rdtsc\n\t"
            : "=a"(lo), "=d"(hi)
            :
            : "memory");
        return ((uint64_t)hi << 32) | lo;
    }

    // Inline assembly for fully serialized rdtsc
    static inline uint64_t rdtsc_serialized()
    {
        uint32_t lo, hi;
        __asm__ __volatile__(
            "lfence\n\t" // Ensure all prior instructions complete
            "rdtsc\n\t"  // Read timestamp counter
            "lfence\n\t" // Ensure rdtsc completes before subsequent instructions
            : "=a"(lo), "=d"(hi)
            :
            : "memory");
        return ((uint64_t)hi << 32) | lo;
    }

    // Get TSC frequency by measuring against steady_clock
    static inline double GetTscFreq()
    {
        const int samples = 10;
        const auto duration = std::chrono::milliseconds(100);

        double freq_sum = 0.0;

        for (int i = 0; i < samples; i++)
        {
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

    double tsc_frequency_{};

    alignas(kCacheLine) std::array<DataBlock, kNumBlocks> blocks_{};

    // Writer-only data
    uint64_t start_timestamp_us_{1};        // Timestamp when app starts
    uint64_t current_timestamp_us_{1};      // Entry version counter
    uint32_t current_logical_block_idx_{0}; // Add this to track current write position
};