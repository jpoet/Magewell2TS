#ifndef _AudioIO_h_
#define _AudioIO_h_

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

#include <string>
#include <deque>
#include <vector>
#include <utility>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>

/**
 * @brief AudioIO class manages audio buffers for muxing with video
 *
 * This class handles audio frame buffering and provides methods for
 * adding, reading, and managing audio data that will be muxed with video
 * in the OutputTS class.
 *
 * @author John Patrick Poet
 * @date 2022-2025
 */
class AudioIO;

/**
 * @brief AudioBuffer class represents a single audio buffer for buffering frames
 *
 * This class manages a queue of audio frames and provides methods for
 * adding frames, reading frames, and handling S/PDIF input.
 *
 * @author John Patrick Poet
 * @date 2022-2025
 */
class AudioBuffer
{
  public:
    /**
     * @brief Type alias for audio frame data
     *
     * Represents a vector of unsigned bytes that constitute an audio frame
     */
    using AudioFrame = std::vector<uint8_t>;

    /**
     * @brief Constructor for AudioBuffer
     *
     * Initializes an audio buffer with specified parameters
     *
     * @param num_channels Number of audio channels
     * @param is_lpcm Flag indicating if audio is LPCM format
     * @param bytes_per_sample Number of bytes per audio sample
     * @param sample_rate Audio sample rate
     * @param samples_per_frame Number of samples per audio frame
     * @param frame_size Size of audio frame in bytes
     * @param parent Pointer to parent AudioIO object
     * @param verbose Verbose level for logging
     * @param id Unique identifier for this buffer
     */
    AudioBuffer(int num_channels, bool is_lpcm,
                int bytes_per_sample, int sample_rate,
                int samples_per_frame, int frame_size,
                AudioIO* parent, int verbose, int id);

    /**
     * @brief Copy constructor
     *
     * @param rhs Right-hand side object to copy from
     */
    AudioBuffer(const AudioBuffer & rhs) { *this = rhs; }

    /**
     * @brief Destructor for AudioBuffer
     *
     * Sets EOF flag to indicate buffer destruction
     */
    ~AudioBuffer(void);

    /**
     * @brief Clear all queued audio frames
     *
     * Clears both the main audio queue and the probed queue
     */
    void PurgeQueue(void);

    /**
     * @brief Rescan S/PDIF input for audio format detection
     *
     * @return true if successful, false otherwise
     */
    bool RescanSPDIF(void);

    /**
     * @brief Transfer ownership of buffer data
     *
     * Used to transfer buffer ownership to another component
     */
    void OwnBuffer(void);

    /**
     * @brief Set end-of-file flag
     *
     * Marks this buffer as having reached end-of-file
     */
    void setEoF(void) { m_EoF.store(true); }

    /**
     * @brief Check if end-of-file flag is set
     *
     * @return true if EOF is set, false otherwise
     */
    bool isEoF(void) const { return m_EoF.load() == true; }

    /**
     * @brief Print current buffer state for debugging
     *
     * @param where Location identifier for logging
     * @param force Force printing regardless of verbose level
     */
    void PrintState(const std::string & where,
                    bool force = false) const;

    /**
     * @brief Print buffer pointers for debugging
     *
     * @param where Location identifier for logging
     * @param force Force printing regardless of verbose level
     */
    void PrintPointers(const std::string & where,
                       bool force = false) const;

    /**
     * @brief Assignment operator
     *
     * @param rhs Right-hand side object to assign from
     * @return Reference to this object
     */
    AudioBuffer& operator=(const AudioBuffer & rhs);

    /**
     * @brief Equality operator
     *
     * @param rhs Right-hand side object to compare with
     * @return true if objects are equal, false otherwise
     */
    bool operator==(const AudioBuffer & rhs);

    /**
     * @brief Add an audio frame to the buffer queue
     *
     * @param buf Pointer to audio frame data to add
     * @param timestamp Timestamp for the frame
     * @return true if successful, false otherwise
     */
    bool Add(AudioFrame *& buf, int64_t timestamp);

    /**
     * @brief Read audio data from buffer
     *
     * @param dest Destination buffer to read into
     * @param len Length of data to read
     * @return Number of bytes read, or AVERROR_EOF on EOF
     */
    int Read(uint8_t* dest, uint32_t len);

    /**
     * @brief Read audio frame from S/PDIF input
     *
     * @return Pointer to AVPacket containing audio frame, or nullptr on error
     */
    AVPacket* ReadSPDIF(void);

    /**
     * @brief Get buffer ID
     *
     * @return Unique identifier for this buffer
     */
    int  Id(void) const { return m_id; }

    /**
     * @brief Check if buffer is empty
     *
     * @return true if empty, false otherwise
     */
    bool Empty(void) const { return m_audio_queue.empty(); }

    /**
     * @brief Get total size of buffered audio data
     *
     * @return Total size in bytes
     */
    int  Size(void) const { return m_audio_queue.size() * m_frame_size; }

    /**
     * @brief Set buffer ready state
     *
     * @param val New ready state value
     */
    void SetReady(bool val) { m_initialized = val; }

    /**
     * @brief Check if buffer is ready
     *
     * @return true if ready, false otherwise
     */
    bool IsReady(void) const { return m_initialized; }

    /**
     * @brief Check if buffer has been flushed
     *
     * @return true if flushed, false otherwise
     */
    bool Flushed(void) const { return m_flushed; }

    /**
     * @brief Get audio codec name
     *
     * @return Codec name string
     */
    std::string CodecName(void) const { return m_codec_name; }

    /**
     * @brief Get number of audio channels
     *
     * @return Number of audio channels
     */
    int         NumChannels(void) const { return m_num_channels; }

    /**
     * @brief Get channel layout
     *
     * @return Pointer to channel layout structure
     */
    const AVChannelLayout* ChannelLayout(void) const { return &m_channel_layout; }

    /**
     * @brief Check if audio is LPCM format
     *
     * @return true if LPCM, false otherwise
     */
    bool LPCM(void) const { return m_lpcm; }

    /**
     * @brief Get sample rate
     *
     * @return Audio sample rate
     */
    int  SampleRate(void) const { return m_sample_rate; }

    /**
     * @brief Get bytes per sample
     *
     * @return Number of bytes per audio sample
     */
    int  BytesPerSample(void) const { return m_bytes_per_sample; }

    /**
     * @brief Get frame size
     *
     * @return Size of audio frame in bytes
     */
    int  FrameSize(void) const { return m_frame_size; }

    /**
     * @brief Get block size
     *
     * @return Block size in bytes
     */
    int  BlockSize(void) const { return m_block_size; }

    /**
     * @brief Detect audio codec from input
     *
     * @return true if codec detection successful, false otherwise
     */
    bool DetectCodec(void);

  private:
    /**
     * @brief Open S/PDIF format context
     *
     * @return true if successful, false otherwise
     */
    bool open_spdif_context(void);

    /**
     * @brief Open S/PDIF input
     *
     * @return true if successful, false otherwise
     */
    bool open_spdif(void);

    /**
     * @brief Initialize buffer after probing
     *
     * Copies probed frames to main queue and sets initialization state
     */
    void initialized(void);

    /**
     * @brief Structure to hold audio frame data and timestamp
     */
    using frame_t = struct {
        AudioFrame* frame;
        int64_t timestamp = {-1LL};
    };

    /**
     * @brief Type alias for audio frame queue
     */
    using frameque_t = std::deque<frame_t>;

    /**
     * @brief Main queue for audio frames
     */
    frameque_t m_audio_queue;

    /**
     * @brief Queue for probed frames during codec detection
     */
    frameque_t m_probed_queue;

    /**
     * @brief End-of-file flag atomically managed
     */
    std::atomic<bool> m_EoF  {false};

    /**
     * @brief Channel layout for audio
     */
    AVChannelLayout  m_channel_layout;

    /**
     * @brief S/PDIF format context for input
     */
    AVFormatContext* m_spdif_format_context {nullptr};

    /**
     * @brief S/PDIF AVIO context for input
     */
    AVIOContext*     m_spdif_avio_context   {nullptr};

    /**
     * @brief Buffer for S/PDIF AVIO context
     */
    uint8_t*         m_spdif_avio_context_buffer  {nullptr};

    /**
     * @brief S/PDIF codec for decoding
     */
    const AVCodec*   m_spdif_codec          {nullptr};

    /**
     * @brief S/PDIF codec ID
     */
    AVCodecID        m_spdif_codec_id;

    /**
     * @brief Last timestamp for audio frames
     */
    int64_t          m_timestamp            {-1};

    /**
     * @brief Flag indicating if audio is LPCM
     */
    bool             m_lpcm                 {true};

    /**
     * @brief Codec name string
     */
    std::string      m_codec_name;

    /**
     * @brief Number of audio channels
     */
    int              m_num_channels         {-1};

    /**
     * @brief Bytes per audio sample
     */
    int              m_bytes_per_sample     {-1};

    /**
     * @brief Size of audio frame in bytes
     */
    int              m_frame_size           {-1};

    /**
     * @brief Number of samples per frame
     */
    int              m_samples_per_frame    {-1};

    /**
     * @brief Audio sample rate
     */
    int              m_sample_rate          {-1};

    /**
     * @brief Block size in bytes
     */
    int              m_block_size           {-1};

    /**
     * @brief Pointer to parent AudioIO object
     */
    AudioIO*         m_parent               {nullptr};

    /**
     * @brief Initialization state flag
     */
    bool             m_initialized          {false};

    /**
     * @brief Flush state flag
     */
    bool             m_flushed              {false};

    /**
     * @brief Probing state flag
     */
    bool             m_probing              {true};

    /**
     * @brief Mutex for write operations
     */
    std::mutex       m_write_mutex;

    /**
     * @brief Condition variable for new buffer availability
     */
    std::condition_variable m_new_buffer;

    /**
     * @brief Condition variable for data availability
     */
    std::condition_variable m_data_avail;

    /**
     * @brief Buffer ID
     */
    int              m_id               {-1};

    /**
     * @brief Verbose level for logging
     */
    int              m_verbose          {0};

    /**
     * @brief Total bytes written to buffer
     */
    int              m_total_write      {0};

    /**
     * @brief Total bytes read from buffer
     */
    int              m_total_read       {0};

    /**
     * @brief Number of packets read
     */
    size_t           m_pkts_read        {0};
};

/**
 * @brief AudioIO class manages multiple audio buffers
 *
 * This class coordinates multiple AudioBuffer instances and provides
 * high-level audio management functionality for the muxing process.
 *
 * @author John Patrick Poet
 * @date 2022-2025
 */
class AudioIO
{
    friend AudioBuffer;

  public:
    /**
     * @brief Type alias for discard image callback function
     */
    using DiscardImageCallback = std::function<void (bool)>;

    /**
     * @brief Constructor for AudioIO
     *
     * @param discard Callback function for discarding images
     * @param verbose Verbose level for logging
     */
    AudioIO(DiscardImageCallback discard, int verbose = 0);

    /**
     * @brief Destructor for AudioIO
     *
     * Sets running flag to false to signal shutdown
     */
    ~AudioIO(void) { m_running.store(false); }

    /**
     * @brief Shutdown audio I/O system
     *
     * Stops all operations and signals shutdown to all buffers
     */
    void Shutdown(void);

    /**
     * @brief Add a new audio buffer with specified parameters
     *
     * @param num_channels Number of audio channels
     * @param is_lpcm Flag indicating LPCM format
     * @param bytes_per_sample Bytes per audio sample
     * @param sample_rate Audio sample rate
     * @param samples_per_frame Samples per audio frame
     * @param frame_size Size of audio frame in bytes
     * @return true if successful, false otherwise
     */
    bool AddBuffer(int num_channels, bool is_lpcm,
                   int bytes_per_sample, int sample_rate,
                   int samples_per_frame, int frame_size);

    /**
     * @brief Rescan S/PDIF input for audio format detection
     *
     * @return true if successful, false otherwise
     */
    bool      RescanSPDIF(void);

    /**
     * @brief Add audio frame to the last buffer
     *
     * @param buf Pointer to audio frame data
     * @param timestamp Timestamp for the frame
     * @return true if successful, false otherwise
     */
    bool      Add(AudioBuffer::AudioFrame *& buf, int64_t timestamp);

    /**
     * @brief Seek in audio data
     *
     * @param offset Seek offset
     * @param whence Seek mode
     * @return New position or -1 on error
     */
    int64_t   Seek(int64_t offset, int whence);

    /**
     * @brief Read audio data from buffers
     *
     * @param dest Destination buffer to read into
     * @param len Length of data to read
     * @return Number of bytes read
     */
    int       Read(uint8_t* dest, int32_t len);

    /**
     * @brief Read audio frame from S/PDIF input
     *
     * @return Pointer to AVPacket containing audio frame, or nullptr on error
     */
    AVPacket* ReadSPDIF(void);

    /**
     * @brief Get current buffer ID
     *
     * @return Current buffer ID
     */
    int     BufId(void) const;

    /**
     * @brief Get last buffer ID
     *
     * @return Last buffer ID
     */
    int     LastBufId(void) const { return m_buf_id - 1; }

    /**
     * @brief Get number of buffers
     *
     * @return Number of audio buffers
     */
    int     Buffers(void) const { return m_buffer_q.size(); }

    /**
     * @brief Get total size of buffered audio data
     *
     * @return Total size in bytes
     */
    int     Size(void) const;

    /**
     * @brief Check if all buffers are empty
     *
     * @return true if empty, false otherwise
     */
    bool    Empty(void) const;

    /**
     * @brief Check if first buffer is ready for data
     *
     * @return true if ready, false otherwise
     */
    bool    BlockReady(void) const;

    /**
     * @brief Get current timestamp
     *
     * @return Current timestamp
     */
    int64_t TimeStamp(void) const { return m_timestamp; }

    /**
     * @brief Get number of audio channels
     *
     * @return Number of audio channels
     */
    int     NumChannels(void) const { return m_num_channels; }

    /**
     * @brief Get audio codec name
     *
     * @return Codec name string
     */
    std::string CodecName(void) const { return m_codec_name; }

    /**
     * @brief Get channel layout
     *
     * @return Pointer to channel layout structure
     */
    const AVChannelLayout* ChannelLayout(void) const;

    /**
     * @brief Get sample rate
     *
     * @return Audio sample rate
     */
    int     SampleRate(void) const { return m_sample_rate; }

    /**
     * @brief Get bytes per sample
     *
     * @return Number of bytes per audio sample
     */
    int     BytesPerSample(void) const { return m_bytes_per_sample; }

    /**
     * @brief Check if audio is bitstream format
     *
     * @return true if bitstream, false if LPCM
     */
    bool    Bitstream(void) { return !m_lpcm; }

    /**
     * @brief Clear all audio queues
     *
     * Clears all audio buffers and queues
     */
    void    PurgeQueue(void);

    /**
     * @brief Reset audio system
     *
     * @param where Location identifier for logging
     */
    void    Reset(const std::string & where);

    /**
     * @brief Check if audio codec has changed
     *
     * @return true if codec changed, false otherwise
     */
    bool    CodecChanged(void);

  private:
    /**
     * @brief Type alias for buffer queue
     */
    using buffer_que_t = std::deque<AudioBuffer>;

    /**
     * @brief Queue of audio buffers
     */
    buffer_que_t     m_buffer_q;

    /**
     * @brief Iterator to last buffer in queue
     */
    buffer_que_t::iterator m_Iback;

    /**
     * @brief Codec name string
     */
    std::string      m_codec_name;

    /**
     * @brief Number of audio channels
     */
    int              m_num_channels     {2};

    /**
     * @brief Audio sample rate
     */
    int              m_sample_rate      {-1};

    /**
     * @brief Bytes per audio sample
     */
    int              m_bytes_per_sample {0};

    /**
     * @brief Flag indicating if audio is LPCM
     */
    bool             m_lpcm             {true};

    /**
     * @brief Current timestamp
     */
    int64_t          m_timestamp        {0LL};

    /**
     * @brief Mutex for buffer operations
     */
    mutable std::mutex m_buffer_mutex;

    /**
     * @brief Flag indicating if codec is initialized
     */
    bool             m_codec_initialized {false};

    /**
     * @brief Buffer ID counter
     */
    int              m_buf_id           {0};

    /**
     * @brief Running state flag
     */
    std::atomic<bool> m_running         {true};

    /**
     * @brief Callback for discarding images
     */
    DiscardImageCallback f_discard_images;

    /**
     * @brief Verbose level for logging
     */
    int              m_verbose          {1};
};

#endif
