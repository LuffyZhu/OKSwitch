#include "okswitch/media/VideoPlayer.hpp"
#include "okswitch/media/CurlVideoInput.hpp"
#include "okswitch/media/LivePacketBuffer.hpp"

#include <switch.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <sstream>
#include <vector>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace okswitch::media
{
namespace
{
struct LiveDeinterlacer {
    AVFilterGraph* graph=nullptr;
    AVFilterContext* input=nullptr;
    AVFilterContext* output=nullptr;
    AVFrame* frame=av_frame_alloc();
    int width=0,height=0,pixelFormat=-1;
    ~LiveDeinterlacer(){reset();av_frame_free(&frame);}
    void reset(){avfilter_graph_free(&graph);input=output=nullptr;}
    int process(AVFrame* source,AVRational timeBase){
        if(!frame)return AVERROR(ENOMEM);
        if(graph&&(width!=source->width||height!=source->height||pixelFormat!=source->format))reset();
        if(!graph){
            graph=avfilter_graph_alloc();if(!graph)return AVERROR(ENOMEM);
            graph->nb_threads=2;
            width=source->width;height=source->height;pixelFormat=source->format;
            const AVRational sar=source->sample_aspect_ratio.num>0?source->sample_aspect_ratio:AVRational{1,1};
            const std::string args="video_size="+std::to_string(width)+"x"+std::to_string(height)
                +":pix_fmt="+std::to_string(pixelFormat)+":time_base="+std::to_string(timeBase.num)+"/"+std::to_string(timeBase.den)
                +":pixel_aspect="+std::to_string(sar.num)+"/"+std::to_string(sar.den);
            AVFilterContext* filter=nullptr;
            int rc=avfilter_graph_create_filter(&input,avfilter_get_by_name("buffer"),"input",args.c_str(),nullptr,graph);
            if(rc>=0)rc=avfilter_graph_create_filter(&filter,avfilter_get_by_name("yadif"),"deinterlace","mode=send_frame:parity=auto:deint=interlaced",nullptr,graph);
            if(rc>=0)rc=avfilter_graph_create_filter(&output,avfilter_get_by_name("buffersink"),"output",nullptr,nullptr,graph);
            if(rc>=0)rc=avfilter_link(input,0,filter,0);
            if(rc>=0)rc=avfilter_link(filter,0,output,0);
            if(rc>=0)rc=avfilter_graph_config(graph,nullptr);
            if(rc<0){reset();return rc;}
        }
        av_frame_unref(frame);
        int rc=av_buffersrc_add_frame_flags(input,source,AV_BUFFERSRC_FLAG_KEEP_REF);
        return rc<0?rc:av_buffersink_get_frame(output,frame);
    }
};
std::string clockText(double seconds)
{
    const int value = std::max(0, static_cast<int>(seconds));
    std::ostringstream text;
    text << value / 60 << ':';
    if (value % 60 < 10) text << '0';
    text << value % 60;
    return text.str();
}

std::string ffmpegError(int code)
{
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, message, sizeof(message));
    return message;
}

const char* hardwareDecoder(AVCodecID id)
{
    switch (id)
    {
        case AV_CODEC_ID_H264: return "h264_nvtegra";
        case AV_CODEC_ID_HEVC: return "hevc_nvtegra";
        case AV_CODEC_ID_MPEG1VIDEO: return "mpeg1_nvtegra";
        case AV_CODEC_ID_MPEG2VIDEO: return "mpeg2_nvtegra";
        case AV_CODEC_ID_MPEG4: return "mpeg4_nvtegra";
        case AV_CODEC_ID_VP8: return "vp8_nvtegra";
        case AV_CODEC_ID_VP9: return "vp9_nvtegra";
        case AV_CODEC_ID_VC1: return "vc1_nvtegra";
        default: return nullptr;
    }
}

AVCodecContext* openDecoder(const AVCodecParameters* parameters, bool video,
    bool allowHardware = true)
{
    const AVCodec* codec = nullptr;
    if (video && allowHardware)
    {
        const char* name = hardwareDecoder(parameters->codec_id);
        if (name != nullptr) codec = avcodec_find_decoder_by_name(name);
    }
    if (codec != nullptr)
    {
        AVCodecContext* context = avcodec_alloc_context3(codec);
        if (context != nullptr
            && avcodec_parameters_to_context(context, parameters) >= 0
            && avcodec_open2(context, codec, nullptr) >= 0)
        {
            return context;
        }
        avcodec_free_context(&context);
    }
    codec = avcodec_find_decoder(parameters->codec_id);
    if (codec == nullptr) return nullptr;
    AVCodecContext* context = avcodec_alloc_context3(codec);
    if (context == nullptr
        || avcodec_parameters_to_context(context, parameters) < 0)
    {
        avcodec_free_context(&context);
        return nullptr;
    }
    if (video)
    {
        context->thread_count = 4;
        context->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        context->flags2 |= AV_CODEC_FLAG2_FAST;
        context->skip_loop_filter = AVDISCARD_NONREF;
    }
    if (avcodec_open2(context, codec, nullptr) < 0)
    {
        avcodec_free_context(&context);
        return nullptr;
    }
    return context;
}

bool hasDeclaredVideoTrack(const AVFormatContext* format)
{
    if (format == nullptr) return false;
    for (unsigned i = 0; i < format->nb_streams; ++i)
    {
        const AVCodecParameters* parameters = format->streams[i]->codecpar;
        if (parameters != nullptr
            && parameters->codec_type == AVMEDIA_TYPE_VIDEO
            && parameters->codec_id != AV_CODEC_ID_NONE
            && !(format->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC))
            return true;
    }
    return false;
}
bool hasDeclaredAudioTrack(const AVFormatContext* format)
{
    if (format == nullptr) return false;
    for (unsigned i = 0; i < format->nb_streams; ++i)
    {
        const AVCodecParameters* parameters = format->streams[i]->codecpar;
        if (parameters != nullptr && parameters->codec_type == AVMEDIA_TYPE_AUDIO
            && parameters->codec_id != AV_CODEC_ID_NONE)
            return true;
    }
    return false;
}

int findPrimaryVideoStream(const AVFormatContext* format)
{
    if (format == nullptr) return AVERROR_STREAM_NOT_FOUND;
    int best = AVERROR_STREAM_NOT_FOUND;
    int bestScore = -1;
    for (unsigned i = 0; i < format->nb_streams; ++i)
    {
        const AVStream* stream = format->streams[i];
        const AVCodecParameters* parameters = stream->codecpar;
        if (parameters == nullptr
            || parameters->codec_type != AVMEDIA_TYPE_VIDEO
            || parameters->codec_id == AV_CODEC_ID_NONE
            || (stream->disposition & AV_DISPOSITION_ATTACHED_PIC))
            continue;
        int score = (stream->disposition & AV_DISPOSITION_DEFAULT) ? 1000 : 0;
        if (parameters->codec_id == AV_CODEC_ID_HEVC
            || parameters->codec_id == AV_CODEC_ID_H264
            || parameters->codec_id == AV_CODEC_ID_VP9)
            score += 500;
        score += std::min(400, parameters->width * parameters->height / 10000);
        if (score > bestScore) { bestScore = score; best = static_cast<int>(i); }
    }
    return best;
}
}

PlaybackResult VideoPlayer::play(
    SDL_Renderer* renderer,
    const cloud::PlaybackSource& source,
    const PlaybackOptions& options,
    const std::function<void(const std::string&)>& showStatus,
    const std::function<void(const std::string&, int, int, int)>& drawText)
{
    PlaybackResult result;
    int introSkipSeconds=std::max(0,options.introSkipSeconds);
    int outroSkipSeconds=std::max(0,options.outroSkipSeconds);
    if (renderer == nullptr || source.url.empty())
    {
        result.error = "播放地址为空";
        return result;
    }

    // Keep the display awake while preparing and playing media. Restore the
    // system's normal dimming and auto-sleep policy on every exit path.
    struct MediaPlaybackStateGuard
    {
        MediaPlaybackStateGuard() { appletSetMediaPlaybackState(true); }
        ~MediaPlaybackStateGuard() { appletSetMediaPlaybackState(false); }
    } mediaPlaybackState;

    std::atomic<bool> prepareCancelled{false};
    std::atomic<bool> networkCancelled{false};
    bool cancelled = false, timedOut = false;
    Uint64 loadingStarted = SDL_GetTicks64();
    std::string stage = "读取视频文件头";
    std::function<bool()> interrupt = [&]() {
        return prepareCancelled.load(std::memory_order_relaxed)
            || networkCancelled.load(std::memory_order_relaxed);
    };
    const bool directNetworkInput = source.segmentUrls.empty() && (source.url.rfind("rtmp://", 0) == 0
        || source.url.rfind("rtmps://", 0) == 0
        || (source.url.rfind("http://", 0) == 0
            && source.url.find(".m3u8") != std::string::npos));
    CurlVideoInput input(source, interrupt);
    std::unique_ptr<CurlVideoInput> separateAudioInput;
    if(!source.audioUrl.empty()){
        cloud::PlaybackSource audioSource=source;
        audioSource.url=source.audioUrl;
        audioSource.audioUrl.clear();
        separateAudioInput=std::make_unique<CurlVideoInput>(audioSource,interrupt);
    }
    AVIOContext* io = nullptr;
    AVIOContext* audioIo = nullptr;
    AVFormatContext* format = nullptr;
    AVFormatContext* audioFormat = nullptr;
    AVCodecContext* videoCodec = nullptr;
    AVCodecContext* audioCodec = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* decoded = nullptr;
    AVFrame* software = nullptr;
    AVFrame* yuv = nullptr;
    LiveDeinterlacer deinterlacer;
    SwsContext* scaler = nullptr;
    SwrContext* resampler = nullptr;
    SDL_Texture* texture = nullptr;
    SDL_AudioDeviceID audioDevice = 0;
    AVChannelLayout outputLayout{};
    struct ControlPollState
    {
        PadState pad{};
        std::atomic<bool> active{false};
        std::atomic<u64> pending{0};
        std::atomic<u64> seekPresses{0};
        std::atomic<u64> held{0};
        std::atomic<int> horizontalDirection{0};
        std::atomic<bool> playbackReady{false};
        std::atomic<bool> qualityMenuOpen{false};
        std::atomic<bool> settingsOpen{false};
        std::atomic<bool>* networkCancelled = nullptr;
        bool interruptNetworkReads=false;
        int stickDirection=0;
        Uint64 stickRepeatAt=0;
        int buttonDirection=0;
        Uint64 buttonRepeatAt=0;
    } controlPoll;
    controlPoll.networkCancelled = &networkCancelled;
    controlPoll.interruptNetworkReads = !directNetworkInput;
    padInitializeDefault(&controlPoll.pad);
    SDL_Thread* controlThread = nullptr;
    LivePacketBuffer liveBuffer;

    struct AudioPrepareState {
        AVFormatContext** format=nullptr;
        std::atomic<bool> done{false};
        int rc=0;
    } audioPrepare;
    SDL_Thread* audioThread=nullptr;

    auto cleanup = [&]() {
        prepareCancelled.store(true, std::memory_order_relaxed);
        if (audioThread) { SDL_WaitThread(audioThread, nullptr); audioThread=nullptr; }

        liveBuffer.stop();
        controlPoll.active.store(false, std::memory_order_relaxed);
        if (controlThread != nullptr)
        {
            SDL_WaitThread(controlThread, nullptr);
            controlThread = nullptr;
        }
        if (audioDevice != 0)
        {
            SDL_ClearQueuedAudio(audioDevice);
            SDL_CloseAudioDevice(audioDevice);
        }
        SDL_DestroyTexture(texture);
        swr_free(&resampler);
        av_channel_layout_uninit(&outputLayout);
        sws_freeContext(scaler);
        av_frame_free(&yuv);
        av_frame_free(&software);
        av_frame_free(&decoded);
        av_packet_free(&packet);
        avcodec_free_context(&audioCodec);
        avcodec_free_context(&videoCodec);
        avformat_close_input(&audioFormat);
        avformat_close_input(&format);
        if (audioIo) { av_freep(&audioIo->buffer); avio_context_free(&audioIo); }
        if (io) { av_freep(&io->buffer); avio_context_free(&io); }
    };
    auto fail = [&](const std::string& message) {
        result.error = cancelled ? "已取消加载" : timedOut
            ? "加载超时：" + stage + " · " + input.stats() : message;
        cleanup();
        return result;
    };

    if (!directNetworkInput)
    {
        auto* buffer = static_cast<unsigned char*>(av_malloc(32768));
        if (!buffer) return fail("播放器读取缓冲内存不足");
        io = avio_alloc_context(buffer, 32768, 0, &input,
            CurlVideoInput::read, nullptr, CurlVideoInput::seek);
        if (!io) { av_free(buffer); return fail("无法创建播放器读取接口"); }
    }
    format = avformat_alloc_context();
    if (!format) return fail("播放器内存不足");
    if (io)
    {
        format->pb = io;
        format->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_NONBLOCK;
    }
    format->interrupt_callback.callback = [](void* context) -> int {
        return (*static_cast<std::function<bool()>*>(context))() ? 1 : 0;
    };
    format->interrupt_callback.opaque = &interrupt;
    // Live HLS uses rolling MPEG-TS fragments. Give FFmpeg a larger network
    // window so a brief CDN slowdown at a fragment boundary does not
    // immediately starve decoding.
    format->probesize = directNetworkInput ? 2 * 1024 * 1024 : 1024 * 1024;
    format->max_analyze_duration = 2 * AV_TIME_BASE;
    struct PrepareState
    {
        AVFormatContext** format = nullptr;
        const char* url = nullptr;
        AVDictionary* options = nullptr;
        std::atomic<int> stage{0};
        bool inspectStreams = false;
        std::atomic<bool> done{false};
        int rc = 0;
    } prepare;
    prepare.format = &format;
    prepare.url = directNetworkInput ? source.url.c_str() : nullptr;
    if (directNetworkInput)
    {
        // The live demuxer opens its own connections, outside CurlVideoInput.
        // Preserve provider headers for both playlist reloads and TS fragments.
        std::string liveHeaders;
        for (const auto& header : source.headers)
            liveHeaders += header.name + ": " + header.value + "\r\n";
        if (!liveHeaders.empty())
            av_dict_set(&prepare.options, "headers", liveHeaders.c_str(), 0);
        av_dict_set(&prepare.options, "buffer_size", "8388608", 0);
        av_dict_set(&prepare.options, "rtbufsize", "16777216", 0);
        av_dict_set(&prepare.options, "reconnect", "1", 0);
        av_dict_set(&prepare.options, "reconnect_streamed", "1", 0);
        av_dict_set(&prepare.options, "reconnect_on_network_error", "1", 0);
        av_dict_set(&prepare.options, "reconnect_delay_max", "2", 0);
        av_dict_set(&prepare.options, "rw_timeout", "15000000", 0);
    }
    // Live MPEG-TS often declares video in the first PAT/PMT but exposes its
    // audio PID only after stream analysis. Skipping this pass caused CCTV to
    // open with picture and no sound.
    prepare.inspectStreams = directNetworkInput || !source.segmentUrls.empty();
    if(separateAudioInput){
        auto* buffer=static_cast<unsigned char*>(av_malloc(32768));
        if(!buffer)return fail("音轨读取缓冲内存不足");
        audioIo=avio_alloc_context(buffer,32768,0,separateAudioInput.get(),
            CurlVideoInput::read,nullptr,CurlVideoInput::seek);
        if(!audioIo){av_free(buffer);return fail("无法创建音轨读取接口");}
        audioFormat=avformat_alloc_context();
        if(!audioFormat)return fail("音轨内存不足");
        audioFormat->pb=audioIo;
        audioFormat->flags|=AVFMT_FLAG_CUSTOM_IO|AVFMT_FLAG_NONBLOCK;
        audioFormat->interrupt_callback.callback=[](void* context)->int{
            return (*static_cast<std::function<bool()>*>(context))()?1:0;
        };
        audioFormat->interrupt_callback.opaque=&interrupt;
        audioPrepare.format=&audioFormat;
        audioThread=SDL_CreateThread([](void* opaque)->int{
            auto& state=*static_cast<AudioPrepareState*>(opaque);
            state.rc=avformat_open_input(state.format,nullptr,nullptr,nullptr);
            // MP4 declares its audio codec in the container header. Scanning
            // remote packets here adds more range requests before first frame.
            if(state.rc>=0&&!hasDeclaredAudioTrack(*state.format))
                state.rc=avformat_find_stream_info(*state.format,nullptr);
            state.done.store(true,std::memory_order_release);return state.rc;
        },"okswitch-audio-probe",&audioPrepare);
        if(!audioThread)return fail("无法启动音轨读取线程");
    }
    SDL_Thread* prepareThread = SDL_CreateThread([](void* opaque) -> int {
        auto& state = *static_cast<PrepareState*>(opaque);
        state.stage.store(1, std::memory_order_relaxed);
        state.rc = avformat_open_input(state.format, state.url, nullptr,
            state.url != nullptr ? &state.options : nullptr);
        // A Matroska track may omit dimensions until the first coded frame.
        // The decoder can learn them there, so a declared video codec is enough
        // to avoid scanning many remote clusters for estimates.
        if (state.rc >= 0
            && (state.inspectStreams || !hasDeclaredVideoTrack(*state.format)))
        {
            state.stage.store(2, std::memory_order_relaxed);
            state.rc = avformat_find_stream_info(*state.format, nullptr);
        }
        state.done.store(true, std::memory_order_release);
        return state.rc;
    }, "okswitch-media-probe", &prepare);
    if (prepareThread == nullptr)
    {
        av_dict_free(&prepare.options);
        return fail("无法启动媒体分析线程");
    }

    PadState loadingPad;
    padInitializeDefault(&loadingPad);
    Uint64 lastStatus = 0;
    Uint64 lastPrepareProgress = loadingStarted;
    uint64_t lastPrepareBytes = input.downloadedBytes()
        + (separateAudioInput ? separateAudioInput->downloadedBytes() : 0);
    while (!prepare.done.load(std::memory_order_acquire)
        || (audioThread && !audioPrepare.done.load(std::memory_order_acquire)))
    {
        if (!appletMainLoop()) cancelled = true;
        padUpdate(&loadingPad);
        const u64 loadingKeys=padGetButtonsDown(&loadingPad);
        if(loadingKeys&HidNpadButton_B)cancelled=true;
        if((loadingKeys&HidNpadButton_Minus)&&options.openSettings){
            const Uint64 settingsStarted=SDL_GetTicks64();
            options.openSettings();
            const Uint64 elapsed=SDL_GetTicks64()-settingsStarted;
            loadingStarted+=elapsed;lastPrepareProgress+=elapsed;
        }
        SDL_Event event;
        while (SDL_PollEvent(&event))
            if (event.type == SDL_QUIT) cancelled = true;
        const Uint64 now = SDL_GetTicks64();
        const uint64_t prepareBytes = input.downloadedBytes()
            + (separateAudioInput ? separateAudioInput->downloadedBytes() : 0);
        if (prepareBytes > lastPrepareBytes)
        {
            lastPrepareBytes = prepareBytes;
            lastPrepareProgress = now;
        }
        if ((!directNetworkInput && now - lastPrepareProgress >= 20000)
            || now - loadingStarted >= 60000)
            timedOut = true;
        if (cancelled || timedOut)
            prepareCancelled.store(true, std::memory_order_relaxed);
        if (now - lastStatus >= 100)
        {
            lastStatus = now;
            stage = separateAudioInput && !audioPrepare.done.load(std::memory_order_acquire)
                ? "并行读取音视频文件头"
                : prepare.stage.load(std::memory_order_relaxed) >= 2
                    ? "分析音视频轨道" : "读取视频文件头";
            showStatus(stage + " · " + (directNetworkInput ? std::string("连接直播流") : input.stats()) + " · "
                + std::to_string((now - loadingStarted) / 1000) + " 秒 · B 取消");
        }
        SDL_Delay(16);
    }
    SDL_WaitThread(prepareThread, nullptr);
    if (audioThread) { SDL_WaitThread(audioThread, nullptr); audioThread=nullptr; }
    av_dict_free(&prepare.options);
    stage = prepare.stage.load(std::memory_order_relaxed) >= 2
        ? "分析音视频轨道" : "读取视频文件头";
    const int rcPrepare = prepare.rc;
    if (cancelled || timedOut) return fail("已取消加载");
    if (rcPrepare < 0)
    {
        if (stage == "读取视频文件头")
            return fail("无法打开播放地址："
                + (directNetworkInput || input.error.empty() ? ffmpegError(rcPrepare) : input.error));
        return fail("无法读取媒体信息：" + ffmpegError(rcPrepare));
    }
    if (separateAudioInput && audioPrepare.rc < 0)
        return fail("无法读取独立音轨：" + ffmpegError(audioPrepare.rc));
    int rc = 0;

    if (input.isSegmented())
    {
        struct BufferState { CurlVideoInput* input=nullptr;std::atomic<bool> done{false};bool ok=false; } bufferState;
        bufferState.input=&input;
        const size_t initialSegments=std::max<size_t>(1,source.initialBufferSegments);
        const double startSeconds=options.resumeSeconds>1?options.resumeSeconds:static_cast<double>(introSkipSeconds);
        struct InitialBufferState { BufferState* buffer=nullptr;size_t count=0;double seconds=0; } initialBuffer{&bufferState,initialSegments,startSeconds};
        SDL_Thread* bufferThread=SDL_CreateThread([](void* opaque)->int{auto& initial=*static_cast<InitialBufferState*>(opaque);auto& state=*initial.buffer;state.ok=state.input->prebufferAtSeconds(initial.seconds,initial.count);state.done.store(true,std::memory_order_release);return state.ok?0:1;},"okswitch-hls-buffer",&initialBuffer);
        if(bufferThread==nullptr)return fail("无法启动视频缓存线程");
        stage="预缓冲视频";Uint64 bufferStarted=SDL_GetTicks64(),lastBufferProgress=bufferStarted;uint64_t lastBufferBytes=input.downloadedBytes();
        while(!bufferState.done.load(std::memory_order_acquire)){
            if(!appletMainLoop())cancelled=true;
            padUpdate(&loadingPad);
            const u64 loadingKeys=padGetButtonsDown(&loadingPad);
            if(loadingKeys&HidNpadButton_B)cancelled=true;
            if((loadingKeys&HidNpadButton_Minus)&&options.openSettings){
                const Uint64 settingsStarted=SDL_GetTicks64();
                options.openSettings();
                const Uint64 elapsed=SDL_GetTicks64()-settingsStarted;
                bufferStarted+=elapsed;lastBufferProgress+=elapsed;
            }
            SDL_Event event;while(SDL_PollEvent(&event))if(event.type==SDL_QUIT)cancelled=true;
            const Uint64 now=SDL_GetTicks64();const uint64_t bytes=input.downloadedBytes();if(bytes>lastBufferBytes){lastBufferBytes=bytes;lastBufferProgress=now;}if(now-lastBufferProgress>=20000||now-bufferStarted>=90000)timedOut=true;if(cancelled||timedOut)prepareCancelled.store(true,std::memory_order_relaxed);
            if(now-lastStatus>=100){lastStatus=now;showStatus(stage+" · "+input.stats()+" · 首播缓存 "+std::to_string(initialSegments)+" 段 · "+std::to_string((now-bufferStarted)/1000)+" 秒 · B 取消");}SDL_Delay(16);
        }
        SDL_WaitThread(bufferThread,nullptr);if(cancelled||timedOut)return fail("预缓冲视频失败");if(!bufferState.ok)return fail("预缓冲视频失败："+input.error);prepareCancelled.store(false,std::memory_order_relaxed);
    }

    int videoStream = findPrimaryVideoStream(format);
    if (videoStream < 0)
        videoStream = av_find_best_stream(
            format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    AVFormatContext* audioContainer=audioFormat?audioFormat:format;
    const int audioStream=av_find_best_stream(
        audioContainer,AVMEDIA_TYPE_AUDIO,-1,-1,nullptr,0);
    if (videoStream < 0) return fail("媒体中没有可播放的视频轨道");
    if(audioFormat&&audioStream<0)return fail("独立音轨中没有可播放的声音");
    if (interrupt()) return fail("已取消加载");
    stage = "启动视频解码器";
    showStatus(stage);
    // The nvtegra path produces corrupted grey/white frames for the
    // interlaced H.264 carried by CCTV's rolling MPEG-TS streams. 720p live
    // television is light enough for the software decoder and is reliable.
    videoCodec = openDecoder(format->streams[videoStream]->codecpar, true,
        !source.forceSoftwareVideo);
    if (videoCodec == nullptr) return fail("无法启动视频解码器");
    stage = "启动音频解码器";
    showStatus(stage);
    if (audioStream >= 0)
        audioCodec = openDecoder(audioContainer->streams[audioStream]->codecpar, false);
    if(audioFormat&&!audioCodec)return fail("无法启动独立音轨解码器");

    packet = av_packet_alloc();
    decoded = av_frame_alloc();
    software = av_frame_alloc();
    yuv = av_frame_alloc();
    if (packet == nullptr || decoded == nullptr || software == nullptr || yuv == nullptr)
        return fail("播放器内存不足");

    SDL_AudioSpec obtained{};
    int bytesPerSecond = 0;
    if (audioCodec != nullptr)
    {
        SDL_AudioSpec wanted{};
        wanted.freq = 48000;
        wanted.format = AUDIO_S16SYS;
        wanted.channels = 2;
        wanted.samples = 2048;
        audioDevice = SDL_OpenAudioDevice(nullptr, 0, &wanted, &obtained, 0);
        if (audioDevice != 0)
        {
            av_channel_layout_default(&outputLayout, obtained.channels);
            AVChannelLayout inputLayout{};
            if (audioCodec->ch_layout.nb_channels > 0)
                av_channel_layout_copy(&inputLayout, &audioCodec->ch_layout);
            else
                av_channel_layout_default(&inputLayout, 2);
            rc = swr_alloc_set_opts2(&resampler,
                &outputLayout, AV_SAMPLE_FMT_S16, obtained.freq,
                &inputLayout, audioCodec->sample_fmt,
                audioCodec->sample_rate > 0 ? audioCodec->sample_rate : obtained.freq,
                0, nullptr);
            av_channel_layout_uninit(&inputLayout);
            if (rc < 0 || swr_init(resampler) < 0)
            {
                swr_free(&resampler);
                SDL_CloseAudioDevice(audioDevice);
                audioDevice = 0;
            }
            else
            {
                bytesPerSecond = obtained.freq * obtained.channels
                    * SDL_AUDIO_BITSIZE(obtained.format) / 8;
                SDL_PauseAudioDevice(audioDevice, 0);
            }
        }
    }

    controlPoll.active.store(true, std::memory_order_relaxed);
    controlThread = SDL_CreateThread([](void* opaque) -> int {
        auto& state = *static_cast<ControlPollState*>(opaque);
        while (state.active.load(std::memory_order_relaxed))
        {
            if(state.settingsOpen.load(std::memory_order_relaxed)){SDL_Delay(8);continue;}
            padUpdate(&state.pad);
            u64 down = padGetButtonsDown(&state.pad);
        u64 seekPresses = down & (HidNpadButton_Left | HidNpadButton_Right);
            const u64 heldButtons=padGetButtons(&state.pad);
            int nextButton=0;
            if(heldButtons&HidNpadButton_Right)nextButton=1;
            else if(heldButtons&HidNpadButton_Left)nextButton=2;
            else if(heldButtons&HidNpadButton_Up)nextButton=3;
            else if(heldButtons&HidNpadButton_Down)nextButton=4;
            const Uint64 now=SDL_GetTicks64();
            bool horizontalStarted=false;
            if(nextButton){
                const bool fire=nextButton!=state.buttonDirection||now>=state.buttonRepeatAt;
                if(nextButton!=state.buttonDirection&&(nextButton==1||nextButton==2))horizontalStarted=true;
                if(fire)down|=nextButton==1?HidNpadButton_Right
                    :nextButton==2?HidNpadButton_Left
                    :nextButton==3?HidNpadButton_Up:HidNpadButton_Down;
                if(fire)state.buttonRepeatAt=now+(nextButton!=state.buttonDirection?360:90);
                state.buttonDirection=nextButton;
            }else state.buttonDirection=0;
            HidAnalogStickState stick=padGetStickPos(&state.pad,0);
            const HidAnalogStickState otherStick=padGetStickPos(&state.pad,1);
            const int firstMagnitude=std::max(stick.x<0?-stick.x:stick.x,
                stick.y<0?-stick.y:stick.y);
            const int secondMagnitude=std::max(otherStick.x<0?-otherStick.x:otherStick.x,
                otherStick.y<0?-otherStick.y:otherStick.y);
            if(secondMagnitude>firstMagnitude)stick=otherStick;
            const int absX=stick.x<0?-stick.x:stick.x,absY=stick.y<0?-stick.y:stick.y;
            int nextStick=0;
            if(absX>16000||absY>16000)
                nextStick=absX>absY?(stick.x>0?1:2):(stick.y>0?3:4);
            if(nextStick){
                const bool fire=nextStick!=state.stickDirection||now>=state.stickRepeatAt;
                if(nextStick!=state.stickDirection&&(nextStick==1||nextStick==2)){
                    horizontalStarted=true;
                    seekPresses|=nextStick==1?HidNpadButton_Right:HidNpadButton_Left;
                }
                if(fire)down|=nextStick==1?HidNpadButton_Right
                    :nextStick==2?HidNpadButton_Left
                    :nextStick==3?HidNpadButton_Up:HidNpadButton_Down;
                if(fire)state.stickRepeatAt=now+(nextStick!=state.stickDirection?320:110);
                state.stickDirection=nextStick;
            }else state.stickDirection=0;
            state.pending.fetch_or(down, std::memory_order_relaxed);
            state.held.store(heldButtons, std::memory_order_relaxed);
            state.horizontalDirection.store(nextButton==1?1:nextButton==2?-1:
                nextStick==1?1:nextStick==2?-1:0,std::memory_order_relaxed);
            state.seekPresses.fetch_or(seekPresses, std::memory_order_relaxed);
            const bool playback = state.playbackReady.load(std::memory_order_relaxed);
            // Direction presses are accumulated by the UI and committed as a
            // single seek. Interrupting AVIO for every press makes rapid seeks
            // visibly stall once per click.
            const u64 interruptButtons = state.qualityMenuOpen.load(std::memory_order_relaxed)
                ? HidNpadButton_Minus : HidNpadButton_A | HidNpadButton_B | HidNpadButton_Minus;
            if (playback && state.interruptNetworkReads
                && ((down & interruptButtons)||horizontalStarted))
                state.networkCancelled->store(true, std::memory_order_relaxed);
            SDL_Delay(8);
        }
        return 0;
    }, "okswitch-controls", &controlPoll);
    if (controlThread == nullptr)
    {
        controlPoll.active.store(false, std::memory_order_relaxed);
        return fail("无法启动手柄读取线程");
    }
    bool running = true;
    bool paused = false;
    bool exitedByBack = false;
    bool playbackReady = false;
    bool menuOpen = false;
    bool qualityMenuOpen = false;
    int qualityMenuIndex = std::clamp(options.currentQualityIndex,0,
        std::max(0,(int)options.liveQualityLabels.size()-1));
    bool progressFocused = false;
    bool seekPreview = false;
    bool buffering = false;
    bool exitConfirm = false;
    bool waitingForVideo = true;
    int menuIndex = 0;
    int volumePercent = std::clamp(options.volumePercent, 0, 100);
    double seekDeltaSeconds = 0;
    double menuSeekTarget = -1;
    double seekPreviewTarget = -1;
    Uint64 seekCommitAt = 0;
    int seekHoldDirection = 0;
    Uint64 seekHoldStarted = 0;
    double seekHoldApplied = 0;
    // Integrate elapsed hold time rather than adding one jump per repeat event.
    // After a short tap window, ramp from 60 to 180 media seconds per second.
    auto adjustSeekTarget = [&](double& target, u64 presses, int direction) {
        const Uint64 now = SDL_GetTicks64();
        if (presses || direction != seekHoldDirection) {
            seekHoldDirection = direction;
            seekHoldStarted = now;
            seekHoldApplied = 0;
            if (presses || direction)
                target += (direction ? direction : (presses & HidNpadButton_Right ? 1 : -1)) * 10.0;
        }
        if (direction) {
            const double seconds = std::max(0.0, (now - seekHoldStarted) / 1000.0 - 0.30);
            const double ramp = std::min(seconds, 3.0);
            const double distance = 60.0 * ramp + 20.0 * ramp * ramp
                + 180.0 * std::max(0.0, seconds - 3.0);
            target += direction * (distance - seekHoldApplied);
            seekHoldApplied = distance;
        }
        target = std::max(0.0, result.durationSeconds > 0
            ? std::min(result.durationSeconds, target) : target);
    };
    bool menuIntroChanged = false;
    bool menuPreferencesChanged = false;
    std::string notice;
    Uint64 noticeUntil = 0;
    Uint64 wallStart = SDL_GetTicks64();
    Uint64 menuStarted = 0;
    Uint64 bufferingStarted = 0;
    auto pauseAudio = [&]() {
        if (audioDevice != 0)
            SDL_PauseAudioDevice(audioDevice, (paused || menuOpen || seekPreview || buffering || exitConfirm || waitingForVideo) ? 1 : 0);
    };
    auto persistPreferences = [&]() {
        if(options.onPreferencesChanged)
            options.onPreferencesChanged(introSkipSeconds,outroSkipSeconds,volumePercent);
    };
    auto closeMenuAndApply = [&]() {
        if(!menuOpen)return;
        menuOpen=false;
        seekHoldDirection=0;
        wallStart+=SDL_GetTicks64()-menuStarted;
        if(menuSeekTarget>=0&&std::abs(menuSeekTarget-result.positionSeconds)>0.01){
            seekDeltaSeconds=menuSeekTarget-result.positionSeconds;
            seekCommitAt=0;
        }
        menuSeekTarget=-1;
        progressFocused=false;
        if(menuIntroChanged&&result.positionSeconds<introSkipSeconds){
            seekDeltaSeconds=introSkipSeconds-result.positionSeconds;
            seekCommitAt=0;
        }
        if(menuPreferencesChanged)persistPreferences();
        menuIntroChanged=false;
        menuPreferencesChanged=false;
        pauseAudio();
    };
    auto controls = [&]() {
        if (!appletMainLoop())
        {
            running = false;
            return;
        }
        u64 down = controlPoll.pending.exchange(0,
            std::memory_order_relaxed);
        const u64 seekPresses = controlPoll.seekPresses.exchange(0, std::memory_order_relaxed);
        const int horizontal = controlPoll.horizontalDirection.load(std::memory_order_relaxed);
        if((down&HidNpadButton_Minus)&&options.openSettings){
            const bool wasPaused=paused;
            const Uint64 started=SDL_GetTicks64();
            paused=true;pauseAudio();
            controlPoll.settingsOpen.store(true,std::memory_order_relaxed);
            options.openSettings();
            controlPoll.pending.store(0,std::memory_order_relaxed);
            controlPoll.seekPresses.store(0,std::memory_order_relaxed);
            controlPoll.settingsOpen.store(false,std::memory_order_relaxed);
            if(!menuOpen)wallStart+=SDL_GetTicks64()-started;
            paused=wasPaused;pauseAudio();
            return;
        }
        if((down&HidNpadButton_Plus)&&!options.liveQualityLabels.empty()){
            if(menuOpen)closeMenuAndApply();
            qualityMenuOpen=!qualityMenuOpen;
            controlPoll.qualityMenuOpen.store(qualityMenuOpen,std::memory_order_relaxed);
            qualityMenuIndex=std::clamp(options.currentQualityIndex,0,(int)options.liveQualityLabels.size()-1);
            return;
        }
        if(qualityMenuOpen){
            if((down&HidNpadButton_Up)&&qualityMenuIndex>0)--qualityMenuIndex;
            if((down&HidNpadButton_Down)&&qualityMenuIndex+1<(int)options.liveQualityLabels.size())++qualityMenuIndex;
            if(down&HidNpadButton_B){qualityMenuOpen=false;controlPoll.qualityMenuOpen.store(false,std::memory_order_relaxed);}
            if(down&HidNpadButton_A){
                qualityMenuOpen=false;controlPoll.qualityMenuOpen.store(false,std::memory_order_relaxed);
                if(qualityMenuIndex!=options.currentQualityIndex){result.requestedQualityIndex=qualityMenuIndex;running=false;}
            }
            return;
        }
        if(exitConfirm){
            seekHoldDirection=0;
            if(down&HidNpadButton_A){exitedByBack=true;running=false;}
            else if(down&HidNpadButton_B){exitConfirm=false;closeMenuAndApply();notice="继续播放";noticeUntil=SDL_GetTicks64()+1000;pauseAudio();}
            return;
        }
        if (down & HidNpadButton_B)
        {
            seekPreview=false;seekPreviewTarget=-1;
            closeMenuAndApply();
            exitConfirm=true;seekHoldDirection=0;pauseAudio();return;
        }
        else if (cancelled) running = false;
        if (!playbackReady) return;
        if (down & (HidNpadButton_L | HidNpadButton_R))
        {
            volumePercent = std::clamp(volumePercent
                + ((down & HidNpadButton_R) ? 10 : -10), 0, 100);
            notice = "音量 " + std::to_string(volumePercent) + "%";
            noticeUntil = SDL_GetTicks64() + 1200;
            persistPreferences();
        }
        if (menuOpen)
        {
            if(progressFocused){
                if(!directNetworkInput)adjustSeekTarget(menuSeekTarget,seekPresses,horizontal);
                if(down&HidNpadButton_Up){progressFocused=false;menuIndex=0;seekHoldDirection=0;}
                if(down&HidNpadButton_A){closeMenuAndApply();notice="继续播放";noticeUntil=SDL_GetTicks64()+1000;}
                return;
            }
            seekHoldDirection=0;
            if((down&HidNpadButton_Down)&&menuIndex<4){progressFocused=true;menuSeekTarget=result.positionSeconds;return;}
            if (down & HidNpadButton_Left) menuIndex = (menuIndex + 5) % 6;
            if (down & HidNpadButton_Right) menuIndex = (menuIndex + 1) % 6;
            if (menuIndex >= 4)
            {
                int change = 0;
                if (down & HidNpadButton_Up) change += 1;
                if (down & HidNpadButton_Down) change -= 1;
                if (down & HidNpadButton_ZR) change += 10;
                if (down & HidNpadButton_ZL) change -= 10;
                int& seconds = menuIndex == 4 ? introSkipSeconds : outroSkipSeconds;
                if (change != 0)
                {
                    const long long adjusted = static_cast<long long>(seconds) + change;
                    seconds = static_cast<int>(std::min<long long>(
                        std::numeric_limits<int>::max(), std::max<long long>(0, adjusted)));
                    if(menuIndex==4)menuIntroChanged=true;
                    menuPreferencesChanged=true;
                    notice = std::string(menuIndex == 4 ? "片头跳过 " : "片尾跳过 ")
                        + std::to_string(seconds) + " 秒";
                    noticeUntil = SDL_GetTicks64() + 1800;
                }
                if(down&HidNpadButton_A){closeMenuAndApply();notice="继续播放";noticeUntil=SDL_GetTicks64()+1000;}
            }
            else if ((menuIndex==0||menuIndex==1)&&(down&HidNpadButton_A))
            {
                const int delta=menuIndex==0?-1:1;
                const bool allowed=delta<0?options.canPrevious:options.canNext;
                if(allowed){result.episodeDelta=delta;running=false;}
                else{notice=delta<0?"已经是第一集":"已经是最后一集";noticeUntil=SDL_GetTicks64()+1500;}
            }
            else if (down & HidNpadButton_A)
            {
                notice = menuIndex == 2
                    ? "弹幕：尚未配置数据源" : "字幕：尚未加载字幕文件";
                noticeUntil = SDL_GetTicks64() + 1800;
            }
            return;
        }
        if (seekPresses || horizontal || seekPreview)
        {
            if(directNetworkInput){notice="直播暂不支持快进或快退";noticeUntil=SDL_GetTicks64()+1800;}
            else{
                if(!seekPreview){seekPreview=true;seekPreviewTarget=result.positionSeconds;pauseAudio();}
                adjustSeekTarget(seekPreviewTarget,seekPresses,horizontal);
            }
        }
        if(seekPreview&&horizontal==0){
            seekDeltaSeconds=seekPreviewTarget-result.positionSeconds;seekCommitAt=0;
            seekPreview=false;seekPreviewTarget=-1;
            if(std::abs(seekDeltaSeconds)<=0.01)pauseAudio();
        }
        if ((down & HidNpadButton_A)&&!seekPreview)
        {
            menuOpen=true;progressFocused=false;menuSeekTarget=result.positionSeconds;menuIndex=0;menuIntroChanged=false;menuPreferencesChanged=false;
            menuStarted=SDL_GetTicks64();notice="已暂停";
            noticeUntil=SDL_GetTicks64()+1200;pauseAudio();
        }
        SDL_Event event;
        while (SDL_PollEvent(&event))
            if (event.type == SDL_QUIT) running = false;
    };

    struct FirstPacketState
    {
        AVFormatContext* format = nullptr;
        AVPacket* packet = nullptr;
        int videoStream = -1;
        bool seekToStart = true;
        std::atomic<bool> done{false};
        std::atomic<unsigned> scanned{0};
        int rc = 0;
    } firstPacket;
    firstPacket.format = format;
    firstPacket.packet = packet;
    firstPacket.videoStream = videoStream;
    firstPacket.seekToStart = !directNetworkInput;
    SDL_Thread* packetThread = SDL_CreateThread([](void* opaque) -> int {
        auto& state = *static_cast<FirstPacketState*>(opaque);
        const AVStream* video = state.format->streams[state.videoStream];
        const int64_t start = video->start_time == AV_NOPTS_VALUE
            ? 0 : video->start_time;
        // read_header may finish after visiting SeekHead/Cues.  Since the full
        // stream-info pass is intentionally skipped for remote MKV files,
        // explicitly restore demuxing to the first video timestamp.
        if(state.seekToStart){
            av_seek_frame(state.format, state.videoStream, start, AVSEEK_FLAG_BACKWARD);
            avformat_flush(state.format);
        }
        do
        {
            state.rc = av_read_frame(state.format, state.packet);
            if (state.rc < 0 || state.packet->stream_index == state.videoStream) break;
            state.scanned.fetch_add(1, std::memory_order_relaxed);
            av_packet_unref(state.packet);
        } while (true);
        state.done.store(true, std::memory_order_release);
        return state.rc;
    }, "okswitch-first-packet", &firstPacket);
    if (packetThread == nullptr) return fail("无法启动视频读取线程");

    stage = "读取首个视频数据包";
    const Uint64 firstPacketStarted = SDL_GetTicks64();
    Uint64 lastPacketProgress = firstPacketStarted;
    uint64_t lastPacketBytes = input.downloadedBytes();
    lastStatus = 0;
    while (!firstPacket.done.load(std::memory_order_acquire))
    {
        controls();
        const Uint64 now = SDL_GetTicks64();
        const uint64_t packetBytes = input.downloadedBytes();
        if (packetBytes > lastPacketBytes)
        {
            lastPacketBytes = packetBytes;
            lastPacketProgress = now;
        }
        if (!running || (!directNetworkInput && now - lastPacketProgress >= 20000)
            || now - firstPacketStarted >= 60000)
        {
            cancelled = !running;
            timedOut = !cancelled;
            prepareCancelled.store(true, std::memory_order_relaxed);
        }
        if (now - lastStatus >= 100)
        {
            lastStatus = now;
            showStatus(stage + " · " + (directNetworkInput ? std::string("直播数据") : input.stats()) + " · "
                + "已扫描 " + std::to_string(firstPacket.scanned.load(std::memory_order_relaxed)) + " 包 · "
                + std::to_string((now - firstPacketStarted) / 1000) + " 秒 · B 取消");
        }
        SDL_Delay(16);
    }
    SDL_WaitThread(packetThread, nullptr);
    if (cancelled || timedOut) return fail("读取首个视频数据包失败");
    if (firstPacket.rc < 0)
        return fail("读取首个视频数据包失败：" + ffmpegError(firstPacket.rc));
    prepareCancelled.store(false, std::memory_order_relaxed);

    showStatus("正在解码第一帧 · L/R 音量 · A 暂停并打开菜单 · 左右快退/快进 · B 退出");
    result.opened = true;
    result.durationSeconds = source.durationSeconds;
    if (format->duration > 0)
        result.durationSeconds = static_cast<double>(format->duration) / AV_TIME_BASE;
    for (unsigned index = 0; index < format->nb_streams; ++index)
    {
        const AVStream* stream = format->streams[index];
        if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0)
        {
            result.durationSeconds = std::max(result.durationSeconds,
                stream->duration * av_q2d(stream->time_base));
        }
    }
    double firstVideoTime = std::numeric_limits<double>::quiet_NaN();
    double syntheticVideoTime = 0;
    double lastRawVideoTime = std::numeric_limits<double>::quiet_NaN();
    double videoTimestampOffset = 0;
    AVRational frameRate = av_guess_frame_rate(
        format, format->streams[videoStream], nullptr);
    const double frameStep = frameRate.num > 0 && frameRate.den > 0
        ? static_cast<double>(frameRate.den) / frameRate.num : 1.0 / 30.0;
    int textureWidth = 0;
    int textureHeight = 0;
    bool reachedEnd = false;
    bool hasPendingPacket = true;
    bool separateAudioEnded = false;
    unsigned audioPacketsSinceVideo = 0;
    bool frameVisible = false;
    double discardBeforeSeconds = -1;
    SDL_Rect lastDestination{};
    auto renderPlayer = [&]() {
        int outputWidth = 1280, outputHeight = 720;
        SDL_GetRendererOutputSize(renderer, &outputWidth, &outputHeight);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        if (frameVisible && texture != nullptr)
            SDL_RenderCopy(renderer, texture, nullptr, &lastDestination);

        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        if(qualityMenuOpen){
            const int width=420,height=90+(int)options.liveQualityLabels.size()*54;
            SDL_Rect panel{outputWidth-width-28,76,width,height};
            SDL_SetRenderDrawColor(renderer,8,13,22,226);SDL_RenderFillRect(renderer,&panel);
            drawText("直播画质 · A 确认 · B/＋ 返回",panel.x+20,panel.y+17,width-40);
            for(size_t i=0;i<options.liveQualityLabels.size();++i){
                SDL_Rect row{panel.x+14,panel.y+62+(int)i*54,width-28,48};
                SDL_SetRenderDrawColor(renderer,(int)i==qualityMenuIndex?40:30,
                    (int)i==qualityMenuIndex?150:45,(int)i==qualityMenuIndex?125:62,245);
                SDL_RenderFillRect(renderer,&row);
                drawText(options.liveQualityLabels[i]+((int)i==options.currentQualityIndex?"  ✓":""),
                    row.x+15,row.y+10,row.w-30);
            }
        }
        if (menuOpen)
        {
            SDL_SetRenderDrawColor(renderer, 8, 13, 22, 232);
            SDL_Rect panel{0, outputHeight - 190, outputWidth, 190};
            SDL_RenderFillRect(renderer, &panel);
            const double shownPosition=progressFocused&&menuSeekTarget>=0
                ?menuSeekTarget:result.positionSeconds;
            drawText(clockText(shownPosition) + " / "
                + clockText(result.durationSeconds), 34, outputHeight - 174, 300);
            const std::string menuHint=progressFocused
                ?"进度条：左右快退/快进 · 上返回上一集 · A 应用并继续"
                :"摇杆/十字键：左右选择 · 下进入进度条 · A 继续";
            const int hintWidth=options.measureText?options.measureText(menuHint):555;
            const int hintX=std::max(360,outputWidth-34-hintWidth);
            drawText(menuHint,hintX,outputHeight-174,outputWidth-34-hintX);
            const std::string labels[] = {
                "上一集", "下一集", "弹幕", "字幕",
                "片头  " + std::to_string(introSkipSeconds) + " 秒",
                "片尾  " + std::to_string(outroSkipSeconds) + " 秒"};
            for (int index = 0; index < 6; ++index)
            {
                SDL_Rect item{20 + index * 208, outputHeight - 118, 196, 62};
                SDL_SetRenderDrawColor(renderer,
                    !progressFocused&&index == menuIndex ? 40 : 27,
                    !progressFocused&&index == menuIndex ? 170 : 52,
                    !progressFocused&&index == menuIndex ? 145 : 72, 245);
                SDL_RenderFillRect(renderer, &item);
                drawText(labels[index], item.x + 14, item.y + 18, item.w - 28);
            }
        }
        else if (paused)
        {
            SDL_SetRenderDrawColor(renderer, 8, 13, 22, 220);
            SDL_Rect panel{0, outputHeight - 92, outputWidth, 92};
            SDL_RenderFillRect(renderer, &panel);
            drawText(clockText(result.positionSeconds) + " / "
                + clockText(result.durationSeconds), 34, outputHeight - 78, 300);
            drawText("L/R 音量 · A 继续 · 左右快退/快进 · B 退出",
                590, outputHeight - 78, 650);
        }
        if (paused || menuOpen)
        {
            const int barX = 34;
            const int barWidth = std::max(1, outputWidth - 68);
            const int barY = outputHeight - 30;
            SDL_SetRenderDrawColor(renderer, 48, 57, 70, 235);
            SDL_Rect track{barX, barY, barWidth, 8};
            SDL_RenderFillRect(renderer, &track);
            const double displayedPosition=menuOpen&&progressFocused&&menuSeekTarget>=0
                ?menuSeekTarget:result.positionSeconds;
            const double progress = result.durationSeconds > 0
                ? std::clamp(displayedPosition / result.durationSeconds, 0.0, 1.0)
                : 0.0;
            const double bufferedProgress = !directNetworkInput && result.durationSeconds > 0
                ? std::clamp(input.bufferedUntilSeconds(result.positionSeconds)
                    / result.durationSeconds, progress, 1.0)
                : progress;
            SDL_SetRenderDrawColor(renderer, 225, 232, 240, 125);
            SDL_Rect bufferedFill{barX, barY,
                static_cast<int>(barWidth * bufferedProgress), 8};
            SDL_RenderFillRect(renderer, &bufferedFill);
            SDL_SetRenderDrawColor(renderer, 55, 200, 160, 255);
            SDL_Rect fill{barX, barY,
                static_cast<int>(barWidth * progress), 8};
            SDL_RenderFillRect(renderer, &fill);
            if(menuOpen&&progressFocused){SDL_SetRenderDrawColor(renderer,80,235,195,255);SDL_Rect focus{barX-4,barY-6,barWidth+8,20};SDL_RenderDrawRect(renderer,&focus);}
        }
        if (paused || menuOpen)
        {
            SDL_SetRenderDrawColor(renderer, 8, 13, 22, 205);
            SDL_Rect badge{24, 22, options.displayTitle.empty() ? 130 : 610, 48};
            SDL_RenderFillRect(renderer, &badge);
            drawText(options.displayTitle.empty() ? "已暂停"
                : "已暂停 · " + options.displayTitle, 43, 34,
                options.displayTitle.empty() ? 100 : 570);
        }
        if(seekPreview&&seekPreviewTarget>=0)
        {
            const std::string preview=(seekPreviewTarget>=result.positionSeconds?"快进至 ":"快退至 ")
                +clockText(seekPreviewTarget);
            SDL_SetRenderDrawColor(renderer,8,13,22,220);SDL_Rect badge{24,22,260,48};SDL_RenderFillRect(renderer,&badge);
            drawText(preview,40,34,228);
        }
        if (!seekPreview&&!notice.empty() && SDL_GetTicks64() < noticeUntil)
        {
            SDL_SetRenderDrawColor(renderer, 8, 13, 22, 205);
            SDL_Rect badge{24, 82, 390, 46};
            SDL_RenderFillRect(renderer, &badge);
            drawText(notice, 40, 93, 360);
        }
        if(exitConfirm){
            SDL_SetRenderDrawColor(renderer,8,13,22,238);SDL_Rect confirm{315,255,650,170};SDL_RenderFillRect(renderer,&confirm);
            const std::string title="确定退出播放？",actions="A 确认退出   B 继续播放";
            const int titleWidth=options.measureText?options.measureText(title):126;
            const int actionsWidth=options.measureText?options.measureText(actions):250;
            drawText(title,confirm.x+(confirm.w-titleWidth)/2,292,titleWidth+4);
            drawText(actions,confirm.x+(confirm.w-actionsWidth)/2,350,actionsWidth+4);
        }
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        SDL_RenderPresent(renderer);
    };

    if (directNetworkInput) {
        const auto appType = appletGetAppletType();
        const bool fullMemory = appType == AppletType_Application
            || appType == AppletType_SystemApplication;
        if (!liveBuffer.start(format, videoStream, audioStream, frameStep,
                (fullMemory ? 48u : 12u) * 1024u * 1024u, prepareCancelled))
            return fail("无法启动直播缓存线程");
    }
    else{
        input.enableNonBlocking();
        if(separateAudioInput)separateAudioInput->enableNonBlocking();
    }
    playbackReady = true;
    controlPoll.playbackReady.store(true, std::memory_order_relaxed);
    if(options.resumeSeconds>1)seekDeltaSeconds=options.resumeSeconds;
    else if (introSkipSeconds > 0)seekDeltaSeconds = introSkipSeconds;

    auto seekReady=[&](){return std::abs(seekDeltaSeconds)>0.01
        &&(seekCommitAt==0||SDL_GetTicks64()>=seekCommitAt);};

    while (running)
    {
        controls();
        if (running && directNetworkInput && seekReady())
        {
            seekDeltaSeconds = 0;
            seekCommitAt = 0;
            notice = "直播暂不支持快进或快退";
            noticeUntil = SDL_GetTicks64() + 1800;
        }
        if (running && seekReady())
        {
            const double target = std::max(0.0,
                result.durationSeconds > 0
                    ? std::min(result.durationSeconds - 0.1,
                        result.positionSeconds + seekDeltaSeconds)
                    : result.positionSeconds + seekDeltaSeconds);
            seekDeltaSeconds = 0;
            seekCommitAt = 0;
            av_packet_unref(packet);
            int seekResult = 0;
            if (input.isSegmented())
            {
                input.cancelReadAhead();
                const int64_t byteOffset = input.byteOffsetForSeconds(target);
                const int64_t ioSeek = byteOffset >= 0
                    ? avio_seek(io, byteOffset, SEEK_SET) : AVERROR(EINVAL);
                seekResult = ioSeek >= 0 ? 0 : static_cast<int>(ioSeek);
            }
            else
            {
                seekResult = avformat_seek_file(format, -1,
                    std::numeric_limits<std::int64_t>::min(),
                    static_cast<std::int64_t>(target * AV_TIME_BASE),
                    std::numeric_limits<std::int64_t>::max(), 0);
                if(seekResult>=0&&audioFormat){
                    seekResult=avformat_seek_file(audioFormat,-1,
                        std::numeric_limits<std::int64_t>::min(),
                        static_cast<std::int64_t>(target*AV_TIME_BASE),
                        std::numeric_limits<std::int64_t>::max(),0);
                }
            }
            if (seekResult >= 0)
            {
                avformat_flush(format);
                deinterlacer.reset();
                if(audioFormat)avformat_flush(audioFormat);
                avcodec_flush_buffers(videoCodec);
                if (audioCodec != nullptr) avcodec_flush_buffers(audioCodec);
                if (audioDevice != 0) SDL_ClearQueuedAudio(audioDevice);
                result.positionSeconds = target;
                firstVideoTime = target;
                syntheticVideoTime = target;
                lastRawVideoTime = std::numeric_limits<double>::quiet_NaN();
                videoTimestampOffset = 0;
                discardBeforeSeconds = input.isSegmented() ? -1 : target;
                wallStart = SDL_GetTicks64();
                hasPendingPacket = false;
                separateAudioEnded=false;audioPacketsSinceVideo=0;
                frameVisible=false;waitingForVideo=true;pauseAudio();
            }
            else
            {
                notice = "跳转失败：" + ffmpegError(seekResult);
                noticeUntil = SDL_GetTicks64() + 1800;
            }
        }
        while (running && (paused || menuOpen || seekPreview || exitConfirm) && !seekReady())
        {
            // Keep the network worker independent from paused decoding. The
            // current segment plus seven following segments fit the bounded
            // eight-slot cache and provide roughly 60-80 seconds of HLS data.
            if (!directNetworkInput) input.pumpReadAhead(result.positionSeconds, 7);
            renderPlayer();
            SDL_Delay(16);
            controls();
        }
        if (!running) break;
        if (seekReady()) continue;
        bool packetFromSeparateAudio=false;
        if (hasPendingPacket)
        {
            hasPendingPacket = false;
            rc = 0;
        }
        else{
            if(audioFormat&&!separateAudioEnded&&!waitingForVideo&&audioCodec&&audioDevice!=0
                &&audioPacketsSinceVideo<2&&bytesPerSecond>0
                &&SDL_GetQueuedAudioSize(audioDevice)<static_cast<Uint32>(bytesPerSecond)){
                rc=av_read_frame(audioFormat,packet);
                if(rc>=0){
                    if(packet->stream_index==audioStream){packetFromSeparateAudio=true;++audioPacketsSinceVideo;}
                    else{av_packet_unref(packet);continue;}
                }else if(rc==AVERROR_EOF)separateAudioEnded=true;
                else if(rc==AVERROR(EAGAIN)){
                    if(audioIo){audioIo->error=0;audioIo->eof_reached=0;}
                }else if (!networkCancelled.load(std::memory_order_relaxed)) {
                    result.error="读取独立音轨失败："+ffmpegError(rc)
                        +(separateAudioInput->error.empty()?std::string():" · "+separateAudioInput->error);
                    break;
                }
            }
            if(!packetFromSeparateAudio){
                rc=directNetworkInput?liveBuffer.read(packet):av_read_frame(format,packet);
                audioPacketsSinceVideo=0;
            }
        }
        if(rc==AVERROR(EAGAIN)){
            if(io!=nullptr){io->error=0;io->eof_reached=0;}
            if(!buffering){buffering=true;bufferingStarted=SDL_GetTicks64();notice="正在缓冲视频...";noticeUntil=bufferingStarted+60000;pauseAudio();}
            controls();renderPlayer();SDL_Delay(8);continue;
        }
        if(buffering){wallStart+=SDL_GetTicks64()-bufferingStarted;buffering=false;notice.clear();pauseAudio();}
        if (rc < 0)
        {
            if (networkCancelled.exchange(false, std::memory_order_relaxed))
            {
                // AVIO keeps an interrupted read as a sticky error. Clear it
                // after handling the button so pause/menu/seek can resume video.
                if (io != nullptr) { io->error = 0; io->eof_reached = 0; }
                if (audioIo != nullptr) { audioIo->error = 0; audioIo->eof_reached = 0; }
                controls();
                if (!running) break;
                continue;
            }
            reachedEnd = rc == AVERROR_EOF;
            if (!reachedEnd) result.error = "读取视频失败：" + ffmpegError(rc)
                + (input.error.empty()?std::string():" · "+input.error);
            break;
        }

        AVCodecContext* codec = packetFromSeparateAudio ? audioCodec
            : (packet->stream_index == videoStream ? videoCodec
                : (!audioFormat && packet->stream_index == audioStream ? audioCodec : nullptr));
        if (codec == nullptr)
        {
            av_packet_unref(packet);
            continue;
        }
        rc = avcodec_send_packet(codec, packet);
        av_packet_unref(packet);
        if (rc < 0 && rc != AVERROR(EAGAIN)) continue;
        while (running && avcodec_receive_frame(codec, decoded) >= 0)
        {
            if (codec == audioCodec)
            {
                if(waitingForVideo){av_frame_unref(decoded);continue;}
                if (discardBeforeSeconds >= 0
                    && decoded->best_effort_timestamp != AV_NOPTS_VALUE)
                {
                    const double audioTime = decoded->best_effort_timestamp
                        * av_q2d(audioContainer->streams[audioStream]->time_base);
                    if (audioTime + 0.01 < discardBeforeSeconds)
                    {
                        av_frame_unref(decoded);
                        continue;
                    }
                }
                if (audioDevice != 0 && resampler != nullptr)
                {
                    const int inputRate = audioCodec->sample_rate > 0
                        ? audioCodec->sample_rate : obtained.freq;
                    const int samples = static_cast<int>(av_rescale_rnd(
                        swr_get_delay(resampler, inputRate) + decoded->nb_samples,
                        obtained.freq, inputRate, AV_ROUND_UP));
                    std::vector<std::uint8_t> audio(
                        static_cast<std::size_t>(samples) * obtained.channels * 2);
                    std::uint8_t* output[] = {audio.data()};
                    const int converted = swr_convert(resampler, output, samples,
                        const_cast<const std::uint8_t**>(decoded->extended_data),
                        decoded->nb_samples);
                    if (converted > 0)
                    {
                        audio.resize(static_cast<std::size_t>(converted)
                            * obtained.channels * 2);
                        if (volumePercent < 100)
                        {
                            auto* samples16 = reinterpret_cast<std::int16_t*>(audio.data());
                            const size_t sampleCount = audio.size() / sizeof(std::int16_t);
                            for (size_t i = 0; i < sampleCount; ++i)
                                samples16[i] = static_cast<std::int16_t>(
                                    static_cast<int>(samples16[i]) * volumePercent / 100);
                        }
                        SDL_QueueAudio(audioDevice, audio.data(), audio.size());
                    }
                    while (running && bytesPerSecond > 0
                        && SDL_GetQueuedAudioSize(audioDevice)
                            > static_cast<Uint32>(bytesPerSecond * 2))
                    {
                        controls();
                        SDL_Delay(10);
                    }
                }
                av_frame_unref(decoded);
                continue;
            }

            double videoTime = syntheticVideoTime;
            if (decoded->best_effort_timestamp != AV_NOPTS_VALUE)
            {
                const double rawVideoTime = decoded->best_effort_timestamp
                    * av_q2d(format->streams[videoStream]->time_base);
                if (input.isSegmented())
                {
                    if (!std::isfinite(lastRawVideoTime))
                        videoTimestampOffset = syntheticVideoTime - rawVideoTime;
                    else if (rawVideoTime + 0.5 < lastRawVideoTime)
                        videoTimestampOffset = syntheticVideoTime - rawVideoTime;
                    lastRawVideoTime = rawVideoTime;
                    videoTime = rawVideoTime + videoTimestampOffset;
                }
                else videoTime = rawVideoTime;
            }
            syntheticVideoTime = videoTime + frameStep;
            if (discardBeforeSeconds >= 0
                && videoTime + frameStep * 0.5 < discardBeforeSeconds)
            {
                av_frame_unref(decoded);
                continue;
            }
            if (discardBeforeSeconds >= 0) discardBeforeSeconds = -1;

            if (std::isfinite(firstVideoTime))
            {
                const double dueMs=std::max(0.0,videoTime-firstVideoTime)*1000.0;
                const double elapsedMs=static_cast<double>(SDL_GetTicks64()-wallStart);
                const double lagMs=elapsedMs-dueMs;
                if(lagMs>1000.0){
                    // If decoding or the network pauses for long enough, a
                    // fixed clock makes every later frame "late" forever while
                    // audio continues. Rebase once and display the next frame.
                    const Uint64 ticks=SDL_GetTicks64();
                    const Uint64 dueTicks=static_cast<Uint64>(dueMs);
                    wallStart=ticks>dueTicks?ticks-dueTicks:0;
                    if(audioDevice!=0&&SDL_GetQueuedAudioSize(audioDevice)
                        >static_cast<Uint32>(bytesPerSecond))SDL_ClearQueuedAudio(audioDevice);
                }else if(lagMs>180.0){
                    result.positionSeconds=std::max(0.0,videoTime);
                    av_frame_unref(decoded);
                    continue;
                }
            }

            AVFrame* image = decoded;
            if (decoded->hw_frames_ctx != nullptr
                || decoded->format == AV_PIX_FMT_NVTEGRA)
            {
                av_frame_unref(software);
                if (av_hwframe_transfer_data(software, decoded, 0) < 0)
                {
                    av_frame_unref(decoded);
                    continue;
                }
                image = software;
                av_frame_copy_props(image,decoded);
            }
            if(source.deinterlaceVideo){
                const auto order=videoCodec->field_order;
                if(order!=AV_FIELD_UNKNOWN&&order!=AV_FIELD_PROGRESSIVE){
                    image->flags|=AV_FRAME_FLAG_INTERLACED;
                    if(order==AV_FIELD_TT||order==AV_FIELD_BT)image->flags|=AV_FRAME_FLAG_TOP_FIELD_FIRST;
                    else image->flags&=~AV_FRAME_FLAG_TOP_FIELD_FIRST;
                }
                if(deinterlacer.graph||(image->flags&AV_FRAME_FLAG_INTERLACED)){
                    image->pts=decoded->best_effort_timestamp;
                    const int filtered=deinterlacer.process(image,format->streams[videoStream]->time_base);
                    if(filtered==AVERROR(EAGAIN)){av_frame_unref(software);av_frame_unref(decoded);continue;}
                    if(filtered<0){result.error="直播去隔行处理失败："+ffmpegError(filtered);running=false;break;}
                    image=deinterlacer.frame;
                    if(image->pts!=AV_NOPTS_VALUE&&decoded->best_effort_timestamp!=AV_NOPTS_VALUE)
                        videoTime+=image->pts*av_q2d(av_buffersink_get_time_base(deinterlacer.output))
                            -decoded->best_effort_timestamp*av_q2d(format->streams[videoStream]->time_base);
                }
            }
            int outputWidth = 1280, outputHeight = 720;
            SDL_GetRendererOutputSize(renderer, &outputWidth, &outputHeight);
            double aspect = static_cast<double>(image->width) / image->height;
            // IPTV commonly carries 16:9 video in a 720x576 anamorphic frame.
            // Hardware-transferred frames may lose their frame-level SAR, so
            // ask the demuxer/stream metadata before falling back to square pixels.
            AVRational sampleAspect = av_guess_sample_aspect_ratio(
                format, format->streams[videoStream], image);
            if (sampleAspect.num <= 0 || sampleAspect.den <= 0)
                sampleAspect = videoCodec->sample_aspect_ratio;
            if (sampleAspect.num > 0 && sampleAspect.den > 0)
                aspect *= av_q2d(sampleAspect);
            if (source.displayAspectRatio > 0.1)
                aspect = source.displayAspectRatio;
            int width = outputWidth;
            int height = static_cast<int>(width / aspect);
            if (height > outputHeight)
            {
                height = outputHeight;
                width = static_cast<int>(height * aspect);
            }
            width = std::max(2, width & ~1);
            height = std::max(2, height & ~1);

            if (width != textureWidth || height != textureHeight)
            {
                SDL_DestroyTexture(texture);
                texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV,
                    SDL_TEXTUREACCESS_STREAMING, width, height);
                av_frame_free(&yuv);
                yuv = av_frame_alloc();
                if (texture == nullptr || yuv == nullptr)
                {
                    running = false;
                    result.error = "无法创建视频画面";
                    av_frame_unref(decoded);
                    break;
                }
                yuv->format = AV_PIX_FMT_YUV420P;
                yuv->width = width;
                yuv->height = height;
                if (av_frame_get_buffer(yuv, 32) < 0)
                {
                    running = false;
                    result.error = "视频画面内存不足";
                    av_frame_unref(decoded);
                    break;
                }
                textureWidth = width;
                textureHeight = height;
            }
            scaler = sws_getCachedContext(scaler,
                image->width, image->height, static_cast<AVPixelFormat>(image->format),
                width, height, AV_PIX_FMT_YUV420P,
                SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
            if (scaler == nullptr
                || sws_scale(scaler, image->data, image->linesize, 0,
                    image->height, yuv->data, yuv->linesize) <= 0)
            {
                av_frame_unref(software);
                av_frame_unref(decoded);
                continue;
            }
            SDL_UpdateYUVTexture(texture, nullptr,
                yuv->data[0], yuv->linesize[0],
                yuv->data[1], yuv->linesize[1],
                yuv->data[2], yuv->linesize[2]);

            if (!std::isfinite(firstVideoTime))
            {
                firstVideoTime = videoTime;
                wallStart = SDL_GetTicks64();
            }
            result.positionSeconds = std::max(0.0, videoTime);
            if (result.durationSeconds > 0 && outroSkipSeconds > 0
                && videoTime >= result.durationSeconds - outroSkipSeconds)
            {
                reachedEnd = true;
                running = false;
                break;
            }
            const double target = std::max(0.0, videoTime - firstVideoTime) * 1000.0;
            while (running && target
                > static_cast<double>(SDL_GetTicks64() - wallStart) + 2.0)
            {
                controls();
                SDL_Delay(static_cast<Uint32>(std::min(10.0,
                    target - static_cast<double>(SDL_GetTicks64() - wallStart))));
            }
            if (!running) break;

            SDL_Rect destination{
                (outputWidth - width) / 2, (outputHeight - height) / 2,
                width, height};
            lastDestination = destination;
            frameVisible = true;
            if(waitingForVideo){waitingForVideo=false;if(audioDevice!=0)SDL_ClearQueuedAudio(audioDevice);firstVideoTime=videoTime;wallStart=SDL_GetTicks64();pauseAudio();}
            renderPlayer();
            av_frame_unref(software);
            av_frame_unref(decoded);
        }
    }

    // main.cpp owns a separate PadState.  Let B return to the released state
    // before handing control back, otherwise that same physical press appears
    // as a fresh edge there and pops a second navigation level.
    while (exitedByBack && appletMainLoop())
    {
        if (!(controlPoll.held.load(std::memory_order_relaxed)
            & HidNpadButton_B)) break;
        SDL_Delay(8);
    }
    while (result.requestedQualityIndex>=0 && appletMainLoop())
    {
        if (!(controlPoll.held.load(std::memory_order_relaxed)
            & HidNpadButton_A)) break;
        SDL_Delay(8);
    }

    result.completed = reachedEnd;
    result.introSkipSeconds=introSkipSeconds;
    result.outroSkipSeconds=outroSkipSeconds;
    result.volumePercent=volumePercent;
    cleanup();
    return result;
}
}
