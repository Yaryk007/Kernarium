// SPDX-License-Identifier: Apache-2.0
//
// Which library is the application's own. The native-method thunks (jni_shim.cpp) go only to functions of the
// application's own native libraries: the ones installed with the application (its native library directory, its
// APK) or in its private data directories. Another package's library loaded into the process (WebView, a
// service client, a platform library) keeps the runtime's own binding.
//
// The package is the process name, read from /proc/self/cmdline up to a ':' (the name of a private process of the
// package is "<package>:<suffix>"). The paths are those the platform uses for an application:
//   /data/app/<random>/<package>-<random>/...   and the older /data/app/<package>-<n>/...
//   /data/data/<package>/..., /data/user/<n>/<package>/..., /data/user_de/<n>/<package>/...
// and the same under /mnt/expand/<volume>/ for an application moved to adoptable storage. A package name has no
// '-', so "<package>-" starts only the install directory of that package.
//
// Freestanding and header-only: the host tests include it.

#pragma once

#include <stddef.h>

namespace oldapp {

// Copies the package out of a process name ("com.example.game:remote" gives "com.example.game"). Returns false for
// an empty or too long name.
inline bool package_of_process(const char* process, char* out, size_t capacity) {
    if (process == nullptr || out == nullptr || capacity == 0) {
        return false;
    }
    size_t length = 0;
    while (process[length] != '\0' && process[length] != ':') {
        if (length + 1 >= capacity) {
            return false;
        }
        out[length] = process[length];
        ++length;
    }
    out[length] = '\0';
    return length != 0;
}

namespace application_path_detail {

inline bool digits(const char* begin, const char* end) {
    if (begin == end) {
        return false;
    }
    for (const char* p = begin; p != end; ++p) {
        if (*p < '0' || *p > '9') {
            return false;
        }
    }
    return true;
}

inline bool is_text(const char* begin, const char* end, const char* text) {
    while (begin != end && *text != '\0' && *begin == *text) {
        ++begin;
        ++text;
    }
    return begin == end && *text == '\0';
}

// Whether the component [begin, end) is the package, or starts "<package>-".
inline bool starts_with_package(const char* begin, const char* end, const char* package, bool dash) {
    while (*package != '\0') {
        if (begin == end || *begin != *package) {
            return false;
        }
        ++begin;
        ++package;
    }
    return dash ? (begin != end && *begin == '-') : begin == end;
}

}  // namespace application_path_detail

// Whether `path` (a library's path as the loader reports it) is in the directories of `package`. A null or empty
// package is "unknown": every path is then the application's (the caller has already excluded the platform).
inline bool in_application_directories(const char* path, const char* package) {
    using namespace application_path_detail;
    if (package == nullptr || package[0] == '\0') {
        return true;
    }
    if (path == nullptr || path[0] != '/') {
        return false;
    }
    const char* previous_begin = path;
    const char* previous_end = path;  // the component before the current one
    const char* p = path;
    while (*p == '/') {
        ++p;
    }
    while (*p != '\0') {
        const char* begin = p;
        while (*p != '\0' && *p != '/') {
            ++p;
        }
        const char* end = p;
        if (starts_with_package(begin, end, package, true)) {
            return true;  // /data/app/.../<package>-<random>/..., /mnt/expand/.../<package>-<n>/...
        }
        if (starts_with_package(begin, end, package, false) &&
            (is_text(previous_begin, previous_end, "data") || digits(previous_begin, previous_end))) {
            return true;  // /data/data/<package>, /data/user/<n>/<package>, /data/user_de/<n>/<package>
        }
        previous_begin = begin;
        previous_end = end;
        while (*p == '/') {
            ++p;
        }
    }
    return false;
}

}  // namespace oldapp
