#include <switch.h>
#include <cerrno>
#include <jansson.h>
#include <sys/stat.h>
#include "okswitch/media/PlaybackHistory.hpp"
#include "okswitch/media/YouTubeHistory.hpp"
#include "okswitch/media/VideoPlayer.hpp"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <curl/curl.h>
#include <png.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <iterator>
#include <string>
#include <optional>
#include <vector>
#include "okswitch/cloud/QuarkProvider.hpp"
#include "okswitch/cloud/QuarkLogin.hpp"
#include "okswitch/cloud/BaiduProvider.hpp"
#include "okswitch/cloud/BaiduLogin.hpp"
#include "okswitch/cloud/BaiduPlaybackLoader.hpp"
#include "okswitch/cloud/CloudLineLoader.hpp"
#include "okswitch/media/MediaSource.hpp"
#include "okswitch/media/CatalogLoader.hpp"
#include "okswitch/media/DetailLoader.hpp"
#include "okswitch/media/FootballProvider.hpp"
#include "okswitch/media/MiguSportsProvider.hpp"
#include "okswitch/media/MiguScheduleLoader.hpp"
#include "okswitch/media/MiguClipsLoader.hpp"
#include "okswitch/media/MiguPlaybackLoader.hpp"
#include "okswitch/media/MiguLogin.hpp"
#include "okswitch/media/CctvLiveProvider.hpp"
#include "okswitch/media/SearchLoader.hpp"
#include "okswitch/media/YouTubeProvider.hpp"
#include "okswitch/media/SourceConfigLoader.hpp"
#include "okswitch/net/HttpClient.hpp"
#include "okswitch/net/OnlineUpdater.hpp"
#include "okswitch/net/LanTextInput.hpp"
#include "okswitch/net/TrojanSubscription.hpp"
#include "okswitch/cloud/qrcodegen.h"
#include "okswitch/ui/PosterCache.hpp"
using namespace okswitch;
namespace {
SDL_Texture* bundledPng(SDL_Renderer* renderer,const char* path,
    SDL_Rect crop={0,0,0,0},int outputWidth=0,int outputHeight=0){
    png_image image{};image.version=PNG_IMAGE_VERSION;
    if(!png_image_begin_read_from_file(&image,path))return nullptr;
    image.format=PNG_FORMAT_RGBA;
    std::vector<unsigned char> pixels(PNG_IMAGE_SIZE(image));
    if(!png_image_finish_read(&image,nullptr,pixels.data(),0,nullptr)){
        png_image_free(&image);return nullptr;
    }
    const int imageWidth=static_cast<int>(image.width),imageHeight=static_cast<int>(image.height);
    if(crop.w<=0||crop.h<=0||crop.x<0||crop.y<0||crop.x+crop.w>imageWidth||crop.y+crop.h>imageHeight)
        crop={0,0,imageWidth,imageHeight};
    const unsigned char* source=pixels.data()+(crop.y*imageWidth+crop.x)*4;
    int width=crop.w,height=crop.h,stride=imageWidth*4;
    std::vector<unsigned char> reduced;
    if(outputWidth>0&&outputHeight>0){
        // Area sampling needs premultiplied colors; the official PNG stores
        // white RGB under transparent pixels, which otherwise makes a halo.
        for(int y=0;y<height;++y)for(int x=0;x<width;++x){
            auto* pixel=pixels.data()+((crop.y+y)*imageWidth+crop.x+x)*4;
            for(int channel=0;channel<3;++channel)
                pixel[channel]=static_cast<unsigned char>((pixel[channel]*pixel[3]+127)/255);
        }
        reduced.resize(static_cast<size_t>(outputWidth)*outputHeight*4);
        SwsContext* scaler=sws_getContext(width,height,AV_PIX_FMT_RGBA,outputWidth,outputHeight,
            AV_PIX_FMT_RGBA,SWS_AREA,nullptr,nullptr,nullptr);
        if(!scaler){png_image_free(&image);return nullptr;}
        const uint8_t* input[]={source};const int inputStride[]={stride};
        uint8_t* output[]={reduced.data()};const int outputStride[]={outputWidth*4};
        const int lines=sws_scale(scaler,input,inputStride,0,height,output,outputStride);
        sws_freeContext(scaler);
        if(lines!=outputHeight){png_image_free(&image);return nullptr;}
        for(size_t i=0;i<reduced.size();i+=4){
            const unsigned alpha=reduced[i+3];
            for(int channel=0;channel<3;++channel)
                reduced[i+channel]=alpha?static_cast<unsigned char>(std::min(255u,
                    (static_cast<unsigned>(reduced[i+channel])*255+alpha/2)/alpha)):0;
        }
        source=reduced.data();width=outputWidth;height=outputHeight;stride=outputWidth*4;
    }
    SDL_Texture* texture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,width,height);
    if(texture&&SDL_UpdateTexture(texture,nullptr,source,stride)!=0){
        SDL_DestroyTexture(texture);texture=nullptr;
    }
    if(texture){
        SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(texture,SDL_ScaleModeLinear);
    }
    png_image_free(&image);return texture;
}
constexpr const char* defaultSourceUrl="";
constexpr const char* settingsPath="sdmc:/switch/OKSwitch/settings.json";
std::string installedNroPath(){
    for(const char* path:{"sdmc:/switch/OKSwitch.nro","sdmc:/switch/OKSwitch/OKSwitch.nro"}){
        FILE* file=std::fopen(path,"rb");if(file){std::fclose(file);return path;}
    }
    return {};
}
std::string loadSourceUrl(){
    json_error_t error{};auto* root=json_load_file(settingsPath,0,&error);
    auto* value=json_object_get(root,"sourceUrl");
    std::string url=json_is_string(value)?json_string_value(value):defaultSourceUrl;
    if(root)json_decref(root);
    return url.rfind("https://",0)==0||url.rfind("http://",0)==0?url:defaultSourceUrl;
}
bool loadAutoPlayNext(){
    json_error_t error{};auto* root=json_load_file(settingsPath,0,&error);
    auto* value=json_object_get(root,"autoPlayNext");
    const bool enabled=json_is_boolean(value)?json_boolean_value(value):true;
    if(root)json_decref(root);
    return enabled;
}
std::string loadYoutubeProxy(){
    json_error_t error{};auto* root=json_load_file(settingsPath,0,&error);
    auto* value=json_object_get(root,"youtubeProxy");
    std::string proxy=json_is_string(value)?json_string_value(value):"";
    if(root)json_decref(root);
    return proxy;
}
std::string loadYoutubeSubscriptionUrl(){
    json_error_t error{};auto* root=json_load_file(settingsPath,0,&error);
    auto* value=json_object_get(root,"youtubeSubscriptionUrl");
    if(json_is_string(value)){
        std::string url=json_string_value(value);
        json_decref(root);return url;
    }
    if(root)json_decref(root);
    FILE* file=std::fopen("sdmc:/switch/OKSwitch/youtube-subscription.txt","rb");
    if(!file)return {};
    char buffer[2049]{};const size_t count=std::fread(buffer,1,sizeof(buffer)-1,file);
    std::fclose(file);
    std::string url(buffer,count);const size_t end=url.find_first_of("\r\n\t ");
    if(end!=std::string::npos)url.resize(end);
    return url.rfind("https://",0)==0?url:"";
}
size_t loadYoutubeSubscriptionIndex(){
    json_error_t error{};auto* root=json_load_file(settingsPath,0,&error);
    auto* value=json_object_get(root,"youtubeSubscriptionIndex");
    const size_t index=json_is_integer(value)&&json_integer_value(value)>=0?(size_t)json_integer_value(value):SIZE_MAX;
    if(root)json_decref(root);
    return index;
}
bool saveSetting(const char* key,json_t* value){
    if(mkdir("sdmc:/switch",0777)!=0&&errno!=EEXIST){json_decref(value);return false;}
    if(mkdir("sdmc:/switch/OKSwitch",0777)!=0&&errno!=EEXIST){json_decref(value);return false;}
    json_error_t error{};auto* root=json_load_file(settingsPath,0,&error);
    if(!json_is_object(root)){if(root)json_decref(root);root=json_object();}
    json_object_set_new(root,key,value);
    const std::string tmp=std::string(settingsPath)+".tmp";
    bool ok=json_dump_file(root,tmp.c_str(),JSON_INDENT(2))==0;json_decref(root);
    if(ok&&std::rename(tmp.c_str(),settingsPath)!=0){
        // Some SD filesystems do not let rename replace an existing file.
        const std::string backup=std::string(settingsPath)+".bak";
        std::remove(backup.c_str());
        if(std::rename(settingsPath,backup.c_str())!=0)ok=false;
        else if(std::rename(tmp.c_str(),settingsPath)==0)std::remove(backup.c_str());
        else{std::rename(backup.c_str(),settingsPath);ok=false;}
    }
    return ok;
}
bool saveSourceUrl(const std::string& url){return saveSetting("sourceUrl",json_string(url.c_str()));}
bool saveAutoPlayNext(bool enabled){return saveSetting("autoPlayNext",json_boolean(enabled));}
bool saveYoutubeProxy(const std::string& url){return saveSetting("youtubeProxy",json_string(url.c_str()));}
bool saveYoutubeSubscription(const std::string& url,size_t index){
    return saveSetting("youtubeSubscriptionUrl",json_string(url.c_str()))
        &&saveSetting("youtubeSubscriptionIndex",json_integer((json_int_t)index));
}
struct SubscriptionBoot {
    net::HttpClient* http=nullptr;
    net::TrojanSocksProxy* proxy=nullptr;
    media::YouTubeProvider* youtube=nullptr;
    std::string url;
    size_t index=0;
    std::string name,error;
    std::atomic<bool> done{false};
    SDL_Thread* thread=nullptr;
};
bool probeYoutubeProxy(net::HttpClient& http,const std::string& proxyUrl,std::string& error){
    net::HttpRequest request;
    request.url="https://www.youtube.com/generate_204";
    request.proxyUrl=proxyUrl;
    request.connectTimeoutSeconds=5;
    request.timeoutSeconds=9;
    request.followRedirects=false;
    request.maxResponseBytes=4096;
    const auto response=http.get(request);
    if(response.succeeded&&response.statusCode==204)return true;
    error="节点无法连接 YouTube";
    if(!response.error.empty())error+="："+response.error;
    else if(response.statusCode)error+="（HTTP "+std::to_string(response.statusCode)+"）";
    return false;
}
int subscriptionBootEntry(void* pointer){
    auto& boot=*(SubscriptionBoot*)pointer;
    std::vector<net::TrojanNode> nodes;
    if(net::loadTrojanSubscription(*boot.http,boot.url,nodes,boot.error)){
        size_t index=boot.index;
        if(index>=nodes.size()){
            auto secure=std::find_if(nodes.begin(),nodes.end(),[](const auto& node){return !node.allowInsecure;});
            index=secure==nodes.end()?0:(size_t)(secure-nodes.begin());
        }
        if(boot.proxy->start(nodes[index],boot.error)){
            if(probeYoutubeProxy(*boot.http,boot.proxy->proxyUrl(),boot.error)){
                boot.name=nodes[index].name;
                boot.youtube->setProxy(boot.proxy->proxyUrl());
            }else boot.proxy->stop();
        }
    }
    boot.done.store(true);return 0;
}
struct SubscriptionFetch {
    net::HttpClient* http=nullptr;
    std::string url,error;
    std::vector<net::TrojanNode> nodes;
    std::atomic<bool> cancel{false},done{false};
};
int subscriptionFetchEntry(void* pointer){
    auto& fetch=*(SubscriptionFetch*)pointer;
    net::loadTrojanSubscription(*fetch.http,fetch.url,fetch.nodes,fetch.error,&fetch.cancel);
    fetch.done.store(true);return 0;
}
bool hasSavedLogin(const char* path){FILE* file=std::fopen(path,"rb");if(!file)return false;std::fclose(file);return true;}
SDL_Texture* miguQrTexture(SDL_Renderer* renderer,const std::vector<unsigned char>& jpeg){
    if(jpeg.empty()||jpeg.size()>1024*1024)return nullptr;
    const AVCodec* codec=avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    AVCodecContext* context=codec?avcodec_alloc_context3(codec):nullptr;
    AVFrame* frame=av_frame_alloc();AVPacket* packet=av_packet_alloc();SDL_Texture* texture=nullptr;
    std::vector<unsigned char> padded(jpeg.begin(),jpeg.end());padded.resize(jpeg.size()+AV_INPUT_BUFFER_PADDING_SIZE,0);
    if(context&&frame&&packet){packet->data=padded.data();packet->size=(int)jpeg.size();
        if(avcodec_open2(context,codec,nullptr)>=0&&avcodec_send_packet(context,packet)>=0&&avcodec_receive_frame(context,frame)>=0
            &&frame->width>0&&frame->height>0&&frame->width<=2048&&frame->height<=2048){
            std::vector<unsigned char> rgba((size_t)frame->width*frame->height*4);
            SwsContext* sws=sws_getContext(frame->width,frame->height,(AVPixelFormat)frame->format,frame->width,frame->height,AV_PIX_FMT_RGBA,SWS_BILINEAR,nullptr,nullptr,nullptr);
            if(sws){uint8_t* dst[]={rgba.data()};int stride[]={frame->width*4};
                if(sws_scale(sws,frame->data,frame->linesize,0,frame->height,dst,stride)==frame->height){texture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,frame->width,frame->height);if(texture)SDL_UpdateTexture(texture,nullptr,rgba.data(),stride[0]);}
                sws_freeContext(sws);}
        }
    }
    av_packet_free(&packet);av_frame_free(&frame);avcodec_free_context(&context);return texture;
}
std::string searchKey(std::string value){
    value.erase(std::remove_if(value.begin(),value.end(),[](unsigned char c){return c<128&&(std::isspace(c)||std::ispunct(c));}),value.end());
    std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return c<128?(char)std::tolower(c):(char)c;});return value;
}
int searchScore(const std::string& title,const std::string& keyword){const std::string t=searchKey(title),q=searchKey(keyword);if(t==q)return 1000;if(t.rfind(q,0)==0)return 800;if(t.find(q)!=std::string::npos)return 600;size_t p=0;for(char c:t)if(p<q.size()&&c==q[p])++p;return p==q.size()?400-(int)(t.size()-q.size()):0;}
bool naturalNameLess(const std::string& left,const std::string& right){
    size_t a=0,b=0;
    while(a<left.size()&&b<right.size()){
        const unsigned char ca=left[a],cb=right[b];
        if(std::isdigit(ca)&&std::isdigit(cb)){
            size_t ae=a,be=b;while(a<left.size()&&std::isdigit((unsigned char)left[a]))++a;while(b<right.size()&&std::isdigit((unsigned char)right[b]))++b;
            size_t az=ae,bz=be;while(az<a&&left[az]=='0')++az;while(bz<b&&right[bz]=='0')++bz;
            const size_t al=a-az,bl=b-bz;if(al!=bl)return al<bl;const int number=left.compare(az,al,right,bz,bl);if(number!=0)return number<0;if(a-ae!=b-be)return a-ae<b-be;continue;
        }
        const unsigned char la=ca<128?(unsigned char)std::tolower(ca):ca,lb=cb<128?(unsigned char)std::tolower(cb):cb;
        if(la!=lb)return la<lb;++a;++b;
    }
    return left.size()<right.size();
}
void sortCloudEntries(std::vector<cloud::CloudEntry>& items){std::stable_sort(items.begin(),items.end(),[](const auto& a,const auto& b){if(a.type!=b.type)return a.type==cloud::EntryType::Directory;return naturalNameLess(a.name,b.name);});}
std::string episodeLabel(const std::string& name,size_t fallback){
    const auto make=[](unsigned long value){return "第"+std::to_string(value)+"集";};
    const size_t chinese=name.find("第");
    if(chinese!=std::string::npos){size_t p=chinese+std::string("第").size(),begin=p;while(p<name.size()&&std::isdigit((unsigned char)name[p]))++p;if(p>begin&&name.compare(p,std::string("集").size(),"集")==0)return "第"+name.substr(begin,p-begin)+"集";}
    std::string upper=name;std::transform(upper.begin(),upper.end(),upper.begin(),[](unsigned char c){return c<128?(char)std::toupper(c):(char)c;});
    for(size_t p=0;p+1<upper.size();++p)if(upper[p]=='E'&&std::isdigit((unsigned char)upper[p+1])){size_t end=p+1;while(end<upper.size()&&std::isdigit((unsigned char)upper[end]))++end;return make(std::strtoul(upper.substr(p+1,end-p-1).c_str(),nullptr,10));}
    unsigned long candidate=0;bool found=false;
    for(size_t p=0;p<upper.size();){if(!std::isdigit((unsigned char)upper[p])){++p;continue;}size_t begin=p;while(p<upper.size()&&std::isdigit((unsigned char)upper[p]))++p;const unsigned long value=std::strtoul(upper.substr(begin,p-begin).c_str(),nullptr,10);const char after=p<upper.size()?upper[p]:'\0';const char before=begin>0?upper[begin-1]:'\0';if(value>0&&value<1000&&after!='P'&&after!='K'&&after!='F'&&!((before=='H'||before=='X')&&(value==264||value==265))){candidate=value;found=true;}}
    return make(found?candidate:fallback);
}
int episodeNumberFromName(const std::string& name){
    const std::string label=episodeLabel(name,0);
    const size_t begin=std::string("第").size();
    return std::atoi(label.c_str()+begin);
}
std::vector<std::string> summaryLines(const std::string& value,size_t width=50){
    std::vector<std::string> lines;std::string line;size_t count=0;
    for(size_t pos=0;pos<value.size();){
        unsigned char c=value[pos];size_t len=c<128?1:(c<224?2:(c<240?3:4));
        len=std::min(len,value.size()-pos);
        if(c=='\n'){lines.push_back(line);line.clear();count=0;++pos;continue;}
        if(c!='\r'){line+=value.substr(pos,len);++count;}
        pos+=len;
        if(count>=width){lines.push_back(line);line.clear();count=0;}
    }
    if(!line.empty())lines.push_back(line);
    if(lines.empty())lines.push_back("暂无简介");
    return lines;
}
const char* favoritesPath="sdmc:/switch/OKSwitch/favorites.json";
const char* searchHistoryPath="sdmc:/switch/OKSwitch/search-history.json";
bool loadSearchHistory(std::vector<std::string>& items){
    errno=0;json_error_t error{};json_t* root=json_load_file(searchHistoryPath,0,&error);
    if(!root)return errno==ENOENT;
    if(!json_is_array(root)){json_decref(root);return false;}
    for(size_t i=0;i<json_array_size(root)&&items.size()<6;++i){json_t* value=json_array_get(root,i);if(json_is_string(value)&&json_string_length(value)>0)items.emplace_back(json_string_value(value));}
    json_decref(root);return true;
}
bool saveSearchHistory(const std::vector<std::string>& items){
    if(mkdir("sdmc:/switch",0777)!=0&&errno!=EEXIST)return false;
    if(mkdir("sdmc:/switch/OKSwitch",0777)!=0&&errno!=EEXIST)return false;
    json_t* root=json_array();if(!root)return false;
    for(const auto& item:items)json_array_append_new(root,json_string(item.c_str()));
    std::string temporary=std::string(searchHistoryPath)+".tmp";
    int rc=json_dump_file(root,temporary.c_str(),JSON_INDENT(2)|JSON_ENSURE_ASCII);json_decref(root);
    if(rc!=0)return false;
    return std::rename(temporary.c_str(),searchHistoryPath)==0;
}
bool loadFavorites(std::vector<media::SearchResult>& items){
    errno=0;json_error_t error{};json_t* root=json_load_file(favoritesPath,0,&error);
    if(!root)return errno==ENOENT;
    if(!json_is_array(root)){json_decref(root);return false;}
    bool valid=true;
    for(size_t i=0;i<json_array_size(root);++i){
        json_t* item=json_array_get(root,i);
        auto string=[&](const char* key)->std::string{json_t* v=json_object_get(item,key);return json_is_string(v)?json_string_value(v):"";};
        media::SearchResult result;result.title=string("title");result.detailUrl=string("url");
        result.posterUrl=string("poster");result.sourceName=string("source");result.status=string("status");
        if(result.title.empty()||result.detailUrl.empty()){valid=false;break;}
        items.push_back(std::move(result));
    }
    json_decref(root);if(!valid)items.clear();return valid;
}
bool saveFavorites(const std::vector<media::SearchResult>& items){
    if(mkdir("sdmc:/switch",0777)!=0&&errno!=EEXIST)return false;
    if(mkdir("sdmc:/switch/OKSwitch",0777)!=0&&errno!=EEXIST)return false;
    json_t* root=json_array();if(!root)return false;
    for(const auto& item:items){
        json_t* row=json_pack("{s:s,s:s,s:s,s:s,s:s}","title",item.title.c_str(),"url",item.detailUrl.c_str(),
            "poster",item.posterUrl.c_str(),"source",item.sourceName.c_str(),"status",item.status.c_str());
        if(!row||json_array_append_new(root,row)!=0){json_decref(root);return false;}
    }
    std::string temporary=std::string(favoritesPath)+".tmp";
    int rc=json_dump_file(root,temporary.c_str(),JSON_INDENT(2)|JSON_ENSURE_ASCII);json_decref(root);
    if(rc!=0)return false;
    return std::rename(temporary.c_str(),favoritesPath)==0;
}

enum class View { Browse, SearchHome, Search, Shares, Files, History, Favorites };
enum class CloudKind { Quark, Baidu, Guazi };
std::vector<TTF_Font*> fallbackFonts;
int textWidth(TTF_Font* f,const std::string& s){
    if(!f)return 0;
    int width=0;
    for(size_t i=0;i<s.size();){
        Uint32 cp=static_cast<unsigned char>(s[i++]);int extra=0;
        if(cp>=0xC2&&cp<=0xDF){cp&=0x1F;extra=1;}
        else if(cp>=0xE0&&cp<=0xEF){cp&=0x0F;extra=2;}
        else if(cp>=0xF0&&cp<=0xF4){cp&=0x07;extra=3;}
        else if(cp>=0x80)cp=0xFFFD;
        for(int j=0;j<extra&&i<s.size();++j)cp=(cp<<6)|(static_cast<unsigned char>(s[i++])&0x3F);
        TTF_Font* chosen=f;
        if(!TTF_GlyphIsProvided32(f,cp))for(TTF_Font* candidate:fallbackFonts)if(TTF_GlyphIsProvided32(candidate,cp)){chosen=candidate;break;}
        int minX=0,maxX=0,minY=0,maxY=0,advance=0;
        width+=TTF_GlyphMetrics32(chosen,cp,&minX,&maxX,&minY,&maxY,&advance)==0?advance:TTF_FontHeight(chosen)/2;
    }
    return width;
}
void text(SDL_Renderer* r, TTF_Font* f, const std::string& s, int x, int y, int maxWidth=1150, bool fit=false){
    if(!f||maxWidth<=0)return;
    SDL_Color color{240,240,240,255};
    struct RunSurface{SDL_Surface* surface;int ascent;};
    std::vector<RunSurface> surfaces;
    int totalWidth=0;
    auto renderRun=[&](TTF_Font* font,const std::string& run){
        if(run.empty())return;
        SDL_Surface* surface=TTF_RenderUTF8_Blended(font,run.c_str(),color);
        if(surface){
            surfaces.push_back({surface,TTF_FontAscent(font)});
            totalWidth+=surface->w;
        }
    };
    TTF_Font* runFont=f;
    std::string run;
    for(size_t i=0;i<s.size();){
        size_t start=i;
        Uint32 cp=static_cast<unsigned char>(s[i++]);
        int extra=0;
        if(cp>=0xC2&&cp<=0xDF){cp&=0x1F;extra=1;}
        else if(cp>=0xE0&&cp<=0xEF){cp&=0x0F;extra=2;}
        else if(cp>=0xF0&&cp<=0xF4){cp&=0x07;extra=3;}
        else if(cp>=0x80)cp=0xFFFD;
        bool valid=cp!=0xFFFD;
        for(int j=0;j<extra;++j){
            if(i>=s.size()||(static_cast<unsigned char>(s[i])&0xC0)!=0x80){valid=false;break;}
            cp=(cp<<6)|(static_cast<unsigned char>(s[i++])&0x3F);
        }
        if((extra==1&&cp<0x80)||(extra==2&&cp<0x800)||(extra==3&&cp<0x10000)
            ||cp>0x10FFFF||(cp>=0xD800&&cp<=0xDFFF))valid=false;
        if(!valid)cp=0xFFFD;
        TTF_Font* chosen=f;
        if(!TTF_GlyphIsProvided32(f,cp)){
            for(TTF_Font* candidate:fallbackFonts){
                if(TTF_GlyphIsProvided32(candidate,cp)){chosen=candidate;break;}
            }
        }
        if(chosen!=runFont){
            renderRun(runFont,run);
            run.clear();
            runFont=chosen;

        }
        run+=valid?s.substr(start,i-start):"\xEF\xBF\xBD";
    }
    renderRun(runFont,run);
    const double scale=fit&&totalWidth>maxWidth?double(maxWidth)/totalWidth:1.0;
    int offset=0;
    for(const auto& part:surfaces){
        SDL_Surface* surface=part.surface;
        int left=int(offset*scale);
        int right=int((offset+surface->w)*scale);
        if(left<maxWidth&&right>left){
            SDL_Texture* texture=SDL_CreateTextureFromSurface(r,surface);
            if(texture){
                SDL_Rect source{0,0,fit?surface->w:std::min(surface->w,maxWidth-left),surface->h};
                SDL_Rect dest{x+left,y+int((TTF_FontAscent(f)-part.ascent)*scale),
                    std::min(right,maxWidth)-left,std::max(1,int(surface->h*scale))};
                SDL_RenderCopy(r,texture,&source,&dest);
                SDL_DestroyTexture(texture);
            }
        }
        offset+=surface->w;
        SDL_FreeSurface(surface);
    }
}
bool keyboard(std::string&out,const char*header,const char*guide){SwkbdConfig c{};if(R_FAILED(swkbdCreate(&c,0)))return false;swkbdConfigMakePresetDefault(&c);swkbdConfigSetHeaderText(&c,header);swkbdConfigSetGuideText(&c,guide);swkbdConfigSetStringLenMax(&c,500);if(!out.empty())swkbdConfigSetInitialText(&c,out.c_str());char b[2048]{};Result rc=swkbdShow(&c,b,sizeof(b));swkbdClose(&c);if(R_FAILED(rc)||!b[0])return false;out=b;return true;}
std::string cleanUrl(const std::string&v){size_t p=v.find("http");if(p==std::string::npos)return v;size_t e=v.find_first_of(" \r\n\t",p);return v.substr(p,e==std::string::npos?v.size()-p:e-p);}
std::string upperAscii(std::string value){for(char& c:value)if(c>='a'&&c<='z')c-=32;return value;}
std::string sourceDisplayLabel(const std::string& value,bool live=false){
    std::string clean;
    for(size_t i=0;i<value.size();){
        const size_t start=i;
        Uint32 cp=static_cast<unsigned char>(value[i++]);int extra=0;
        if(cp>=0xC2&&cp<=0xDF){cp&=0x1F;extra=1;}
        else if(cp>=0xE0&&cp<=0xEF){cp&=0x0F;extra=2;}
        else if(cp>=0xF0&&cp<=0xF4){cp&=0x07;extra=3;}
        else if(cp>=0x80){continue;}
        bool valid=true;
        for(int j=0;j<extra;++j){if(i>=value.size()||(static_cast<unsigned char>(value[i])&0xC0)!=0x80){valid=false;break;}cp=(cp<<6)|(static_cast<unsigned char>(value[i++])&0x3F);}
        if(valid&&(cp<0x80||(cp>=0x3400&&cp<=0x9FFF)))clean.append(value,start,i-start);
    }
    while(!clean.empty()&&clean.front()==' ')clean.erase(clean.begin());
    while(!clean.empty()&&clean.back()==' ')clean.pop_back();
    return live?"◎ "+clean:clean;
}
std::string findResolution(const std::string& value){
    const std::string upper=upperAscii(value);
    for(const char* mark:{"2160P","1080P","720P","8K","4K"})if(upper.find(mark)!=std::string::npos)return mark;
    return "未提供";
}
std::string findFrameRate(const std::string& value){
    const std::string upper=upperAscii(value);
    for(const char* mark:{"120FPS","120HZ","60FPS","60HZ","50FPS","50HZ","30FPS","30HZ","25FPS","25HZ","24FPS","24HZ"})
        if(upper.find(mark)!=std::string::npos)return mark;
    return "未提供";
}
}
int main(){
const bool appletMode=appletGetAppletType()!=AppletType_Application;
if(SDL_Init(SDL_INIT_VIDEO|(appletMode?0:SDL_INIT_AUDIO))<0)return 1;
TTF_Init();plInitialize(PlServiceType_User);PlFontData fd{};if(R_FAILED(plGetSharedFontByType(&fd,PlSharedFontType_Standard)))return 1;TTF_Font*f=TTF_OpenFontRW(SDL_RWFromConstMem(fd.address,(int)fd.size),1,18);
if(!f)return 1;
for(PlSharedFontType type:{PlSharedFontType_ChineseSimplified,
    PlSharedFontType_ExtChineseSimplified,PlSharedFontType_ChineseTraditional,PlSharedFontType_KO}){
    PlFontData data{};
    if(R_SUCCEEDED(plGetSharedFontByType(&data,type))){
        TTF_Font* font=TTF_OpenFontRW(SDL_RWFromConstMem(data.address,(int)data.size),1,18);
        if(font)fallbackFonts.push_back(font);
    }
}
SDL_Window*w=SDL_CreateWindow("OKSwitch",0,0,1280,720,0);SDL_Renderer*r=SDL_CreateRenderer(w,0,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
if(appletMode){
    padConfigureInput(1,HidNpadStyleSet_NpadStandard);PadState guidePad;padInitializeDefault(&guidePad);
    while(appletMainLoop()){
        padUpdate(&guidePad);
        if(padGetButtonsDown(&guidePad)&(HidNpadButton_A|HidNpadButton_B|HidNpadButton_Plus))break;
        SDL_SetRenderDrawColor(r,18,24,38,255);SDL_RenderClear(r);
        text(r,f,"OKSwitch 需要完整内存模式",340,68,630,true);
        text(r,f,"从相册打开属于 Applet 模式，视频播放可能闪退。请按下面步骤重新启动。",180,125,940,true);
        const char* labels[]={"返回桌面","按住 R 打开游戏","选择 OKSwitch"};
        const char* details[]={"按 HOME 键","保持按住 R，启动任意游戏","在 hbmenu 中按 A 打开"};
        const int phase=(SDL_GetTicks()/1000)%3;
        for(int i=0;i<3;++i){
            SDL_Rect card{80+i*400,230,340,220};
            SDL_SetRenderDrawColor(r,i==phase?45:29,i==phase?150:48,i==phase?120:68,255);SDL_RenderFillRect(r,&card);
            text(r,f,std::to_string(i+1),card.x+145,260,60,true);
            text(r,f,labels[i],card.x+35,325,270,true);
            text(r,f,details[i],card.x+18,380,305,true);
            if(i<2)text(r,f,"→",card.x+354,325,42,true);
        }
        text(r,f,"也可以安装桌面入口，以后直接从桌面启动。",335,510,680,true);
        text(r,f,"A / B / + 退出指引",495,606,300,true);
        SDL_RenderPresent(r);SDL_Delay(16);
    }
    SDL_DestroyRenderer(r);SDL_DestroyWindow(w);
    for(TTF_Font* font:fallbackFonts)TTF_CloseFont(font);
    fallbackFonts.clear();TTF_CloseFont(f);
    plExit();TTF_Quit();SDL_Quit();return 0;
}
bool romfsReady=R_SUCCEEDED(romfsInit());
SDL_Texture* youtubeLogo=romfsReady?bundledPng(r,"romfs:/images/youtube-logo.png",
    {204,141,1294,289},179,40):nullptr;
SDL_Texture* heartEmoji=romfsReady?bundledPng(r,"romfs:/images/emoji-1f493.png"):nullptr;
SDL_Texture* boomEmoji=romfsReady?bundledPng(r,"romfs:/images/emoji-1f4a5.png"):nullptr;
SDL_Texture* sparkleEmoji=romfsReady?bundledPng(r,"romfs:/images/emoji-1f387.png"):nullptr;
SDL_Texture* melonEmoji=romfsReady?bundledPng(r,"romfs:/images/emoji-1f349.png"):nullptr;
bool netReady=R_SUCCEEDED(socketInitializeDefault());if(netReady){curl_global_init(CURL_GLOBAL_DEFAULT);avformat_network_init();}
const bool dockedMode=appletGetOperationMode()==AppletOperationMode_Console;
net::HttpClient http;ui::PosterCache posters(http,r);media::YouTubeProvider youtube(http,loadYoutubeProxy(),dockedMode?1080:720);media::MediaSource mediaSource(http);media::CatalogLoader catalogLoader(mediaSource),hotSearchLoader(mediaSource),suggestionLoader(mediaSource);media::DetailLoader detailLoader(mediaSource);media::SearchLoader searchLoader(mediaSource);media::SourceConfigLoader sourceConfigLoader(http);media::FootballProvider football(http);media::MiguSportsProvider migu(http);media::MiguScheduleLoader miguScheduleLoader(http);media::MiguClipsLoader miguClipsLoader(http);bool miguClipsLoading=false;media::MiguPlaybackLoader miguPlaybackLoader(migu);bool miguPlaybackLoading=false;media::MiguClip pendingMiguClip;media::MiguLogin miguLogin(http);migu.setSession(miguLogin.savedSession());media::CctvLiveProvider cctv(http);cloud::QuarkProvider quark(http);cloud::QuarkLogin quarkLogin(http);cloud::BaiduProvider baidu(http);cloud::BaiduLogin baiduLogin(http);cloud::CloudLineLoader cloudLineLoader(quark,baidu);cloud::BaiduPlaybackLoader baiduPlaybackLoader(quark,baidu);bool loginApplied=false,baiduLoginApplied=false,playerFullscreen=false,resourceLoading=false,searchPending=false,lineLoading=false,autoOpenFirstShare=false,autoTryingLines=false;size_t autoTryLine=0,loadingShareIndex=0,pendingLoginLine=SIZE_MAX;Uint32 autoOpenShareAt=0;cloud::LoginView loginView;cloud::BaiduLoginView baiduLoginView;CloudKind activeCloud=CloudKind::Quark,playbackCloud=CloudKind::Quark;cloud::PlaybackSource playbackSource;std::string playbackSourceFileId;std::vector<media::SourceDefinition> sources={{"剧透","https://1.star2.cn","AiNewJuTou","csp_AiNewJuTouGuard"},{"至臻","https://www.miqk.cc","NewZhiZhen","csp_NewZhiZhenGuard"},{"虎斑","http://43.248.128.118:16969","AiNewHuBan","csp_AiNewHuBanGuard"},{"秒播","https://sdapi.e2wu4ht.com","WexGuaZi","csp_WexGuaZiGuard"}};size_t configuredSiteCount=0;bool sourceConfigOnline=false;
net::TrojanSocksProxy youtubeTunnel;
SubscriptionBoot subscriptionBoot;
subscriptionBoot.http=&http;subscriptionBoot.proxy=&youtubeTunnel;subscriptionBoot.youtube=&youtube;
subscriptionBoot.url=loadYoutubeSubscriptionUrl();subscriptionBoot.index=loadYoutubeSubscriptionIndex();
if(netReady&&!subscriptionBoot.url.empty())subscriptionBoot.thread=SDL_CreateThread(subscriptionBootEntry,"youtube-subscription",&subscriptionBoot);
quark.setPreferredMaxHeight(dockedMode&&!appletMode?1080:720);baidu.setPreferredMaxHeight(dockedMode&&!appletMode?1080:720);
struct Category{const char*name;int kind;};std::vector<Category> categories={{"Home",0},{"Hot Movies",1},{"Hot Series",2},{"Hot Shows",3},{"Movie Filter",4},{"TV Filter",5},{"Movie Rank",6},{"TV Rank",7},{"YouTube",9},{"足球直播",8}};const char*sortCodes[]={"T","U","R","S"};const char*sortNames[]={"General","Recent Hot","Release Time","High Score"};size_t category=0;int page=1,sortIndex=0;
View view=View::Browse;std::vector<media::SearchResult> browseItems,searchItems;std::vector<std::pair<std::string,std::string>> searchSourceStatus;media::MediaDetail detail;cloud::ShareSession session;std::vector<cloud::CloudEntry> cloudItems;std::vector<std::string> dirs;std::string status=netReady?"Loading recommendations...":"Network failed";size_t selected=0;
enum class YouTubePage { Explore, Subscriptions, Channel, History };
bool youtubeNavFocus=false;int youtubeNavSelected=1;int youtubePlaybackRefreshes=0;double youtubeResumeOverride=-1;
YouTubePage youtubePage=YouTubePage::Explore;std::string youtubeChannelId,youtubeChannelName;bool youtubeLoading=false,youtubePlaying=false,youtubeSearchMode=false,youtubeAccountHome=false,youtubeSubscriptionHome=false,youtubeAccountHistory=false,youtubeHomeWaitingForProxy=false;std::string youtubePendingTitle;media::SearchResult youtubePendingItem;
std::vector<media::SearchResult> youtubeQueuedCards;
media::YouTubeHistory youtubeHistory;youtubeHistory.load();
std::vector<std::string> searchHistory,hotSearches;bool searchHistoryReadable=loadSearchHistory(searchHistory),hotSearchLoading=false;
std::string activeSearchKeyword;
std::string searchInput,lastSuggestionQuery;int keyboardRow=0,keyboardCol=0;bool suggestionFocus=false,suggestionLoading=false;size_t suggestionSelected=0;std::vector<std::string> searchSuggestions;Uint32 suggestionDue=0;
int sourceFilter=-1;size_t activeShare=0;std::string detailSource;
media::PlaybackHistory playbackHistory;bool historyReadable=playbackHistory.load();
std::optional<media::PlaybackRecord> pendingHistoryResume;bool autoPlayEpisode=false;size_t historyLineStart=0,historyLinesTried=0;
std::vector<media::SearchResult> favorites,homeRecommendations;
std::vector<std::string> footballSources={"电视频道","咪咕精选赛事","咪咕中超"};
std::vector<media::CctvChannel> cctvChannels=cctv.channels();bool cctvChannelFocus=false;std::string liveError;
std::vector<media::FootballMatch> footballMatches;bool footballMatchFocus=false;size_t footballSourceSelected=0;
std::vector<media::MiguMatch> miguMatches;std::vector<media::MiguClip> miguClips;bool miguMatchFocus=false,miguClipFocus=false;size_t miguMatchSelected=0;
std::vector<media::MiguMatch> miguOverview,miguSchedule;
bool miguScheduleFocus=false,miguNoEarlier=false,miguNoLater=false;
Uint32 miguScheduleRetryAt=0,miguRefreshAt=0;
std::string miguTeamId,miguTeamName;bool miguTeamPicker=false;size_t miguTeamSelected=0;
std::vector<std::pair<std::string,std::string>> miguTeams;
auto filteredMiguSchedule=[&](){std::vector<media::MiguMatch> items;for(const auto& match:miguSchedule)if(miguTeamId.empty()||match.homeId==miguTeamId||match.awayId==miguTeamId)items.push_back(match);return items;};
auto sortMiguMatches=[](auto& list){std::stable_sort(list.begin(),list.end(),[](const auto& a,const auto& b){return a.kickoffTime==b.kickoffTime?a.id<b.id:a.kickoffTime<b.kickoffTime;});};
std::vector<media::MiguLiveMatch> miguLiveMatches;bool miguLiveFocus=false;
bool favoritesReadable=loadFavorites(favorites);
auto historyCards=[&](){
    std::vector<const media::PlaybackRecord*> cards;
    for(const auto& record:playbackHistory.items()){
        if(std::none_of(cards.begin(),cards.end(),[&](const auto* saved){return saved->item.detailUrl==record.item.detailUrl;}))cards.push_back(&record);
    }
    return cards;
};
media::SearchResult detailItem;
    bool summaryOpen=false;size_t summaryScroll=0;bool detailFieldOpen=false;size_t detailFieldScroll=0;std::string detailFieldTitle,detailFieldText;int detailAction=-1;
    struct DetailField{std::string title,text;int y;SDL_Rect button;bool overflow;};
    auto makeDetailFields=[&](){
        std::string tags=detailItem.status;
        if(view==View::Files&&selected<cloudItems.size())tags+=" "+cloudItems[selected].name;
        std::vector<DetailField> fields;
        fields.push_back({"来源标注","来源标注："+findResolution(tags)+" / "+findFrameRate(tags),111,{1145,107,80,27},false});
        fields.push_back({"来源与年份","来源："+sourceDisplayLabel(detailSource)+"    年份："+detail.year,140,{1145,136,80,27},false});
        fields.push_back({"导演","导演："+(detail.director.empty()?std::string("未提供"):detail.director),169,{1145,165,80,27},false});
        fields.push_back({"演员","演员："+(detail.actors.empty()?std::string("未提供"):detail.actors),198,{1145,194,80,27},false});
        for(auto& field:fields)field.overflow=textWidth(f,field.text)>645;
        return fields;
    };
auto isFavorite=[&](const std::string& url){return !url.empty()&&std::any_of(favorites.begin(),favorites.end(),[&](const auto& item){return item.detailUrl==url;});};
auto refreshHome=[&](){
    if(category!=0)return;
    browseItems.clear();media::SearchResult historyCard;historyCard.title="最近观看";historyCard.detailUrl="okswitch:history";browseItems.push_back(historyCard);media::SearchResult favoritesCard;favoritesCard.title="收藏";favoritesCard.detailUrl="okswitch:favorites";browseItems.push_back(favoritesCard);browseItems.insert(browseItems.end(),homeRecommendations.begin(),homeRecommendations.end());
};
auto toggleFavorite=[&](){
    if(detailItem.detailUrl.empty()){status="此条目暂无可收藏的详情地址";return;}
    if(!favoritesReadable){status="收藏文件读取失败，未覆盖原文件";return;}
    auto updated=favorites;
    auto found=std::find_if(updated.begin(),updated.end(),[&](const auto& item){return item.detailUrl==detailItem.detailUrl;});
    bool remove=found!=updated.end();
    if(remove)updated.erase(found);else{auto item=detailItem;if(!detail.posterUrl.empty())item.posterUrl=detail.posterUrl;updated.insert(updated.begin(),item);}
    if(!saveFavorites(updated)){status="收藏保存失败，请检查 SD 卡是否可写";return;}
    favorites=std::move(updated);status=remove?"已取消收藏":"已加入收藏";
};
// Save the page being left, including its selection and already loaded data.
struct NavigationPage {
    media::SearchResult detailItem;
    int sourceFilter;size_t activeShare;std::string detailSource;
    View view;
    size_t selected;
    std::string status;
    std::vector<media::SearchResult> searchItems;
    media::MediaDetail detail;
    cloud::ShareSession session;
    std::vector<cloud::CloudEntry> cloudItems;
    std::vector<std::string> dirs;
};
std::vector<NavigationPage> navigation;
auto rememberPage=[&](){navigation.push_back({detailItem,sourceFilter,activeShare,detailSource,view,selected,status,searchItems,detail,session,cloudItems,dirs});};
auto backPage=[&](){
    if(navigation.empty())return;
    auto previous=std::move(navigation.back());navigation.pop_back();
        detailItem=std::move(previous.detailItem);detailAction=-1;summaryOpen=false;detailFieldOpen=false;playerFullscreen=false;
    sourceFilter=previous.sourceFilter;activeShare=previous.activeShare;detailSource=std::move(previous.detailSource);
    view=previous.view;selected=previous.selected;status=std::move(previous.status);
    searchItems=std::move(previous.searchItems);detail=std::move(previous.detail);
    session=std::move(previous.session);cloudItems=std::move(previous.cloudItems);dirs=std::move(previous.dirs);
    if(view==View::Browse&&category==0){
        std::string oldTitle=selected<browseItems.size()?browseItems[selected].title:"";
        refreshHome();selected=0;
        for(size_t i=0;i<browseItems.size();++i)if(browseItems[i].title==oldTitle){selected=i;break;}
    }
};
auto runSearch=[&](const std::string& keyword){
    if(keyword.empty())return;
    if(youtubeSearchMode){
        youtubeSearchMode=false;backPage();youtubePage=YouTubePage::Explore;youtubeLoading=true;youtubePlaying=false;
        browseItems.clear();posters.clear();selected=0;youtube.search(keyword);status="YouTube · 正在搜索 "+keyword;return;
    }
    if(searchHistoryReadable){
        searchHistory.erase(std::remove(searchHistory.begin(),searchHistory.end(),keyword),searchHistory.end());
        searchHistory.insert(searchHistory.begin(),keyword);
        if(searchHistory.size()>6)searchHistory.resize(6);
        if(!saveSearchHistory(searchHistory)){searchHistoryReadable=false;status="搜索历史保存失败";}
    }
    activeSearchKeyword=keyword;rememberPage();searchItems.clear();searchSourceStatus.clear();posters.clear();view=View::Search;selected=0;sourceFilter=-1;
    searchPending=searchLoader.request(sources,keyword);
    status=searchPending?"正在搜索资源...":"无法启动资源搜索";
};
auto openSearchHome=[&](){
    rememberPage();view=View::SearchHome;selected=0;status="应用内键盘搜索";searchInput.clear();lastSuggestionQuery.clear();suggestionDue=0;suggestionLoading=false;keyboardRow=keyboardCol=0;suggestionFocus=false;suggestionSelected=0;searchSuggestions.clear();for(const auto& title:searchHistory)if(std::find(searchSuggestions.begin(),searchSuggestions.end(),title)==searchSuggestions.end())searchSuggestions.push_back(title);for(const auto& title:hotSearches)if(searchSuggestions.size()<10&&std::find(searchSuggestions.begin(),searchSuggestions.end(),title)==searchSuggestions.end())searchSuggestions.push_back(title);
    if(hotSearches.empty()&&!hotSearchLoading){hotSearchLoader.request("okswitch:360-hot");hotSearchLoading=true;status="正在加载 360kan 热搜...";}
};
std::string pendingLoginFileId, pendingLoginShareId;
CloudKind pendingLoginCloud=CloudKind::Quark;
auto rememberLoginPlayback=[&](){
    pendingLoginFileId=cloudItems[selected].id;
    pendingLoginShareId=session.shareId;
    pendingLoginCloud=activeCloud;
};
auto startCloudLogin=[&](bool force=false){
    if(activeCloud==CloudKind::Baidu){
        const auto current=baiduLogin.snapshot();
        if(force){
            baiduLogin.invalidate();baidu.setSession({});baiduLoginApplied=false;
            status="已清除旧的百度登录状态，正在生成新二维码...";
        }
        baiduLogin.start();
    }else quarkLogin.start();
};
auto cleanupPlayback=[&](std::string& error){if(playbackCloud==CloudKind::Guazi){error.clear();return true;}return playbackCloud==CloudKind::Baidu?baidu.cleanupPlaybackSource(playbackSource,error):quark.cleanupPlaybackSource(playbackSource,error);};
auto openShare=[&](const std::string&raw,bool savePage=true){
    const std::string url=cleanUrl(raw);cloud::ProviderResult<cloud::ShareSession> q;
    const CloudKind requestedCloud=baidu.canHandle({url,""})?CloudKind::Baidu:CloudKind::Quark;
    if(requestedCloud==CloudKind::Baidu)baidu.resetBrowseCancel();else quark.resetBrowseCancel();
    if(requestedCloud==CloudKind::Baidu)q=baidu.resolveShare({url,""});else q=quark.resolveShare({url,""});
    if(!q.succeeded){status=q.error.message;return;}
    auto l=requestedCloud==CloudKind::Baidu?baidu.listFiles(q.value,q.value.rootId):quark.listFiles(q.value,q.value.rootId);
    if(!l.succeeded){status=l.error.message;return;}if(savePage)rememberPage();activeCloud=requestedCloud;session=std::move(q.value);cloudItems=std::move(l.value);sortCloudEntries(cloudItems);dirs={session.rootId};selected=0;view=View::Files;status="Loaded "+std::to_string(cloudItems.size())+(activeCloud==CloudKind::Baidu?" Baidu items":" Quark items");
};
auto openDetailLine=[&](size_t index)->bool{
    autoOpenFirstShare=false;
    if(index>=detail.shares.size())return false;
    // Keep the highlighted tab aligned with the line that is actually being
    // opened. Automatic fallback can cross from Quark to Baidu.
    activeShare=index;
    // A line switch from the file browser must stop exposing files belonging
    // to the previous provider while the new line is loading or logging in.
    if(view==View::Files){view=View::Shares;selected=index;cloudItems.clear();dirs.clear();session={};}
    const bool yueyueLine=detail.shares[index].url=="okswitch:yueyue";
    if(detail.shares[index].url=="okswitch:guazi"||yueyueLine){
        activeCloud=CloudKind::Guazi;pendingLoginLine=SIZE_MAX;autoTryingLines=false;
        cloudItems.clear();dirs.clear();session={};
        for(size_t episodeIndex=0;episodeIndex<detail.episodes.size();++episodeIndex){cloud::CloudEntry entry;entry.id=(yueyueLine?"yueyue-":"guazi-")+std::to_string(episodeIndex);entry.name=detail.episodes[episodeIndex].label;entry.playbackHint=(yueyueLine?"yueyue:":"")+detail.episodes[episodeIndex].opaque;entry.type=cloud::EntryType::File;cloudItems.push_back(std::move(entry));}sortCloudEntries(cloudItems);
        selected=0;view=View::Files;status="秒播线路已加载 · "+std::to_string(cloudItems.size())+" 集";return true;
    }
    if(baidu.canHandle({cleanUrl(detail.shares[index].url),""})&&!baiduLoginApplied){
        activeCloud=CloudKind::Baidu;autoTryingLines=false;playerFullscreen=true;
        pendingLoginLine=index;
        startCloudLogin();status="请先扫码登录百度网盘，成功后将自动加载线路";return false;
    }
    pendingLoginLine=SIZE_MAX;
    loadingShareIndex=index;
    status="正在加载线路 "+std::to_string(index+1)+" / "+std::to_string(detail.shares.size())+"...";
    lineLoading=cloudLineLoader.request(detail.shares[index].url,index);
    if(!lineLoading)status="上一条线路仍在结束，请稍后重试";
    return lineLoading;
};
auto resumeEpisodeIndex=[&](const media::PlaybackRecord& record)->std::optional<size_t>{
    const bool sameLine=activeShare<detail.shares.size()&&cleanUrl(detail.shares[activeShare].url)==cleanUrl(record.shareUrl);
    size_t ordinal=0;
    for(size_t i=0;i<cloudItems.size();++i){
        if(cloudItems[i].type!=cloud::EntryType::File)continue;
        ++ordinal;
        if(sameLine&&(!record.fileId.empty()&&cloudItems[i].id==record.fileId))return i;
        if(!record.fileName.empty()&&cloudItems[i].name==record.fileName)return i;
    }
    int number=record.episodeNumber>0?record.episodeNumber:episodeNumberFromName(record.fileName);
    if(number>0){
        bool hasNumberedFiles=false;
        for(size_t i=0;i<cloudItems.size();++i)if(cloudItems[i].type==cloud::EntryType::File){const int candidate=episodeNumberFromName(cloudItems[i].name);if(candidate==number)return i;if(candidate>0)hasNumberedFiles=true;}
        if(hasNumberedFiles)return std::nullopt;
    }
    if(record.episodeOrdinal>0){
        ordinal=0;for(size_t i=0;i<cloudItems.size();++i)if(cloudItems[i].type==cloud::EntryType::File&&++ordinal==(size_t)record.episodeOrdinal)return i;
    }
    return std::nullopt;
};
auto continueHistoryResume=[&](){
    if(!pendingHistoryResume)return;
    while(historyLinesTried<detail.shares.size()){
        const size_t line=(historyLineStart+historyLinesTried++)%detail.shares.size();
        openDetailLine(line);
        if(view==View::Files){
            if(auto index=resumeEpisodeIndex(*pendingHistoryResume)){selected=*index;status="已定位上次观看的 "+cloudItems[selected].name+" · 按 A 继续";return;}
            continue;
        }
        if(lineLoading||pendingLoginLine<detail.shares.size())return;
    }
    status="当前来源没有找到上次观看的集数，请选择其他来源";
    if(detailItem.detailUrl==pendingHistoryResume->item.detailUrl){
        const std::string title=pendingHistoryResume->item.title;
        runSearch(title);
        status="原来源不可用，已搜索同剧其他来源 · 选中后继续播放";
    }
};
auto openResource=[&](const media::SearchResult& item){
    if(resourceLoading)return;
    if(!pendingHistoryResume||searchKey(pendingHistoryResume->item.title)!=searchKey(item.title)){
        if(const auto* prior=playbackHistory.findSeriesByTitle(item.title))pendingHistoryResume=*prior;
        else pendingHistoryResume.reset();
    }
    if(searchPending){
        status="正在停止后台搜索...";
        searchLoader.cancel();
        searchPending=false;
    }
    if(!detailLoader.request(item)){status="无法启动详情加载";return;}
    resourceLoading=true;status="正在后台加载详情...";
};
auto drawLogin=[&](SDL_Rect area,bool full){
    SDL_SetRenderDrawColor(r,8,13,22,255);SDL_RenderFillRect(r,&area);
    const auto& modules=activeCloud==CloudKind::Baidu?baiduLoginView.modules:loginView.modules;
    const int qrSize=activeCloud==CloudKind::Baidu?baiduLoginView.size:loginView.size;
    const std::string message=activeCloud==CloudKind::Baidu?baiduLoginView.message:loginView.message;
    if(!modules.empty()&&qrSize>0){
        int cell=std::max(1,std::min(area.w-30,area.h-(full?100:46))/(qrSize+8));
        int pixels=cell*(qrSize+8);
        int x=area.x+(area.w-pixels)/2,y=area.y+8;
        SDL_SetRenderDrawColor(r,255,255,255,255);SDL_Rect paper{x,y,pixels,pixels};SDL_RenderFillRect(r,&paper);
        SDL_SetRenderDrawColor(r,0,0,0,255);
        for(int row=0;row<qrSize;++row)for(int col=0;col<qrSize;++col)if(modules[row*qrSize+col]){
            SDL_Rect module{x+(col+4)*cell,y+(row+4)*cell,cell,cell};SDL_RenderFillRect(r,&module);
        }
    }
    text(r,f,message,area.x+10,area.y+area.h-35,area.w-20,true);
    if(full)text(r,f,"B 返回小窗 · 二维码超时后按 A 重试",area.x+10,area.y+area.h-10,area.w-20,true);
    if(!full&&detailAction==2){SDL_SetRenderDrawColor(r,55,200,160,255);SDL_RenderDrawRect(r,&area);}
};
const std::string apiKey="0ac44ae016490db2204ce0a042db2916";
auto requestYoutubeHome=[&](){
    // A saved subscription is started in the background. Do not send the first
    // YouTube request directly while its local proxy is still coming up.
    // Without a subscription, direct access starts immediately.
    if(subscriptionBoot.thread&&!subscriptionBoot.done.load()){
        youtubeHomeWaitingForProxy=true;
        youtubeLoading=true;
        status="YouTube · 正在连接已选代理节点 · B 取消";
        return;
    }
    youtubeHomeWaitingForProxy=false;
    youtube.home();
    youtubeLoading=true;
    status=subscriptionBoot.error.empty()?"YouTube · 正在加载首页...":
        "YouTube · 订阅节点未连接，正在尝试直连...";
};
auto loadBrowse=[&](){miguPlaybackLoader.cancel();miguPlaybackLoading=false;miguClipsLoader.cancel();miguClipsLoading=false;miguScheduleLoader.cancel();miguScheduleFocus=false;miguTeamPicker=false;youtube.cancel();youtubeLoading=false;youtubePlaying=false;youtubeSearchMode=false;youtubeHomeWaitingForProxy=false;youtubeQueuedCards.clear();if(categories[category].kind==9){youtubeNavFocus=false;youtubeNavSelected=1;youtubePage=YouTubePage::Explore;browseItems.clear();posters.clear();selected=0;view=View::Browse;if(netReady){requestYoutubeHome();}else status="YouTube · 网络尚未就绪";return;}if(categories[category].kind==8){browseItems.clear();posters.clear();footballMatches.clear();miguMatches.clear();miguClips.clear();miguLiveMatches.clear();footballMatchFocus=false;miguMatchFocus=false;miguClipFocus=false;miguLiveFocus=false;cctvChannelFocus=false;liveError.clear();footballSourceSelected=0;selected=0;view=View::Browse;status="足球直播 · 选择线路后按 A 加载赛事";return;}int start=(page-1)*20;std::string suffix="?apikey="+apiKey+"&start="+std::to_string(start)+"&count=20";std::string endpoint;switch(categories[category].kind){case 0:endpoint="subject_collection/subject_real_time_hotest/items"+suffix;break;case 1:endpoint="movie/hot_gaia?apikey="+apiKey+"&sort=recommend&area=%E5%85%A8%E9%83%A8&start="+std::to_string(start)+"&count=20";break;case 2:endpoint="subject_collection/tv_hot/items"+suffix;break;case 3:endpoint="subject_collection/show_hot/items"+suffix;break;case 4:endpoint="movie/recommend?apikey="+apiKey+"&sort="+sortCodes[sortIndex]+"&tags=&start="+std::to_string(start)+"&count=20";break;case 5:endpoint="tv/recommend?apikey="+apiKey+"&sort="+sortCodes[sortIndex]+"&tags=&start="+std::to_string(start)+"&count=20";break;case 6:endpoint="subject_collection/movie_real_time_hotest/items"+suffix;break;default:endpoint="subject_collection/tv_real_time_hotest/items"+suffix;break;}catalogLoader.request(endpoint);browseItems.clear();if(category==0)refreshHome();status="Loading "+std::string(categories[category].name)+"...";posters.clear();selected=0;view=View::Browse;};
auto receiveBrowse=[&](){media::SearchResponse q;if(!catalogLoader.poll(q))return;
    if(categories[category].kind==8||categories[category].kind==9)return;
    if(q.succeeded){if(category==0){homeRecommendations=std::move(q.items);refreshHome();}else browseItems=std::move(q.items);if(view==View::Browse)status=std::string(categories[category].name)+" | "+std::to_string(q.total?q.total:(int)browseItems.size())+" total";}
    else if(view==View::Browse)status=q.error.empty()?"No catalog items":q.error;
};
padConfigureInput(1,HidNpadStyleSet_NpadStandard);PadState pad;padInitializeDefault(&pad);
auto miguSignIn=[&](){
    miguLogin.start();SDL_Texture* qrTexture=nullptr;unsigned shownQrRevision=0;bool accepted=false;
    while(appletMainLoop()){
        padUpdate(&pad);const u64 key=padGetButtonsDown(&pad);
        if(key&HidNpadButton_B){miguLogin.cancel();break;}
        auto state=miguLogin.snapshot();
        if((key&HidNpadButton_A)&&!state.running&&!state.authenticated){
            if(qrTexture){SDL_DestroyTexture(qrTexture);qrTexture=nullptr;}shownQrRevision=0;
            miguLogin.start();state=miguLogin.snapshot();
        }
        if(state.qrRevision!=shownQrRevision&&!state.qrJpeg.empty()){
            if(qrTexture)SDL_DestroyTexture(qrTexture);
            qrTexture=miguQrTexture(r,state.qrJpeg);shownQrRevision=state.qrRevision;
        }
        SDL_SetRenderDrawColor(r,8,13,22,255);SDL_RenderClear(r);
        text(r,f,"咪咕视频官方扫码登录",385,75,520,true);
        if(qrTexture){SDL_Rect box{455,130,370,370};SDL_RenderCopy(r,qrTexture,nullptr,&box);}
        text(r,f,state.message,250,535,800,true);
        text(r,f,state.running?"请用咪咕视频 App 扫码 · B 取消":"A 获取新二维码 · B 返回",300,590,690,true);
        SDL_RenderPresent(r);
        if(state.authenticated){migu.setSession(state.session);accepted=true;break;}
        SDL_Delay(16);
    }
    if(qrTexture)SDL_DestroyTexture(qrTexture);
    return accepted;
};
std::string sourceConfigUrl=loadSourceUrl();
std::string youtubeProxyUrl=loadYoutubeProxy();
std::string youtubeSubscriptionUrl=subscriptionBoot.url;
size_t youtubeSubscriptionIndex=subscriptionBoot.index;
std::string youtubeSubscriptionName;
bool autoPlayNext=loadAutoPlayNext();
Uint32 lastEpisodeNoticeUntil=0;
auto settingsCloudLogin=[&](bool isBaidu){
    if(isBaidu)baiduLogin.start();else quarkLogin.start();
    while(appletMainLoop()){
        padUpdate(&pad);const u64 key=padGetButtonsDown(&pad);
        if(key&HidNpadButton_B){if(isBaidu)baiduLogin.cancel();else quarkLogin.cancel();break;}
        auto q=quarkLogin.snapshot();auto b=baiduLogin.snapshot();
        bool running=isBaidu?b.running:q.running,authenticated=isBaidu?b.authenticated:q.authenticated;
        if(authenticated){if(isBaidu){baidu.setSession(b.session);baiduLoginApplied=true;}else{quark.setSession(q.session);loginApplied=true;}break;}
        if((key&HidNpadButton_A)&&!running){if(isBaidu)baiduLogin.start();else quarkLogin.start();}
        const auto& modules=isBaidu?b.modules:q.modules;const int size=isBaidu?b.size:q.size;
        SDL_SetRenderDrawColor(r,18,24,38,255);SDL_RenderClear(r);
        text(r,f,isBaidu?"百度网盘登录":"夸克网盘登录",440,65,450,true);
        if(size>0&&modules.size()>=(size_t)size*size){
            const int cell=std::max(1,380/(size+8)),pixels=cell*(size+8),x=(1280-pixels)/2,y=135;
            SDL_SetRenderDrawColor(r,255,255,255,255);SDL_Rect paper{x,y,pixels,pixels};SDL_RenderFillRect(r,&paper);
            SDL_SetRenderDrawColor(r,0,0,0,255);
            for(int row=0;row<size;++row)for(int col=0;col<size;++col)if(modules[row*size+col]){SDL_Rect dot{x+(col+4)*cell,y+(row+4)*cell,cell,cell};SDL_RenderFillRect(r,&dot);}
        }
        text(r,f,isBaidu?b.message:q.message,180,545,920,true);
        text(r,f,"请用对应网盘 App 扫码 · A 重试 · B 返回",280,610,850,true);
        SDL_RenderPresent(r);SDL_Delay(16);
    }
};
auto settingsYouTubeLogin=[&](bool tvProbe=false){
    if(!youtube.configured())return std::string("请将 youtube-cookie.txt 放到 SD 卡 /switch/OKSwitch/，或配置 youtube-oauth.json");
    if(tvProbe)youtube.beginTvLoginProbe();else youtube.beginLogin();std::string message,qrUrl,outcome;
    uint8_t loginQr[qrcodegen_BUFFER_LEN_MAX]{},loginTemp[qrcodegen_BUFFER_LEN_MAX]{};bool loginQrReady=false;
    while(appletMainLoop()){
        padUpdate(&pad);const u64 key=padGetButtonsDown(&pad);
        if(key&HidNpadButton_B){if(!outcome.empty())return outcome;youtube.cancel();return std::string("已取消 YouTube 配对");}
        if(!outcome.empty()){
            if(key&HidNpadButton_A){outcome.clear();qrUrl.clear();youtube.beginTvLoginProbe();continue;}
            SDL_SetRenderDrawColor(r,18,24,38,255);SDL_RenderClear(r);
            text(r,f,"YouTube 电视端实验结果",375,95,650,true);
            const auto details=outcome.find(" · ");
            text(r,f,outcome.substr(0,details),130,220,1050,true);
            if(details!=std::string::npos){
                const auto extra=outcome.substr(details+3);const auto split=extra.find("; ");
                text(r,f,extra.substr(0,split),130,315,1050,true);
                if(split!=std::string::npos)text(r,f,extra.substr(split+2),130,395,1050,true);
            }
            text(r,f,"A 重新测试 · B 返回设置",410,605,600,true);
            SDL_RenderPresent(r);SDL_Delay(16);continue;
        }
        media::YouTubeResult result;if(youtube.poll(result)){
            outcome=result.succeeded?result.message:result.error;
            if(!tvProbe)return outcome;
            continue;
        }
        const auto state=youtube.loginState();
        if((key&HidNpadButton_A)&&!state.running){if(tvProbe)youtube.beginTvLoginProbe();else youtube.beginLogin();}
        SDL_SetRenderDrawColor(r,18,24,38,255);SDL_RenderClear(r);
        text(r,f,tvProbe?"YouTube 电视端配对实验":"YouTube 账号登录",445,75,550,true);
        if(!state.verificationUrl.empty()){
            if(qrUrl!=state.verificationUrl){qrUrl=state.verificationUrl;loginQrReady=qrcodegen_encodeText(qrUrl.c_str(),loginTemp,loginQr,qrcodegen_Ecc_LOW,qrcodegen_VERSION_MIN,qrcodegen_VERSION_MAX,qrcodegen_Mask_AUTO,true);}
            if(loginQrReady){
                const int size=qrcodegen_getSize(loginQr),cell=300/(size+8),pixels=cell*(size+8);
                SDL_SetRenderDrawColor(r,255,255,255,255);SDL_Rect paper{190,170,pixels,pixels};SDL_RenderFillRect(r,&paper);
                SDL_SetRenderDrawColor(r,0,0,0,255);
                for(int row=0;row<size;++row)for(int col=0;col<size;++col)if(qrcodegen_getModule(loginQr,col,row)){SDL_Rect dot{190+(col+4)*cell,170+(row+4)*cell,cell,cell};SDL_RenderFillRect(r,&dot);}
            }
            text(r,f,"手机扫码打开授权页",550,185,600,true);
            text(r,f,state.verificationUrl,550,245,640,true);
            text(r,f,"输入配对码",550,325,400,true);
            text(r,f,state.userCode,550,385,640,true);
        }
        text(r,f,state.message,160,535,960,true);
        text(r,f,"A 重新申请 · B 返回",465,630,500,true);
        SDL_RenderPresent(r);SDL_Delay(16);
    }
    youtube.cancel();return std::string("已取消 YouTube 配对");
};
auto settingsLanSubscription=[&](std::string& url,bool oauthConfig=false)->std::string{
    if(!netReady)return "网络尚未就绪";
    net::LanTextInput input(oauthConfig);std::string error;
    if(!input.start(error))return error;
    uint8_t temporary[qrcodegen_BUFFER_LEN_MAX]{},qr[qrcodegen_BUFFER_LEN_MAX]{};
    const bool encoded=qrcodegen_encodeText(input.url().c_str(),temporary,qr,qrcodegen_Ecc_LOW,
        qrcodegen_VERSION_MIN,qrcodegen_VERSION_MAX,qrcodegen_Mask_AUTO,true);
    if(!encoded){input.stop();return "无法生成局域网二维码";}
    const int size=qrcodegen_getSize(qr);
    while(appletMainLoop()&&!input.completed()){
        padUpdate(&pad);if(padGetButtonsDown(&pad)&HidNpadButton_B){input.stop();return "已取消手机输入";}
        SDL_SetRenderDrawColor(r,18,24,38,255);SDL_RenderClear(r);
        text(r,f,oauthConfig?"YouTube 客户端 · 手机扫码导入":"机场订阅 · 手机扫码输入",365,48,700,true);
        const int cell=std::max(1,390/(size+8)),pixels=cell*(size+8),x=(1280-pixels)/2,y=122;
        SDL_SetRenderDrawColor(r,255,255,255,255);SDL_Rect paper{x,y,pixels,pixels};SDL_RenderFillRect(r,&paper);
        SDL_SetRenderDrawColor(r,0,0,0,255);
        for(int row=0;row<size;++row)for(int col=0;col<size;++col)
            if(qrcodegen_getModule(qr,col,row)){SDL_Rect dot{x+(col+4)*cell,y+(row+4)*cell,cell,cell};SDL_RenderFillRect(r,&dot);}
        text(r,f,input.url(),(1280-textWidth(f,input.url()))/2,535,1120,true);
        text(r,f,oauthConfig?"同一局域网 · 粘贴 Google 客户端 JSON · B 取消":"手机和 Switch 连接同一局域网 · 粘贴链接后发送 · B 取消",180,605,1000,true);
        SDL_RenderPresent(r);SDL_Delay(16);
    }
    const bool received=input.completed();
    if(received)url=input.value();
    input.stop();
    return received?"":"已取消手机输入";
};
auto importYoutubeOAuth=[&]()->std::string{
    std::string raw;const std::string cancelled=settingsLanSubscription(raw,true);
    if(!cancelled.empty())return cancelled;
    json_error_t parseError{};json_t* root=json_loadb(raw.data(),raw.size(),0,&parseError);
    if(!root)return "客户端 JSON 格式错误";
    json_t* values=json_object_get(root,"installed");if(!json_is_object(values))values=root;
    auto* idValue=json_object_get(values,"client_id");auto* secretValue=json_object_get(values,"client_secret");
    const std::string id=json_is_string(idValue)?json_string_value(idValue):"";
    const std::string secret=json_is_string(secretValue)?json_string_value(secretValue):"";
    json_decref(root);
    if(id.size()>250||id.size()<30||id.find(".apps.googleusercontent.com")==std::string::npos||secret.empty()||secret.size()>300||
       id.find_first_of(" \r\n\t")!=std::string::npos||secret.find_first_of(" \r\n\t")!=std::string::npos)return "不是有效的 Google 电视客户端 JSON";
    if(mkdir("sdmc:/switch",0777)!=0&&errno!=EEXIST)return "无法建立 SD 卡目录";
    if(mkdir("sdmc:/switch/OKSwitch",0777)!=0&&errno!=EEXIST)return "无法建立 SD 卡目录";
    const char* path="sdmc:/switch/OKSwitch/youtube-oauth.json";
    const std::string temporary=std::string(path)+".tmp";
    json_t* saved=json_pack("{s:s,s:s}","client_id",id.c_str(),"client_secret",secret.c_str());
    const bool written=saved&&json_dump_file(saved,temporary.c_str(),JSON_INDENT(2))==0;
    if(saved)json_decref(saved);
    if(!written)return "客户端配置保存失败";
    const std::string backup=std::string(path)+".bak";std::remove(backup.c_str());
    if(std::rename(path,backup.c_str())!=0&&errno!=ENOENT)return "无法备份原客户端配置";
    if(std::rename(temporary.c_str(),path)!=0){std::rename(backup.c_str(),path);return "客户端配置保存失败";}
    std::remove(backup.c_str());youtube.reloadConfig();
    return "YouTube 客户端已导入；按 A 申请配对码";
};
auto settingsSubscription=[&](bool fromLan)->std::string{
    if(subscriptionBoot.thread){
        if(!subscriptionBoot.done.load())return "订阅正在后台连接，请稍后再试";
        SDL_WaitThread(subscriptionBoot.thread,nullptr);subscriptionBoot.thread=nullptr;
        youtubeSubscriptionName=subscriptionBoot.name;
    }
    std::string url=youtubeSubscriptionUrl;
    if(fromLan){
        const std::string error=settingsLanSubscription(url);
        if(!error.empty())return error;
    }else if(url.empty()&&!keyboard(url,"Proxy subscription","Paste HTTPS subscription URL"))return "已取消订阅设置";
    if(url.empty()){
        if(!saveYoutubeSubscription("",0))return "清除订阅失败，请检查 SD 卡";
        youtubeSubscriptionUrl.clear();youtubeSubscriptionName.clear();youtubeTunnel.stop();youtube.setProxy(youtubeProxyUrl);
        return "已清除订阅代理";
    }
    if(url.rfind("https://",0)!=0||url.find_first_of(" \r\n\t")!=std::string::npos)return "订阅地址必须是完整的 HTTPS 链接";
    if(!netReady)return "网络尚未就绪";
    SubscriptionFetch fetch;fetch.http=&http;fetch.url=url;
    SDL_Thread* thread=SDL_CreateThread(subscriptionFetchEntry,"subscription-fetch",&fetch);
    if(!thread)return "无法启动订阅下载";
    bool cancelled=false;
    while(appletMainLoop()&&!fetch.done.load()){
        padUpdate(&pad);if(padGetButtonsDown(&pad)&HidNpadButton_B){fetch.cancel.store(true);cancelled=true;break;}
        SDL_SetRenderDrawColor(r,18,24,38,255);SDL_RenderClear(r);
        text(r,f,"正在读取代理订阅...",420,270,500,true);
        text(r,f,"B 取消",565,360,220,true);
        SDL_RenderPresent(r);SDL_Delay(16);
    }
    if(!fetch.done.load())fetch.cancel.store(true);
    SDL_WaitThread(thread,nullptr);
    if(cancelled)return "已取消订阅下载";
    if(!fetch.error.empty())return fetch.error;
    if(fetch.nodes.empty())return "订阅里没有可用节点";
    net::NodeLatency latency(fetch.nodes);
    size_t selected=0; // First row selects a measured, reachable node automatically.
    auto activateNode=[&](size_t index,std::string& error)->bool{
        if(!youtubeTunnel.start(fetch.nodes[index],error)){
            youtube.setProxy(youtubeProxyUrl);youtubeSubscriptionName.clear();return false;
        }
        if(!probeYoutubeProxy(http,youtubeTunnel.proxyUrl(),error)){
            youtubeTunnel.stop();youtube.setProxy(youtubeProxyUrl);youtubeSubscriptionName.clear();return false;
        }
        youtube.setProxy(youtubeTunnel.proxyUrl());
        if(!saveYoutubeSubscription(url,index)){
            youtubeTunnel.stop();youtube.setProxy(youtubeProxyUrl);youtubeSubscriptionName.clear();
            error="节点可连接，但订阅保存失败";return false;
        }
        youtubeSubscriptionUrl=url;youtubeSubscriptionIndex=index;youtubeSubscriptionName=fetch.nodes[index].name;
        return true;
    };
    int previousDirection=0;
    while(appletMainLoop()){
        padUpdate(&pad);u64 key=padGetButtonsDown(&pad);
        const auto stick=padGetStickPos(&pad,0);
        const int direction=stick.y>16000?-1:stick.y<-16000?1:0;
        if(direction!=previousDirection){if(direction<0)key|=HidNpadButton_Up;if(direction>0)key|=HidNpadButton_Down;}previousDirection=direction;
        if(key&HidNpadButton_B)return "已取消节点选择";
        if((key&HidNpadButton_Up)&&selected>0)--selected;
        if((key&HidNpadButton_Down)&&selected<fetch.nodes.size())++selected;
        if(key&HidNpadButton_A){
            std::string error;
            if(selected>0){
                if(!activateNode(selected-1,error))return error;
                return "已选择代理节点："+youtubeSubscriptionName;
            }
            const Uint64 started=SDL_GetTicks64();
            while(appletMainLoop()&&SDL_GetTicks64()-started<5000){
                padUpdate(&pad);if(padGetButtonsDown(&pad)&HidNpadButton_B)return "已取消自动选择";
                SDL_SetRenderDrawColor(r,18,24,38,255);SDL_RenderClear(r);
                text(r,f,"正在检测节点连接延迟... · B 取消",300,320,800,true);
                SDL_RenderPresent(r);SDL_Delay(40);
            }
            const auto measured=latency.snapshot();std::vector<size_t> candidates;
            for(size_t i=0;i<measured.size();++i)if(measured[i]>0)candidates.push_back(i);
            std::stable_sort(candidates.begin(),candidates.end(),[&](size_t a,size_t b){
                if(fetch.nodes[a].allowInsecure!=fetch.nodes[b].allowInsecure)return !fetch.nodes[a].allowInsecure;
                return measured[a]<measured[b];
            });
            // If the first measured nodes failed, also try unmeasured nodes.
            for(size_t i=0;i<measured.size();++i)if(measured[i]==-1||measured[i]==-2)candidates.push_back(i);
            if(candidates.empty())return "尚无可连接节点，请稍后重试或手动选择";
            for(size_t i=0;i<std::min<size_t>(3,candidates.size());++i){
                SDL_SetRenderDrawColor(r,18,24,38,255);SDL_RenderClear(r);
                text(r,f,"正在验证 YouTube 连通性："+fetch.nodes[candidates[i]].name,130,320,1000,true);
                SDL_RenderPresent(r);
                if(activateNode(candidates[i],error))return "已自动选择："+youtubeSubscriptionName;
            }
            return "自动选择失败："+error;
        }
        SDL_SetRenderDrawColor(r,18,24,38,255);SDL_RenderClear(r);
        text(r,f,"YouTube 订阅节点 · "+std::to_string(fetch.nodes.size())+" 个",60,35,1000,true);
        text(r,f,"上下选择 · A 使用 · B 返回",60,650,1100,true);
        const size_t first=(selected/9)*9;
        latency.focus(first?first-1:0);
        const auto delays=latency.snapshot();
        text(r,f,"自动选择依据连接延迟与可用性，不代表视频下载速度",60,610,1100,true);
        for(size_t i=first;i<std::min(first+9,fetch.nodes.size()+1);++i){
            SDL_Rect box{60,88+(int)(i-first)*60,1160,52};
            SDL_SetRenderDrawColor(r,i==selected?55:28,i==selected?150:46,i==selected?125:68,255);SDL_RenderFillRect(r,&box);
            if(i==0){text(r,f,"自动选择",78,box.y+13,700,true);text(r,f,"优先可信节点",800,box.y+13,185,true);continue;}
            const size_t index=i-1;
            text(r,f,fetch.nodes[index].name,78,box.y+13,700,true);
            text(r,f,fetch.nodes[index].allowInsecure?"不校验证书":"校验证书",800,box.y+13,185,true);
            const int delay=delays[index];
            const std::string label=delay>=0?std::to_string(delay)+" ms":delay==-1?"待检测":delay==-2?"检测中":delay==-3?"超时":"连接失败";
            text(r,f,label,1020,box.y+13,180,true);
        }
        SDL_RenderPresent(r);SDL_Delay(16);
    }
    return "已取消节点选择";
};
auto openSettings=[&](){
    int row=0;bool confirmLogout=false;std::string message="选择账号登录，或调整播放与视频源设置";
    const char* names[]={"夸克网盘","百度网盘","咪咕视频","YouTube 账号","自动播放下集","YouTube 网络代理","YouTube 机场订阅","视频源配置地址","重新加载视频源","清除视频源地址","在线更新"};
    while(appletMainLoop()){
        padUpdate(&pad);
        if(!(padGetButtons(&pad)&HidNpadButton_Minus))break;
        SDL_Delay(8);
    }
    while(appletMainLoop()){
        if(subscriptionBoot.thread&&subscriptionBoot.done.load()){
            SDL_WaitThread(subscriptionBoot.thread,nullptr);subscriptionBoot.thread=nullptr;
            youtubeSubscriptionName=subscriptionBoot.name;
            if(!subscriptionBoot.error.empty())message="订阅连接失败："+subscriptionBoot.error;
        }
        padUpdate(&pad);u64 key=padGetButtonsDown(&pad);
        const auto stick=padGetStickPos(&pad,0);
        static int previousDirection=0;
        const int direction=stick.y>16000?-1:stick.y<-16000?1:0;
        if(direction!=previousDirection){if(direction<0)key|=HidNpadButton_Up;if(direction>0)key|=HidNpadButton_Down;}previousDirection=direction;
        if(confirmLogout){
            if(key&HidNpadButton_B)confirmLogout=false;
            else if(key&HidNpadButton_A){
                bool ok=false;
                if(row==0){ok=quarkLogin.logout();quark.setSession({});loginApplied=false;loginView={};}
                else if(row==1){ok=baiduLogin.logout();baidu.setSession({});baiduLoginApplied=false;baiduLoginView={};}
                else if(row==2){ok=miguLogin.logout();migu.setSession({});}
                else{youtube.signOut();ok=true;}
                pendingLoginFileId.clear();pendingLoginLine=SIZE_MAX;autoOpenFirstShare=false;autoTryingLines=false;
                message=ok?std::string(names[row])+" · 已退出登录":"本次登录已清除，但 SD 卡上的登录文件删除失败";
                confirmLogout=false;
            }
            key=0;
        }else{
            if(key&(HidNpadButton_B|HidNpadButton_Minus))break;
            if((key&HidNpadButton_Up)&&row>0)--row;
            if((key&HidNpadButton_Down)&&row<10)++row;
            if((key&HidNpadButton_X)&&row<4)confirmLogout=true;
            else if((key&HidNpadButton_X)&&row==6){
                if(subscriptionBoot.thread&&!subscriptionBoot.done.load())message="订阅仍在连接，请稍后清除";
                else if(!saveYoutubeSubscription("",0))message="清除订阅失败，请检查 SD 卡";
                else{youtubeSubscriptionUrl.clear();youtubeSubscriptionName.clear();youtubeTunnel.stop();youtube.setProxy(youtubeProxyUrl);message="已清除机场订阅";}
            }
            else if((key&HidNpadButton_Y)&&row==3){message=importYoutubeOAuth();
            }
            else if((key&HidNpadButton_Y)&&row==6){message=settingsSubscription(true);
            }
            else if((key&HidNpadButton_Plus)&&row==3){message=settingsYouTubeLogin(true);
            }
            else if(key&HidNpadButton_A){
                if(row<4){
                    if(!netReady)message="网络尚未就绪";
                    else if(row==2)message=miguSignIn()?"咪咕登录成功":"咪咕登录未完成";
                    else if(row==3)message=settingsYouTubeLogin();
                    else{settingsCloudLogin(row==1);message="账号状态已更新";}
                }else if(row==4){
                    const bool next=!autoPlayNext;
                    if(saveAutoPlayNext(next)){autoPlayNext=next;message=next?"已开启自动播放下集":"已关闭自动播放下集";}
                    else message="设置保存失败，请检查 SD 卡";
                }else if(row==5){
                    if(subscriptionBoot.thread&&!subscriptionBoot.done.load())message="订阅仍在连接，请稍后切换代理";
                    else{
                    std::string proxy=youtubeProxyUrl;
                    if(keyboard(proxy,"YouTube proxy","Enter http://host:port or leave empty")){
                        const bool valid=proxy.empty()||proxy.rfind("http://",0)==0||proxy.rfind("socks5://",0)==0||proxy.rfind("socks5h://",0)==0;
                        if(!valid||proxy.find_first_of(" \r\n\t")!=std::string::npos)message="代理格式应为 http://地址:端口 或 socks5h://地址:端口";
                        else if(!saveYoutubeProxy(proxy)||!saveYoutubeSubscription("",0))message="代理保存失败，请检查 SD 卡";
                        else{youtubeProxyUrl=proxy;youtubeSubscriptionUrl.clear();youtubeSubscriptionName.clear();youtubeTunnel.stop();youtube.setProxy(proxy);message=proxy.empty()?"YouTube 已切换为直连":"YouTube 代理已更新";}
                    }
                    }
                }else if(row==6){message=settingsSubscription(false);
                }else if(row==10){
                    if(!netReady)message="网络尚未就绪";
                    else{
                        net::OnlineUpdater updater(http,youtube.currentProxy(),installedNroPath());
                        if(!updater.start())message="无法启动更新检查";
                        else{
                            while(appletMainLoop()){
                                padUpdate(&pad);
                                if((padGetButtonsDown(&pad)&HidNpadButton_B)&&!updater.done())updater.cancel();
                                SDL_SetRenderDrawColor(r,18,24,38,255);SDL_RenderClear(r);
                                text(r,f,"OKSwitch · 在线更新",65,55,1100,true);
                                text(r,f,updater.message(),65,180,1120,true);
                                text(r,f,std::to_string(updater.percent())+"%",65,250,500,true);
                                text(r,f,updater.done()?"A 返回设置":"B 取消下载",65,650,1100,true);
                                SDL_RenderPresent(r);
                                if(updater.done()&&(padGetButtonsDown(&pad)&HidNpadButton_A))break;
                                SDL_Delay(32);
                            }
                            if(!updater.done())updater.cancel();updater.join();message=updater.message();
                        }
                    }
                }else{
                    std::string candidate=sourceConfigUrl;bool apply=row==8||row==9;
                    if(row==7)apply=keyboard(candidate,"Video source URL","Enter http:// or https:// configuration URL");
                    if(row==9)candidate=defaultSourceUrl;
                    if(apply){
                        if(row==8&&candidate.empty())message="请先设置视频源地址";
                        else if(!candidate.empty()&&(candidate.rfind("https://",0)!=0&&candidate.rfind("http://",0)!=0)||candidate.find_first_of(" \r\n\t")!=std::string::npos)message="请输入完整的 http:// 或 https:// 配置地址";
                        else if(!saveSourceUrl(candidate))message="地址保存失败，请检查 SD 卡";
                        else{sourceConfigUrl=candidate;if(!candidate.empty()){sourceConfigLoader.request(candidate);status="正在加载视频源配置...";}else status="已清除视频源地址";break;}
                    }
                }
            }
        }
        const bool logged[]={loginApplied||hasSavedLogin("sdmc:/switch/OKSwitch/quark-session.json"),baiduLoginApplied||hasSavedLogin("sdmc:/switch/OKSwitch/baidu-session-v2.json"),miguLogin.savedSession().valid(),youtube.signedIn()};
        SDL_SetRenderDrawColor(r,18,24,38,255);SDL_RenderClear(r);text(r,f,"设置 · 账号、播放与视频源",60,32,1000,true);
        for(int i=0;i<11;++i){SDL_Rect box{60,77+i*48,1160,43};SDL_SetRenderDrawColor(r,i==row?55:28,i==row?150:46,i==row?125:68,255);SDL_RenderFillRect(r,&box);text(r,f,names[i],85,box.y+9,350,true);
            text(r,f,i<4?(logged[i]?"已有登录信息":i==3&&!youtube.configured()?"未配置配对码客户端":"未登录"):i==4?(autoPlayNext?"开启":"关闭"):i==5?(!youtubeSubscriptionName.empty()?"当前使用订阅节点":subscriptionBoot.thread?"订阅连接中":youtubeProxyUrl.empty()?"直连（不使用代理）":youtubeProxyUrl):i==6?(youtubeSubscriptionUrl.empty()?"未配置":subscriptionBoot.thread?"正在连接...":youtubeSubscriptionName.empty()?"已配置 · 未连接":"已启用 · "+youtubeSubscriptionName):i==7?(sourceConfigUrl.empty()?"未配置":sourceConfigUrl):i==8?"立即更新来源列表":i==9?"清除已保存的地址":"经当前 YouTube 代理检查和下载",455,box.y+9,740,true);
        }
        text(r,f,message,65,615,1150,true);text(r,f,row==6?"A 选择节点 · Y 扫码设置地址 · X 清除订阅 · B 返回":row==3?"A 现有配对 · + 电视端实验 · Y 导入客户端 · X 退出 · B 返回":"上下选择 · A 登录／修改 · X 退出账号 · B 返回",65,665,1150,true);
        if(confirmLogout){SDL_SetRenderDrawColor(r,12,21,32,255);SDL_Rect dialog{240,250,800,200};SDL_RenderFillRect(r,&dialog);std::string title=std::string("退出 ")+names[row]+" 的登录？";text(r,f,title,(1280-textWidth(f,title))/2,290,760,true);text(r,f,"A 确认退出    B 取消",465,375,600,true);}
        SDL_RenderPresent(r);SDL_Delay(16);
    }
    while(appletMainLoop()){
        padUpdate(&pad);
        if(!(padGetButtons(&pad)&(HidNpadButton_B|HidNpadButton_Minus)))break;
        SDL_Delay(8);
    }
};
if(netReady){loadBrowse();if(!sourceConfigUrl.empty())sourceConfigLoader.request(sourceConfigUrl);}
auto playMiguLive=[&](const media::MiguLiveMatch& match){
    status="咪咕 · 正在获取 "+match.title+" 直播...";
    SDL_SetRenderDrawColor(r,18,24,38,255);SDL_RenderClear(r);text(r,f,"OKSwitch | "+status,40,22);SDL_RenderPresent(r);
    int qualityIndex=1;
    constexpr int qualityRates[]={2,3,4};
    auto stream=migu.playbackLive(match,qualityRates[qualityIndex]);
    if(!stream.succeeded&&stream.error.code==cloud::ProviderErrorCode::AuthenticationRequired){
        if(miguSignIn())stream=migu.playbackLive(match,qualityRates[qualityIndex]);
    }
    if(!stream.succeeded){liveError=stream.error.message;status="咪咕直播 · "+liveError;}
    else{
        liveError.clear();auto showPlayerStatus=[&](const std::string& message){SDL_SetRenderDrawColor(r,8,13,22,255);SDL_RenderClear(r);text(r,f,message,30,24,1220,true);SDL_RenderPresent(r);};
        cloud::PlaybackSource previousStream;
        int previousQualityIndex=-1;
        while(true){
            media::PlaybackOptions options;options.displayTitle=match.title;options.openSettings=openSettings;
            options.liveQualityLabels={"标清 · 540P","高清 · 720P","蓝光 · 1080P（可能需会员）"};
            options.currentQualityIndex=qualityIndex;
            if(!stream.value.accessNotice.empty())options.displayTitle+=" · "+stream.value.accessNotice;
            options.measureText=[&](const std::string& value){return textWidth(f,value);};
            auto played=media::VideoPlayer::play(r,stream.value,options,showPlayerStatus,[&](const std::string& value,int x,int y,int width){text(r,f,value,x,y,width,true);});
            if(played.requestedQualityIndex<0){
                if(!played.opened&&previousQualityIndex>=0){
                    showPlayerStatus("新画质无法播放，恢复原画质");
                    stream.value=std::move(previousStream);qualityIndex=previousQualityIndex;
                    previousQualityIndex=-1;continue;
                }
                status=played.error.empty()?"已退出咪咕直播":played.error;break;
            }
            const int requested=played.requestedQualityIndex;
            showPlayerStatus("咪咕 · 正在切换画质...");
            auto next=migu.playbackLive(match,qualityRates[requested]);
            if(next.succeeded){previousStream=stream.value;previousQualityIndex=qualityIndex;
                stream=std::move(next);qualityIndex=requested;}
            else showPlayerStatus("画质切换失败，恢复原画质："+next.error.message);
        }
    }
};
int stickDirection=0;Uint32 stickRepeatAt=0;bool historyAHeld=false;Uint32 historyAHoldStarted=0;size_t historyAHoldIndex=0;bool historyDeletePrompt=false;size_t historyDeleteIndex=0;
while(appletMainLoop()){padUpdate(&pad);u64 k=padGetButtonsDown(&pad);const u64 heldButtons=padGetButtons(&pad);bool historyOpenRequested=false;
HidAnalogStickState stick=padGetStickPos(&pad,0);int absX=stick.x<0?-stick.x:stick.x,absY=stick.y<0?-stick.y:stick.y;int nextStick=0;
if(absX>16000||absY>16000)nextStick=absX>absY?(stick.x>0?1:2):(stick.y>0?3:4);
Uint32 now=SDL_GetTicks();if(nextStick){bool fire=nextStick!=stickDirection||SDL_TICKS_PASSED(now,stickRepeatAt);if(fire){k|=nextStick==1?HidNpadButton_Right:nextStick==2?HidNpadButton_Left:nextStick==3?HidNpadButton_Up:HidNpadButton_Down;stickRepeatAt=now+(nextStick!=stickDirection?320:110);}stickDirection=nextStick;}else stickDirection=0;
if(k&HidNpadButton_Minus){openSettings();if(view==View::Browse&&categories[category].kind==9)loadBrowse();k=0;}
if(youtubeHomeWaitingForProxy&&(!subscriptionBoot.thread||subscriptionBoot.done.load())){
    if(view==View::Browse&&categories[category].kind==9&&youtubePage==YouTubePage::Explore)
        requestYoutubeHome();
    else youtubeHomeWaitingForProxy=false;
}
receiveBrowse();
if(view==View::Browse&&categories[category].kind==9){
    media::YouTubeResult result;
    if(youtube.poll(result)){
        youtubeLoading=false;
        if(result.durationOnly){
            for(const auto& item:result.items){
                for(auto& card:browseItems)
                    if(card.detailUrl=="yt:video:"+item.id)card.duration=item.duration;
                for(auto& card:youtubeQueuedCards)
                    if(card.detailUrl=="yt:video:"+item.id)card.duration=item.duration;
            }
        }else if(youtubePlaying){
            youtubePlaying=false;
            if(result.succeeded&&!result.stream.url.empty()){
                // Free the proxy for media before starting video and audio range requests.
                posters.clear();
                auto showPlayerStatus=[&](const std::string& message){SDL_SetRenderDrawColor(r,8,13,22,255);SDL_RenderClear(r);text(r,f,message,30,24,1220,true);SDL_RenderPresent(r);};
                media::PlaybackOptions options;options.displayTitle=youtubePendingTitle
                    +(result.videoHeight>0?" · "+std::to_string(result.videoHeight)+"p":"");options.openSettings=openSettings;
                options.measureText=[&](const std::string& value){return textWidth(f,value);};
                if(const auto* previous=youtubeHistory.find(youtubePendingItem.detailUrl.substr(9)))
                    if(previous->durationSeconds<=0||previous->positionSeconds<previous->durationSeconds-15)options.resumeSeconds=previous->positionSeconds;
                if(youtubeResumeOverride>=0)options.resumeSeconds=youtubeResumeOverride;
                auto played=media::VideoPlayer::play(r,result.stream,options,showPlayerStatus,
                    [&](const std::string& value,int x,int y,int width){text(r,f,value,x,y,width,true);});
                if(!played.opened&&!result.fallbackStream.url.empty()&&played.error!="已取消加载"){
                    showPlayerStatus("高清线路无法打开，正在尝试 "+std::to_string(result.fallbackHeight)+"p 备用线路 · B 取消");
                    options.displayTitle=youtubePendingTitle+" · "+std::to_string(result.fallbackHeight)+"p";
                    played=media::VideoPlayer::play(r,result.fallbackStream,options,showPlayerStatus,
                        [&](const std::string& value,int x,int y,int width){text(r,f,value,x,y,width,true);});
                }
                // The player uses its own PadState. Consume held buttons before
                // browsing can interpret R as a category switch.
                padUpdate(&pad);k=0;
                bool historySaved=true;
                if(played.opened){media::YouTubeHistoryEntry entry;entry.id=youtubePendingItem.detailUrl.substr(9);
                    entry.title=youtubePendingItem.title;entry.channel=youtubePendingItem.sourceName;entry.thumbnail=youtubePendingItem.posterUrl;
                    entry.positionSeconds=played.completed?0:played.positionSeconds;entry.durationSeconds=played.durationSeconds;
                    entry.updatedAt=std::time(nullptr);
                    historySaved=youtubeHistory.record(entry);
                }
                if(youtubePlaybackRefreshes<1&&played.error.find("HTTP 403")!=std::string::npos){
                    // A signed media URL can be rejected after playback starts,
                    // particularly on a later range request. Resolve it again
                    // and resume near the last displayed frame once.
                    ++youtubePlaybackRefreshes;
                    youtubeResumeOverride=played.opened?std::max(0.0,played.positionSeconds-2.0):options.resumeSeconds;
                    youtube.playback(youtubePendingItem.detailUrl.substr(9));
                    youtubeLoading=true;youtubePlaying=true;
                    status="YouTube · 媒体请求被拒绝，正在刷新地址并续播...";
                }else{youtubeResumeOverride=-1;status=played.error.empty()?(historySaved?"YouTube · 已退出播放":"YouTube · 本机历史保存失败"):played.error;}
            }else status="YouTube · "+result.error;
        }else if(result.succeeded){
            browseItems.clear();youtubeQueuedCards.clear();posters.clear();selected=0;
            if(youtubePage==YouTubePage::Explore){youtubeAccountHome=result.message.rfind("账号推荐",0)==0;youtubeSubscriptionHome=result.message.rfind("推荐暂不可用",0)==0;}
            if(youtubePage==YouTubePage::History)youtubeAccountHistory=result.message.rfind("账号观看历史",0)==0;
            for(const auto& item:result.items){media::SearchResult card;card.title=item.title;card.sourceName=item.channel;
                card.posterUrl=item.thumbnail;card.duration=item.duration;
                card.detailUrl=std::string(item.isChannel?"yt:channel:":"yt:video:")+item.id;
                browseItems.push_back(std::move(card));}
            youtube.durations(result.items);
            if(youtubePage==YouTubePage::Explore&&youtube.signedIn()&&!youtubeAccountHome){
                size_t inserted=0;
                for(const auto& watched:youtubeHistory.items()){
                    if(inserted>=3)break;
                    if(std::any_of(browseItems.begin(),browseItems.end(),[&](const auto& card){return card.detailUrl=="yt:video:"+watched.id;}))continue;
                    media::SearchResult card;card.title=watched.title;card.sourceName="最近观看 · "+watched.channel;
                    card.posterUrl=watched.thumbnail;card.duration=watched.durationSeconds>0?std::to_string((int)watched.durationSeconds/60)+":"+(watched.durationSeconds-(int)watched.durationSeconds/60*60<10?"0":"")+std::to_string((int)watched.durationSeconds%60):"";card.detailUrl="yt:video:"+watched.id;
                    browseItems.insert(browseItems.begin()+inserted,std::move(card));++inserted;
                }
            }
            if(youtubePage==YouTubePage::Explore&&youtubeNavSelected==1&&browseItems.size()>6){
                youtubeQueuedCards.assign(std::make_move_iterator(browseItems.begin()+6),
                    std::make_move_iterator(browseItems.end()));
                browseItems.resize(6);
            }
            status="YouTube · "+result.message;
        }else if(youtubePage==YouTubePage::History)status="YouTube · 本机观看历史 · 账号历史不可用（"+result.error+"）";
        else{if(youtubePage==YouTubePage::Explore){youtubeAccountHome=false;youtubeSubscriptionHome=false;}status="YouTube · "+result.error;}
    }
}

if(miguPlaybackLoading&&(k&HidNpadButton_B)){miguPlaybackLoader.cancel();miguPlaybackLoading=false;status="已取消获取视频";k=0;}
cloud::ProviderResult<cloud::PlaybackSource> miguStream;
if(miguPlaybackLoader.poll(miguStream)&&miguPlaybackLoading){
    miguPlaybackLoading=false;
    if(!miguStream.succeeded&&miguStream.error.code==cloud::ProviderErrorCode::AuthenticationRequired){
        if(miguSignIn()&&miguPlaybackLoader.start(pendingMiguClip)){miguPlaybackLoading=true;status="咪咕 · 已登录，重新获取视频 · B 立即返回";}
        else status="咪咕 · 登录未完成";
    }else if(!miguStream.succeeded){liveError=miguStream.error.message;status="咪咕 · "+liveError;}
    else{
        liveError.clear();auto showPlayerStatus=[&](const std::string& message){SDL_SetRenderDrawColor(r,8,13,22,255);SDL_RenderClear(r);text(r,f,message,30,24,1220,true);SDL_RenderPresent(r);};
        media::PlaybackOptions options;options.displayTitle=pendingMiguClip.title;options.measureText=[&](const std::string& value){return textWidth(f,value);};options.openSettings=openSettings;
        auto played=media::VideoPlayer::play(r,miguStream.value,options,showPlayerStatus,[&](const std::string& value,int x,int y,int width){text(r,f,value,x,y,width,true);});
        status=played.error.empty()?"已退出咪咕视频":played.error;
    }
    k=0;
}
if(miguPlaybackLoading)k=0;
// Cancel before polling, so a completion on the same frame cannot reopen the list.
if(miguClipsLoading&&(k&HidNpadButton_B)){miguClipsLoader.cancel();miguClipsLoading=false;selected=miguMatchSelected;status="已取消加载赛事视频";k=0;}
media::MiguResult<media::MiguClip> clipsResult;
if(miguClipsLoader.poll(clipsResult)&&miguClipsLoading){
    miguClipsLoading=false;
    if(clipsResult.succeeded){miguClips=std::move(clipsResult.items);miguClipFocus=true;selected=0;status="咪咕 · 选择全场集锦或回放";}
    else status="咪咕 · "+clipsResult.error;
}
if(miguClipsLoading)k=0;
media::MiguResult<media::MiguMatch> scheduleResult;std::string scheduleDirection;
if(miguScheduleLoader.poll(scheduleResult,scheduleDirection)&&view==View::Browse&&categories[category].kind==8&&miguMatchFocus){
    miguRefreshAt=SDL_GetTicks()+60000;
    if(!scheduleResult.succeeded){status="咪咕中超 · "+scheduleResult.error; miguScheduleRetryAt=SDL_GetTicks()+5000;}
    else if(scheduleDirection.empty()){
        const bool wasLoaded=!miguSchedule.empty();
        const bool allButton=wasLoaded&&!miguScheduleFocus&&selected==miguMatches.size();
        const std::string keep=selected<miguMatches.size()?miguMatches[selected].id:"";
        for(auto& match:scheduleResult.items){auto it=std::find_if(miguSchedule.begin(),miguSchedule.end(),[&](const auto& old){return old.id==match.id;});if(it==miguSchedule.end())miguSchedule.push_back(std::move(match));else *it=std::move(match);}
        sortMiguMatches(miguSchedule);miguOverview.clear();
        auto upcoming=std::find_if(miguSchedule.begin(),miguSchedule.end(),[](const auto& m){return m.status!=2;});
        size_t pivot=(size_t)(upcoming-miguSchedule.begin());size_t begin=pivot>3?pivot-3:0;
        for(size_t i=begin;i<std::min(miguSchedule.size(),pivot+12);++i)miguOverview.push_back(miguSchedule[i]);
        miguMatches=miguScheduleFocus?filteredMiguSchedule():miguOverview;selected=0;
        auto focus=std::find_if(miguMatches.begin(),miguMatches.end(),[&](const auto& m){return m.id==keep;});
        if(focus!=miguMatches.end())selected=(size_t)(focus-miguMatches.begin());
        if(allButton)selected=miguMatches.size();
        if(!wasLoaded)status="咪咕中超 · A 打开比赛 · 底部查看全部赛程";
    }else{
        const std::string keep=selected<miguMatches.size()?miguMatches[selected].id:"";
        const size_t oldSize=miguSchedule.size();
        for(auto& match:scheduleResult.items)if(std::none_of(miguSchedule.begin(),miguSchedule.end(),[&](const auto& old){return old.id==match.id;}))miguSchedule.push_back(std::move(match));
        sortMiguMatches(miguSchedule);
        if(miguSchedule.size()==oldSize){if(scheduleDirection=="up")miguNoEarlier=true;else miguNoLater=true;}
        if(miguScheduleFocus&&!miguClipFocus){miguMatches=filteredMiguSchedule();auto it=std::find_if(miguMatches.begin(),miguMatches.end(),[&](const auto& m){return m.id==keep;});if(it!=miguMatches.end())selected=(size_t)(it-miguMatches.begin());}
        status=miguSchedule.size()==oldSize?"咪咕中超 · 官网暂无更多该方向赛程":"咪咕中超 · 已加载 "+std::to_string(miguSchedule.size())+" 场比赛";
    }
}

if(view==View::Browse&&categories[category].kind==8&&footballMatchFocus&&miguMatchFocus&&!miguClipFocus&&!miguClipsLoading&&!miguSchedule.empty()&&!miguScheduleLoader.busy()&&SDL_TICKS_PASSED(SDL_GetTicks(),miguRefreshAt)){
    if(miguScheduleLoader.start())miguRefreshAt=SDL_GetTicks()+60000;
}
media::SourceConfigResult configuredSources;if(sourceConfigLoader.poll(configuredSources)){
    if(configuredSources.succeeded)configuredSiteCount=configuredSources.totalSites;
    if(!configuredSources.footballSources.empty()){footballSources=std::move(configuredSources.footballSources);if(std::find(footballSources.begin(),footballSources.end(),"电视频道")==footballSources.end())footballSources.insert(footballSources.begin(),"电视频道");}
    footballSources.erase(std::remove_if(footballSources.begin(),footballSources.end(),[](const std::string& name){return name.find("飞球")!=std::string::npos;}),footballSources.end());
    for(auto& name:footballSources)if(name=="咪咕")name="咪咕中超";
    if(std::find(footballSources.begin(),footballSources.end(),"咪咕中超")==footballSources.end())footballSources.insert(footballSources.begin()+std::min<size_t>(1,footballSources.size()),"咪咕中超");
    if(std::find(footballSources.begin(),footballSources.end(),"咪咕精选赛事")==footballSources.end())footballSources.insert(footballSources.begin()+std::min<size_t>(1,footballSources.size()),"咪咕精选赛事");
    if(configuredSources.succeeded){sources=std::move(configuredSources.sources);sourceConfigOnline=true;sourceFilter=-1;}
    if(status=="正在加载视频源配置...")status=configuredSources.succeeded?"视频源已更新 · "+std::to_string(sources.size())+" 个可用来源":"视频源加载失败，保留原来源 · "+configuredSources.error;
}
media::SearchProgress searchProgress;if(searchLoader.poll(searchProgress)&&searchPending){
    for(auto& item:searchProgress.items)searchItems.push_back(std::move(item));
    for(auto& source:searchProgress.sourceStatus)searchSourceStatus.push_back(std::move(source));
    std::stable_sort(searchItems.begin(),searchItems.end(),[&](const auto&a,const auto&b){return searchScore(a.title,activeSearchKeyword)>searchScore(b.title,activeSearchKeyword);});
    if(selected>=searchItems.size())selected=searchItems.empty()?0:searchItems.size()-1;
    if(searchProgress.done){searchPending=false;status=searchItems.empty()?(searchProgress.error.empty()?"没有找到资源":searchProgress.error):"找到 "+std::to_string(searchItems.size())+" 个资源";}
    else status="正在搜索资源 · "+std::to_string(searchProgress.finished)+" / "+std::to_string(searchProgress.total)+" · 已找到 "+std::to_string(searchItems.size())+" 个";
}
media::SearchResponse hotResult;if(hotSearchLoader.poll(hotResult)){hotSearchLoading=false;hotSearches.clear();if(hotResult.succeeded){for(const auto& item:hotResult.items)if(hotSearches.size()<15)hotSearches.push_back(item.title);if(view==View::SearchHome)status="搜索";}else if(view==View::SearchHome)status=hotResult.error.empty()?"360kan 热搜加载失败":hotResult.error;}
if(view==View::SearchHome&&!searchInput.empty()&&suggestionDue&&SDL_TICKS_PASSED(SDL_GetTicks(),suggestionDue)){lastSuggestionQuery=searchInput;suggestionLoader.request("okswitch:suggest:"+searchInput);suggestionDue=0;suggestionLoading=true;status="正在联想片名...";}
media::SearchResponse suggestionResult;if(suggestionLoader.poll(suggestionResult)){suggestionLoading=false;if(view==View::SearchHome&&lastSuggestionQuery==searchInput){searchSuggestions.clear();if(suggestionResult.succeeded)for(const auto& item:suggestionResult.items)searchSuggestions.push_back(item.title);suggestionSelected=0;suggestionFocus=false;status=searchSuggestions.empty()?"没有联想结果":"选择候选片名，或继续输入";}}
media::SearchResult loadedItem;media::DetailResponse loadedDetail;
if(detailLoader.poll(loadedItem,loadedDetail)&&resourceLoading){
    resourceLoading=false;
    if(!loadedDetail.succeeded){
        status=loadedDetail.error.empty()?"详情加载失败":loadedDetail.error;
        if(pendingHistoryResume&&loadedItem.detailUrl==pendingHistoryResume->item.detailUrl){
            const std::string title=pendingHistoryResume->item.title;
            runSearch(title);status="原来源详情失效，已搜索同剧其他来源 · 选中后继续播放";
        }
    }
    else{
        rememberPage();detailItem=std::move(loadedItem);detailAction=-1;summaryOpen=false;detailFieldOpen=false;detail=std::move(loadedDetail.detail);detailSource=detailItem.sourceName;activeShare=0;selected=0;
        if(detail.posterUrl.empty())detail.posterUrl=detailItem.posterUrl;
        view=View::Shares;cloudItems.clear();dirs.clear();
        if(detail.shares.empty()){status="没有可用的播放线路";if(pendingHistoryResume)continueHistoryResume();}
        else if(pendingHistoryResume){historyLineStart=0;historyLinesTried=0;for(size_t i=0;i<detail.shares.size();++i)if(cleanUrl(detail.shares[i].url)==cleanUrl(pendingHistoryResume->shareUrl)){historyLineStart=i;break;}status="正在恢复上次播放线路...";continueHistoryResume();}
        else{status="详情已加载，正在自动加载首条线路...";autoTryingLines=true;autoTryLine=0;autoOpenFirstShare=true;autoOpenShareAt=SDL_GetTicks()+1;}
    }
}
cloud::CloudLineResult loadedLine;
if(cloudLineLoader.poll(loadedLine)&&lineLoading){
    lineLoading=false;
    activeCloud=loadedLine.baidu?CloudKind::Baidu:CloudKind::Quark;
    if(loadedLine.succeeded){
        activeShare=loadedLine.index;
        session=std::move(loadedLine.session);cloudItems=std::move(loadedLine.items);sortCloudEntries(cloudItems);dirs=std::move(loadedLine.directories);selected=0;view=View::Files;
        status="线路 "+std::to_string(activeShare+1)+" / "+std::to_string(detail.shares.size())+" · 已加载 "+std::to_string(cloudItems.size())+" 个文件或文件夹";
        autoTryingLines=false;
        if(pendingHistoryResume){if(auto index=resumeEpisodeIndex(*pendingHistoryResume)){selected=*index;status="已定位上次观看的 "+cloudItems[selected].name+" · 按 A 继续";}else continueHistoryResume();}
    }else{
        status="线路 "+std::to_string(loadedLine.index+1)+" 加载失败："+loadedLine.error;
        const bool baiduSessionInvalid=loadedLine.baidu&&(
            loadedLine.error.find("登录状态无效")!=std::string::npos
            ||loadedLine.error.find("重新扫码")!=std::string::npos
            ||loadedLine.error.find("会话不完整")!=std::string::npos);
        if(baiduSessionInvalid){
            autoTryingLines=false;activeCloud=CloudKind::Baidu;
            pendingLoginLine=loadedLine.index;
            baiduLogin.invalidate();baidu.setSession({});baiduLoginApplied=false;
            baiduLogin.start();playerFullscreen=true;
            status="已清除旧的百度登录状态，请扫描新二维码，成功后将自动重试线路";
        }else if(pendingHistoryResume){continueHistoryResume();}
        else if(autoTryingLines&&loadedLine.index+1<detail.shares.size()){
            autoTryLine=loadedLine.index+1;autoOpenFirstShare=true;autoOpenShareAt=SDL_GetTicks()+50;
        }else autoTryingLines=false;
    }
}
loginView=quarkLogin.snapshot();
if(loginView.authenticated&&!loginApplied){quark.setSession(loginView.session);loginApplied=true;}
baiduLoginView=baiduLogin.snapshot();
if(baiduLoginView.authenticated&&!baiduLoginApplied){
    baidu.setSession(baiduLoginView.session);baiduLoginApplied=true;
    status=pendingLoginFileId.empty()?"百度网盘登录成功":"百度网盘登录成功，正在继续播放...";
    if(playerFullscreen&&activeCloud==CloudKind::Baidu)playerFullscreen=false;
    if(pendingLoginLine<detail.shares.size()){
        const size_t line=pendingLoginLine;pendingLoginLine=SIZE_MAX;
        status="百度网盘登录成功，正在自动加载线路和剧集...";
        openDetailLine(line);
    }
}
auto detailFields=makeDetailFields();
SDL_Event event;
while(SDL_PollEvent(&event)){
    if(event.type==SDL_MOUSEBUTTONDOWN&&event.button.button==SDL_BUTTON_LEFT&&(view==View::Files||view==View::Shares)&&!summaryOpen&&!detailFieldOpen){
        int width=1280,height=720;SDL_GetWindowSize(w,&width,&height);
        int mx=width?event.button.x*1280/width:0,my=height?event.button.y*720/height:0;
        bool openedField=false;
        for(size_t i=0;i<detailFields.size();++i){const auto& field=detailFields[i];if(field.overflow&&mx>=field.button.x&&mx<field.button.x+field.button.w&&my>=field.button.y&&my<field.button.y+field.button.h){detailFieldTitle=field.title;detailFieldText=field.text;detailFieldScroll=0;detailFieldOpen=true;detailAction=3+(int)i;openedField=true;k=0;break;}}
        if(!openedField&&(playerFullscreen||(mx>=30&&mx<460&&my>=75&&my<300))){playerFullscreen=true;startCloudLogin(activeCloud==CloudKind::Baidu);k=0;}
    }
}
if(playerFullscreen){
    if(k&HidNpadButton_B){playerFullscreen=false;pendingLoginFileId.clear();pendingLoginLine=SIZE_MAX;}
    else if(k&HidNpadButton_A)startCloudLogin(activeCloud==CloudKind::Baidu);
    k=0;
}

if(historyDeletePrompt){
    if(k&HidNpadButton_A){
        const auto cards=historyCards();
        if(historyDeleteIndex<cards.size()){
            const std::string detailUrl=cards[historyDeleteIndex]->item.detailUrl;
            if(playbackHistory.removeSeries(detailUrl)){
                status="已删除观看记录";
                const size_t remaining=historyCards().size();
                if(selected>=remaining)selected=remaining?remaining-1:0;
            }else status="删除观看记录失败";
        }
        historyDeletePrompt=false;
    }else if(k&HidNpadButton_B)historyDeletePrompt=false;
    k=0;
}else if(view==View::History){
    if(k&HidNpadButton_A){historyAHeld=true;historyAHoldStarted=SDL_GetTicks();historyAHoldIndex=selected;k&=~HidNpadButton_A;}
    if(historyAHeld){
        if(!(heldButtons&HidNpadButton_A)){
            if(SDL_GetTicks()-historyAHoldStarted<650&&historyAHoldIndex<historyCards().size()){
                selected=historyAHoldIndex;historyOpenRequested=true;
            }
            historyAHeld=false;
        }else if(SDL_GetTicks()-historyAHoldStarted>=650){
            historyDeleteIndex=historyAHoldIndex;historyDeletePrompt=historyDeleteIndex<historyCards().size();historyAHeld=false;
        }
        k=0;
    }
}else historyAHeld=false;

if(view==View::Browse&&categories[category].kind==8&&miguScheduleFocus&&miguMatchFocus&&!miguClipFocus){
    if(!miguTeamPicker&&(k&HidNpadButton_Y)){
        miguTeams={{"","全部球队"}};
        auto addTeam=[&](const std::string& id,const std::string& name){if(!id.empty()&&std::none_of(miguTeams.begin(),miguTeams.end(),[&](const auto& t){return t.first==id;}))miguTeams.emplace_back(id,name);};
        for(const auto& match:miguSchedule){addTeam(match.homeId,match.home);addTeam(match.awayId,match.away);}
        miguTeamSelected=0;for(size_t i=0;i<miguTeams.size();++i)if(miguTeams[i].first==miguTeamId)miguTeamSelected=i;
        miguTeamPicker=true;k=0;
    }else if(miguTeamPicker){
        if((k&HidNpadButton_Up)&&miguTeamSelected>=2)miguTeamSelected-=2;
        if((k&HidNpadButton_Down)&&miguTeamSelected+2<miguTeams.size())miguTeamSelected+=2;
        if((k&HidNpadButton_Left)&&miguTeamSelected%2)--miguTeamSelected;
        if((k&HidNpadButton_Right)&&miguTeamSelected%2==0&&miguTeamSelected+1<miguTeams.size())++miguTeamSelected;
        if(k&HidNpadButton_A){miguTeamId=miguTeams[miguTeamSelected].first;miguTeamName=miguTeams[miguTeamSelected].second;miguMatches=filteredMiguSchedule();selected=0;miguTeamPicker=false;status="咪咕中超 · "+miguTeamName;}
        else if(k&(HidNpadButton_B|HidNpadButton_Y))miguTeamPicker=false;
        k=0;
    }
}
if(summaryOpen){
    auto lines=summaryLines(detail.description);size_t maxScroll=lines.size()>10?lines.size()-10:0;
    if((k&HidNpadButton_Down)&&summaryScroll<maxScroll)++summaryScroll;
    if((k&HidNpadButton_Up)&&summaryScroll)--summaryScroll;
    if(k&(HidNpadButton_B|HidNpadButton_A))summaryOpen=false;
    k=0;
}else if(view==View::Shares||view==View::Files){
    if(k&HidNpadButton_Y){summaryOpen=true;summaryScroll=0;k=0;}
    else if(k&HidNpadButton_X){toggleFavorite();k=0;}
    else if(detailAction>=0){
        if(detailAction<3){
        if(k&HidNpadButton_Left)detailAction=detailAction==1?0:2;
        if(k&HidNpadButton_Right)detailAction=detailAction==2?0:1;
        if(k&HidNpadButton_Up)for(int i=(int)detailFields.size()-1;i>=0;--i)if(detailFields[i].overflow){detailAction=3+i;break;}
        if(k&HidNpadButton_A){if(detailAction==2){playerFullscreen=true;startCloudLogin(activeCloud==CloudKind::Baidu);}else if(detailAction==0){summaryOpen=true;summaryScroll=0;}else toggleFavorite();}
        if(k&HidNpadButton_Down)detailAction=-1;
        }else{
            int fieldIndex=detailAction-3;
            if(k&HidNpadButton_Up)for(int i=fieldIndex-1;i>=0;--i)if(detailFields[i].overflow){detailAction=3+i;break;}
            if(k&HidNpadButton_Down){detailAction=0;for(size_t i=fieldIndex+1;i<detailFields.size();++i)if(detailFields[i].overflow){detailAction=3+(int)i;break;}}
            if((k&HidNpadButton_A)&&fieldIndex>=0&&fieldIndex<(int)detailFields.size()){detailFieldTitle=detailFields[fieldIndex].title;detailFieldText=detailFields[fieldIndex].text;detailFieldScroll=0;detailFieldOpen=true;}
        }
        // Keep shoulder-button events available for the line switch handler
        // even while focus is on Summary/Favorite/QR actions.
        k&=(HidNpadButton_B|HidNpadButton_L|HidNpadButton_R);
    }else if((k&HidNpadButton_Up)&&(selected<2||view==View::Shares)){detailAction=0;k=0;}
}

if(view==View::Browse&&categories[category].kind==9){
    if(!youtubeNavFocus&&(k&HidNpadButton_Left)&&(browseItems.empty()||selected%3==0)){
        youtubeNavFocus=true;k&=~HidNpadButton_Left;
    }
    if(youtubeNavFocus){
        if((k&HidNpadButton_Up)&&youtubeNavSelected>0)--youtubeNavSelected;
        if((k&HidNpadButton_Down)&&youtubeNavSelected<5)++youtubeNavSelected;
        if(k&(HidNpadButton_Right|HidNpadButton_B))youtubeNavFocus=false;
        if(k&HidNpadButton_A){
            if(youtubeNavSelected==0){openSearchHome();youtubeSearchMode=true;searchSuggestions.clear();status="YouTube · 输入视频名称";youtubeNavFocus=false;}
            else if(youtubeNavSelected==5){openSettings();loadBrowse();}
            else if(youtubeNavSelected==4){
                youtube.cancel();youtubeLoading=false;youtubePlaying=false;youtubeAccountHistory=false;youtubePage=YouTubePage::History;
                browseItems.clear();posters.clear();selected=0;
                for(const auto& watched:youtubeHistory.items()){
                    media::SearchResult card;card.title=watched.title;card.sourceName=watched.channel;
                    card.posterUrl=watched.thumbnail;card.detailUrl="yt:video:"+watched.id;
                    if(watched.durationSeconds>0){const int length=(int)watched.durationSeconds;
                        char value[32]{};if(length>=3600)std::snprintf(value,sizeof(value),"%d:%02d:%02d",length/3600,(length/60)%60,length%60);
                        else std::snprintf(value,sizeof(value),"%d:%02d",length/60,length%60);
                        card.duration=value;}
                    browseItems.push_back(std::move(card));
                }
                youtubeNavFocus=false;status="YouTube · 本机观看历史 · "+std::to_string(browseItems.size())+" 个视频";
                if(netReady&&youtube.signedIn()){youtube.accountHistory();youtubeLoading=true;status="YouTube · 正在尝试读取账号历史 · B 取消";}
            }
            else if(!netReady)status="YouTube · 网络尚未就绪";
            else if(youtubeNavSelected==2&&!youtube.signedIn())status="请在设置中登录 YouTube 后查看订阅";
            else{
                youtube.cancel();youtubeHomeWaitingForProxy=false;browseItems.clear();posters.clear();selected=0;youtubePlaying=false;
                youtubePage=youtubeNavSelected==2?YouTubePage::Subscriptions:YouTubePage::Explore;
                if(youtubeNavSelected==2)youtube.subscriptions();else if(youtubeNavSelected==3)youtube.search("热门视频");else requestYoutubeHome();
                if(youtubeNavSelected!=1){youtubeLoading=true;status="YouTube · 正在加载...";}youtubeNavFocus=false;
            }
        }
        k&=~(HidNpadButton_Left|HidNpadButton_Right|HidNpadButton_Up|HidNpadButton_Down|HidNpadButton_A|HidNpadButton_B);
    }
}

if(netReady&&(k&HidNpadButton_Y)&&view==View::Browse&&(category==0||categories[category].kind==9)){openSearchHome();if(categories[category].kind==9){youtubeSearchMode=true;searchSuggestions.clear();status="YouTube · 输入视频名称";}k=0;}
if(view==View::SearchHome&&youtubeSearchMode&&(k&HidNpadButton_ZR)){
    std::string query=searchInput;
    if(keyboard(query,"YouTube 搜索","输入中文或英文视频名称")&&!query.empty())runSearch(query);
    k=0;
}
if(netReady&&(k&HidNpadButton_X)&&view!=View::SearchHome){if(view==View::Browse&&categories[category].kind==9){
    if(youtubePage!=YouTubePage::Explore){youtubeNavSelected=1;youtubePage=YouTubePage::Explore;youtube.cancel();requestYoutubeHome();browseItems.clear();selected=0;}
    else if(!youtube.signedIn())status="YouTube · 在 − 设置中使用配对码登录后可看订阅";
    else{youtubeNavSelected=2;youtubePage=YouTubePage::Subscriptions;youtube.cancel();youtube.subscriptions();youtubeLoading=true;browseItems.clear();selected=0;status="YouTube · 正在加载订阅频道...";}
    k=0;
}else if(view==View::Browse&&categories[category].kind==8){if(!miguClipFocus&&!miguLiveFocus)status="体育 · 进入咪咕赛事列表后可按 X 扫码登录";}else if(view==View::Browse&&(categories[category].kind==4||categories[category].kind==5)){sortIndex=(sortIndex+1)%4;page=1;loadBrowse();}else{std::string u;if(keyboard(u,"Quark share link","Paste https://pan.quark.cn/s/..."))openShare(u);}}
if(view==View::Browse&&!youtubePlaying&&(k&HidNpadButton_L)){category=category?category-1:categories.size()-1;page=1;sortIndex=0;loadBrowse();}
if(view==View::Browse&&!youtubePlaying&&(k&HidNpadButton_R)){category=(category+1)%categories.size();page=1;sortIndex=0;loadBrowse();}
if(view==View::Browse&&categories[category].kind>0&&categories[category].kind<6&&(k&HidNpadButton_ZL)&&page>1){--page;loadBrowse();}
if(view==View::Browse&&categories[category].kind>0&&categories[category].kind<6&&(k&HidNpadButton_ZR)){++page;loadBrowse();}
// Keep source identity stable while ranking tabs by the number of results.
std::vector<size_t> sourceOrder,sourceResultCounts(sources.size(),0);
sourceOrder.reserve(sources.size());
for(size_t i=0;i<sources.size();++i)sourceOrder.push_back(i);
for(const auto& item:searchItems){
    for(size_t i=0;i<sources.size();++i)if(item.sourceName==sources[i].name){++sourceResultCounts[i];break;}
}
std::stable_sort(sourceOrder.begin(),sourceOrder.end(),[&](size_t a,size_t b){
    const auto rank=[&](size_t i){
        if(sources[i].api.find("GuaZi")!=std::string::npos)return 1;
        return sourceResultCounts[i]>0?0:2;
    };
    const int left=rank(a),right=rank(b);
    return left==right?sourceResultCounts[a]>sourceResultCounts[b]:left<right;
});
if(view==View::Search&&(k&(HidNpadButton_L|HidNpadButton_R))){
    int row=0;
    if(sourceFilter>=0){
        const auto found=std::find(sourceOrder.begin(),sourceOrder.end(),static_cast<size_t>(sourceFilter));
        if(found!=sourceOrder.end())row=static_cast<int>(found-sourceOrder.begin())+1;
    }
    const int n=static_cast<int>(sourceOrder.size())+1;
    row=(row+((k&HidNpadButton_R)?1:n-1))%n;
    sourceFilter=row?static_cast<int>(sourceOrder[row-1]):-1;selected=0;
}
if(!lineLoading&&(view==View::Shares||view==View::Files)&&!detail.shares.empty()&&(k&(HidNpadButton_L|HidNpadButton_R))){
    size_t next=(activeShare+((k&HidNpadButton_R)?1:detail.shares.size()-1))%detail.shares.size();
    autoTryingLines=false;
    openDetailLine(next);
}
std::vector<size_t> visibleSearch;
for(size_t i=0;i<searchItems.size();++i)
    if(sourceFilter<0||searchItems[i].sourceName==sources[sourceFilter].name)visibleSearch.push_back(i);
 if(view==View::Browse&&categories[category].kind==9&&youtubePage==YouTubePage::Explore
     &&youtubeNavSelected==1&&!youtubeQueuedCards.empty()
     &&(((k&HidNpadButton_Down)!=0&&selected+3>=browseItems.size())
         ||((k&HidNpadButton_Right)!=0&&selected+1>=browseItems.size()))){
     const size_t next=std::min<size_t>(6,youtubeQueuedCards.size());
     browseItems.insert(browseItems.end(),std::make_move_iterator(youtubeQueuedCards.begin()),
         std::make_move_iterator(youtubeQueuedCards.begin()+next));
     youtubeQueuedCards.erase(youtubeQueuedCards.begin(),youtubeQueuedCards.begin()+next);
 }
 size_t count=view==View::SearchHome?searchSuggestions.size():view==View::History?historyCards().size():view==View::Favorites?favorites.size():view==View::Browse?(categories[category].kind==8?(footballMatchFocus?(cctvChannelFocus?cctvChannels.size():(miguLiveFocus?miguLiveMatches.size():(miguClipFocus?miguClips.size():(miguMatchFocus?miguMatches.size()+(miguScheduleFocus?0:1):footballMatches.size())))):footballSources.size()):browseItems.size()):(view==View::Search?visibleSearch.size():(view==View::Shares?detail.shares.size():cloudItems.size()));if(view==View::Browse&&count){if(categories[category].kind==8){if((k&HidNpadButton_Down)&&selected+1<count)++selected;if((k&HidNpadButton_Up)&&selected)--selected;}else{if((k&HidNpadButton_Right)&&selected+1<count)++selected;if((k&HidNpadButton_Left)&&selected)--selected;const size_t columns=categories[category].kind==9?3:6;if((k&HidNpadButton_Down)&&selected+columns<count)selected+=columns;if((k&HidNpadButton_Up)&&selected>=columns)selected-=columns;}}else if(view==View::SearchHome){
const std::string rows[]={"1234567890","QWERTYUIOP","ASDFGHJKL","ZXCVBNM"};const int rowLengths[]={10,10,9,7,3};
auto refreshSuggestions=[&](){searchSuggestions.clear();if(youtubeSearchMode)return;auto add=[&](const std::string& title){if(title.empty()||std::find(searchSuggestions.begin(),searchSuggestions.end(),title)!=searchSuggestions.end())return;if(searchInput.empty()||searchScore(title,searchInput)>0)searchSuggestions.push_back(title);};for(const auto& title:searchHistory)add(title);for(const auto& title:hotSearches)add(title);for(const auto& item:homeRecommendations)add(item.title);if(searchSuggestions.size()>10)searchSuggestions.resize(10);if(suggestionSelected>=searchSuggestions.size())suggestionSelected=searchSuggestions.empty()?0:searchSuggestions.size()-1;};
bool changed=false;
if(suggestionFocus){if((k&HidNpadButton_Up)&&suggestionSelected)--suggestionSelected;if((k&HidNpadButton_Down)&&suggestionSelected+1<searchSuggestions.size())++suggestionSelected;if(k&HidNpadButton_Left)suggestionFocus=false;if((k&HidNpadButton_A)&&suggestionSelected<searchSuggestions.size()){std::string keyword=searchSuggestions[suggestionSelected];runSearch(keyword);k=0;}}
else{if((k&HidNpadButton_Up)&&keyboardRow>0){--keyboardRow;keyboardCol=std::min(keyboardCol,rowLengths[keyboardRow]-1);}if((k&HidNpadButton_Down)&&keyboardRow<4){++keyboardRow;keyboardCol=std::min(keyboardCol,rowLengths[keyboardRow]-1);}if((k&HidNpadButton_Left)&&keyboardCol>0)--keyboardCol;if(k&HidNpadButton_Right){if(keyboardCol+1<rowLengths[keyboardRow])++keyboardCol;else if(!searchSuggestions.empty())suggestionFocus=true;}if(k&HidNpadButton_X){if(!searchInput.empty()){searchInput.pop_back();changed=true;}k=0;}if(k&HidNpadButton_A){if(keyboardRow<4){searchInput.push_back(rows[keyboardRow][keyboardCol]);changed=true;}else if(keyboardCol==0){if(!searchInput.empty()){searchInput.pop_back();changed=true;}}else if(keyboardCol==1){searchInput.clear();changed=true;}else if(!searchInput.empty()){std::string keyword=searchInput;runSearch(keyword);}k=0;}}
if(changed){refreshSuggestions();if(searchInput.empty()){lastSuggestionQuery.clear();suggestionDue=0;suggestionLoading=false;}else suggestionDue=SDL_GetTicks()+250;}
}else if((view==View::Search||view==View::Files||view==View::History||view==View::Favorites)&&count){
int columns=view==View::Search?5:(view==View::Files?2:6);
if((k&HidNpadButton_Right)&&selected+1<count)++selected;
if((k&HidNpadButton_Left)&&selected)--selected;
if((k&HidNpadButton_Down)&&selected+columns<count)selected+=columns;
if((k&HidNpadButton_Up)&&selected>=(size_t)columns)selected-=columns;
}else{if(count&&(k&HidNpadButton_Down))selected=std::min(selected+1,count-1);if(count&&(k&HidNpadButton_Up)&&selected)--selected;}
if(view==View::Browse&&categories[category].kind==8&&miguMatchFocus&&miguScheduleFocus&&!miguClipFocus&&!miguSchedule.empty()&&!miguScheduleLoader.busy()&&SDL_TICKS_PASSED(SDL_GetTicks(),miguScheduleRetryAt)){
    if((k&HidNpadButton_Up)&&selected<3&&!miguNoEarlier){if(miguScheduleLoader.start(miguSchedule.front().day,"up"))status="咪咕中超 · 正在加载更早比赛...";}
    else if((k&HidNpadButton_Down)&&selected+3>=miguMatches.size()&&!miguNoLater){if(miguScheduleLoader.start(miguSchedule.back().day,"down"))status="咪咕中超 · 正在加载更晚比赛...";}
}
if(resourceLoading&&(k&HidNpadButton_B)){
    detailLoader.cancel();resourceLoading=false;pendingHistoryResume.reset();status="已取消加载影片详情";k=0;
}
// The first A press has already queued the selected item.  Freeze navigation
// until that exact request finishes so further button presses cannot make the
// old result grid look interactive or change the apparent target.
if(resourceLoading)k=0;
bool resumeLoginPlayback=false;
if(!pendingLoginFileId.empty()){
    if((k&HidNpadButton_B)||view!=View::Files||activeCloud!=pendingLoginCloud
        ||session.shareId!=pendingLoginShareId){pendingLoginFileId.clear();}
    else if(activeCloud==CloudKind::Baidu?baiduLoginApplied:loginApplied){
        for(size_t i=0;i<cloudItems.size();++i){
            if(cloudItems[i].id==pendingLoginFileId&&cloudItems[i].type==cloud::EntryType::File){
                selected=i;resumeLoginPlayback=true;playerFullscreen=false;detailAction=-1;
                status="登录成功，正在继续播放...";break;
            }
        }
        pendingLoginFileId.clear();
    }
}
if((k&HidNpadButton_A)&&view==View::Browse&&categories[category].kind==8&&!footballMatchFocus&&selected<footballSources.size()){
    footballSourceSelected=selected;
    cctvChannelFocus=false;miguLiveFocus=false;liveError.clear();
    if(footballSources[selected]=="电视频道"){cctvChannelFocus=true;footballMatchFocus=true;selected=0;status="电视频道 · 选择频道后按 A 播放";}
    else if(footballSources[selected]=="咪咕精选赛事"){
        status="咪咕 · 正在加载精选足球赛事...";SDL_SetRenderDrawColor(r,18,24,38,255);SDL_RenderClear(r);text(r,f,"OKSwitch | "+status,40,22);SDL_RenderPresent(r);
        auto loaded=migu.featuredFootball();
        if(!loaded.succeeded)status="咪咕 · "+loaded.error;
        else{miguLiveMatches=std::move(loaded.items);miguLiveFocus=true;miguMatchFocus=false;miguClipFocus=false;footballMatchFocus=true;selected=0;status="咪咕精选赛事 · 直播中可按 A 播放";}
    }
    else if(footballSources[selected]=="咪咕中超"){
        if(miguScheduleLoader.start()){
            miguScheduleFocus=false;miguNoEarlier=false;miguNoLater=false;miguMatches.clear();miguOverview.clear();miguSchedule.clear();
            miguMatchFocus=true;miguClipFocus=false;footballMatchFocus=true;selected=0;
            status="咪咕中超 · 正在加载近期赛程...";
        }else status="咪咕中超 · 上次请求正在结束，请稍后按 A 重试";
    }
    else if(footballSources[selected].find("飞球")==std::string::npos){status=sourceDisplayLabel(footballSources[selected],true)+" · 协议待接入";}
    else{
        status="飞球体育 · 正在加载足球赛事...";SDL_SetRenderDrawColor(r,18,24,38,255);SDL_RenderClear(r);text(r,f,"OKSwitch | "+status,40,22);SDL_RenderPresent(r);
        auto loaded=football.schedule();
        if(!loaded.succeeded)status="飞球体育 · "+loaded.error;
        else{footballMatches=std::move(loaded.matches);footballMatchFocus=true;selected=0;status="飞球体育 · 已加载 "+std::to_string(footballMatches.size())+" 场，A 播放 · B 返回线路";}
    }
}else if((k&HidNpadButton_A)&&view==View::Browse&&categories[category].kind==8&&footballMatchFocus&&miguMatchFocus&&!miguClipFocus){
    if(!miguScheduleFocus&&selected==miguMatches.size()&&!miguScheduleLoader.busy()){
        miguScheduleFocus=true;miguMatches=filteredMiguSchedule();selected=0;
        auto it=std::find_if(miguMatches.begin(),miguMatches.end(),[](const auto& m){return m.status!=2;});
        if(it!=miguMatches.end())selected=(size_t)(it-miguMatches.begin());
        status="咪咕中超 · 全部赛程 · 上下滚动加载更早／更晚比赛";
    }else if(selected<miguMatches.size()){
        const auto match=miguMatches[selected];miguMatchSelected=selected;miguScheduleLoader.cancel();
        if(match.status!=2){
            media::MiguLiveMatch live;
            live.id=match.liveId;live.title=match.home+" VS "+match.away;
            live.live=true;live.kickoffTime=match.kickoffTime;
            if(live.id.empty()){
                status="咪咕中超 · 正在查找赛前直播间...";
                SDL_SetRenderDrawColor(r,18,24,38,255);SDL_RenderClear(r);text(r,f,"OKSwitch | "+status,40,22);SDL_RenderPresent(r);
                auto featured=migu.featuredFootball();
                if(featured.succeeded){
                    auto found=std::find_if(featured.items.begin(),featured.items.end(),[&](const auto& item){
                        return item.title.find(match.home)!=std::string::npos&&item.title.find(match.away)!=std::string::npos;
                    });
                    if(found!=featured.items.end())live=*found;
                }
            }
            if(live.id.empty())status=match.home+" VS "+match.away+" · 将于 "+match.date+" 开赛，直播间尚未开放";
            else if(!live.live)status=live.title+" · 直播间将于 "+live.startTime.substr(8,2)+":"+live.startTime.substr(10,2)+" 开放";
            else playMiguLive(live);
        }else{
            if(miguClipsLoader.start(match)){miguClipsLoading=true;status="咪咕 · 正在加载赛事视频 · B 立即返回";}
            else status="咪咕 · 上次请求正在结束，请稍后按 A 重试";
        }
    }
}else if((k&HidNpadButton_X)&&view==View::Browse&&categories[category].kind==8&&footballMatchFocus&&(miguMatchFocus||miguLiveFocus)){
    status=miguSignIn()?"咪咕 · 登录成功，可选择视频播放":"咪咕 · 登录未完成";
}else if((k&HidNpadButton_A)&&view==View::Browse&&categories[category].kind==8&&footballMatchFocus&&miguLiveFocus&&selected<miguLiveMatches.size()){
    playMiguLive(miguLiveMatches[selected]);
}else if((k&HidNpadButton_A)&&view==View::Browse&&categories[category].kind==8&&footballMatchFocus&&miguClipFocus&&selected<miguClips.size()){
    pendingMiguClip=miguClips[selected];
    if(miguPlaybackLoader.start(pendingMiguClip)){miguPlaybackLoading=true;liveError.clear();status="咪咕 · 正在获取 "+pendingMiguClip.title+" · B 立即返回";}
    else status="咪咕 · 上次请求正在结束，请稍后按 A 重试";
}else if((k&HidNpadButton_A)&&view==View::Browse&&categories[category].kind==8&&footballMatchFocus&&!miguMatchFocus&&selected<(cctvChannelFocus?cctvChannels.size():footballMatches.size())){
    std::string liveName;
    cloud::ProviderResult<cloud::PlaybackSource> stream;
    if(cctvChannelFocus){liveName=cctvChannels[selected].name;status="正在获取 "+liveName+" 的直播地址...";}
    else{const auto match=footballMatches[selected];liveName=match.home+" VS "+match.away;status="正在获取 "+liveName+" 的直播地址...";}
    SDL_SetRenderDrawColor(r,18,24,38,255);SDL_RenderClear(r);text(r,f,"OKSwitch | "+status,40,22);SDL_RenderPresent(r);
    stream=cctvChannelFocus?cctv.playback(cctvChannels[selected].id):football.playback(footballMatches[selected]);
    if(!stream.succeeded){liveError=stream.error.message;status=liveName+" · "+liveError;}
    else{
        liveError.clear();
        auto showPlayerStatus=[&](const std::string& message){SDL_SetRenderDrawColor(r,8,13,22,255);SDL_RenderClear(r);text(r,f,message,30,24,1220,true);SDL_RenderPresent(r);};
        media::PlaybackOptions options;options.measureText=[&](const std::string& value){return textWidth(f,value);};options.openSettings=openSettings;
        options.liveQualityLabels={"当前线路固定画质"};
        auto played=media::VideoPlayer::play(r,stream.value,options,showPlayerStatus,[&](const std::string& value,int x,int y,int width){text(r,f,value,x,y,width,true);});
        status=played.error.empty()?"已退出足球直播":played.error;
    }
}else if(!resourceLoading&&(k&HidNpadButton_A)&&view==View::Browse&&selected<browseItems.size()&&browseItems[selected].detailUrl=="okswitch:history"){
    rememberPage();view=View::History;selected=0;status="最近观看";
}else if(!resourceLoading&&(k&HidNpadButton_A)&&view==View::Browse&&selected<browseItems.size()&&browseItems[selected].detailUrl=="okswitch:favorites"){
    rememberPage();view=View::Favorites;selected=0;status="收藏";
}else if(historyOpenRequested&&view==View::History&&selected<historyCards().size()){
    pendingHistoryResume=*historyCards()[selected];openResource(pendingHistoryResume->item);
}else if((k&HidNpadButton_A)&&view==View::Favorites&&selected<favorites.size()){
    openResource(favorites[selected]);
}
else if((k&HidNpadButton_A)&&view==View::Browse&&categories[category].kind==9&&!youtubeLoading&&selected<browseItems.size()){
    const auto item=browseItems[selected];
    if(item.detailUrl.rfind("yt:channel:",0)==0){youtubeChannelId=item.detailUrl.substr(11);youtubeChannelName=item.title;youtubePage=YouTubePage::Channel;
        youtube.channelVideos(youtubeChannelId);youtubeLoading=true;browseItems.clear();selected=0;status="YouTube · 正在加载 "+youtubeChannelName;}
    else if(item.detailUrl.rfind("yt:video:",0)==0){youtubePendingItem=item;youtubePendingTitle=item.title;youtubePlaybackRefreshes=0;youtubeResumeOverride=-1;
        // Give the proxy to playback immediately, not after resolving the URL.
        posters.clear();youtube.playback(item.detailUrl.substr(9));youtubeLoading=true;youtubePlaying=true;status="YouTube · 正在获取播放地址...";}
}
else if((k&HidNpadButton_A)&&view==View::Browse&&categories[category].kind!=9&&!browseItems.empty())runSearch(browseItems[selected].title);
else if((k&HidNpadButton_A)&&view==View::Search&&selected<visibleSearch.size()){
    openResource(searchItems[visibleSearch[selected]]);
}
else if(!lineLoading&&(k&HidNpadButton_A)&&view==View::Shares&&!detail.shares.empty()){
    autoTryingLines=false;openDetailLine(activeShare);
}
else if((k&HidNpadButton_A)&&view==View::Files&&activeCloud!=CloudKind::Guazi&&!cloudItems.empty()&&cloudItems[selected].type==cloud::EntryType::Directory){const std::string directory=activeCloud==CloudKind::Baidu?cloudItems[selected].shareToken:cloudItems[selected].id;auto l=activeCloud==CloudKind::Baidu?baidu.listFiles(session,directory):quark.listFiles(session,directory);if(l.succeeded){rememberPage();dirs.push_back(directory);cloudItems=l.value;sortCloudEntries(cloudItems);selected=0;status="Loaded "+std::to_string(cloudItems.size())+(activeCloud==CloudKind::Baidu?" Baidu items":" Quark items");}else status=l.error.message;}
else if(((k&HidNpadButton_A)||resumeLoginPlayback||autoPlayEpisode)&&view==View::Files&&selected<cloudItems.size()){
    autoPlayEpisode=false;
    const bool authenticated=activeCloud==CloudKind::Guazi?true:(activeCloud==CloudKind::Baidu?baiduLoginApplied:loginApplied);
    const bool baiduStatusInvalid=activeCloud==CloudKind::Baidu&&(
        status.find("登录状态无效")!=std::string::npos
        ||status.find("重新扫码")!=std::string::npos
        ||status.find("会话不完整")!=std::string::npos
        ||status.find("权限")!=std::string::npos);
    if(!authenticated||baiduStatusInvalid){rememberLoginPlayback();playerFullscreen=true;startCloudLogin(baiduStatusInvalid);status=activeCloud==CloudKind::Baidu?"请扫描新的百度网盘登录二维码":"请先扫码登录夸克";}
    else{
        auto showPlayerStatus=[&](const std::string& message){
            SDL_SetRenderDrawColor(r,8,13,22,255);SDL_RenderClear(r);
            text(r,f,message,30,24,1220,true);SDL_RenderPresent(r);
        };
        const std::string requestedFileId=cloudItems[selected].id;
        bool sourceReady=!playbackSource.url.empty()&&playbackSourceFileId==requestedFileId&&playbackCloud==activeCloud;
        if(!sourceReady){
            if(!playbackSource.url.empty()){
                showPlayerStatus("正在清理上一集的临时转存文件...");
                std::string ignored;cleanupPlayback(ignored);
                playbackSource={};playbackSourceFileId.clear();
            }
            status=activeCloud==CloudKind::Guazi?"正在解析秒播地址...":(activeCloud==CloudKind::Baidu?"正在解析百度播放地址...":"正在转存并解析播放地址...");showPlayerStatus(status);
            cloud::ProviderResult<cloud::PlaybackSource> source;
            bool cancelled=false;
            if(activeCloud==CloudKind::Guazi){source=mediaSource.guaziPlayback(cloudItems[selected].playbackHint);}
            else if(!baiduPlaybackLoader.request(session,cloudItems[selected],activeCloud==CloudKind::Baidu))status="上一次播放解析正在结束，请稍后重试";
            else{
                const Uint32 resolveStarted=SDL_GetTicks();
                while(!baiduPlaybackLoader.poll(source)){
                    padUpdate(&pad);const u64 waitKeys=padGetButtonsDown(&pad);
                    if(waitKeys&HidNpadButton_B){baiduPlaybackLoader.cancel();cancelled=true;status="已取消播放地址解析";break;}
                    const std::string stage=activeCloud==CloudKind::Baidu?"正在解析百度播放地址":"正在转存并解析播放地址";
                    showPlayerStatus(stage+" · "+std::to_string((SDL_GetTicks()-resolveStarted)/1000)+" 秒 · B 取消");
                    SDL_Delay(16);
                }
            }
            if(!cancelled&&source.succeeded){playbackSource=std::move(source.value);playbackSourceFileId=requestedFileId;playbackCloud=activeCloud;sourceReady=true;}
            else if(!cancelled&&!source.error.message.empty()){
                status=source.error.message;
                const bool baiduCredentialFailure=activeCloud==CloudKind::Baidu&&(
                    source.error.code==cloud::ProviderErrorCode::AuthenticationRequired
                    ||source.error.message.find("登录状态")!=std::string::npos
                    ||source.error.message.find("重新扫码")!=std::string::npos
                    ||source.error.message.find("会话不完整")!=std::string::npos
                    ||source.error.message.find("权限")!=std::string::npos);
                if(baiduCredentialFailure&&!resumeLoginPlayback){
                    rememberLoginPlayback();playerFullscreen=true;startCloudLogin(true);
                    status="百度登录或权限已失效，请扫描新的登录二维码";
                }
            }
            else if(!cancelled&&status.rfind("上一次",0)!=0)status="播放地址解析失败";
        }
        if(sourceReady){
            playerFullscreen=false;
            playbackSource.parallelFetches=appletMode?1:(dockedMode?3:2);
            media::PlaybackOptions playOptions;playOptions.measureText=[&](const std::string& value){return textWidth(f,value);};playOptions.openSettings=openSettings;size_t episodeOrdinal=0;for(size_t i=0;i<=selected&&i<cloudItems.size();++i)if(cloudItems[i].type==cloud::EntryType::File)++episodeOrdinal;playOptions.displayTitle=detailItem.title+" · "+episodeLabel(cloudItems[selected].name,episodeOrdinal);playOptions.canPrevious=std::any_of(cloudItems.begin(),cloudItems.begin()+selected,[](const auto& item){return item.type==cloud::EntryType::File;});playOptions.canNext=std::any_of(cloudItems.begin()+std::min(selected+1,cloudItems.size()),cloudItems.end(),[](const auto& item){return item.type==cloud::EntryType::File;});
            const std::string shareUrl=activeShare<detail.shares.size()?detail.shares[activeShare].url:"";
            if(const auto* preferences=playbackHistory.findPreferences(detailItem.title,detailItem.detailUrl)){playOptions.introSkipSeconds=preferences->introSkipSeconds;playOptions.outroSkipSeconds=preferences->outroSkipSeconds;playOptions.volumePercent=preferences->volumePercent;}
            else if(const auto* series=playbackHistory.findSeries(detailItem.detailUrl)){playOptions.introSkipSeconds=series->introSkipSeconds;playOptions.outroSkipSeconds=series->outroSkipSeconds;playOptions.volumePercent=series->volumePercent;}
            const int episodeNumber=episodeNumberFromName(cloudItems[selected].name);
            if(const auto* prior=playbackHistory.find(detailItem.detailUrl,shareUrl,cloudItems[selected].id))playOptions.resumeSeconds=prior->positionSeconds;
            else if(const auto* prior=playbackHistory.findTitleEpisode(detailItem.title,episodeNumber,(int)episodeOrdinal))playOptions.resumeSeconds=prior->positionSeconds;
            if(pendingHistoryResume){if(auto index=resumeEpisodeIndex(*pendingHistoryResume);index&&*index==selected)playOptions.resumeSeconds=pendingHistoryResume->positionSeconds;}
            bool preferencesWriteFailed=false;
            playOptions.onPreferencesChanged=[&](int intro,int outro,int volume){media::SeriesPlaybackPreferences preferences;preferences.title=detailItem.title;preferences.detailUrl=detailItem.detailUrl;preferences.introSkipSeconds=intro;preferences.outroSkipSeconds=outro;preferences.volumePercent=volume;preferences.updatedAt=std::time(nullptr);if(!playbackHistory.recordPreferences(preferences))preferencesWriteFailed=true;};
            auto played=media::VideoPlayer::play(r,playbackSource,playOptions,showPlayerStatus,
                [&](const std::string& value,int x,int y,int width){text(r,f,value,x,y,width,true);});
            bool playbackPreferencesSaved=!preferencesWriteFailed;
            if(played.opened){pendingHistoryResume.reset();media::SeriesPlaybackPreferences preferences;preferences.title=detailItem.title;preferences.detailUrl=detailItem.detailUrl;preferences.introSkipSeconds=played.introSkipSeconds;preferences.outroSkipSeconds=played.outroSkipSeconds;preferences.volumePercent=played.volumePercent;preferences.updatedAt=std::time(nullptr);playbackPreferencesSaved=playbackHistory.recordPreferences(preferences)&&playbackPreferencesSaved;}
            if(played.positionSeconds>0){
                media::PlaybackRecord record;record.item=detailItem;
                if(!detail.posterUrl.empty())record.item.posterUrl=detail.posterUrl;
                if(activeShare<detail.shares.size())record.shareUrl=detail.shares[activeShare].url;
                record.fileId=cloudItems[selected].id;record.fileName=cloudItems[selected].name;record.episodeNumber=episodeNumber;record.episodeOrdinal=(int)episodeOrdinal;
                record.positionSeconds=played.positionSeconds;record.durationSeconds=played.durationSeconds;
                record.introSkipSeconds=played.introSkipSeconds;record.outroSkipSeconds=played.outroSkipSeconds;record.volumePercent=played.volumePercent;
                record.updatedAt=std::time(nullptr);playbackHistory.recordProgress(record);
            }
            if(!played.error.empty())status=played.error;
            else status=played.completed?"播放完成":"已退出播放";
            if(played.completed||!played.error.empty()||played.episodeDelta!=0){
                showPlayerStatus("正在清理本次临时转存文件...");
                std::string cleanupError;bool cleaned=cleanupPlayback(cleanupError);
                playbackSource={};playbackSourceFileId.clear();
                // When opening the stream already failed, keep that useful
                // playback error visible. Cleanup is best effort in this path.
                if(!cleaned&&played.error.empty())status+=" · 临时文件清理失败："+cleanupError;
            }else status+="，再次打开本集可直接继续";
            const int episodeDelta=played.episodeDelta!=0?played.episodeDelta:(autoPlayNext&&played.completed&&played.error.empty()?1:0);
            if(episodeDelta!=0){long long next=(long long)selected;do{next+=episodeDelta;}while(next>=0&&next<(long long)cloudItems.size()&&cloudItems[(size_t)next].type!=cloud::EntryType::File);if(next>=0&&next<(long long)cloudItems.size()){selected=(size_t)next;autoPlayEpisode=true;status=episodeDelta<0?"正在播放上一集...":"正在播放下一集...";}else if(played.completed&&autoPlayNext&&episodeDelta>0){status="播放完成 · 已经是最后一集";lastEpisodeNoticeUntil=SDL_GetTicks()+2500;}}
            if(!playbackPreferencesSaved)status+=" · 剧集设置保存失败，请检查 SD 卡";
        }
    }
}
if((k&HidNpadButton_B)&&view==View::Browse&&categories[category].kind==8&&footballMatchFocus){
    if(miguClipFocus){miguClipFocus=false;selected=miguMatchSelected;status="咪咕中超 · 选择比赛查看录播和集锦";}
    else if(miguScheduleFocus){miguScheduleLoader.cancel();miguScheduleFocus=false;miguMatches=miguOverview;selected=miguMatches.size();status="咪咕中超 · 近期赛程";}
    else{miguScheduleLoader.cancel();footballMatchFocus=false;miguMatchFocus=false;miguLiveFocus=false;cctvChannelFocus=false;liveError.clear();selected=footballSourceSelected;status="足球赛事 · 选择线路后按 A 加载";}
    k=0;
}
if(lineLoading&&(k&HidNpadButton_B)){cloudLineLoader.cancel();lineLoading=false;autoOpenFirstShare=false;autoTryingLines=false;status="已取消线路加载";k=0;}
if((k&HidNpadButton_B)&&view==View::Browse&&categories[category].kind==9){
    if(youtubeLoading){youtube.cancel();youtubeLoading=false;youtubePlaying=false;youtubeHomeWaitingForProxy=false;status="YouTube · 已取消加载";}
    else if(youtubePage==YouTubePage::History){youtubeNavSelected=1;youtubePage=YouTubePage::Explore;requestYoutubeHome();browseItems.clear();selected=0;}
    else if(youtubePage==YouTubePage::Channel){youtubePage=YouTubePage::Subscriptions;youtube.subscriptions();youtubeLoading=true;browseItems.clear();selected=0;status="YouTube · 正在返回订阅频道...";}
    else if(youtubePage==YouTubePage::Subscriptions){youtubeNavSelected=1;youtubePage=YouTubePage::Explore;requestYoutubeHome();browseItems.clear();selected=0;}
    k=0;
}
if(k&HidNpadButton_B){autoOpenFirstShare=false;autoTryingLines=false;if(view==View::SearchHome)youtubeSearchMode=false;if(view==View::Search&&searchPending){searchLoader.cancel();searchPending=false;}backPage();}
if(autoOpenFirstShare&&view==View::Shares&&SDL_TICKS_PASSED(SDL_GetTicks(),autoOpenShareAt)){
    if(autoTryLine<detail.shares.size())openDetailLine(autoTryLine);
    else{autoOpenFirstShare=false;autoTryingLines=false;status="所有 "+std::to_string(detail.shares.size())+" 条网盘线路均已失效或不可用";}
}
visibleSearch.clear();for(size_t i=0;i<searchItems.size();++i)if(sourceFilter<0||searchItems[i].sourceName==sources[sourceFilter].name)visibleSearch.push_back(i);
SDL_SetRenderDrawColor(r,18,24,38,255);SDL_RenderClear(r);std::string posterError=(view==View::Browse&&selected<browseItems.size()&&browseItems[selected].detailUrl!="okswitch:history")?posters.error(browseItems[selected].posterUrl):"";text(r,f,"OKSwitch | "+status+(posterError.empty()?"":" | Poster: "+posterError),40,22);size_t first=selected>9?selected-9:0;
if(view==View::Browse&&categories[category].kind==9){
    const std::string imageProxy=youtube.currentProxy();
    static std::string previousImageProxy;
    if(imageProxy!=previousImageProxy){posters.clear();previousImageProxy=imageProxy;}
    static std::string scrollingTitle;
    static Uint64 titleSelectedAt=0;
    const std::string currentTitle=selected<browseItems.size()?browseItems[selected].title:"";
    if(currentTitle!=scrollingTitle){scrollingTitle=currentTitle;titleSelectedAt=SDL_GetTicks64();}
    SDL_SetRenderDrawColor(r,10,10,12,255);SDL_RenderClear(r);
    SDL_Rect rail{0,0,238,720};SDL_SetRenderDrawColor(r,25,25,28,255);SDL_RenderFillRect(r,&rail);
    if(youtubeLogo){SDL_Rect brand{28,36,179,40};SDL_RenderCopy(r,youtubeLogo,nullptr,&brand);}
    const char* navigation[]={"搜索","首页","订阅频道","热门视频","观看历史","设置"};
    for(int n=0;n<6;++n){
        const int y=145+n*62;
        if(youtubeNavSelected==n){SDL_Rect active{16,y-12,206,53};SDL_SetRenderDrawColor(r,youtubeNavFocus?175:55,youtubeNavFocus?35:55,youtubeNavFocus?35:60,255);SDL_RenderFillRect(r,&active);}
        text(r,f,navigation[n],28,y,192,false);
    }
    posters.beginFrame(youtubePlaying?0:3);
    const auto accountAvatar=youtube.signedIn()?youtube.profileImage():std::string();
    auto* avatar=accountAvatar.empty()?nullptr:posters.get(accountAvatar,imageProxy);
    if(avatar){
        constexpr int segments=64;constexpr float pi=3.14159265358979323846f;
        SDL_Vertex circle[1+segments*2];int triangles[segments*9];
        const float cx=1240.0f,cy=55.0f,radius=22.0f;
        circle[0]={{cx,cy},{255,255,255,255},{0.5f,0.5f}};
        for(int i=0;i<segments;++i){
            const float angle=2.0f*pi*i/segments;
            const float dx=std::cos(angle),dy=std::sin(angle);
            circle[1+i]={{cx+(radius-1)*dx,cy+(radius-1)*dy},{255,255,255,255},{0.5f+0.5f*dx*(radius-1)/radius,0.5f+0.5f*dy*(radius-1)/radius}};
            circle[1+segments+i]={{cx+(radius+1)*dx,cy+(radius+1)*dy},{255,255,255,0},{0.5f+0.5f*dx,0.5f+0.5f*dy}};
            const int next=(i+1)%segments;
            triangles[i*9]=0;triangles[i*9+1]=1+i;triangles[i*9+2]=1+next;
            triangles[i*9+3]=1+i;triangles[i*9+4]=1+segments+i;triangles[i*9+5]=1+segments+next;
            triangles[i*9+6]=1+i;triangles[i*9+7]=1+segments+next;triangles[i*9+8]=1+next;
        }
        SDL_SetTextureScaleMode(avatar,SDL_ScaleModeLinear);
        SDL_RenderGeometry(r,avatar,circle,1+segments*2,triangles,segments*9);
    }
    const std::string heading=youtubePage==YouTubePage::Channel?youtubeChannelName:youtubePage==YouTubePage::Subscriptions?"订阅频道":youtubePage==YouTubePage::History?(youtubeAccountHistory?"账号观看历史":"本机观看历史"):youtubeNavSelected==3?"热门视频":youtubeSubscriptionHome?"订阅频道":youtubeAccountHome?"账号推荐":youtube.signedIn()?"账号推荐":"发现视频";
    text(r,f,heading,270,42,680,true);
    text(r,f,"L/R 切页 · − 设置",950,42,255,true);
    std::string youtubeDisplayStatus=status;
    if(youtube.signedIn()&&!avatar){
        const auto avatarError=accountAvatar.empty()?youtube.profileStatus():posters.error(accountAvatar);
        if(!avatarError.empty())youtubeDisplayStatus+=" · 头像："+avatarError;
    }
    text(r,f,youtubeDisplayStatus,270,83,975,true);
    const size_t start=(selected/6)*6;
    for(size_t i=start;i<std::min(start+6,browseItems.size());++i){
        const int slot=(int)(i-start),x=270+(slot%3)*325,y=125+(slot/3)*272;
        const bool focused=i==selected&&!youtubeNavFocus;
        SDL_Rect card{x,y,305,259};SDL_SetRenderDrawColor(r,focused?65:24,focused?65:24,focused?70:27,255);SDL_RenderFillRect(r,&card);
        if(focused){SDL_SetRenderDrawColor(r,235,235,240,255);SDL_RenderDrawRect(r,&card);}
        SDL_Rect cover{x+3,y+3,299,168};
        SDL_SetRenderDrawColor(r,15,15,18,255);SDL_RenderFillRect(r,&cover);
        if(auto* poster=posters.get(browseItems[i].posterUrl,imageProxy)){
            SDL_SetTextureScaleMode(poster,SDL_ScaleModeLinear);int w=0,h=0;SDL_QueryTexture(poster,nullptr,nullptr,&w,&h);
            SDL_Rect dst=cover;
            if(browseItems[i].detailUrl.rfind("yt:channel:",0)==0){dst.w=128;dst.h=128;dst.x+=(cover.w-dst.w)/2;dst.y+=(cover.h-dst.h)/2;}
            else if(w>0&&h>0){const float scale=std::min((float)cover.w/w,(float)cover.h/h);dst.w=(int)(w*scale);dst.h=(int)(h*scale);dst.x+=(cover.w-dst.w)/2;dst.y+=(cover.h-dst.h)/2;}
            SDL_RenderCopy(r,poster,nullptr,&dst);
        }
        if(!browseItems[i].duration.empty()&&browseItems[i].detailUrl.rfind("yt:video:",0)==0){
            const auto& duration=browseItems[i].duration;
            const int width=std::min(110,textWidth(f,duration)+14);
            SDL_Rect badge{cover.x+cover.w-width-7,cover.y+cover.h-35,width,29};
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(r,0,0,0,210);SDL_RenderFillRect(r,&badge);
            text(r,f,duration,badge.x+7,badge.y+2,width-12,true);
        }
        const auto& title=browseItems[i].title;size_t split=0;
        while(split<title.size()){
            size_t next=split+1;while(next<title.size()&&((unsigned char)title[next]&0xc0)==0x80)++next;
            if(textWidth(f,title.substr(0,next))>285)break;split=next;
        }
        text(r,f,title.substr(0,split),x+10,y+179,285,false);
        const std::string remainder=title.substr(split);const int titleWidth=textWidth(f,remainder);int offset=0;
        if(focused&&titleWidth>285){const Uint64 elapsed=SDL_GetTicks64()-titleSelectedAt;const Uint64 travel=(Uint64)(titleWidth-285)*1000/35;const Uint64 phase=elapsed%(travel+2800);if(phase>1400)offset=(int)std::min<Uint64>(titleWidth-285,(phase-1400)*35/1000);}
        SDL_Rect titleClip{x+10,y+205,285,TTF_FontHeight(f)};SDL_RenderSetClipRect(r,&titleClip);
        text(r,f,remainder,x+10-offset,y+205,std::max(285,titleWidth),false);SDL_RenderSetClipRect(r,nullptr);
        SDL_Rect channelClip{x+10,y+233,285,TTF_FontHeight(f)};SDL_RenderSetClipRect(r,&channelClip);
        text(r,f,browseItems[i].sourceName,x+10,y+233,std::max(285,textWidth(f,browseItems[i].sourceName)),false);SDL_RenderSetClipRect(r,nullptr);
    }
    if(youtubeLoading)text(r,f,"正在加载 · B 取消",620,685,500,true);
    else if(browseItems.empty())text(r,f,"暂无内容 · Y 搜索 · 左键打开导航",365,350,750,true);
    else text(r,f,std::to_string(selected+1)+" / "+std::to_string(browseItems.size())+"   方向键选择 · A 打开 · Y 搜索 · X 订阅",270,685,975,true);
}else if(view==View::Browse){std::string tabs="L/R  ["+std::string(categories[category].name)+"]";if(categories[category].kind==4||categories[category].kind==5)tabs+="  X Sort: "+std::string(sortNames[sortIndex]);text(r,f,tabs+((categories[category].kind>0&&categories[category].kind<6)?"  Page "+std::to_string(page):""),40,62);if(categories[category].kind==8){
    if(miguScheduleFocus&&miguMatchFocus&&!miguClipFocus){
        posters.beginFrame(2);
        text(r,f,"咪咕中超 · "+(miguTeamId.empty()?std::string("全部赛程"):miguTeamName),40,108,550,true);
        const std::string scheduleHelp="上下滚动 · Y 球队 · A 打开 · X 登录 · B 返回";
        const int helpWidth=std::min(680,textWidth(f,scheduleHelp));text(r,f,scheduleHelp,1235-helpWidth,108,helpWidth,true);
        const size_t start=selected>4?std::min(selected-4,miguMatches.size()>9?miguMatches.size()-9:0):0;
        for(size_t i=start;i<std::min(start+9,miguMatches.size());++i){
            const auto& match=miguMatches[i];SDL_Rect row{35,148+(int)(i-start)*52,1200,46};
            SDL_SetRenderDrawColor(r,i==selected?55:28,i==selected?150:46,i==selected?125:68,255);SDL_RenderFillRect(r,&row);
            const int teamY=row.y+(row.h-TTF_FontHeight(f))/2;
            text(r,f,match.date,row.x+12,teamY,180,true);
            // Equal slots around the centre: name / badge / score / badge / name.
            const int centre=row.x+585;
            const int homeRight=centre-80,awayLeft=centre+80;
            text(r,f,match.home,homeRight-std::min(280,textWidth(f,match.home)),teamY,280,true);
            text(r,f,match.score,centre-textWidth(f,match.score)/2,teamY,80,true);
            text(r,f,match.away,awayLeft,teamY,280,true);
            auto badge=[&](const std::string& url,int x){if(url.empty())return;if(auto* logo=posters.get(url)){SDL_SetTextureScaleMode(logo,SDL_ScaleModeLinear);int w=0,h=0;SDL_QueryTexture(logo,nullptr,nullptr,&w,&h);if(w>0&&h>0){const float scale=36.0f/std::max(w,h);SDL_Rect dst{x+(36-(int)(w*scale))/2,row.y+(row.h-(int)(h*scale))/2,(int)(w*scale),(int)(h*scale)};SDL_RenderCopy(r,logo,nullptr,&dst);}}};
            badge(match.homeLogo,centre-68);badge(match.awayLogo,centre+32);
            text(r,f,match.status==2?"集锦／回放":match.status==1?"直播中":"未开赛",row.x+990,teamY,195,true);
        }
        if(miguMatches.empty())text(r,f,"当前日期范围没有该球队比赛，上／下加载更多，Y 更换球队",55,190,1150,true);
        text(r,f,std::to_string(miguMatches.empty()?0:selected+1)+" / "+std::to_string(miguMatches.size())+
            (miguScheduleLoader.busy()?" · 加载赛程中…":" · 滚动到边缘加载相邻日期"),40,628,1150,true);
    }else{
    text(r,f,"体育线路",40,112,330,true);text(r,f,footballMatchFocus?(cctvChannelFocus?"电视频道 · A 播放":(miguLiveFocus?"咪咕精选足球 · A 播放 · X 扫码登录":(miguMatchFocus?(miguClipFocus?"咪咕视频 · A 播放 · X 扫码登录":"咪咕中超 · A 打开比赛 · X 登录"):"飞球赛事 · A 播放"))):"足球赛事与体育频道直播",410,112,800,true);
    for(size_t i=0;i<footballSources.size();++i){const size_t activeSource=footballMatchFocus?footballSourceSelected:selected;SDL_Rect row{35,155+(int)i*72,315,56};SDL_SetRenderDrawColor(r,i==activeSource?55:28,i==activeSource?180:46,i==activeSource?150:68,255);SDL_RenderFillRect(r,&row);text(r,f,sourceDisplayLabel(footballSources[i],true),row.x+20,row.y+18,row.w-40,true);}
    SDL_Rect panel{400,155,835,430};SDL_SetRenderDrawColor(r,23,34,50,255);SDL_RenderFillRect(r,&panel);
    if(!footballMatchFocus){text(r,f,"选择左侧线路并按 A 打开",445,220,730,true);text(r,f,"咪咕精选直播、中超回放与电视频道",445,290,730,true);}
    else if(cctvChannelFocus){for(size_t i=0;i<cctvChannels.size();++i){SDL_Rect row{420,170+(int)i*70,795,56};SDL_SetRenderDrawColor(r,i==selected?55:31,i==selected?150:49,i==selected?125:66,255);SDL_RenderFillRect(r,&row);text(r,f,"◎ "+cctvChannels[i].name,row.x+18,row.y+16,650,true);text(r,f,"公共直播",row.x+665,row.y+16,110,true);}if(!liveError.empty())text(r,f,liveError,435,465,770,true);}
    else if(miguLiveFocus){
        if(miguLiveMatches.empty())text(r,f,"当前精选赛事没有直播或即将开始的足球比赛",435,220,770,true);
        const size_t matchFirst=selected>6?selected-6:0;
        for(size_t i=matchFirst;i<std::min(matchFirst+7,miguLiveMatches.size());++i){
            const auto& match=miguLiveMatches[i];SDL_Rect row{420,170+(int)(i-matchFirst)*57,795,49};
            SDL_SetRenderDrawColor(r,i==selected?55:31,i==selected?150:49,i==selected?125:66,255);SDL_RenderFillRect(r,&row);
            text(r,f,std::string(match.live?"[直播] ":"[预约] ")+match.title,row.x+14,row.y+7,610,true);
            const std::string when=match.kickoffTime.size()>=12?match.kickoffTime.substr(4,2)+"/"+match.kickoffTime.substr(6,2)+" "+match.kickoffTime.substr(8,2)+":"+match.kickoffTime.substr(10,2):"";
            text(r,f,when,row.x+625,row.y+7,155,true);
        }
        if(!liveError.empty())text(r,f,liveError,435,590,770,true);
    }
    else if(miguMatchFocus){
        posters.beginFrame(2);
        if(miguClipFocus){
            if(miguClips.empty())text(r,f,"该场赛事暂无回放或集锦",445,220,730,true);
            const size_t clipFirst=selected>6?selected-6:0;for(size_t i=clipFirst;i<std::min(clipFirst+7,miguClips.size());++i){const auto& clip=miguClips[i];SDL_Rect row{420,170+(int)(i-clipFirst)*57,795,49};SDL_SetRenderDrawColor(r,i==selected?55:31,i==selected?150:49,i==selected?125:66,255);SDL_RenderFillRect(r,&row);text(r,f,clip.title,row.x+14,row.y+7,630,true);text(r,f,clip.duration,row.x+670,row.y+7,100,true);}
        }else{
            const size_t matchFirst=selected>6?selected-6:0;
            for(size_t i=matchFirst;i<std::min(matchFirst+7,miguMatches.size()+1);++i){
                SDL_Rect row{420,170+(int)(i-matchFirst)*57,795,49};SDL_SetRenderDrawColor(r,i==selected?55:31,i==selected?150:49,i==selected?125:66,255);SDL_RenderFillRect(r,&row);
                if(i==miguMatches.size()){text(r,f,miguScheduleLoader.busy()?"正在加载赛程…":"全部赛程  >",row.x+14,row.y+13,760,true);continue;}
                const auto& match=miguMatches[i];
                text(r,f,match.date,row.x+14,row.y+5,160,true);
                const int centre=row.x+480;
                const int teamY=row.y+(row.h-TTF_FontHeight(f))/2;
                text(r,f,match.home,centre-80-std::min(210,textWidth(f,match.home)),teamY,210,true);
                text(r,f,match.score,centre-textWidth(f,match.score)/2,teamY,65,true);
                text(r,f,match.away,centre+80,teamY,210,true);
                auto badge=[&](const std::string& url,int x){if(url.empty())return;if(auto* logo=posters.get(url)){SDL_SetTextureScaleMode(logo,SDL_ScaleModeLinear);int w=0,h=0;SDL_QueryTexture(logo,nullptr,nullptr,&w,&h);if(w>0&&h>0){float scale=36.0f/std::max(w,h);SDL_Rect dst{x+(36-(int)(w*scale))/2,row.y+(row.h-(int)(h*scale))/2,(int)(w*scale),(int)(h*scale)};SDL_RenderCopy(r,logo,nullptr,&dst);}}};
                badge(match.homeLogo,centre-68);badge(match.awayLogo,centre+32);
                text(r,f,(match.status==2?"集锦／回放  ":match.status==1?"直播中  ":"未开赛")+(match.status==0?std::string():match.score),row.x+14,row.y+28,760,true);
            }
        }
        if(!liveError.empty())text(r,f,liveError,435,590,770,true);
    }
    else if(footballMatches.empty())text(r,f,"当前没有可用足球赛事",445,220,730,true);
    else{const size_t matchFirst=selected>6?selected-6:0;for(size_t i=matchFirst;i<std::min(matchFirst+7,footballMatches.size());++i){const auto& match=footballMatches[i];SDL_Rect row{420,170+(int)(i-matchFirst)*57,795,49};SDL_SetRenderDrawColor(r,i==selected?55:31,i==selected?150:49,i==selected?125:66,255);SDL_RenderFillRect(r,&row);const std::string prefix=match.liveStatus?"[直播] ":(match.time.empty()?"":match.time+"  ");text(r,f,prefix+match.home+" VS "+match.away,row.x+14,row.y+7,610,true);text(r,f,match.league,row.x+635,row.y+7,140,true);}}}}else{posters.beginFrame(2);first=(selected/12)*12;for(size_t i=first;i<std::min(first+12,browseItems.size());++i){int slot=(int)(i-first),col=slot%6,row=slot/6,x=35+col*205,y=105+row*265;SDL_Rect card{x,y,180,245};SDL_SetRenderDrawColor(r,i==selected?55:28,i==selected?180:46,i==selected?150:68,255);SDL_RenderFillRect(r,&card);SDL_Texture*p=posters.get(browseItems[i].posterUrl);SDL_Rect cover{x+4,y+4,172,190};if(browseItems[i].detailUrl=="okswitch:history"){text(r,f,"最近观看",x+35,y+85,130,true);text(r,f,std::to_string(historyCards().size())+" 部剧集",x+40,y+115,120,true);}else if(browseItems[i].detailUrl=="okswitch:favorites"){text(r,f,"收藏",x+55,y+80,120,true);text(r,f,std::to_string(favorites.size())+" 部剧集",x+40,y+120,130,true);}else if(p)SDL_RenderCopy(r,p,nullptr,&cover);else{SDL_SetRenderDrawColor(r,45,57,76,255);SDL_RenderFillRect(r,&cover);}text(r,f,browseItems[i].title,x+6,y+198,168,true);if(category==0&&isFavorite(browseItems[i].detailUrl)){SDL_Rect badge{x+4,y+4,62,24};SDL_SetRenderDrawColor(r,26,100,80,255);SDL_RenderFillRect(r,&badge);text(r,f,"收藏",x+10,y+6,50);}char meta[40]{};if(browseItems[i].rating>0)std::snprintf(meta,sizeof(meta),"评分 %.1f",browseItems[i].rating);if(meta[0])text(r,f,meta,x+6,y+221,168);}}}
else if(view==View::SearchHome){
    text(r,f,youtubeSearchMode?"搜索 YouTube 视频 · ZR 可输入中文":"搜索影片与资源",30,62,730);SDL_Rect input{30,94,735,55};SDL_SetRenderDrawColor(r,31,63,82,255);SDL_RenderFillRect(r,&input);text(r,f,searchInput.empty()?"输入拼音或英文…":searchInput,50,111,690,true);
    const std::string rows[]={"1234567890","QWERTYUIOP","ASDFGHJKL","ZXCVBNM"};const int lengths[]={10,10,9,7};
    for(int row=0;row<4;++row)for(int col=0;col<lengths[row];++col){const int indent=row==2?28:(row==3?82:0);SDL_Rect key{30+indent+col*70,175+row*72,60,54};const bool focus=!suggestionFocus&&keyboardRow==row&&keyboardCol==col;SDL_SetRenderDrawColor(r,focus?55:35,focus?180:74,focus?150:94,255);SDL_RenderFillRect(r,&key);text(r,f,std::string(1,rows[row][col]),key.x+21,key.y+16,30,true);}
    const std::string actions[]={"退格","清空","搜索"};const int widths[]={160,160,270};int actionX=52;for(int col=0;col<3;++col){SDL_Rect key{actionX,475,widths[col],58};const bool focus=!suggestionFocus&&keyboardRow==4&&keyboardCol==col;SDL_SetRenderDrawColor(r,focus?55:35,focus?180:74,focus?150:94,255);SDL_RenderFillRect(r,&key);text(r,f,actions[col],key.x+20,key.y+18,key.w-40,true);actionX+=widths[col]+22;}
    SDL_SetRenderDrawColor(r,42,52,68,255);SDL_Rect divider{790,75,2,550};SDL_RenderFillRect(r,&divider);text(r,f,searchInput.empty()?"历史与热门":"可能的名称",825,76,390,true);
    if(suggestionLoading)text(r,f,"正在联想…",825,125,390,true);else if(searchSuggestions.empty())text(r,f,searchInput.empty()?"暂无候选":"继续输入或直接搜索",825,125,390,true);
    for(size_t i=0;i<searchSuggestions.size();++i){SDL_Rect item{815,112+(int)i*49,425,41};const bool focus=suggestionFocus&&i==suggestionSelected;SDL_SetRenderDrawColor(r,focus?55:28,focus?180:53,focus?150:72,255);SDL_RenderFillRect(r,&item);text(r,f,searchSuggestions[i],item.x+15,item.y+11,item.w-30,true);}
}
else if(view==View::Search){
    // Start visible covers while providers are still returning results. Four
    // bounded workers keep the grid responsive without decoding on the UI thread.
    posters.beginFrame(10);
    constexpr int visibleSourceRows=9;
    const int sourceRows=(int)sources.size()+1;
    int selectedSourceRow=0;
    if(sourceFilter>=0){
        const auto found=std::find(sourceOrder.begin(),sourceOrder.end(),static_cast<size_t>(sourceFilter));
        if(found!=sourceOrder.end())selectedSourceRow=static_cast<int>(found-sourceOrder.begin())+1;
    }
    const int maxSourceFirst=std::max(0,sourceRows-visibleSourceRows);
    int sourceFirst=std::max(0,selectedSourceRow-visibleSourceRows+1);
    sourceFirst=std::min(sourceFirst,maxSourceFirst);
    const int sourceLast=std::min(sourceRows,sourceFirst+visibleSourceRows);
    auto sourceLogo=[&](const std::string& name)->SDL_Texture*{
        if(name.find("💓")!=std::string::npos)return heartEmoji;
        if(name.find("💥")!=std::string::npos)return boomEmoji;
        if(name.find("🎇")!=std::string::npos)return sparkleEmoji;
        if(name.find("🍉")!=std::string::npos)return melonEmoji;
        if(name.find("秒播")!=std::string::npos)return boomEmoji;
        if(name.find("4K")!=std::string::npos||name.find("剧透")!=std::string::npos)return heartEmoji;
        return nullptr;
    };
    text(r,f,"选择来源  L / R  · "+std::string(sourceConfigOnline?"在线配置 ":"内置配置 ")+std::to_string(sources.size())+(configuredSiteCount?" / "+std::to_string(configuredSiteCount):"")+" · 显示 "+std::to_string(sourceFirst+1)+"-"+std::to_string(sourceLast)+" / "+std::to_string(sourceRows),30,70,900,true);
    for(int row=sourceFirst;row<sourceLast;++row){
        const int i=row?static_cast<int>(sourceOrder[row-1]):-1;
        SDL_Rect tab{25,110+(row-sourceFirst)*58,180,44};
        SDL_SetRenderDrawColor(r,i==sourceFilter?45:28,i==sourceFilter?150:46,i==sourceFilter?120:68,255);SDL_RenderFillRect(r,&tab);
        SDL_Texture* logo=i<0?nullptr:sourceLogo(sources[i].name);
        std::string label=i<0?"全部来源":sourceDisplayLabel(sources[i].name);
        const int labelX=logo?tab.x+33:40;
        const int labelWidth=logo?112:150;
        if(logo){SDL_Rect left{tab.x+5,tab.y+9,25,25};SDL_RenderCopy(r,logo,nullptr,&left);}
        text(r,f,label,labelX,tab.y+12,labelWidth,true);
        if(logo){SDL_Rect right{labelX+std::min(labelWidth,textWidth(f,label))+4,tab.y+9,25,25};SDL_RenderCopy(r,logo,nullptr,&right);}
    }
    first=(selected/10)*10;
    for(size_t i=first;i<std::min(first+10,visibleSearch.size());++i){
        const auto& item=searchItems[visibleSearch[i]];
        int slot=(int)(i-first),x=230+(slot%5)*203,y=110+(slot/5)*257;
        SDL_Rect card{x,y,188,245};SDL_SetRenderDrawColor(r,i==selected?55:28,i==selected?180:46,i==selected?150:68,255);SDL_RenderFillRect(r,&card);
        SDL_Texture* image=posters.get(item.posterUrl);SDL_Rect cover{x+4,y+4,180,177};
        if(image)SDL_RenderCopy(r,image,nullptr,&cover);
        else{text(r,f,"海报加载中",x+18,y+90,152,true);}
        SDL_Rect badge{x+4,y+4,180,25};SDL_SetRenderDrawColor(r,26,65,62,255);SDL_RenderFillRect(r,&badge);
        SDL_Texture* logo=sourceLogo(item.sourceName);
        const std::string sourceLabel=sourceDisplayLabel(item.sourceName);
        const int labelX=x+(logo?32:9),labelWidth=logo?118:170;
        if(logo){SDL_Rect left{x+7,y+6,22,22};SDL_RenderCopy(r,logo,nullptr,&left);}
        text(r,f,sourceLabel,labelX,y+6,labelWidth,true);
        if(logo){SDL_Rect right{labelX+std::min(labelWidth,textWidth(f,sourceLabel))+4,y+6,22,22};SDL_RenderCopy(r,logo,nullptr,&right);}
        text(r,f,item.title,x+6,y+186,176,true);
        text(r,f,item.status,x+6,y+216,176,true);
    }
    if(visibleSearch.empty()){
        std::string reason=searchPending?"该来源正在搜索...":"该来源暂无搜索结果";
        if(sourceFilter>=0){
            const auto found=std::find_if(searchSourceStatus.begin(),searchSourceStatus.end(),[&](const auto& entry){return entry.first==sources[sourceFilter].name;});
            if(found!=searchSourceStatus.end())reason=found->second;
        }
        text(r,f,reason,245,120,850,true);
    }
}
else if(view==View::History||view==View::Favorites){
    const bool watching=view==View::History;
    const auto recent=historyCards();
    const size_t total=watching?recent.size():favorites.size();
    text(r,f,watching?"最近观看":"收藏",35,68,400,true);
    if(watching&&!historyReadable)text(r,f,"历史文件读取失败，原文件已保留",40,130);
    else if(!watching&&!favoritesReadable)text(r,f,"收藏文件读取失败，原文件已保留",40,130);
    else if(!total)text(r,f,watching?"暂无播放历史":"暂无收藏，打开资源详情后按 X 收藏",40,135);
    else{
        posters.beginFrame(4);
        first=(selected/12)*12;
        for(size_t i=first;i<std::min(first+12,total);++i){
            const auto* record=watching?recent[i]:nullptr;
            const auto& item=watching?record->item:favorites[i];
            const int slot=(int)(i-first),x=35+(slot%6)*205,y=105+(slot/6)*265;
            SDL_Rect card{x,y,180,245};SDL_SetRenderDrawColor(r,i==selected?55:28,i==selected?180:46,i==selected?150:68,255);SDL_RenderFillRect(r,&card);
            SDL_Rect cover{x+4,y+4,172,190};
            if(auto* poster=posters.get(item.posterUrl))SDL_RenderCopy(r,poster,nullptr,&cover);
            else{SDL_SetRenderDrawColor(r,45,57,76,255);SDL_RenderFillRect(r,&cover);text(r,f,"暂无海报",x+40,y+85,100,true);}
            text(r,f,item.title,x+6,y+198,168,true);
            const std::string meta=watching?item.sourceName+" · "+episodeLabel(record->fileName,(size_t)std::max(0,record->episodeOrdinal)):item.sourceName;
            text(r,f,meta,x+6,y+221,168,true);
        }
        text(r,f,watching?"A 打开剧集 · 长按 A 删除这部剧的历史 · B 返回":"A 打开资源 · B 返回",40,615,1200,true);
    }
}
else{
    posters.beginFrame(2);
    if(lineLoading){SDL_SetRenderDrawColor(r,8,13,22,255);SDL_Rect loadingPanel{30,75,430,225};SDL_RenderFillRect(r,&loadingPanel);text(r,f,"正在切换并加载线路...",50,260,390,true);}
    else drawLogin({30,75,430,225},false);
    text(r,f,detail.title.empty()?"夸克分享":detail.title,485,80,740,true);
    for(size_t i=0;i<detailFields.size();++i){
        const auto& field=detailFields[i];
        text(r,f,field.text,485,field.y,field.overflow?645:740,false);
        if(field.overflow){SDL_SetRenderDrawColor(r,detailAction==3+(int)i?55:28,detailAction==3+(int)i?180:65,detailAction==3+(int)i?150:80,255);SDL_RenderFillRect(r,&field.button);text(r,f,"展开",field.button.x+18,field.button.y+4,55,false);}
    }
    // Wrap the synopsis on UTF-8 character boundaries.
    std::string line;size_t pos=0;
    for(int row=0;row<1&&pos<detail.description.size();++row){
        line.clear();for(int n=0;n<36&&pos<detail.description.size();++n){
            unsigned char c=detail.description[pos];size_t len=c<128?1:(c<224?2:(c<240?3:4));
            len=std::min(len,detail.description.size()-pos);line+=detail.description.substr(pos,len);pos+=len;
        }
        text(r,f,"简介："+line,485,232+row*27,740,false);
    }
    for(int action=0;action<2;++action){
        SDL_Rect button{485+action*220,270,205,34};
        SDL_SetRenderDrawColor(r,detailAction==action?55:28,detailAction==action?180:65,detailAction==action?150:80,255);SDL_RenderFillRect(r,&button);
        text(r,f,action==0?"Summary / 简介  Y":(isFavorite(detailItem.detailUrl)?"已收藏 / 取消  X":"收藏  X"),button.x+10,button.y+7,185,true);
    }
    size_t lineFirst=(activeShare/5)*5;
    for(size_t i=lineFirst;i<std::min(lineFirst+5,detail.shares.size());++i){
        int x=30+(int)(i-lineFirst)*244;SDL_Rect tab{x,320,230,40};
        SDL_SetRenderDrawColor(r,i==activeShare?45:28,i==activeShare?150:46,i==activeShare?120:68,255);SDL_RenderFillRect(r,&tab);
        text(r,f,detail.shares[i].label,x+12,330,206,true);
    }
    text(r,f,"L/R 自动加载线路    A 打开文件夹 / 选择剧集    B 返回",30,373);
    if(view==View::Shares)text(r,f,detail.shares.empty()?"暂无可用线路":"线路未加载，按 A 重试",30,420);
    else{
        first=(selected/8)*8;
        for(size_t i=first;i<std::min(first+8,cloudItems.size());++i){
            int slot=(int)(i-first),x=30+(slot%2)*615,y=412+(slot/2)*47;
            SDL_Rect row{x,y,595,39};SDL_SetRenderDrawColor(r,i==selected?55:28,i==selected?180:46,i==selected&&detailAction<0?150:68,255);SDL_RenderFillRect(r,&row);
            text(r,f,std::string(cloudItems[i].type==cloud::EntryType::Directory?"[文件夹] ":"")+cloudItems[i].name,x+10,y+10,575,true);
        }
        if(cloudItems.empty())text(r,f,"此目录没有文件",30,420);
    }
}

if(!(view==View::Browse&&categories[category].kind==9))text(r,f,view==View::Browse?(categories[category].kind==8?"L/R 分类 · 上下选择足球线路 · A 打开":categories[category].kind==9?"L/R 分类 · Y 搜索 · X 游客/订阅 · A 打开 · B 返回 · − 设置":"L/R 分类  ZL/ZR 翻页  X 排序/链接  Y 搜索  A 打开  B 返回"):view==View::SearchHome?"方向键选择 · A 输入/搜索 · X 退格 · 右键进入候选 · B 返回":"方向键 选择  L/R 来源或线路  A 确认  B 逐级返回",40,670);
if(playerFullscreen){SDL_SetRenderDrawColor(r,8,13,22,255);SDL_RenderClear(r);drawLogin({300,35,680,640},true);}
if(detailFieldOpen){
    auto lines=summaryLines(detailFieldText);size_t maxScroll=lines.size()>10?lines.size()-10:0;
    if((k&HidNpadButton_Down)&&detailFieldScroll<maxScroll)++detailFieldScroll;
    if((k&HidNpadButton_Up)&&detailFieldScroll)--detailFieldScroll;
    if(k&(HidNpadButton_B|HidNpadButton_A))detailFieldOpen=false;
    k=0;
}else if(summaryOpen){
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);SDL_SetRenderDrawColor(r,0,0,0,190);SDL_Rect dim{0,0,1280,720};SDL_RenderFillRect(r,&dim);
    SDL_SetRenderDrawColor(r,28,42,59,255);SDL_Rect dialog{110,110,1060,480};SDL_RenderFillRect(r,&dialog);
    text(r,f,"简介 · "+detail.title,140,135,990,true);
    auto lines=summaryLines(detail.description);
    for(size_t i=summaryScroll;i<std::min(summaryScroll+10,lines.size());++i)text(r,f,lines[i],140,185+(int)(i-summaryScroll)*31,990,true);
    text(r,f,"↑ / ↓ 滚动    A / B 关闭    "+std::to_string(summaryScroll+1)+" / "+std::to_string(lines.size()),140,550,990);
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_NONE);
}
if(detailFieldOpen){
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);SDL_SetRenderDrawColor(r,0,0,0,190);SDL_Rect dim{0,0,1280,720};SDL_RenderFillRect(r,&dim);
    SDL_SetRenderDrawColor(r,28,42,59,255);SDL_Rect dialog{110,110,1060,480};SDL_RenderFillRect(r,&dialog);
    text(r,f,detailFieldTitle+" · "+detail.title,140,135,990,false);
    auto lines=summaryLines(detailFieldText);
    for(size_t i=detailFieldScroll;i<std::min(detailFieldScroll+10,lines.size());++i)text(r,f,lines[i],140,185+(int)(i-detailFieldScroll)*31,990,false);
    text(r,f,"↑ / ↓ 滚动    A / B 关闭    "+std::to_string(detailFieldScroll+1)+" / "+std::to_string(lines.size()),140,550,990,false);
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_NONE);
}
if(resourceLoading){
    SDL_SetRenderDrawColor(r,18,24,38,255);SDL_RenderClear(r);
    text(r,f,"正在加载影片详情",40,35,1200,true);
    text(r,f,"已接收 A 键，请稍候…",40,90,1200,true);
    unsigned dots=(SDL_GetTicks()/350)%4;
    text(r,f,"正在连接影片来源"+std::string(dots,'.'),40,145,1200,true);
}
if(historyDeletePrompt&&historyDeleteIndex<historyCards().size()){
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);SDL_SetRenderDrawColor(r,0,0,0,190);SDL_Rect dim{0,0,1280,720};SDL_RenderFillRect(r,&dim);
    SDL_SetRenderDrawColor(r,28,42,59,255);SDL_Rect dialog{350,245,580,230};SDL_RenderFillRect(r,&dialog);
    const auto& record=*historyCards()[historyDeleteIndex];
    text(r,f,"删除这部剧的观看记录？",390,285,500,true);
    text(r,f,record.item.title,390,335,500,true);
    SDL_SetRenderDrawColor(r,55,180,150,255);SDL_Rect confirm{390,395,220,48};SDL_RenderFillRect(r,&confirm);
    SDL_SetRenderDrawColor(r,46,68,88,255);SDL_Rect cancel{670,395,220,48};SDL_RenderFillRect(r,&cancel);
    text(r,f,"A 删除",455,408,130,true);text(r,f,"B 取消",735,408,130,true);
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_NONE);
}
if(miguTeamPicker){
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);SDL_SetRenderDrawColor(r,0,0,0,190);SDL_Rect shade{0,0,1280,720};SDL_RenderFillRect(r,&shade);
    SDL_SetRenderDrawColor(r,23,34,50,255);SDL_Rect box{170,70,940,575};SDL_RenderFillRect(r,&box);
    text(r,f,"选择球队 · A 确定 · B 取消",205,95,850,true);
    const size_t firstTeam=(miguTeamSelected/18)*18;
    for(size_t i=firstTeam;i<std::min(firstTeam+18,miguTeams.size());++i){const int slot=(int)(i-firstTeam);SDL_Rect row{195+(slot%2)*445,142+(slot/2)*52,425,43};SDL_SetRenderDrawColor(r,i==miguTeamSelected?55:28,i==miguTeamSelected?150:46,i==miguTeamSelected?125:68,255);SDL_RenderFillRect(r,&row);text(r,f,miguTeams[i].second,row.x+16,row.y+12,row.w-32,true);}
}
if(view==View::Browse&&category==0&&!resourceLoading)text(r,f,"− 设置",1130,62,110,true);
if(lastEpisodeNoticeUntil&&!SDL_TICKS_PASSED(SDL_GetTicks(),lastEpisodeNoticeUntil)){
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r,8,13,22,225);
    SDL_Rect messageBox{430,304,420,112};SDL_RenderFillRect(r,&messageBox);
    const std::string message="已经是最后一集";
    text(r,f,message,(1280-textWidth(f,message))/2,345,400,true);
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_NONE);
}
SDL_RenderPresent(r);}
if(subscriptionBoot.thread)SDL_WaitThread(subscriptionBoot.thread,nullptr);
youtube.shutdown();youtubeTunnel.stop();miguPlaybackLoader.shutdown();miguClipsLoader.shutdown();miguScheduleLoader.shutdown();
if(!playbackSource.url.empty()){std::string ignored;cleanupPlayback(ignored);}
baiduPlaybackLoader.shutdown();cloudLineLoader.shutdown();baiduLogin.shutdown();quarkLogin.shutdown();
detailLoader.shutdown();searchLoader.shutdown();sourceConfigLoader.shutdown();hotSearchLoader.shutdown();catalogLoader.shutdown();posters.shutdown();http.shutdown();
if(youtubeLogo)SDL_DestroyTexture(youtubeLogo);
if(heartEmoji)SDL_DestroyTexture(heartEmoji);
if(boomEmoji)SDL_DestroyTexture(boomEmoji);
if(sparkleEmoji)SDL_DestroyTexture(sparkleEmoji);
if(melonEmoji)SDL_DestroyTexture(melonEmoji);
SDL_DestroyRenderer(r);SDL_DestroyWindow(w);
for(TTF_Font* font:fallbackFonts)TTF_CloseFont(font);
fallbackFonts.clear();TTF_CloseFont(f);plExit();TTF_Quit();
if(netReady){avformat_network_deinit();curl_global_cleanup();socketExit();}
if(romfsReady)romfsExit();SDL_Quit();return 0;}



