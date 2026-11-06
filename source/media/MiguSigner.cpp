#include "okswitch/media/MiguSigner.hpp"
#include "wasm3.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace okswitch::media
{
namespace
{
constexpr const char* kModuleUrl = "https://www.miguvideo.com/mgs/player/prd/dist/mgprtcl.wasm";
constexpr const char* kFactor = "BjfS7eNf3OIROs2T1E8hHQ==";

std::string decode(const std::string& encoded)
{
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string result;
    result.reserve(encoded.size());
    for (size_t i = 0; i < encoded.size(); ++i)
    {
        if (encoded[i] == '%' && i + 2 < encoded.size() &&
            hex(encoded[i + 1]) >= 0 && hex(encoded[i + 2]) >= 0)
        {
            result.push_back(static_cast<char>((hex(encoded[i + 1]) << 4) | hex(encoded[i + 2])));
            i += 2;
        }
        else result.push_back(encoded[i] == '+' ? ' ' : encoded[i]);
    }
    return result;
}

std::string parameter(const std::string& url, const char* key)
{
    const std::string needle = std::string(key) + '=';
    size_t start = url.find('?');
    if (start == std::string::npos) return {};
    ++start;
    while (start < url.size())
    {
        const size_t end = url.find('&', start);
        const size_t limit = end == std::string::npos ? url.size() : end;
        if (url.compare(start, needle.size(), needle) == 0)
            return decode(url.substr(start + needle.size(), limit - start - needle.size()));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return {};
}

m3ApiRawFunction(noop0) { return m3Err_none; }
m3ApiRawFunction(zero0) { m3ApiReturnType(uint32_t); m3ApiReturn(0); }
m3ApiRawFunction(domain) { m3ApiReturnType(uint32_t); m3ApiReturn(1); }
uint32_t fingerprintOffset = 0;
m3ApiRawFunction(fingerprint) { m3ApiReturnType(uint32_t); m3ApiReturn(fingerprintOffset); }

m3ApiRawFunction(indirect)
{
    m3ApiReturnType(uint32_t);
    m3ApiGetArg(uint32_t, index);
    m3ApiGetArg(uint32_t, typesOffset);
    m3ApiGetArg(uint32_t, argsOffset);
    const size_t memorySize = m3_GetMemorySizeAt(_mem);
    auto* memory = static_cast<uint8_t*>(_mem);
    if (typesOffset >= memorySize || argsOffset > memorySize || memorySize - argsOffset < 8)
        m3ApiReturn(0);
    uint32_t first = 0, second = 0;
    std::memcpy(&first, memory + argsOffset, 4);
    std::memcpy(&second, memory + argsOffset + 4, 4);
    if (index == 15285 && first <= memorySize && memorySize - first >= 4)
    {
        // The browser's asynchronous IDBFS attempt can fail without preventing
        // the official signer from using the in-memory state.
        const int32_t unavailable = -3;
        std::memcpy(memory + first, &unavailable, 4);
        m3ApiReturn(0);
    }
    if (index == 14363 && first <= memorySize && second <= memorySize &&
        memorySize - first >= 32 && memorySize - second >= 64)
    {
        // The public, unauthenticated highlights path accepts an empty state.
        // This does not grant access to member-only replays.
        std::memset(memory + first, 0, 32);
        std::memset(memory + second, 0, 64);
        m3ApiReturn(1);
    }
    m3ApiReturn(0);
}

struct Import
{
    const char* name;
    const char* signature;
    M3RawCall function;
};

constexpr Import kImports[] = {
    {"a", "i(i)", zero0}, {"b", "i(iiii)", zero0},
    {"c", "i(iii)", zero0}, {"d", "i(iii)", indirect},
    {"e", "i()", fingerprint}, {"f", "i(iiii)", zero0},
    {"g", "i(iii)", zero0}, {"h", "i(iiii)", zero0},
    {"i", "i()", domain}, {"j", "i(i)", zero0},
    {"k", "i(iIii)", zero0}, {"l", "v(iiii)", noop0}
};

struct Machine
{
    IM3Environment environment = nullptr;
    IM3Runtime runtime = nullptr;
    IM3Module module = nullptr;
    ~Machine()
    {
        if (runtime) m3_FreeRuntime(runtime);
        if (environment) m3_FreeEnvironment(environment);
    }
    bool call(const char* name, const std::initializer_list<uint32_t>& arguments,
              uint32_t& value, std::string& error)
    {
        IM3Function function = nullptr;
        M3Result status = m3_FindFunction(&function, runtime, name);
        if (status) { error = std::string("咪咕模块缺少函数 ") + name; return false; }
        std::array<std::string, 5> values;
        std::array<const char*, 5> argv{};
        unsigned count = 0;
        for (uint32_t argument : arguments)
        {
            if (count == values.size()) { error = "咪咕模块参数过多"; return false; }
            values[count] = std::to_string(argument);
            argv[count] = values[count].c_str();
            ++count;
        }
        status = m3_CallArgv(function, count, argv.data());
        if (status) { error = std::string("咪咕模块调用失败: ") + status; return false; }
        value = 0;
        if (m3_GetRetCount(function))
        {
            status = m3_GetResultsV(function, &value);
            if (status) { error = std::string("咪咕模块返回失败: ") + status; return false; }
        }
        return true;
    }
    uint8_t* memory(size_t& size) { return m3_GetMemory(module, &size, 0); }
    bool allocate(const std::string& value, uint32_t& offset, std::string& error)
    {
        if (!call("p", {static_cast<uint32_t>(value.size() + 1)}, offset, error)) return false;
        size_t size = 0;
        auto* base = memory(size);
        if (!base || offset > size || size - offset < value.size() + 1)
        { error = "咪咕模块内存不足"; return false; }
        std::memcpy(base + offset, value.c_str(), value.size() + 1);
        return true;
    }
    bool push(const char* name, uint32_t context, const std::string& value, std::string& error)
    {
        uint32_t offset = 0, result = 0;
        return allocate(value, offset, error) &&
            call(name, {context, offset, static_cast<uint32_t>(value.size())}, result, error) &&
            result == 0;
    }
};
}

bool signMiguPlaylist(net::HttpClient& http, const std::string& url,
                      std::string& signedUrl, std::string& error, const std::atomic<bool>* cancel)
{
    static std::mutex signerMutex;
    std::lock_guard<std::mutex> lock(signerMutex);
    if(cancel&&cancel->load()){error="已取消获取视频";return false;}
    static std::string wasm;
    if (wasm.empty())
    {
        net::HttpRequest request;
        request.url = kModuleUrl;
        request.cancel=cancel;request.closeConnection=true;
        request.connectTimeoutSeconds = 7;
        request.timeoutSeconds = 18;
        request.maxResponseBytes = 128 * 1024;
        request.headers = {{"Referer", "https://www.miguvideo.com/"}, {"User-Agent", "Mozilla/5.0"}, {"Accept-Encoding", "identity"}};
        auto response = http.get(request);
        if(!response.succeeded&&response.error.find("Timeout")!=std::string::npos&&!(cancel&&cancel->load()))response=http.get(request);
        if(cancel&&cancel->load()){error="已取消获取视频";return false;}
        if (!response.succeeded || response.statusCode != 200 || response.body.size() < 1000 || response.body.compare(0,8,std::string("\0asm\1\0\0\0",8))!=0)
        {
            error = "无法下载咪咕播放校验模块: " + response.error;
            return false;
        }
        wasm = std::move(response.body);
    }
    const std::string program = parameter(url, "ProgramID");
    const std::string timestamp = parameter(url, "timestamp");
    const std::string userid = parameter(url, "userid");
    const std::string channel = parameter(url, "Channel_ID");
    const std::string puData = parameter(url, "puData");
    if (program.empty() || timestamp.empty() || channel.empty() || puData.empty())
    { error = "咪咕播放地址缺少校验参数"; return false; }
    Machine machine;
    machine.environment = m3_NewEnvironment();
    if (!machine.environment) { error = "无法初始化咪咕校验环境"; return false; }
    M3Result status = m3_ParseModule(machine.environment, &machine.module,
                                     reinterpret_cast<const uint8_t*>(wasm.data()), wasm.size());
    if (status) { error = "咪咕校验模块格式不支持"; return false; }
    machine.runtime = m3_NewRuntime(machine.environment, 512 * 1024, nullptr);
    if (!machine.runtime) { error = "无法启动咪咕校验环境"; return false; }
    status = m3_LoadModule(machine.runtime, machine.module);
    if (status) { error = "咪咕校验模块加载失败"; return false; }
    for (const auto& import : kImports)
    {
        status = m3_LinkRawFunction(machine.module, "a", import.name,
                                    import.signature, import.function);
        if (status) { error = "咪咕校验模块接口不兼容"; return false; }
    }
    uint32_t unused = 0, context = 0, fp = 0;
    if (!machine.call("n", {}, unused, error) ||
        !machine.allocate("unknown|unknown|unknown|unknown", fp, error)) return false;
    fingerprintOffset = fp;
    if (!machine.call("y", {}, context, error)) return false;
    if (!machine.push("t", context, program, error) ||
        !machine.push("C", context, timestamp, error) ||
        !machine.push("B", context, userid, error) ||
        !machine.call("v", {context, 0, 0}, unused, error) ||
        !machine.call("D", {context, 0, 0}, unused, error) ||
        !machine.push("A", context, puData, error) ||
        !machine.push("u", context, channel, error)) return false;
    uint32_t factor = 0, output = 0, generated = 0;
    if (!machine.allocate(kFactor, factor, error) ||
        !machine.call("p", {128}, output, error) ||
        !machine.call("F", {context, factor, static_cast<uint32_t>(std::strlen(kFactor)), output, 128}, generated, error)) return false;
    size_t memorySize = 0;
    uint8_t* memory = machine.memory(memorySize);
    if (!memory || output >= memorySize || generated == 0 || generated > 128)
    { error = "咪咕校验参数异常"; return false; }
    const char* intermediateData = reinterpret_cast<char*>(memory + output);
    const size_t intermediateLength = strnlen(intermediateData, std::min<size_t>(128, memorySize - output));
    if (intermediateLength == 0 || intermediateLength >= 128)
    { error = "咪咕校验参数为空"; return false; }
    const std::string intermediate(intermediateData, intermediateLength);
    if (!machine.push("z", context, intermediate, error)) return false;
    uint32_t hashOffset = 0;
    if (!machine.call("p", {128}, hashOffset, error)) return false;
    for (int i = 0; i < 8; ++i)
    {
        uint32_t code = 0;
        if (!machine.call("w", {context, hashOffset, 128}, code, error)) return false;
        if (code == 0)
        {
            memory = machine.memory(memorySize);
            if (!memory || hashOffset >= memorySize) break;
            const char* hash = reinterpret_cast<char*>(memory + hashOffset);
            const size_t available = std::min<size_t>(128, memorySize - hashOffset);
            const size_t length = strnlen(hash, available);
            if (length == 0 || length == available) break;
            signedUrl = url + "&ddCalcu=" + std::string(hash, length) + "&sv=119";
            return true;
        }
        if (code == UINT32_MAX) break;
        // The callback is synchronous in this embedded runtime, but allow the
        // official module to complete a short pending state transition.
        SDL_Delay(25);
    }
    error = "咪咕未生成播放校验值";
    return false;
}
}
