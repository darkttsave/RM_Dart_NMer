#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// 兼容本仓库相机模块依赖的 RMVL 基础日志宏。
#ifndef PASS_
#define PASS_(fmt, ...) std::fprintf(stdout, "[PASS] " fmt "\n", ##__VA_ARGS__)
#endif

#ifndef INFO_
#define INFO_(fmt, ...) std::fprintf(stdout, "[INFO] " fmt "\n", ##__VA_ARGS__)
#endif

#ifndef WARNING_
#define WARNING_(fmt, ...) std::fprintf(stderr, "[WARN] " fmt "\n", ##__VA_ARGS__)
#endif

#ifndef ERROR_
#define ERROR_(fmt, ...) std::fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)
#endif
