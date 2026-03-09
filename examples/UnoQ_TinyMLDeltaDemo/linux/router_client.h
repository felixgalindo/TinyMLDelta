/*
 * router_client.h — MsgPack-RPC client for arduino-router
 *
 * Connects to arduino-router's Unix socket, registers RPC methods, and
 * dispatches incoming requests (demo/tick, demo/cmd) to application handlers.
 *
 * Depends on: msgpack.h
 *
 * Author:  Felix Galindo
 * License: Apache-2.0
 */

#pragma once

#include "msgpack.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

// Forward-declare the logging macros (defined in demo_app.cpp).
// If you prefer, move logging to its own header.
extern void log_msg(const char *level, const char *fmt, ...);
#define RC_LOG_INFO(fmt, ...)  log_msg("INFO ", fmt, ##__VA_ARGS__)
#define RC_LOG_WARN(fmt, ...)  log_msg("WARN ", fmt, ##__VA_ARGS__)
#define RC_LOG_ERROR(fmt, ...) log_msg("ERROR", fmt, ##__VA_ARGS__)

// Callback interface: the application provides these two handlers.
struct RouterHandlers {
    virtual ~RouterHandlers() = default;
    virtual std::string handle_tick(const std::string &param) = 0;
    virtual std::string handle_cmd(const std::string &param) = 0;
};

class RouterClient {
public:
    RouterClient(const char *sock_path, RouterHandlers &handlers)
        : sock_path_(sock_path), handlers_(handlers) {}

    bool connect() {
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) { RC_LOG_ERROR("socket: %s", strerror(errno)); return false; }
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, sock_path_.c_str(), sizeof(addr.sun_path) - 1);
        if (::connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            ::close(fd);
            return false;
        }
        fd_ = fd;
        RC_LOG_INFO("Connected to router at %s", sock_path_.c_str());
        return true;
    }

    bool connect_retry(int attempts = 10, int delay_sec = 2) {
        for (int i = 0; i < attempts; ++i) {
            if (connect()) return true;
            RC_LOG_INFO("Waiting for arduino-router... (%d/%d)", i + 1, attempts);
            sleep(delay_sec);
        }
        return false;
    }

    // Send a $/register request and wait up to 5 s for the response.
    bool register_method(const char *method) {
        uint32_t mid = next_mid_++;
        auto pkt = mp_request(mid, "$/register", method);
        send_all(pkt);

        // Timed blocking recv: read until we get the matching RESPONSE.
        struct timeval tv = {5, 0};
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        uint8_t chunk[4096];
        time_t deadline = time(nullptr) + 5;
        while (time(nullptr) < deadline) {
            ssize_t n = recv(fd_, chunk, sizeof(chunk), 0);
            if (n > 0) {
                recv_buf_.insert(recv_buf_.end(), chunk, chunk + (size_t)n);
                MpVal msg;
                while (mp_try_decode(recv_buf_, msg)) {
                    if (msg.is_array() && msg.as_array().size() >= 4 &&
                        msg.as_array()[0].as_uint() == 1 &&   // RESPONSE
                        msg.as_array()[1].as_uint() == mid) {
                        // Clear timeout for run_forever.
                        struct timeval tv0 = {0, 0};
                        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv0, sizeof(tv0));
                        // Any non-nil result is a success.
                        bool ok = !msg.as_array()[2].is_nil()
                               || !msg.as_array()[3].is_nil();
                        RC_LOG_INFO("Registered %s: %s", method, ok ? "OK" : "FAILED");
                        return ok;
                    }
                }
            }
        }
        struct timeval tv0 = {0, 0};
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv0, sizeof(tv0));
        RC_LOG_WARN("Timeout waiting for registration response for %s", method);
        return false;
    }

    // Blocking recv loop — dispatches incoming RPC requests in detached threads.
    void run_forever() {
        uint8_t chunk[4096];
        while (true) {
            ssize_t n = recv(fd_, chunk, sizeof(chunk), 0);
            if (n <= 0) {
                RC_LOG_WARN("Router disconnected (recv=%zd, %s)", n, strerror(errno));
                break;
            }
            recv_buf_.insert(recv_buf_.end(), chunk, chunk + (size_t)n);
            MpVal msg;
            while (mp_try_decode(recv_buf_, msg)) {
                if (!msg.is_array() || msg.as_array().size() < 3) continue;
                uint64_t kind = msg.as_array()[0].as_uint();
                if (kind == 0 && msg.as_array().size() >= 4) {
                    // Incoming RPC request: [0, msgid, method, params]
                    uint32_t    msgid  = (uint32_t)msg.as_array()[1].as_uint();
                    std::string method = msg.as_array()[2].as_str();
                    std::string param;
                    const auto &params = msg.as_array()[3];
                    if (params.is_array() && !params.as_array().empty()) {
                        const auto &p0 = params.as_array()[0];
                        if (p0.is_str()) param = p0.as_str();
                    }
                    // Dispatch in a new thread; send response when done.
                    std::thread([this, msgid, method, param]() mutable {
                        std::string result;
                        if      (method == "demo/tick") result = handlers_.handle_tick(param);
                        else if (method == "demo/cmd")  result = handlers_.handle_cmd(param);
                        auto pkt = mp_response(msgid, nullptr, result);
                        send_all(pkt);
                    }).detach();
                }
            }
        }
    }

private:
    std::string          sock_path_;
    RouterHandlers      &handlers_;
    int                  fd_        = -1;
    uint32_t             next_mid_  = 0;
    std::vector<uint8_t> recv_buf_;
    std::mutex           send_lock_;

    void send_all(const std::vector<uint8_t> &pkt) {
        std::lock_guard<std::mutex> lk(send_lock_);
        const uint8_t *p = pkt.data();
        size_t rem = pkt.size();
        while (rem > 0) {
            ssize_t r = ::send(fd_, p, rem, MSG_NOSIGNAL);
            if (r <= 0) { RC_LOG_ERROR("send failed: %s", strerror(errno)); break; }
            p += r; rem -= (size_t)r;
        }
    }
};
