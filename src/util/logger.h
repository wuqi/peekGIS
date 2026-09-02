#pragma once
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <string>
#include <memory>
#include <vector>

// 初始化 spdlog: 同时写 exe 同目录的 peekgis.log 与 stderr(终端可见)
inline void initLogger(const std::string& exeDir) {
    try {
        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            exeDir + "/peekgis.log", true);
        auto console_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
        std::vector<spdlog::sink_ptr> sinks = {file_sink, console_sink};
        auto logger = std::make_shared<spdlog::logger>("peek", sinks.begin(), sinks.end());
        logger->set_level(spdlog::level::debug);
        logger->flush_on(spdlog::level::info);
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
        spdlog::set_default_logger(logger);
        spdlog::info("日志初始化完成 -> {}/peekgis.log", exeDir);
    } catch (const spdlog::spdlog_ex& e) {
        fprintf(stderr, "logger init failed: %s\n", e.what());
    }
}
