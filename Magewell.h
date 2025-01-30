#ifndef Magewell_h_
#define Magewell_h_

#include <string>
#include <deque>
#include <set>

#include <mutex>
#include <condition_variable>

#include <MWFOURCC.h>
#include <LibMWCapture/MWCapture.h>
#include "LibMWCapture/MWEcoCapture.h"

#include "OutputTS.h"

class Magewell
{
    static const int k_max_eco_buffer_count = 4;

    using imageset_t = std::set<uint8_t*>;
    using imageque_t = std::deque<uint8_t*>;

  public:
    Magewell(void);
    ~Magewell(void);

    void Verbose(int v) { m_verbose = v; }
    bool OpenChannel(int idx, double boardId);
    bool CloseChannel(void);

    void ListInputs(void);
    bool WaitForInputs(int cnt) const;
    void DisplayVolume(void);
    bool SetVolume(int volume_level);
    bool ReadEDID(const std::string & filepath);
    bool WriteEDID(const std::string & filepath);

    bool Capture(const std::string & video_codec, const std::string & preset,
                 int quality, int look_ahead, bool no_audio,
                 const std::string & gpu_device);
    void Stop(void);
    void Reset(void) { m_reset.store(true); }


    bool operator! (void) { return m_fatal; }

  private:
    bool describe_input(HCHANNEL channel);

    void image_buffer_available(uint8_t* pbImage);
    bool add_image_buffer(DWORD dwImageSize);
    void free_image_buffers(void);

    void set_notify(HNOTIFY&  notify,
                    HCHANNEL  hChannel,
                    MWCAP_PTR hNotifyEvent,
                    DWORD     flags);

    bool capture_video(void);
    bool capture_pro_video(void);

    bool open_eco_video(MWCAP_VIDEO_ECO_CAPTURE_OPEN & eco_params,
                        MWCAP_VIDEO_ECO_CAPTURE_FRAME (& eco_frames)[k_max_eco_buffer_count]);
    void close_eco_video(MWCAP_VIDEO_ECO_CAPTURE_FRAME (& eco_frames)[k_max_eco_buffer_count]);

    bool capture_audio(void);

  private:
    int m_audio_buf_sz   {2048};  // TODO: Needs to be made adaptive

    OutputTS*            m_out2ts  {nullptr};
    HCHANNEL             m_channel {nullptr};
    MWCAP_CHANNEL_INFO   m_channel_info  {0};
    int                  m_channel_idx   {0};

    imageset_t           m_image_buffers;
    imageque_t           m_avail_image_buffers;
    std::mutex           m_image_buffer_mutex;
    std::condition_variable m_image_returned;

    bool m_isEco            {false};
    int  m_frame_ms         {17};
    int  m_frame_ms2        {34};
    int  m_image_buffer_cnt {0};

    std::thread       m_audio_thread;

    std::atomic<bool> m_running  {true};
    std::atomic<bool> m_reset    {false};

    std::function<bool (void)>  f_open_video;

    bool m_fatal   {false};
    int  m_verbose {1};
};

#endif
