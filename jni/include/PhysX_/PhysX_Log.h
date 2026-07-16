//
// Created by binglen on 2025/6/8.
//

#ifndef LOG_H
#define LOG_H

#include <android/log.h>

#define TAG "PhysX"

// 颜色定义
#define COLOR_RED     "\033[38;5;203m"
#define COLOR_GREEN   "\033[38;5;114m"
#define COLOR_YELLOW  "\033[38;5;221m"
#define COLOR_BLUE    "\033[38;5;111m"
#define COLOR_MAGENTA "\033[38;5;213m"
#define COLOR_CYAN    "\033[38;5;117m"
#define COLOR_ORANGE  "\033[38;5;214m"
#define COLOR_PINK    "\033[38;5;211m"
#define COLOR_RESET   "\033[0m"

// 分级日志宏
#define LOG_DEBUG(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOG_INFO(...)  __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOG_WARN(...)  __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOG_ERROR(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOG_FATAL(...) __android_log_print(ANDROID_LOG_FATAL, TAG, __VA_ARGS__)

#define SCENE_LOG(fmt, ...)       LOG_INFO(COLOR_BLUE "[Scene] " fmt COLOR_RESET, ##__VA_ARGS__)
#define SCENE_DEBUG(fmt, ...)     LOG_DEBUG(COLOR_CYAN "[Scene] " fmt COLOR_RESET, ##__VA_ARGS__)
#define SCENE_WARN(fmt, ...)      LOG_WARN(COLOR_YELLOW "[Scene] " fmt COLOR_RESET, ##__VA_ARGS__)
#define SCENE_ERROR(fmt, ...)     LOG_ERROR(COLOR_RED "[Scene] " fmt COLOR_RESET, ##__VA_ARGS__)

#define COLLECTOR_LOG(fmt, ...)   LOG_INFO(COLOR_GREEN "[Collector] " fmt COLOR_RESET, ##__VA_ARGS__)
#define COLLECTOR_DEBUG(fmt, ...) LOG_DEBUG(COLOR_CYAN "[Collector] " fmt COLOR_RESET, ##__VA_ARGS__)
#define COLLECTOR_WARN(fmt, ...)  LOG_WARN(COLOR_YELLOW "[Collector] " fmt COLOR_RESET, ##__VA_ARGS__)
#define COLLECTOR_ERROR(fmt, ...) LOG_ERROR(COLOR_RED "[Collector] " fmt COLOR_RESET, ##__VA_ARGS__)

#define LOADER_LOG(fmt, ...)      LOG_INFO(COLOR_MAGENTA "[Loader] " fmt COLOR_RESET, ##__VA_ARGS__)
#define LOADER_DEBUG(fmt, ...)    LOG_DEBUG(COLOR_CYAN "[Loader] " fmt COLOR_RESET, ##__VA_ARGS__)
#define LOADER_WARN(fmt, ...)     LOG_WARN(COLOR_YELLOW "[Loader] " fmt COLOR_RESET, ##__VA_ARGS__)
#define LOADER_ERROR(fmt, ...)    LOG_ERROR(COLOR_RED "[Loader] " fmt COLOR_RESET, ##__VA_ARGS__)

#define CORE_LOG(fmt, ...)        LOG_INFO(COLOR_ORANGE "[Core] " fmt COLOR_RESET, ##__VA_ARGS__)
#define CORE_DEBUG(fmt, ...)      LOG_DEBUG(COLOR_CYAN "[Core] " fmt COLOR_RESET, ##__VA_ARGS__)
#define CORE_WARN(fmt, ...)       LOG_WARN(COLOR_YELLOW "[Core] " fmt COLOR_RESET, ##__VA_ARGS__)
#define CORE_ERROR(fmt, ...)      LOG_ERROR(COLOR_RED "[Core] " fmt COLOR_RESET, ##__VA_ARGS__)

#define EMBREE_LOG(fmt, ...)      LOG_INFO(COLOR_PINK "[Embree] " fmt COLOR_RESET, ##__VA_ARGS__)
#define EMBREE_DEBUG(fmt, ...)    LOG_DEBUG(COLOR_CYAN "[Embree] " fmt COLOR_RESET, ##__VA_ARGS__)
#define EMBREE_WARN(fmt, ...)     LOG_WARN(COLOR_YELLOW "[Embree] " fmt COLOR_RESET, ##__VA_ARGS__)
#define EMBREE_ERROR(fmt, ...)    LOG_ERROR(COLOR_RED "[Embree] " fmt COLOR_RESET, ##__VA_ARGS__)

#define RAYCAST_LOG(fmt, ...)     LOG_DEBUG(COLOR_CYAN "[Raycast] " fmt COLOR_RESET, ##__VA_ARGS__)
#define RAYCAST_DEBUG(fmt, ...)   LOG_DEBUG(COLOR_CYAN "[Raycast] " fmt COLOR_RESET, ##__VA_ARGS__)
#define RAYCAST_WARN(fmt, ...)    LOG_WARN(COLOR_YELLOW "[Raycast] " fmt COLOR_RESET, ##__VA_ARGS__)
#define RAYCAST_ERROR(fmt, ...)   LOG_ERROR(COLOR_RED "[Raycast] " fmt COLOR_RESET, ##__VA_ARGS__)

#define MEMORY_LOG(fmt, ...)      LOG_INFO(COLOR_GREEN "[Memory] " fmt COLOR_RESET, ##__VA_ARGS__)
#define MEMORY_ERROR(fmt, ...)   LOG_ERROR(COLOR_RED "[Memory] " fmt COLOR_RESET, ##__VA_ARGS__)
#define THREAD_LOG(fmt, ...)      LOG_INFO(COLOR_MAGENTA "[Thread] " fmt COLOR_RESET, ##__VA_ARGS__)
#define PERF_LOG(fmt, ...)        LOG_DEBUG(COLOR_CYAN "[Performance] " fmt COLOR_RESET, ##__VA_ARGS__)

#endif