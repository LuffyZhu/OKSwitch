#include "okswitch/cloud/QuarkSession.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <jansson.h>
#include <utility>

namespace okswitch::cloud
{
QuarkSessionStore::QuarkSessionStore(std::string path)
    : path_(std::move(path))
{
}

SessionLoadResult QuarkSessionStore::load() const
{
    SessionLoadResult result;
    json_error_t jsonError{};
    json_t* root = json_load_file(path_.c_str(), 0, &jsonError);
    if (root == nullptr)
    {
        if (errno == ENOENT)
        {
            result.status = SessionLoadStatus::NotFound;
        }
        else if (jsonError.line > 0)
        {
            result.status = SessionLoadStatus::Invalid;
            result.error = jsonError.text;
        }
        else
        {
            result.status = SessionLoadStatus::IoError;
            result.error = std::strerror(errno);
        }
        return result;
    }

    json_t* cookie = json_object_get(root, "cookie");
    json_t* userAgent = json_object_get(root, "user_agent");
    if (!json_is_string(cookie)
        || (userAgent != nullptr && !json_is_string(userAgent)))
    {
        result.status = SessionLoadStatus::Invalid;
        result.error = "invalid Quark session fields";
        json_decref(root);
        return result;
    }

    result.session.cookie = json_string_value(cookie);
    if (json_is_string(userAgent))
    {
        result.session.userAgent = json_string_value(userAgent);
    }
    result.status = result.session.hasCredentials()
        ? SessionLoadStatus::Loaded
        : SessionLoadStatus::Invalid;
    if (result.status == SessionLoadStatus::Invalid)
    {
        result.error = "empty Quark cookie";
    }

    json_decref(root);
    return result;
}

bool QuarkSessionStore::save(
    const QuarkSession& session, std::string& error) const
{
    if (!session.hasCredentials())
    {
        error = "cannot save an empty Quark session";
        return false;
    }

    json_t* root = json_object();
    if (root == nullptr)
    {
        error = "failed to allocate session JSON";
        return false;
    }

    json_object_set_new(root, "version", json_integer(1));
    json_object_set_new(root, "cookie", json_string(session.cookie.c_str()));
    json_object_set_new(
        root, "user_agent", json_string(session.userAgent.c_str()));

    const int writeResult = json_dump_file(
        root, path_.c_str(), JSON_INDENT(2) | JSON_SORT_KEYS);
    json_decref(root);
    if (writeResult != 0)
    {
        error = std::strerror(errno);
        return false;
    }

    return true;
}

bool QuarkSessionStore::clear(std::string& error) const
{
    if (std::remove(path_.c_str()) == 0 || errno == ENOENT)
    {
        return true;
    }

    error = std::strerror(errno);
    return false;
}

const std::string& QuarkSessionStore::path() const
{
    return path_;
}
}
