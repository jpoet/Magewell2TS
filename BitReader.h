#include <span>
#include <cstdint>
#include <bit>
#include <algorithm>

#include <cstdint>

constexpr uint64_t swap64(uint64_t val) noexcept
{
    // A standard mask-and-shift idiom that modern compilers collapse
    // cleanly into a single ultra-fast hardware assembly instruction.
    return ((val & 0xFF00000000000000ULL) >> 56) |
           ((val & 0x00FF000000000000ULL) >> 40) |
           ((val & 0x0000FF0000000000ULL) >> 24) |
           ((val & 0x000000FF00000000ULL) >> 8)  |
           ((val & 0x00000000FF000000ULL) << 8)  |
           ((val & 0x0000000000FF0000ULL) << 24) |
           ((val & 0x000000000000FF00ULL) << 40) |
           ((val & 0x00000000000000FFULL) << 56);
}

class BitReader
{
public:
    explicit BitReader(std::span<const uint8_t> data)
        : m_data(data)
    {
        // Prime the cache with the first block of data
        refill();
    }

    uint32_t getBits(unsigned count)
    {
        if (count == 0) [[unlikely]] return 0;
        if (count > 32) [[unlikely]] count = 32;

        // If we don't have enough bits cached, pull from the cache and refill
        if (count > m_bitsLeft)
        {
            uint32_t result = 0;
            unsigned missing = count - m_bitsLeft;

            // Take whatever is left in the current cache
            if (m_bitsLeft > 0)
            {
                result = static_cast<uint32_t>(m_cache >> (64 - m_bitsLeft));
                result <<= missing;
            }

            refill();

            // Fetch the remaining missing bits from the refreshed cache
            unsigned bitsToTake = std::min(missing, m_bitsLeft);
            if (bitsToTake > 0)
            {
                result |= static_cast<uint32_t>(m_cache >> (64 - bitsToTake));
                m_cache <<= bitsToTake;
                m_bitsLeft -= bitsToTake;
            }
            return result;
        }

        // --- Fast Path (95%+ of all calls) ---
        // Grab bits from the top of our 64-bit register and shift them out
        uint32_t value = static_cast<uint32_t>(m_cache >> (64 - count));
        m_cache <<= count;
        m_bitsLeft -= count;
        m_totalBitsRead += count;

        return value;
    }

    bool getBit()
    {
        // Micro-optimization: inline directly into the fast path condition
        if (m_bitsLeft > 0) [[likely]]
        {
            bool value = (m_cache & (1ULL << 63)) != 0;
            m_cache <<= 1;
            m_bitsLeft--;
            m_totalBitsRead++;
            return value;
        }
        return getBits(1) != 0;
    }

    void skipBits(unsigned count)
    {
        // For simplicity, we just leverage getBits to discard data safely
        while (count > 32)
        {
            getBits(32);
            count -= 32;
        }
        getBits(count);
    }

    size_t getBitPosition() const
    {
        return m_totalBitsRead;
    }

private:
    void refill()
    {
        m_cache = 0;
        size_t bytesRemaining = m_data.size() - m_bytePos;

        if (bytesRemaining == 0)
        {
            m_bitsLeft = 0;
            return;
        }

        size_t bytesToLoad = std::min<size_t>(bytesRemaining, 8);

        uint64_t rawWord = 0;
        std::memcpy(&rawWord, m_data.data() + m_bytePos, bytesToLoad);

        // Conditional branch resolved completely at compile time
        if constexpr (std::endian::native == std::endian::little)
        {
            rawWord = swap64(rawWord); // Replaced std::byteswap with our custom helper
        }

        if (bytesToLoad < 8)
        {
            rawWord >>= (8 - bytesToLoad) * 8;
            m_bitsLeft = bytesToLoad * 8;
        }
        else
        {
            m_bitsLeft = 64;
        }

        m_cache = rawWord;
        m_bytePos += bytesToLoad;
    }

    std::span<const uint8_t> m_data;
    uint64_t m_cache = 0;       // Holds up to 8 bytes of current stream data
    unsigned m_bitsLeft = 0;    // Number of valid unread bits inside m_cache
    size_t m_bytePos = 0;       // Track our byte read pointer in the source span
    size_t m_totalBitsRead = 0; // Cumulative bit position counter
};
