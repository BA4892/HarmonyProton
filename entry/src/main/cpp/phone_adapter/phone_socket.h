// phone_socket.h — 手机适配层 socket 读写工具
// header-only inline 实现，phone_virgl_relay.cpp 和 phone_virgl_dispatch.cpp 共享
#pragma once
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <sys/socket.h>

namespace phone_adapter {

inline bool SockWriteAll(int fd, const void* buf, size_t len) {
    const auto* p = static_cast<const uint8_t*>(buf);
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        p += n; len -= static_cast<size_t>(n);
    }
    return true;
}

inline bool SockReadAll(int fd, void* buf, size_t len) {
    auto* p = static_cast<uint8_t*>(buf);
    while (len > 0) {
        ssize_t n = recv(fd, p, len, 0);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        if (n == 0) return false;                     // EOF
        p += n; len -= static_cast<size_t>(n);
    }
    return true;
}

template <typename T>
inline bool SockWritePod(int fd, const T& v) {
    return SockWriteAll(fd, &v, sizeof(v));
}

template <typename T>
inline bool SockReadPod(int fd, T& v) {
    return SockReadAll(fd, &v, sizeof(v));
}

inline bool SockWriteStr(int fd, const char* s) {
    uint32_t len = s ? static_cast<uint32_t>(strlen(s)) + 1 : 0;
    return SockWritePod(fd, len) && (len == 0 || SockWriteAll(fd, s, len));
}

inline bool SockReadStr(int fd, std::string& out) {
    uint32_t len = 0;
    if (!SockReadPod(fd, len) || len > 1024) return false;
    if (len == 0) { out.clear(); return true; }
    std::vector<char> buf(len);
    if (!SockReadAll(fd, buf.data(), len)) return false;
    out.assign(buf.data(), len - 1);   // 去掉 NUL 结尾
    return true;
}

} // namespace phone_adapter
