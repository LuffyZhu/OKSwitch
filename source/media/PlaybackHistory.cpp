#include "okswitch/media/PlaybackHistory.hpp"
#include <jansson.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <cctype>
#include <cstdlib>
namespace okswitch::media {
namespace {
const char* path="sdmc:/switch/OKSwitch/history.json";
const char* preferencesPath="sdmc:/switch/OKSwitch/playback-settings.json";
std::string string(json_t* row,const char* key){auto v=json_object_get(row,key);return json_is_string(v)?json_string_value(v):"";}
std::string titleKey(const std::string& title){std::string key;for(unsigned char c:title)if(c>=128||std::isalnum(c))key.push_back(c<128?(char)std::tolower(c):(char)c);return key;}
int legacyEpisodeNumber(const std::string& name){
    auto marker=name.find("第");
    if(marker!=std::string::npos){size_t begin=marker+std::string("第").size(),end=begin;while(end<name.size()&&std::isdigit((unsigned char)name[end]))++end;if(end>begin&&name.compare(end,std::string("集").size(),"集")==0)return std::atoi(name.substr(begin,end-begin).c_str());}
    for(size_t p=0;p+1<name.size();++p)if(std::toupper((unsigned char)name[p])=='E'&&std::isdigit((unsigned char)name[p+1])){size_t end=p+1;while(end<name.size()&&std::isdigit((unsigned char)name[end]))++end;return std::atoi(name.substr(p+1,end-p-1).c_str());}
    int candidate=0;
    for(size_t p=0;p<name.size();){if(!std::isdigit((unsigned char)name[p])){++p;continue;}size_t begin=p;while(p<name.size()&&std::isdigit((unsigned char)name[p]))++p;int value=std::atoi(name.substr(begin,p-begin).c_str());char after=p<name.size()?(char)std::toupper((unsigned char)name[p]):'\0';char before=begin>0?(char)std::toupper((unsigned char)name[begin-1]):'\0';if(value>0&&value<1000&&after!='P'&&after!='K'&&after!='F'&&!((before=='H'||before=='X')&&(value==264||value==265)))candidate=value;}
    return candidate;
}
bool replaceFile(const std::string& temporary,const std::string& destination){
    if(std::rename(temporary.c_str(),destination.c_str())==0)return true;
    const std::string backup=destination+".bak";
    std::remove(backup.c_str());
    const bool hadPrevious=std::rename(destination.c_str(),backup.c_str())==0;
    if(std::rename(temporary.c_str(),destination.c_str())==0){if(hadPrevious)std::remove(backup.c_str());return true;}
    if(hadPrevious)std::rename(backup.c_str(),destination.c_str());
    return false;
}
bool saveHistory(const std::vector<PlaybackRecord>& records){
    if(mkdir("sdmc:/switch",0777)!=0&&errno!=EEXIST)return false;
    if(mkdir("sdmc:/switch/OKSwitch",0777)!=0&&errno!=EEXIST)return false;
    auto root=json_array();if(!root)return false;
    for(const auto& r:records){
        auto row=json_pack("{s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:i,s:i,s:f,s:f,s:i,s:i,s:i,s:I}","title",r.item.title.c_str(),"source",r.item.sourceName.c_str(),"url",r.item.detailUrl.c_str(),"poster",r.item.posterUrl.c_str(),"share",r.shareUrl.c_str(),"file_id",r.fileId.c_str(),"file_name",r.fileName.c_str(),"episode_number",r.episodeNumber,"episode_ordinal",r.episodeOrdinal,"position",r.positionSeconds,"duration",r.durationSeconds,"intro_skip",r.introSkipSeconds,"outro_skip",r.outroSkipSeconds,"volume",std::clamp(r.volumePercent,0,100),"updated_at",(json_int_t)r.updatedAt);
        if(!row||json_array_append_new(root,row)!=0){json_decref(root);return false;}
    }
    const std::string temporary=std::string(path)+".tmp";
    int result=json_dump_file(root,temporary.c_str(),JSON_INDENT(2)|JSON_ENSURE_ASCII);json_decref(root);
    return result==0&&replaceFile(temporary,path);
}
}
bool PlaybackHistory::load(){
    errno=0;json_error_t error{};auto root=json_load_file(path,0,&error);
    records_.clear();readable_=true;
    if(!root){readable_=errno==ENOENT;}
    else{
    if(!json_is_array(root)){json_decref(root);return readable_=false;}
    for(size_t i=0;i<json_array_size(root);++i){
        auto row=json_array_get(root,i);PlaybackRecord r;
        r.item.title=string(row,"title");r.item.sourceName=string(row,"source");r.item.detailUrl=string(row,"url");r.item.posterUrl=string(row,"poster");
        r.shareUrl=string(row,"share");r.fileId=string(row,"file_id");r.fileName=string(row,"file_name");
        r.episodeNumber=std::max(0,(int)json_integer_value(json_object_get(row,"episode_number")));r.episodeOrdinal=std::max(0,(int)json_integer_value(json_object_get(row,"episode_ordinal")));if(r.episodeNumber==0)r.episodeNumber=legacyEpisodeNumber(r.fileName);
        auto position=json_object_get(row,"position");auto duration=json_object_get(row,"duration");
        r.positionSeconds=json_number_value(position);r.durationSeconds=json_number_value(duration);
        r.introSkipSeconds=(int)json_integer_value(json_object_get(row,"intro_skip"));r.outroSkipSeconds=(int)json_integer_value(json_object_get(row,"outro_skip"));
        auto volume=json_object_get(row,"volume");r.volumePercent=json_is_integer(volume)?std::clamp((int)json_integer_value(volume),0,100):100;
        r.updatedAt=json_integer_value(json_object_get(row,"updated_at"));
        if(r.item.detailUrl.empty()||r.fileId.empty()||!json_is_number(position)||!json_is_number(duration)||r.positionSeconds<=0||r.durationSeconds<0){readable_=false;break;}
        records_.push_back(std::move(r));
    }
    json_decref(root);
    if(!readable_)records_.clear();
    else{std::stable_sort(records_.begin(),records_.end(),[](const auto& a,const auto& b){return a.updatedAt>b.updatedAt;});if(records_.size()>100)records_.resize(100);}
    }
    preferences_.clear();
    json_error_t preferencesError{};auto settings=json_load_file(preferencesPath,0,&preferencesError);
    if(settings&&json_is_array(settings))for(size_t i=0;i<json_array_size(settings);++i){auto row=json_array_get(settings,i);SeriesPlaybackPreferences p;p.title=string(row,"title");p.detailUrl=string(row,"url");p.introSkipSeconds=std::max(0,(int)json_integer_value(json_object_get(row,"intro_skip")));p.outroSkipSeconds=std::max(0,(int)json_integer_value(json_object_get(row,"outro_skip")));auto volume=json_object_get(row,"volume");p.volumePercent=json_is_integer(volume)?std::clamp((int)json_integer_value(volume),0,100):100;p.updatedAt=json_integer_value(json_object_get(row,"updated_at"));if(!p.title.empty()||!p.detailUrl.empty())preferences_.push_back(std::move(p));}
    if(settings)json_decref(settings);
    std::stable_sort(preferences_.begin(),preferences_.end(),[](const auto& a,const auto& b){return a.updatedAt>b.updatedAt;});
    return readable_;
}
bool PlaybackHistory::recordProgress(const PlaybackRecord& record){
    if(!readable_||record.item.detailUrl.empty()||record.fileId.empty()||!std::isfinite(record.positionSeconds)||!std::isfinite(record.durationSeconds)||record.positionSeconds<=0||record.durationSeconds<0)return false;
    auto next=records_;
    next.erase(std::remove_if(next.begin(),next.end(),[&](const auto& r){return r.item.detailUrl==record.item.detailUrl&&r.shareUrl==record.shareUrl&&r.fileId==record.fileId;}),next.end());
    next.insert(next.begin(),record);if(next.size()>100)next.resize(100);
    if(!saveHistory(next))return false;
    records_=std::move(next);return true;
}
bool PlaybackHistory::remove(const std::string& detailUrl,const std::string& shareUrl,const std::string& fileId){
    if(!readable_)return false;
    auto next=records_;
    const auto oldSize=next.size();
    next.erase(std::remove_if(next.begin(),next.end(),[&](const auto& r){return r.item.detailUrl==detailUrl&&r.shareUrl==shareUrl&&r.fileId==fileId;}),next.end());
    if(next.size()==oldSize)return false;
    if(!saveHistory(next))return false;
    records_=std::move(next);return true;
}
bool PlaybackHistory::removeSeries(const std::string& detailUrl){
    if(!readable_||detailUrl.empty())return false;
    auto next=records_;
    const auto oldSize=next.size();
    next.erase(std::remove_if(next.begin(),next.end(),[&](const auto& r){return r.item.detailUrl==detailUrl;}),next.end());
    if(next.size()==oldSize||!saveHistory(next))return false;
    records_=std::move(next);return true;
}
const PlaybackRecord* PlaybackHistory::find(const std::string& detailUrl,const std::string& shareUrl,const std::string& fileId) const{
    auto it=std::find_if(records_.begin(),records_.end(),[&](const auto& r){return r.item.detailUrl==detailUrl&&r.shareUrl==shareUrl&&r.fileId==fileId;});
    return it==records_.end()?nullptr:&*it;
}
const PlaybackRecord* PlaybackHistory::findSeries(const std::string& detailUrl) const{
    auto it=std::find_if(records_.begin(),records_.end(),[&](const auto& r){return r.item.detailUrl==detailUrl;});
    return it==records_.end()?nullptr:&*it;
}
const PlaybackRecord* PlaybackHistory::findSeriesByTitle(const std::string& title) const{
    const auto key=titleKey(title);if(key.empty())return nullptr;
    auto it=std::find_if(records_.begin(),records_.end(),[&](const auto& r){return titleKey(r.item.title)==key;});
    return it==records_.end()?nullptr:&*it;
}
const PlaybackRecord* PlaybackHistory::findTitleEpisode(const std::string& title,int episodeNumber,int episodeOrdinal) const{
    const auto key=titleKey(title);if(key.empty())return nullptr;
    auto it=std::find_if(records_.begin(),records_.end(),[&](const auto& r){
        if(titleKey(r.item.title)!=key)return false;
        if(episodeNumber>0&&r.episodeNumber>0)return r.episodeNumber==episodeNumber;
        return episodeOrdinal>0&&r.episodeOrdinal==episodeOrdinal;
    });
    return it==records_.end()?nullptr:&*it;
}
const SeriesPlaybackPreferences* PlaybackHistory::findPreferences(const std::string& title,const std::string& detailUrl) const{
    auto it=std::find_if(preferences_.begin(),preferences_.end(),[&](const auto& p){return (!title.empty()&&p.title==title)||(!detailUrl.empty()&&p.detailUrl==detailUrl);});
    return it==preferences_.end()?nullptr:&*it;
}
bool PlaybackHistory::recordPreferences(const SeriesPlaybackPreferences& preferences){
    if(preferences.title.empty()&&preferences.detailUrl.empty())return false;
    auto next=preferences_;
    next.erase(std::remove_if(next.begin(),next.end(),[&](const auto& p){return (!preferences.title.empty()&&p.title==preferences.title)||(!preferences.detailUrl.empty()&&p.detailUrl==preferences.detailUrl);}),next.end());
    auto saved=preferences;saved.introSkipSeconds=std::max(0,saved.introSkipSeconds);saved.outroSkipSeconds=std::max(0,saved.outroSkipSeconds);saved.volumePercent=std::clamp(saved.volumePercent,0,100);next.insert(next.begin(),std::move(saved));if(next.size()>100)next.resize(100);
    if(mkdir("sdmc:/switch",0777)!=0&&errno!=EEXIST)return false;
    if(mkdir("sdmc:/switch/OKSwitch",0777)!=0&&errno!=EEXIST)return false;
    auto root=json_array();if(!root)return false;
    for(const auto& p:next){auto row=json_pack("{s:s,s:s,s:i,s:i,s:i,s:I}","title",p.title.c_str(),"url",p.detailUrl.c_str(),"intro_skip",p.introSkipSeconds,"outro_skip",p.outroSkipSeconds,"volume",p.volumePercent,"updated_at",(json_int_t)p.updatedAt);if(!row||json_array_append_new(root,row)!=0){json_decref(root);return false;}}
    const std::string temporary=std::string(preferencesPath)+".tmp";int result=json_dump_file(root,temporary.c_str(),JSON_INDENT(2)|JSON_ENSURE_ASCII);json_decref(root);if(result!=0||!replaceFile(temporary,preferencesPath))return false;preferences_=std::move(next);return true;
}
}
