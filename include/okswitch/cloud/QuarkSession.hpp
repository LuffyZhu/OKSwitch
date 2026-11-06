#pragma once

#include <string>

namespace okswitch::cloud
{
struct QuarkSession
{
    std::string cookie;
    std::string userAgent;

    bool hasCredentials() const
    {
        return !cookie.empty();
    }
};

enum class SessionLoadStatus
{
    Loaded,
    NotFound,
    Invalid,
    IoError
};

struct SessionLoadResult
{
    SessionLoadStatus status = SessionLoadStatus::NotFound;
    QuarkSession session;
    std::string error;
};

class QuarkSessionStore
{
public:
    explicit QuarkSessionStore(std::string path);

    SessionLoadResult load() const;
    bool save(const QuarkSession& session, std::string& error) const;
    bool clear(std::string& error) const;
    const std::string& path() const;

private:
    std::string path_;
};
}
