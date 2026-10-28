// ---------------------------------------------------------------------------
//  Logger.h -- журнал событий приложения (ЛР №4, вариант А5)
//
//  Требование п.4: "журналирование всех фоновых операций с временными
//  метками" и "кнопка Сохранить отчёт". Журнал ведётся в памяти, дублируется
//  в файл <каталог сохранения>/camera_log.txt и может быть выгружен в отчёт.
// ---------------------------------------------------------------------------
#pragma once

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace cam {

enum class LogLevel { Info, Warn, Error, Background };

struct LogRecord {
    SYSTEMTIME time{};          // локальное время события
    LogLevel level = LogLevel::Info;
    std::wstring text;          // текст события
};

// Потокобезопасный журнал. Все методы можно вызывать из любого потока,
// включая поток камеры и обработчик горячей клавиши.
class Logger {
public:
    static Logger& instance();

    void set_log_file(const std::wstring& path);   // "<dir>/camera_log.txt"
    void set_callback(std::function<void(const LogRecord&)> cb);

    void info(const std::wstring& text);
    void warn(const std::wstring& text);
    void error(const std::wstring& text);
    void background(const std::wstring& text);     // события скрытого режима

    // Выгрузка всего журнала в текстовый файл (кнопка "Сохранить отчёт").
    // Возвращает false, если файл не удалось открыть.
    bool save_report(const std::wstring& path) const;

    // Готовый текст отчёта (заголовок + записи).
    std::wstring build_report(const std::wstring& app_title) const;

    std::vector<LogRecord> snapshot() const;
    void clear();

private:
    Logger() = default;
    void add(LogLevel level, const std::wstring& text);

    mutable std::mutex mutex_;
    std::deque<LogRecord> records_;
    std::wstring log_file_;          // пустая строка -> только память
    std::function<void(const LogRecord&)> callback_;
};

// --- вспомогательные функции форматирования времени -------------------------

// "20261028_183012" -- для имён файлов (snapshot_YYYYMMDD_HHMMSS.jpg)
std::wstring file_timestamp(const SYSTEMTIME& st);
// "18:30:12.123" -- для журнала; time_precise берётся из GetLocalTime
std::wstring log_timestamp(const SYSTEMTIME& st);
// "28.10.2026 18:30:12"
std::wstring human_timestamp(const SYSTEMTIME& st);

const wchar_t* level_name(LogLevel level);

// --- утилиты для работы с путями/файлами -----------------------------------

std::wstring get_module_dir();
std::wstring join_path(const std::wstring& dir, const std::wstring& name);
std::wstring join_path(const std::wstring& dir, const std::wstring& sub,
                       const std::wstring& name);
bool file_exists(const std::wstring& path);
bool dir_exists(const std::wstring& path);
bool ensure_dir(const std::wstring& path);
bool make_dirs(const std::wstring& path);   // рекурсивно, как mkdir -p
std::wstring file_name_of(const std::wstring& path);
std::wstring file_ext_lower(const std::wstring& path);
unsigned long long file_size(const std::wstring& path);
std::wstring basename_without_ext(const std::wstring& path);
bool write_text_file_utf8(const std::wstring& path, const std::wstring& text);

std::string to_utf8(const std::wstring& text);
std::wstring from_utf8(const std::string& text);
std::wstring format(const wchar_t* fmt, ...);

}  // namespace cam
