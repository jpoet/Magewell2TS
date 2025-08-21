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
#if 0
    static const int k_max_eco_buffer_count = 10;
#endif
    static const int k_min_video_buffers = 6;

    using imageset_t = std::set<uint8_t*>;
    using imageque_t = std::deque<uint8_t*>;
    using ecoque_t   = std::set<MWCAP_VIDEO_ECO_CAPTURE_FRAME *>;

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
                 int quality, int look_ahead, bool no_audio, bool p010,
                 const std::string & gpu_device);
    void Shutdown(void);
    void Reset(void) { m_reset_audio.store(true); }

    bool operator! (void) { return m_fatal; }

  private:
    bool describe_input(HCHANNEL channel);

    bool update_HDRcolorspace(MWCAP_VIDEO_SIGNAL_STATUS signal_status);
    bool update_HDRframe(void);
    bool update_HDRinfo(void);

    void pro_image_buffer_available(uint8_t* pbImage, void* pEco);
    void eco_image_buffer_available(uint8_t* pbImage, void* pEco);
    bool add_pro_image_buffer(void);
    bool add_eco_image_buffer(void);
    void free_image_buffers(void);

    void set_notify(HNOTIFY&  notify,
                    HCHANNEL  hChannel,
                    MWCAP_PTR hNotifyEvent,
                    DWORD     flags);

    bool open_eco_video(MWCAP_VIDEO_ECO_CAPTURE_OPEN & eco_params);
    void close_eco_video(void);

    void capture_pro_video(MWCAP_VIDEO_ECO_CAPTURE_OPEN eco_params,
                           HNOTIFY video_notify,
                           MWCAP_PTR notify_event,
                           MWCAP_PTR capture_event,
                           int       frame_wrap_idx,
                           DWORD     event_mask,
                           ULONGLONG ullStatusBits,
                           bool interlaced);
    void capture_eco_video(MWCAP_VIDEO_ECO_CAPTURE_OPEN eco_params,
                           int eco_event,
                           HNOTIFY video_notify,
                           ULONGLONG ullStatusBits,
                           bool interlaced);

    bool capture_video(void);
    bool capture_audio(void);

  private:
    int m_audio_buf_frames {12288};

    OutputTS*            m_out2ts  {nullptr};
    HCHANNEL             m_channel {nullptr};
    MWCAP_CHANNEL_INFO   m_channel_info  {0};
    int                  m_channel_idx   {0};

    HDMI_INFOFRAME_PACKET m_infoPacket      {0};
    HDMI_HDR_INFOFRAME_PAYLOAD& m_HDRinfo {m_infoPacket.hdrInfoFramePayload};
    HDMI_INFOFRAME_PACKET m_infoPacket_prev {0};
    HDMI_HDR_INFOFRAME_PAYLOAD& m_HDRinfo_prev {m_infoPacket_prev.hdrInfoFramePayload};

    size_t       m_image_buffer_total     {0};
    size_t       m_image_buffer_avail     {0};
    size_t       m_image_buffers_desired  {6};
    size_t       m_image_buffers_inflight {0};
    imageset_t   m_image_buffers;
    imageque_t   m_avail_image_buffers;
    ecoque_t     m_eco_buffers;
    std::mutex   m_image_buffer_mutex;
    std::condition_variable m_image_returned;

    int m_num_pixels         {0};
    int m_image_size         {0};
    int m_min_stride         {0};
    int m_frame_ms           {17};
    int m_frame_ms2          {34};

    std::thread       m_audio_thread;

    std::atomic<bool> m_running     {true};
    std::atomic<bool> m_reset_audio {true};

    std::function<bool (void)>  f_open_video;

    bool m_isEco   {false};
    bool m_isHDR   {false};
    bool m_p010    {false};
    bool m_fatal   {false};
    int  m_verbose {1};
};

#endif
