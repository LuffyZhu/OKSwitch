#include "okswitch/cloud/CloudLineLoader.hpp"

#include <algorithm>

namespace okswitch::cloud {
CloudLineLoader::CloudLineLoader(QuarkProvider& quark,BaiduProvider& baidu)
    :quark_(quark),baidu_(baidu),mutex_(SDL_CreateMutex()){}
CloudLineLoader::~CloudLineLoader(){shutdown();}
bool CloudLineLoader::request(const std::string& url,size_t index){
    if(!mutex_)return false;
    SDL_LockMutex(mutex_);const bool busy=running_;SDL_UnlockMutex(mutex_);
    if(busy)return false;
    if(thread_){SDL_WaitThread(thread_,nullptr);thread_=nullptr;}
    quark_.resetBrowseCancel();baidu_.resetBrowseCancel();
    SDL_LockMutex(mutex_);url_=url;index_=index;result_={};cancelled_=false;ready_=false;running_=true;SDL_UnlockMutex(mutex_);
    thread_=SDL_CreateThread(entry,"cloud-line",this);
    if(!thread_){SDL_LockMutex(mutex_);running_=false;SDL_UnlockMutex(mutex_);return false;}
    return true;
}
bool CloudLineLoader::poll(CloudLineResult& result){
    if(!mutex_)return false;SDL_LockMutex(mutex_);const bool ready=ready_;
    if(ready){result=std::move(result_);ready_=false;}SDL_UnlockMutex(mutex_);
    if(ready&&thread_){SDL_WaitThread(thread_,nullptr);thread_=nullptr;}return ready;
}
void CloudLineLoader::cancel(){if(!mutex_)return;SDL_LockMutex(mutex_);cancelled_=true;SDL_UnlockMutex(mutex_);quark_.cancelBrowse();baidu_.cancelBrowse();}
bool CloudLineLoader::running(){if(!mutex_)return false;SDL_LockMutex(mutex_);bool value=running_;SDL_UnlockMutex(mutex_);return value;}
void CloudLineLoader::shutdown(){cancel();if(thread_){SDL_WaitThread(thread_,nullptr);thread_=nullptr;}if(mutex_){SDL_DestroyMutex(mutex_);mutex_=nullptr;}}
int CloudLineLoader::entry(void* self){return static_cast<CloudLineLoader*>(self)->run();}
int CloudLineLoader::run(){
    CloudLineResult output;output.index=index_;ShareReference reference{url_,""};
    CloudDriveProvider* provider=nullptr;
    if(baidu_.canHandle(reference)){output.baidu=true;provider=&baidu_;}
    else provider=&quark_;
    auto resolved=provider->resolveShare(reference);
    if(!resolved.succeeded)output.error=resolved.error.message;
    else{
        auto listed=provider->listFiles(resolved.value,resolved.value.rootId);
        if(!listed.succeeded)output.error=listed.error.message;
        else{
            output.session=std::move(resolved.value);output.items=std::move(listed.value);output.directories={output.session.rootId};
            for(int depth=0;depth<8&&output.items.size()==1&&output.items[0].type==EntryType::Directory;++depth){
                SDL_LockMutex(mutex_);bool stop=cancelled_;SDL_UnlockMutex(mutex_);if(stop)break;
                const std::string id=output.baidu?output.items[0].shareToken:output.items[0].id;
                if(std::find(output.directories.begin(),output.directories.end(),id)!=output.directories.end())break;
                auto child=provider->listFiles(output.session,id);if(!child.succeeded){output.error=child.error.message;break;}
                output.directories.push_back(id);output.items=std::move(child.value);
            }
            output.succeeded=output.error.empty();
        }
    }
    SDL_LockMutex(mutex_);if(cancelled_){output.succeeded=false;output.error="已取消线路加载";}result_=std::move(output);running_=false;ready_=true;SDL_UnlockMutex(mutex_);return 0;
}
}
