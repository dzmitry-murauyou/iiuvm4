#include "Logger.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <cstdarg>

#include <iterator>
#include <cstdio>
#include <cwchar>

namespace cam {

// ---------------------------------------------------------------------------
//  Logger
// ---------------------------------------------------------------------------

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::set_log_file(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    log_file_ = path;
}

void Logger::set_callback(std::function<void(const LogRecord&)> cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(cb);
}

void Logger::info(const std::wstring& text)       { add(LogLevel::Info, text); }
void Logger::warn(const std::wstring& text)       { add(LogLevel::Warn, text); }
void Logger::error(const std::wstring& text)      { add(LogLevel::Error, text); }
void Logger::background(const std::wstring& text){ add(LogLevel::Background, text); }

void Logger::add(LogLevel level, const std::wstring& text) {
    LogRecord record;
    GetLocalTime(&record.time);
    record.level = level;
    record.text = text;

    std::function<void(const LogRecord&)> cb;
    std::wstring log_file;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        records_.push_back(record);
        // Ограничиваем журнал в памяти, чтобы приложение не росло бесконечно.
        while (records_.size() > 5000) records_.pop_front();
        cb = callback_;
        log_file = log_file_;
    }

    // Дублируем в файл (append). Пишем построчно, чтобы файл читался даже
    // если приложение аварийно завершилось.
    if (!log_file.empty()) {
        HANDLE h = CreateFileW(log_file.c_str(), FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            std::wstring line = L"[" + log_timestamp(record.time) + L"] [" +
                                level_name(record.level) + L"] " + record.text + L"\r\n";
            std::string utf8 = to_utf8(line);
            DWORD written = 0;
            // Маркер BOM нужен только при создании нового файла.
            LARGE_INTEGER size{};
            if (GetFileSizeEx(h, &size) && size.QuadPart == 0) {
                static const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
                WriteFile(h, bom, 3, &written, nullptr);
            }
            WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
            CloseHandle(h);
        }
    }

    if (cb) cb(record);
}

std::wstring Logger::build_report(const std::wstring& app_title) const {
    std::vector<LogRecord> copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        copy.assign(records_.begin(), records_.end());
    }

    SYSTEMTIME now;
    GetLocalTime(&now);

    std::wstring out;
    out.reserve(copy.size() * 64 + 256);
    out += L"=" + std::wstring(74, L'=') + L"\r\n";
    out += L" " + app_title + L"\r\n";
    out += L" Отчёт сформирован: " + human_timestamp(now) + L"\r\n";
    out += L" Записей в журнале: " + std::to_wstring(copy.size()) + L"\r\n";
    out += L"=" + std::wstring(74, L'=') + L"\r\n\r\n";

    for (const auto& r : copy) {
        out += L"[" + log_timestamp(r.time) + L"] ";
        std::wstring tag = level_name(r.level);
        tag.resize(10, L' ');
        out += L"[" + tag + L"] " + r.text + L"\r\n";
    }

    if (copy.empty()) out += L"(журнал пуст)\r\n";
    return out;
}

bool Logger::save_report(const std::wstring& path) const {
    std::wstring text = build_report(L"ЛР №4. Система видеонаблюдения (вариант А5)");
    return write_text_file_utf8(path, text);
}

std::vector<LogRecord> Logger::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<LogRecord>(records_.begin(), records_.end());
}

void Logger::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    records_.clear();
}

const wchar_t* level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Info:       return L"INFO";
        case LogLevel::Warn:       return L"WARN";
        case LogLevel::Error:      return L"ERROR";
        case LogLevel::Background: return L"BACKGROUND";
    }
    return L"INFO";
}

// ---------------------------------------------------------------------------
//  Время
// ---------------------------------------------------------------------------

std::wstring file_timestamp(const SYSTEMTIME& st) {
    // Не используем GetDateFormatEx -- возвращает ':' в части времени.
    wchar_t buffer[32];
    swprintf_s(buffer, L"%04u%02u%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond);
    return buffer;
}

std::wstring log_timestamp(const SYSTEMTIME& st) {
    wchar_t buffer[48];
    swprintf_s(buffer, L"%02u.%02u.%04u %02u:%02u:%02u.%03u", st.wDay, st.wMonth,
               st.wYear, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buffer;
}

std::wstring human_timestamp(const SYSTEMTIME& st) {
    wchar_t buffer[48];
    swprintf_s(buffer, L"%02u.%02u.%04u %02u:%02u:%02u", st.wDay, st.wMonth, st.wYear,
               st.wHour, st.wMinute, st.wSecond);
    return buffer;
}

// ---------------------------------------------------------------------------
//  Файлы и пути
// ---------------------------------------------------------------------------

std::wstring get_module_dir() {
    wchar_t path[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (n == 0) return L".";
    std::wstring s(path, n);
    size_t pos = s.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L".";
    return s.substr(0, pos);
}

std::wstring join_path(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    wchar_t last = dir.back();
    if (last == L'\\' || last == L'/') return dir + name;
    return dir + L"\\" + name;
}

std::wstring join_path(const std::wstring& dir, const std::wstring& sub,
                       const std::wstring& name) {
    return join_path(join_path(dir, sub), name);
}

bool file_exists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool dir_exists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool make_dirs(const std::wstring& path) {
    if (path.empty()) return false;
    if (dir_exists(path)) return true;

    std::wstring current;
    current.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        wchar_t c = path[i];
        current += c;
        bool last = (i + 1 == path.size());
        if (c == L'\\' || c == L'/' || last) {
            // Пропускаем "C:" и повторные разделители
            if (current.size() <= 3 && current.size() >= 2 &&
                current[1] == L':') {
                continue;
            }
            std::wstring probe = current;
            while (!probe.empty() && (probe.back() == L'\\' || probe.back() == L'/')) {
                probe.pop_back();
            }
            if (probe.empty()) continue;
            if (!dir_exists(probe)) {
                if (!CreateDirectoryW(probe.c_str(), nullptr) &&
                    GetLastError() != ERROR_ALREADY_EXISTS) {
                    return false;
                }
            }
        }
    }
    return dir_exists(path);
}

bool ensure_dir(const std::wstring& path) { return make_dirs(path); }

std::wstring file_name_of(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? path : path.substr(pos + 1);
}

std::wstring file_ext_lower(const std::wstring& path) {
    std::wstring name = file_name_of(path);
    size_t pos = name.find_last_of(L'.');
    if (pos == std::wstring::npos) return L"";
    std::wstring ext = name.substr(pos + 1);
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
    return ext;
}

std::wstring basename_without_ext(const std::wstring& path) {
    std::wstring name = file_name_of(path);
    size_t pos = name.find_last_of(L'.');
    return (pos == std::wstring::npos) ? name : name.substr(0, pos);
}

unsigned long long file_size(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

bool write_text_file_utf8(const std::wstring& path, const std::wstring& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    std::string utf8 = to_utf8(text);
    static const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
    DWORD written = 0;
    WriteFile(h, bom, 3, &written, nullptr);
    BOOL ok = WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(h);
    return ok != FALSE;
}

std::string to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                   nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                        size, nullptr, nullptr);
    return out;
}

std::wstring from_utf8(const std::string& text) {
    if (text.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                   nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                        size);
    return out;
}

std::wstring format(const wchar_t* fmt, ...) {
    wchar_t buffer[2048];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buffer, std::size(buffer), _TRUNCATE, fmt, args);
    va_end(args);
    return buffer;
}

}  // namespace cam
