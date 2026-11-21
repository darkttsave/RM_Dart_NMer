/**
 * @file Utils.cpp
 * @brief 飞镖制导系统工具函数实现
 * @author lxy
 * @date 2025-10-24
 */

#include "Utils.h"
#include <algorithm>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <streambuf>
#include <vector>

namespace
{
class LineLimitedTeeBuf : public std::streambuf
{
public:
    struct SharedState
    {
        std::size_t lineLimit = 0;
        std::size_t capturedLines = 0;
    };

    LineLimitedTeeBuf(std::streambuf *originalBuf, std::ostream &captureStream, SharedState *sharedState)
        : originalBuf_(originalBuf), captureStream_(captureStream), sharedState_(sharedState)
    {
    }

    ~LineLimitedTeeBuf() override
    {
        flushPendingLine();
    }

protected:
    int overflow(int ch) override
    {
        if (ch == traits_type::eof())
        {
            return sync() == 0 ? 0 : traits_type::eof();
        }

        originalBuf_->sputc(static_cast<char>(ch));
        buffer_.push_back(static_cast<char>(ch));
        if (ch == '\n')
        {
            flushPendingLine();
        }
        return ch;
    }

    int sync() override
    {
        flushPendingLine();
        return originalBuf_->pubsync();
    }

private:
    void flushPendingLine()
    {
        if (buffer_.empty())
        {
            return;
        }

        if (sharedState_ != nullptr && sharedState_->capturedLines < sharedState_->lineLimit)
        {
            captureStream_ << buffer_;
            captureStream_.flush();
            ++sharedState_->capturedLines;
        }

        buffer_.clear();
    }

    std::streambuf *originalBuf_;
    std::ostream &captureStream_;
    SharedState *sharedState_;
    std::string buffer_;
};

std::ofstream g_terminalLogFile;
std::streambuf *g_originalCoutBuf = nullptr;
std::streambuf *g_originalCerrBuf = nullptr;
std::unique_ptr<LineLimitedTeeBuf> g_coutTee;
std::unique_ptr<LineLimitedTeeBuf> g_cerrTee;
LineLimitedTeeBuf::SharedState g_sharedLogState;
}

std::string makeRunTimestamp()
{
    std::time_t now = std::time(nullptr);
    std::tm localTime = *std::localtime(&now);
    std::ostringstream oss;
    oss << std::put_time(&localTime, "%y%m%d%H%M");
    return oss.str();
}

void pruneRunLogs(const fs::path &logRoot, const fs::path &currentRun, std::size_t limit)
{
    std::error_code ec;
    std::vector<fs::path> runs;
    fs::directory_iterator it(logRoot, ec), end;
    for (; !ec && it != end; it.increment(ec))
    {
        const auto &entry = *it;
        const std::string name = entry.path().filename().string();
        std::error_code typeError;
        const auto status = entry.symlink_status(typeError);
        if (!typeError && fs::is_directory(status) && name.size() == 10 &&
            std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; }))
            runs.push_back(entry.path());
    }
    if (ec)
    {
        std::cerr << "[RunLog] Warning: cannot list log directories: " << ec.message() << std::endl;
        return;
    }
    std::sort(runs.begin(), runs.end(), std::greater<fs::path>());
    std::size_t kept = 0;
    const auto currentName = currentRun.filename();
    const bool hasCurrent = std::any_of(runs.begin(), runs.end(), [&](const fs::path &p) {
        return p.filename() == currentName;
    });
    if (hasCurrent) ++kept;
    for (const auto &path : runs)
    {
        if (path.filename() == currentName) continue;
        if (kept < limit) { ++kept; continue; }
        fs::remove_all(path, ec);
        if (ec)
        {
            std::cerr << "[RunLog] Warning: cannot remove " << path << ": " << ec.message() << std::endl;
            ec.clear();
        }
    }
}

fs::path resolveProjectRoot(const std::string &loadedConfigPath)
{
    if (!loadedConfigPath.empty())
    {
        fs::path configPath = fs::absolute(fs::path(loadedConfigPath));
        if (configPath.has_parent_path() && configPath.parent_path().has_parent_path())
        {
            return configPath.parent_path().parent_path();
        }
    }

    fs::path currentPath = fs::current_path();
    if (currentPath.filename() == "build" && currentPath.has_parent_path())
    {
        return currentPath.parent_path();
    }
    return currentPath;
}

bool setupTerminalLogCapture(const fs::path &logFilePath, int lineLimit)
{
    g_terminalLogFile.open(logFilePath, std::ios::out | std::ios::trunc);
    if (!g_terminalLogFile.is_open())
    {
        std::cerr << "[RunLog] Warning: failed to open startup terminal log file" << std::endl;
        return false;
    }

    g_originalCoutBuf = std::cout.rdbuf();
    g_originalCerrBuf = std::cerr.rdbuf();
    g_sharedLogState.lineLimit = static_cast<std::size_t>(lineLimit);
    g_sharedLogState.capturedLines = 0;

    g_coutTee = std::make_unique<LineLimitedTeeBuf>(g_originalCoutBuf, g_terminalLogFile, &g_sharedLogState);
    g_cerrTee = std::make_unique<LineLimitedTeeBuf>(g_originalCerrBuf, g_terminalLogFile, &g_sharedLogState);

    std::cout.rdbuf(g_coutTee.get());
    std::cerr.rdbuf(g_cerrTee.get());

    return true;
}

void restoreTerminalOutput()
{
    if (g_originalCoutBuf && g_originalCerrBuf)
    {
        std::cout.flush();
        std::cerr.flush();
        std::cout.rdbuf(g_originalCoutBuf);
        std::cerr.rdbuf(g_originalCerrBuf);
    }
}
