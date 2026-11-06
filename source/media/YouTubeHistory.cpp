#include "okswitch/media/YouTubeHistory.hpp"
#include <jansson.h>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <sys/stat.h>

namespace okswitch::media {
namespace {
constexpr const char* historyPath="sdmc:/switch/OKSwitch/youtube-history.json";
std::string field(json_t* row,const char* key){auto* value=json_object_get(row,key);return json_is_string(value)?json_string_value(value):"";}
bool save(const std::vector<YouTubeHistoryEntry>& items){
    if(mkdir("sdmc:/switch",0777)!=0&&errno!=EEXIST)return false;
    if(mkdir("sdmc:/switch/OKSwitch",0777)!=0&&errno!=EEXIST)return false;
    auto* root=json_array();if(!root)return false;
    for(const auto& item:items){
        auto* row=json_pack("{s:s,s:s,s:s,s:s,s:f,s:f,s:I}",
            "id",item.id.c_str(),"title",item.title.c_str(),"channel",item.channel.c_str(),
            "thumbnail",item.thumbnail.c_str(),"position",item.positionSeconds,
            "duration",item.durationSeconds,"updated_at",(json_int_t)item.updatedAt);
        if(!row||json_array_append_new(root,row)!=0){json_decref(root);return false;}
    }
    const std::string temp=std::string(historyPath)+".tmp";
    const bool written=json_dump_file(root,temp.c_str(),JSON_INDENT(2)|JSON_ENSURE_ASCII)==0;
    json_decref(root);if(!written)return false;
    const std::string backup=std::string(historyPath)+".bak";
    std::remove(backup.c_str());
    const bool previous=std::rename(historyPath,backup.c_str())==0;
    if(std::rename(temp.c_str(),historyPath)==0){if(previous)std::remove(backup.c_str());return true;}
    if(previous)std::rename(backup.c_str(),historyPath);
    return false;
}
}
bool YouTubeHistory::load(){
    json_error_t error{};auto* root=json_load_file(historyPath,0,&error);
    items_.clear();if(!root)return errno==ENOENT;
    if(!json_is_array(root)){json_decref(root);return false;}
    for(size_t i=0;i<json_array_size(root);++i){
        auto* row=json_array_get(root,i);YouTubeHistoryEntry entry;
        entry.id=field(row,"id");entry.title=field(row,"title");entry.channel=field(row,"channel");
        entry.thumbnail=field(row,"thumbnail");entry.positionSeconds=json_number_value(json_object_get(row,"position"));
        entry.durationSeconds=json_number_value(json_object_get(row,"duration"));
        entry.updatedAt=json_integer_value(json_object_get(row,"updated_at"));
        if(!entry.id.empty()&&!entry.title.empty()&&std::isfinite(entry.positionSeconds)&&std::isfinite(entry.durationSeconds))items_.push_back(std::move(entry));
    }
    json_decref(root);
    std::stable_sort(items_.begin(),items_.end(),[](const auto& a,const auto& b){return a.updatedAt>b.updatedAt;});
    if(items_.size()>100)items_.resize(100);
    return true;
}
bool YouTubeHistory::record(const YouTubeHistoryEntry& entry){
    if(entry.id.empty()||entry.title.empty()||!std::isfinite(entry.positionSeconds)||!std::isfinite(entry.durationSeconds))return false;
    auto next=items_;
    next.erase(std::remove_if(next.begin(),next.end(),[&](const auto& old){return old.id==entry.id;}),next.end());
    next.insert(next.begin(),entry);if(next.size()>100)next.resize(100);
    if(!save(next))return false;
    items_=std::move(next);return true;
}
const YouTubeHistoryEntry* YouTubeHistory::find(const std::string& id) const{
    auto it=std::find_if(items_.begin(),items_.end(),[&](const auto& entry){return entry.id==id;});
    return it==items_.end()?nullptr:&*it;
}
}
