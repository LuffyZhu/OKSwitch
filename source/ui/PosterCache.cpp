#include "okswitch/ui/PosterCache.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

#include <vector>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <sys/stat.h>

namespace okswitch::ui
{
namespace {
constexpr const char* artworkDirectory="sdmc:/switch/OKSwitch/youtube-artwork";
constexpr size_t maxCachedArtwork=256*1024;
std::string artworkPath(const std::string& url){
    uint64_t hash=14695981039346656037ULL;
    for(unsigned char ch:url){hash^=ch;hash*=1099511628211ULL;}
    // Fixed slots cap disk use at about 32 MiB. Full URL comparison handles collisions.
    return std::string(artworkDirectory)+"/"+std::to_string(hash%128)+".bin";
}
bool readArtwork(const std::string& url,std::string& bytes){
    const auto path=artworkPath(url);struct stat info{};
    if(stat(path.c_str(),&info)!=0||std::time(nullptr)-info.st_mtime>7*24*60*60)return false;
    FILE* file=std::fopen(path.c_str(),"rb");if(!file)return false;
    uint32_t lengths[2]{};bool ok=std::fread(lengths,sizeof(lengths),1,file)==1;
    ok=ok&&lengths[0]==url.size()&&lengths[0]<=2048&&lengths[1]>0&&lengths[1]<=maxCachedArtwork
        &&info.st_size==(long long)(sizeof(lengths)+lengths[0]+lengths[1]);
    if(ok){
        std::string storedUrl(lengths[0],'\0');bytes.resize(lengths[1]);
        ok=std::fread(storedUrl.data(),1,storedUrl.size(),file)==storedUrl.size()&&storedUrl==url
            &&std::fread(bytes.data(),1,bytes.size(),file)==bytes.size();
    }
    std::fclose(file);if(!ok)bytes.clear();return ok;
}
void writeArtwork(const std::string& url,const std::string& bytes){
    if(url.size()>2048||bytes.empty()||bytes.size()>maxCachedArtwork)return;
    mkdir("sdmc:/switch",0777);mkdir("sdmc:/switch/OKSwitch",0777);mkdir(artworkDirectory,0777);
    const auto path=artworkPath(url);FILE* file=std::fopen(path.c_str(),"wb");if(!file)return;
    const uint32_t lengths[]={(uint32_t)url.size(),(uint32_t)bytes.size()};
    bool ok=std::fwrite(lengths,sizeof(lengths),1,file)==1
        &&std::fwrite(url.data(),1,url.size(),file)==url.size()
        &&std::fwrite(bytes.data(),1,bytes.size(),file)==bytes.size();
    if(std::fclose(file)!=0)ok=false;
    if(!ok)std::remove(path.c_str());
}
}
PosterCache::PosterCache(net::HttpClient& httpClient, SDL_Renderer* renderer)
    : httpClient_(httpClient), renderer_(renderer) {
    mutex_=SDL_CreateMutex();
    diskMutex_=SDL_CreateMutex();
    condition_=SDL_CreateCond();
    if(mutex_&&condition_)for(int i=0;i<4;++i){
        SDL_Thread* worker=SDL_CreateThread(workerEntry,"posters",this);
        if(worker)workers_.push_back(worker);
    }
}
PosterCache::~PosterCache() { shutdown(); }

void PosterCache::shutdown() {
    if(!mutex_&&!condition_)return;
    stopping_.store(true);
    generationCancel_->store(true);
    if(mutex_){
        SDL_LockMutex(mutex_);
        jobs_.clear();
        if(condition_)SDL_CondBroadcast(condition_);
        SDL_UnlockMutex(mutex_);
    }
    for(SDL_Thread* worker:workers_)SDL_WaitThread(worker,nullptr);
    workers_.clear();
    clear();
    if(condition_){SDL_DestroyCond(condition_);condition_=nullptr;}
    if(mutex_){SDL_DestroyMutex(mutex_);mutex_=nullptr;}
    if(diskMutex_){SDL_DestroyMutex(diskMutex_);diskMutex_=nullptr;}
}
int PosterCache::workerEntry(void* self) { return static_cast<PosterCache*>(self)->workerLoop(); }
int PosterCache::workerLoop() {
    for(;;){
        SDL_LockMutex(mutex_);
        while(jobs_.empty()&&!stopping_.load())SDL_CondWait(condition_,mutex_);
        if(stopping_.load()){SDL_UnlockMutex(mutex_);return 0;}
        Job job=std::move(jobs_.front());jobs_.pop_front();
        SDL_UnlockMutex(mutex_);
        if(job.cancel->load())continue;
        Image image;image.job=job;
        load(job.url,image);
        SDL_LockMutex(mutex_);
        if(!stopping_.load()&&job.generation==generation_)completed_.push_back(std::move(image));
        SDL_UnlockMutex(mutex_);
    }
}
void PosterCache::beginFrame(int loadBudget) {
    loadBudget_=loadBudget;
    if(!mutex_)return;
    // Only the main thread creates or destroys renderer textures.
    std::deque<Image> ready;
    SDL_LockMutex(mutex_);
    ready.swap(completed_);
    SDL_UnlockMutex(mutex_);
    for(auto& image:ready){
        if(image.job.generation!=generation_)continue;
        SDL_Texture* texture=nullptr;
        if(!image.pixels.empty()){
            texture=SDL_CreateTexture(renderer_,SDL_PIXELFORMAT_RGBA32,
                SDL_TEXTUREACCESS_STATIC,image.width,image.height);
            if(texture&&SDL_UpdateTexture(texture,nullptr,image.pixels.data(),image.width*4)!=0){
                SDL_DestroyTexture(texture);texture=nullptr;
            }
            if(texture)SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND);
            if(!texture)image.error=std::string("Texture: ")+SDL_GetError();
        }
        textures_[image.job.url]=texture;
        errors_[image.job.url]=image.error;
        if(texture)retryAt_.erase(image.job.url);
        else retryAt_[image.job.url]=SDL_GetTicks()+30000;
    }
}
void PosterCache::clear() {
    if(mutex_){
        SDL_LockMutex(mutex_);
        generationCancel_->store(true);
        generationCancel_=std::make_shared<std::atomic<bool>>(false);
        ++generation_;jobs_.clear();completed_.clear();
        SDL_UnlockMutex(mutex_);
    }
    // Keep successful thumbnails across category changes, bounded to 96.
    for(auto it=textures_.begin();it!=textures_.end();){
        if(stopping_.load()||!it->second||textures_.size()>96){
            if(it->second)SDL_DestroyTexture(it->second);
            it=textures_.erase(it);
        }else ++it;
    }
    errors_.clear();retryAt_.clear();
}

void PosterCache::setRenderer(SDL_Renderer* renderer) {
    if(mutex_){
        SDL_LockMutex(mutex_);
        generationCancel_->store(true);
        generationCancel_=std::make_shared<std::atomic<bool>>(false);
        ++generation_;jobs_.clear();completed_.clear();
        SDL_UnlockMutex(mutex_);
    }
    for(auto& item:textures_)if(item.second)SDL_DestroyTexture(item.second);
    textures_.clear();errors_.clear();retryAt_.clear();renderer_=renderer;
}

std::string PosterCache::error(const std::string& url) const
{
    if(url.empty())return "No image URL";
    const auto found=errors_.find(url);
    return found==errors_.end()?std::string():found->second;
}
SDL_Texture* PosterCache::get(const std::string& url,const std::string& proxyUrl)
{
    if (url.empty()) return nullptr;
    const auto found = textures_.find(url);
    if (found != textures_.end()) {
        if(found->second)return found->second;
        const auto retry=retryAt_.find(url);
        if(retry==retryAt_.end()||!SDL_TICKS_PASSED(SDL_GetTicks(),retry->second))return nullptr;
        textures_.erase(found);retryAt_.erase(retry);
    }
    if (loadBudget_ <= 0) return nullptr;
    --loadBudget_;
    textures_[url]=nullptr;
    if(workers_.empty()){errors_[url]="Background loader unavailable";return nullptr;}
    SDL_LockMutex(mutex_);
    if(jobs_.size()<24){
        const bool avatar=url.find("ggpht.com")!=std::string::npos
            ||url.find("googleusercontent.com")!=std::string::npos;
        if(avatar)jobs_.push_front({url,generation_,proxyUrl,generationCancel_});
        else jobs_.push_back({url,generation_,proxyUrl,generationCancel_});
        SDL_CondSignal(condition_);
    }
    else textures_.erase(url);
    SDL_UnlockMutex(mutex_);
    return nullptr;
}

bool PosterCache::load(const std::string& url, Image& image)
{
    net::HttpRequest request;
    request.url = url;
    request.proxyUrl=image.job.proxyUrl;
    request.timeoutSeconds = 15;
    // YouTube serves many tiles from the same CDN. Reusing those TLS sessions
    // avoids a fresh emulator SSL handshake for every thumbnail.
    const bool youtubeVideo=url.find("ytimg.com")!=std::string::npos;
    const bool youtubeAvatar=url.find("ggpht.com")!=std::string::npos
        ||url.find("googleusercontent.com")!=std::string::npos;
    const bool youtubeArtwork=youtubeVideo||youtubeAvatar;
    request.closeConnection = !youtubeArtwork;
    request.cancel = image.job.cancel.get();
    // The image CDN rejects the generic OKSwitch user agent (HTTP 418).
    request.headers.push_back({"User-Agent",
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36"});
    const size_t hostStart=url.find("://");
    if(hostStart!=std::string::npos){
        const size_t start=hostStart+3;
        const std::string host=url.substr(start,url.find_first_of("/:?#",start)-start);
        const std::string suffix=".doubanio.com";
        if(host=="doubanio.com"||(host.size()>suffix.size()
            &&host.compare(host.size()-suffix.size(),suffix.size(),suffix)==0))
            request.headers.push_back({"Referer","https://movie.douban.com/"});
    }
    net::HttpResponse response;
    bool fromDisk=false;
    if(youtubeArtwork&&diskMutex_){
        SDL_LockMutex(diskMutex_);fromDisk=readArtwork(url,response.body);SDL_UnlockMutex(diskMutex_);
    }
    if(fromDisk){response.succeeded=true;response.statusCode=200;}
    else{
        if(youtubeArtwork)request.maxResponseBytes=512*1024;
        response=httpClient_.get(request);
    }
    auto fail=[&](const std::string& message)->bool{
        image.error=message;
        if(fromDisk&&diskMutex_){SDL_LockMutex(diskMutex_);std::remove(artworkPath(url).c_str());SDL_UnlockMutex(diskMutex_);}
        SDL_Log("Poster %s: %s",url.c_str(),message.c_str());
        return false;
    };
    if (!response.succeeded) return fail("Download: "+response.error);
    if (response.body.empty()) return fail("HTTP "+std::to_string(response.statusCode)+" empty body");

    // The response format can differ from the URL suffix.
    const auto* data=reinterpret_cast<const unsigned char*>(response.body.data());
    const size_t size=response.body.size();
    AVCodecID codecId=AV_CODEC_ID_NONE;
    if(size>=3&&data[0]==0xff&&data[1]==0xd8&&data[2]==0xff)codecId=AV_CODEC_ID_MJPEG;
    else if(size>=8&&response.body.compare(0,8,"\x89PNG\r\n\x1a\n",8)==0)codecId=AV_CODEC_ID_PNG;
    else if(size>=12&&response.body.compare(0,4,"RIFF")==0&&response.body.compare(8,4,"WEBP")==0)codecId=AV_CODEC_ID_WEBP;
    else return fail("Unknown image format ("+std::to_string(size)+" bytes)");
    const AVCodec* codec = avcodec_find_decoder(codecId);
    if (codec == nullptr) return fail("Decoder unavailable");
    AVCodecContext* context = avcodec_alloc_context3(codec);
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    if (context == nullptr || frame == nullptr || packet == nullptr)
    {
        av_packet_free(&packet); av_frame_free(&frame); avcodec_free_context(&context);
        return fail("Decoder allocation failed");
    }
    std::vector<unsigned char> bytes(response.body.begin(), response.body.end());
    bytes.resize(bytes.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    packet->data = bytes.data();
    packet->size = static_cast<int>(response.body.size());

    int rc=avcodec_open2(context,codec,nullptr);
    const char* stage="open";
    if(rc>=0){stage="send";rc=avcodec_send_packet(context,packet);}
    if(rc>=0){stage="receive";rc=avcodec_receive_frame(context,frame);}
    if(rc<0)fail(std::string("Decode ")+stage+": "+std::to_string(rc));
    if(rc>=0&&(frame->width<=0||frame->height<=0||frame->width>4096||frame->height>4096)){
        fail("Invalid or oversized image");rc=-1;
    }
    if(rc>=0)
    {
        const int outputWidth=youtubeVideo?300:youtubeAvatar?128:172;
        const int outputHeight=youtubeVideo?169:youtubeAvatar?128:190;
        SwsContext* scaler = sws_getContext(frame->width, frame->height,
            static_cast<AVPixelFormat>(frame->format), outputWidth, outputHeight,
            AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (scaler != nullptr)
        {
            image.width=outputWidth;image.height=outputHeight;
            image.pixels.resize(static_cast<size_t>(image.width)*image.height*4);
            uint8_t* output[]={image.pixels.data()};
            int stride[]={image.width*4};
            if(sws_scale(scaler,frame->data,frame->linesize,0,frame->height,output,stride)!=image.height){
                image.pixels.clear();fail("Image conversion failed");
            }
            sws_freeContext(scaler);
        } else fail("Image conversion unavailable");
    }
    av_packet_free(&packet); av_frame_free(&frame); avcodec_free_context(&context);
    if(!image.pixels.empty()&&youtubeArtwork&&!fromDisk&&diskMutex_){
        SDL_LockMutex(diskMutex_);writeArtwork(url,response.body);SDL_UnlockMutex(diskMutex_);
    }
    return !image.pixels.empty();
}
}
