/*
 * demo_app.cpp — TinyMLDelta UNO Q Demo (Qualcomm aarch64 Linux)
 *
 * Compiled and deployed to the Arduino UNO Q Qualcomm Linux co-processor.
 * Connects to arduino-router's Unix socket (MsgPack-RPC) and handles two
 * RPC methods called by the thin STM32 sketch via Bridge.call():
 *
 *   demo/tick  (temp_c: str) -> display_line: str
 *   demo/cmd   (char: str)   -> response: str
 *
 * Directly integrates:
 *   TinyMLDelta C library — tmd_apply_patch_from_memory() for OTA model updates
 *   TFLite C API          — autoencoder inference via libtensorflowlite_c.so
 *
 * Build:   make            (on board, or cross with CXX=aarch64-linux-gnu-g++)
 * Deploy:  ./deploy_service.sh
 *
 * Author:  Felix Galindo
 * License: Apache-2.0
 */

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* TinyMLDelta C library + in-memory flash port (shared with edgeimpulse/) */
#include "tmd_port_memory.h"

// =============================================================================
// Configuration
// =============================================================================

static constexpr const char *kRouterSock  = "/var/run/arduino-router.sock";
static constexpr const char *kModelPath   = "/home/arduino/tinymldelta/model.tflite";
static constexpr const char *kPatchPath   = "/home/arduino/tinymldelta/pending_patch.tmd";
static constexpr const char *kCsvPath     = "/home/arduino/tinymldelta/training_data.csv";
static constexpr const char *kConfigPath  = "/home/arduino/tinymldelta/model_config.json";
static constexpr const char *kCmdFifo     = "/home/arduino/tinymldelta/cmd.fifo";
static constexpr int         kWindowSize  = 4;      // must match sketch
static constexpr float       kAnomalyMse  = 0.04f;
static constexpr int         kMaxSamples  = 200;
static constexpr float       kBadTemp     = -999.0f;

// =============================================================================
// Logging (also used by router_client.h)
// =============================================================================

void log_msg(const char *level, const char *fmt, ...) {
    time_t now = time(nullptr);
    struct tm t{};
    localtime_r(&now, &t);
    fprintf(stderr, "%02d:%02d:%02d [demo_app] %s ",
            t.tm_hour, t.tm_min, t.tm_sec, level);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

#define LOG_INFO(fmt, ...)  log_msg("INFO ", fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  log_msg("WARN ", fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) log_msg("ERROR", fmt, ##__VA_ARGS__)

/* MsgPack codec and RouterClient (shared with edgeimpulse/). */
#include "msgpack.h"
#include "router_client.h"

// =============================================================================
// File helpers
// =============================================================================

static std::vector<uint8_t> read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { LOG_ERROR("Cannot open file: %s (%s)", path, strerror(errno)); return {}; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); return {}; }
    std::vector<uint8_t> buf((size_t)sz);
    if ((long)fread(buf.data(), 1, (size_t)sz, f) != sz) {
        LOG_ERROR("Short read: %s", path);
        fclose(f); return {};
    }
    fclose(f);
    return buf;
}

static bool write_file(const char *path, const uint8_t *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) { LOG_ERROR("Cannot write: %s (%s)", path, strerror(errno)); return false; }
    bool ok = (fwrite(data, 1, len, f) == len);
    fclose(f);
    return ok;
}

// =============================================================================
// TFLite C API — classic interpreter (libtensorflowlite_c.so v2.17)
//
// The new LiteRT compiled model API (LiteRtCreateCompiledModel in libLiteRt.so)
// does NOT work with standard .tflite flatbuffers — it expects pre-compiled
// .lrt models.  The classic TFLite C API works with any .tflite file.
// Link with: -ltensorflowlite_c -Wl,-rpath,<dir>
// =============================================================================

extern "C" {

typedef struct TfLiteModel       TfLiteModel;
typedef struct TfLiteInterpreterOptions TfLiteInterpreterOptions;
typedef struct TfLiteInterpreter TfLiteInterpreter;
typedef struct TfLiteTensor      TfLiteTensor;
typedef int32_t                  TfLiteStatus;

TfLiteModel*       TfLiteModelCreateFromFile(const char* model_path);
void               TfLiteModelDelete(TfLiteModel* model);
TfLiteInterpreterOptions* TfLiteInterpreterOptionsCreate(void);
void               TfLiteInterpreterOptionsSetNumThreads(TfLiteInterpreterOptions* options, int32_t num_threads);
void               TfLiteInterpreterOptionsDelete(TfLiteInterpreterOptions* options);
TfLiteInterpreter* TfLiteInterpreterCreate(const TfLiteModel* model, const TfLiteInterpreterOptions* options);
void               TfLiteInterpreterDelete(TfLiteInterpreter* interpreter);
TfLiteStatus       TfLiteInterpreterAllocateTensors(TfLiteInterpreter* interpreter);
TfLiteStatus       TfLiteInterpreterInvoke(TfLiteInterpreter* interpreter);
const TfLiteTensor* TfLiteInterpreterGetInputTensor(const TfLiteInterpreter* interpreter, int32_t input_index);
const TfLiteTensor* TfLiteInterpreterGetOutputTensor(const TfLiteInterpreter* interpreter, int32_t output_index);
TfLiteStatus       TfLiteTensorCopyFromBuffer(TfLiteTensor* tensor, const void* input_data, size_t input_data_size);
TfLiteStatus       TfLiteTensorCopyToBuffer(const TfLiteTensor* output_tensor, void* output_data, size_t output_data_size);

} // extern "C"

static constexpr TfLiteStatus kTfLiteOk = 0;

// =============================================================================
// ModelRunner — TFLite C API inference + TinyMLDelta patch apply
// =============================================================================

class ModelRunner {
public:
    explicit ModelRunner(const char *model_path) : model_path_(model_path) { load(); }

    ~ModelRunner() {
        std::lock_guard<std::mutex> lk(lock_);
        unload_locked();
    }

    bool loaded() const {
        std::lock_guard<std::mutex> lk(lock_);
        return interp_ != nullptr;
    }

    // Run autoencoder on a window of kWindowSize normalized floats.
    // Returns reconstruction MSE, or -1.0f if no model is loaded.
    float infer(const float window[kWindowSize]) {
        std::lock_guard<std::mutex> lk(lock_);
        if (!interp_) return -1.0f;

        // Copy input into the input tensor
        TfLiteStatus st = TfLiteTensorCopyFromBuffer(
            (TfLiteTensor *)TfLiteInterpreterGetInputTensor(interp_, 0),
            window, kWindowSize * sizeof(float));
        if (st != kTfLiteOk) {
            LOG_ERROR("TFLite: CopyFromBuffer failed (%d)", st);
            return -1.0f;
        }

        st = TfLiteInterpreterInvoke(interp_);
        if (st != kTfLiteOk) {
            LOG_ERROR("TFLite: Invoke failed (%d)", st);
            return -1.0f;
        }

        float out[kWindowSize] = {};
        st = TfLiteTensorCopyToBuffer(
            TfLiteInterpreterGetOutputTensor(interp_, 0),
            out, sizeof(out));
        if (st != kTfLiteOk) {
            LOG_ERROR("TFLite: CopyToBuffer failed (%d)", st);
            return -1.0f;
        }

        float mse = 0.0f;
        for (int i = 0; i < kWindowSize; ++i) {
            float d = window[i] - out[i];
            mse += d * d;
        }
        return mse / (float)kWindowSize;
    }

    // Apply a TinyMLDelta .tmd patch directly via tmd_apply_patch_from_memory().
    // Atomically replaces the model file and reloads the TFLite interpreter.
    bool apply_patch(const char *patch_path) {
        auto base  = read_file(model_path_.c_str());
        auto patch = read_file(patch_path);
        if (base.empty() || patch.empty()) return false;

        LOG_INFO("Applying patch: base=%zu B  patch=%zu B  (%.1fx smaller)",
                 base.size(), patch.size(), (double)base.size() / (double)patch.size());

        std::lock_guard<std::mutex> lk(lock_);
        unload_locked();

        // Set up in-memory TinyMLDelta port and apply patch.
        size_t slot_sz = base.size() + 8192;
        tmd_mem_setup(base.data(), base.size(), slot_sz);

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        tmd_status_t st = tmd_apply_patch_from_memory(patch.data(), (uint32_t)patch.size());
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;

        size_t updated_len;
        const uint8_t *updated = tmd_mem_result(&updated_len);

        if (st != TMD_STATUS_OK) {
            LOG_ERROR("tmd_apply_patch_from_memory failed (status=%d)", (int)st);
            tmd_mem_cleanup();
            return false;
        }
        LOG_INFO("Patch applied in %.2f ms", ms);

        std::string tmp = model_path_ + ".new";
        bool wrote = write_file(tmp.c_str(), updated, updated_len);
        tmd_mem_cleanup();
        if (!wrote) return false;
        if (rename(tmp.c_str(), model_path_.c_str()) != 0) {
            LOG_ERROR("rename failed: %s", strerror(errno));
            return false;
        }
        LOG_INFO("Updated model: %s  (%zu bytes)", model_path_.c_str(), updated_len);

        return load_locked();
    }

private:
    std::string          model_path_;
    TfLiteModel         *model_  = nullptr;
    TfLiteInterpreter   *interp_ = nullptr;
    mutable std::mutex   lock_;

    void unload_locked() {
        if (interp_) { TfLiteInterpreterDelete(interp_); interp_ = nullptr; }
        if (model_)  { TfLiteModelDelete(model_);         model_  = nullptr; }
    }

    bool load_locked() {
        unload_locked();
        struct stat st;
        if (::stat(model_path_.c_str(), &st) != 0) {
            LOG_WARN("Model not found: %s", model_path_.c_str());
            return false;
        }

        model_ = TfLiteModelCreateFromFile(model_path_.c_str());
        if (!model_) {
            LOG_ERROR("TFLite: CreateModelFromFile failed: %s", model_path_.c_str());
            return false;
        }

        TfLiteInterpreterOptions *opts = TfLiteInterpreterOptionsCreate();
        TfLiteInterpreterOptionsSetNumThreads(opts, 2);
        interp_ = TfLiteInterpreterCreate(model_, opts);
        TfLiteInterpreterOptionsDelete(opts);

        if (!interp_) {
            LOG_ERROR("TFLite: InterpreterCreate failed");
            unload_locked();
            return false;
        }

        TfLiteStatus s = TfLiteInterpreterAllocateTensors(interp_);
        if (s != kTfLiteOk) {
            LOG_ERROR("TFLite: AllocateTensors failed (%d)", s);
            unload_locked();
            return false;
        }

        LOG_INFO("Model loaded via TFLite C API: %s", model_path_.c_str());
        return true;
    }

    bool load() {
        std::lock_guard<std::mutex> lk(lock_);
        return load_locked();
    }
};

// =============================================================================
// Normalization config — loaded from model_config.json (written by make_model.py)
// =============================================================================

struct NormConfig {
    float mean = 0.0f;
    float std  = 1.0f;
    bool  loaded = false;

    float normalize(float t) const { return (t - mean) / std; }
};

// Minimal JSON parser: extract "mean" and "std" from model_config.json.
static NormConfig load_norm_config(const char *path) {
    NormConfig cfg;
    auto data = read_file(path);
    if (data.empty()) return cfg;
    std::string json((const char *)data.data(), data.size());

    auto extract = [&](const char *key) -> float {
        std::string needle = std::string("\"") + key + "\"";
        size_t pos = json.find(needle);
        if (pos == std::string::npos) return 0.0f;
        pos = json.find(':', pos);
        if (pos == std::string::npos) return 0.0f;
        return std::stof(json.substr(pos + 1));
    };

    cfg.mean   = extract("mean");
    cfg.std    = extract("std");
    cfg.loaded = true;
    if (cfg.std < 1e-6f) cfg.std = 1.0f;
    LOG_INFO("Norm config: mean=%.3f  std=%.3f", cfg.mean, cfg.std);
    return cfg;
}

// =============================================================================
// AppState
// =============================================================================

enum class Mode { IDLE, TRAINING, INFERENCE };

struct AppState {
    Mode  mode         = Mode::IDLE;
    // Welford online algorithm for tracking training baseline
    int      z_n       = 0;
    double   z_mean    = 0.0;
    double   z_M2      = 0.0;
    // Training buffer
    std::vector<float> train_buf;
    // Sliding window (z-score normalized)
    float    window[kWindowSize] = {};
    // Runtime stats
    int      readings  = 0;
    float    last_temp = 0.0f;
    float    last_score= 0.0f;
    bool     anomaly   = false;

    void zscore_reset() { z_n = 0; z_mean = z_M2 = 0.0; }
    void zscore_update(float t) {
        ++z_n;
        double d = t - z_mean;
        z_mean += d / z_n;
        z_M2   += d * (t - z_mean);
    }
    double zscore_std() const {
        return z_n > 1 ? std::sqrt(z_M2 / (z_n - 1)) : 0.0;
    }
    void window_push(float v) {
        memmove(window, window + 1, (kWindowSize - 1) * sizeof(float));
        window[kWindowSize - 1] = v;
    }
};

// =============================================================================
// DemoApp — state machine + RPC handlers
// =============================================================================

class DemoApp : public RouterHandlers {
public:
    DemoApp() : model_(kModelPath) {
        norm_ = load_norm_config(kConfigPath);
    }

    // Called by RouterClient for each demo/tick RPC.
    std::string handle_tick(const std::string &temp_str) override {
        float temp_c;
        try { temp_c = std::stof(temp_str); }
        catch (...) { return ""; }
        LOG_INFO("Tick: %.2f C", temp_c);
        if (temp_c == kBadTemp) return "[SENSOR] Not connected.";

        Mode mode;
        {
            std::lock_guard<std::mutex> lk(lock_);
            state_.last_temp = temp_c;
            mode = state_.mode;
        }
        if (mode == Mode::TRAINING)  return training_tick(temp_c);
        if (mode == Mode::INFERENCE) return inference_tick(temp_c);
        return "";
    }

    // Called by RouterClient for each demo/cmd RPC, or FIFO thread.
    std::string handle_cmd(const std::string &cmd_str) override {
        if (cmd_str.empty()) return "";
        char ch = (char)tolower((unsigned char)cmd_str[0]);
        LOG_INFO("CMD: %c", ch);
        switch (ch) {
            case 't': return start_training();
            case 'i': return toggle_inference();
            case 'u': return do_update();
            case 's': return do_status();
            case '?': return menu();
            default:  return "";
        }
    }

private:
    AppState    state_;
    ModelRunner model_;
    NormConfig  norm_;
    std::mutex  lock_;      // protects state_ only; model_ has its own lock

    // ── TRAINING ──────────────────────────────────────────────────────────────

    std::string start_training() {
        std::lock_guard<std::mutex> lk(lock_);
        state_.mode = Mode::TRAINING;
        state_.train_buf.clear();
        state_.zscore_reset();
        memset(state_.window, 0, sizeof(state_.window));
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[TRAIN] Collecting %d samples. Keep sensor steady.", kMaxSamples);
        return buf;
    }

    std::string training_tick(float temp_c) {
        bool done;
        int  n;
        {
            std::lock_guard<std::mutex> lk(lock_);
            if ((int)state_.train_buf.size() >= kMaxSamples) return "";
            state_.train_buf.push_back(temp_c);
            state_.zscore_update(temp_c);
            n    = (int)state_.train_buf.size();
            done = (n >= kMaxSamples);
        }

        std::string out;
        if (n % 10 == 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "[TRAIN] %d/%d  %.2f C", n, kMaxSamples, temp_c);
            out += buf;
        }

        if (done) {
            float mean, std;
            std::vector<float> buf;
            {
                std::lock_guard<std::mutex> lk(lock_);
                buf   = state_.train_buf;
                mean  = (float)state_.z_mean;
                std   = (float)state_.zscore_std();
                state_.mode = Mode::IDLE;
            }
            // Save CSV to file for model generation
            FILE *fp = fopen(kCsvPath, "w");
            if (fp) {
                fprintf(fp, "temp_c\n");
                for (float v : buf) fprintf(fp, "%.4f\n", v);
                fclose(fp);
                LOG_INFO("Training CSV saved to %s", kCsvPath);
            } else {
                LOG_WARN("Failed to save CSV: %s", strerror(errno));
            }
            char footer[256];
            snprintf(footer, sizeof(footer),
                     "[TRAIN] Done. Baseline: mean=%.2f std=%.2f C\n"
                     "[TRAIN] CSV saved to %s\n"
                     "[TRAIN] Run: python3 make_model.py training_data.csv",
                     mean, std, kCsvPath);
            out += footer;
        }
        return out;
    }

    // ── INFERENCE ─────────────────────────────────────────────────────────────

    std::string toggle_inference() {
        std::lock_guard<std::mutex> lk(lock_);
        if (state_.mode == Mode::INFERENCE) {
            state_.mode = Mode::IDLE;
            char buf[64];
            snprintf(buf, sizeof(buf), "[INFER] Stopped. Readings: %d", state_.readings);
            return buf;
        }
        state_.mode = Mode::INFERENCE;
        state_.readings = 0;
        memset(state_.window, 0, sizeof(state_.window));
        std::string msg = "[INFER] Started. Using TFLite autoencoder.";
        if (!norm_.loaded)
            msg += "\n[INFER] Warning: no model_config.json — normalization may be wrong.";
        else {
            char nb[64];
            snprintf(nb, sizeof(nb), "\n[INFER] Baseline: mean=%.2f std=%.2f C",
                     norm_.mean, norm_.std);
            msg += nb;
        }
        return msg;
    }

    std::string inference_tick(float temp_c) {
        // Snapshot window under lock, then infer without holding it.
        float window_snap[kWindowSize];
        {
            std::lock_guard<std::mutex> lk(lock_);
            state_.readings++;
            state_.window_push(norm_.normalize(temp_c));
            memcpy(window_snap, state_.window, sizeof(window_snap));
        }

        float mse = model_.infer(window_snap);  // model_ has its own lock
        if (mse < 0.0f)
            return "[INFER] No model loaded. Push a model and send 'u' to apply.";

        bool anom = mse > kAnomalyMse;
        {
            std::lock_guard<std::mutex> lk(lock_);
            state_.last_score = mse;
            state_.anomaly    = anom;
        }

        char line[128];
        snprintf(line, sizeof(line), "TEMP %.2f C  score=%.4f%s",
                 temp_c, mse, anom ? "  *** ANOMALY ***" : "");
        LOG_INFO("%s", line);
        return line;
    }

    // ── UPDATE ────────────────────────────────────────────────────────────────

    std::string do_update() {
        struct stat st;
        if (::stat(kPatchPath, &st) != 0)
            return std::string("[UPDATE] No pending patch found.\n"
                               "[UPDATE] Push a patch from your PC:\n"
                               "[UPDATE]   adb push patch.tmd ") + kPatchPath +
                               "\n[UPDATE] Then send 'u' again.";

        LOG_INFO("Applying patch from %s", kPatchPath);
        bool ok = model_.apply_patch(kPatchPath);  // model_ has its own lock
        ::remove(kPatchPath);

        // Reload normalization config (make_model.py may have updated it)
        norm_ = load_norm_config(kConfigPath);

        return ok ? "[UPDATE] Patch applied. New model active. Send 'i' to resume inference."
                  : "[UPDATE] Patch apply FAILED. Check logs.";
    }

    // ── STATUS / MENU ─────────────────────────────────────────────────────────

    std::string do_status() {
        const char *mode_str;
        int z_n, readings;
        float z_mean, z_std, last_temp, last_score;
        bool  anom;
        {
            std::lock_guard<std::mutex> lk(lock_);
            switch (state_.mode) {
                case Mode::TRAINING:  mode_str = "TRAINING";  break;
                case Mode::INFERENCE: mode_str = "INFERENCE"; break;
                default:              mode_str = "IDLE";       break;
            }
            z_n      = state_.z_n;
            z_mean   = (float)state_.z_mean;
            z_std    = (float)state_.zscore_std();
            readings = state_.readings;
            last_temp= state_.last_temp;
            last_score=state_.last_score;
            anom     = state_.anomaly;
        }
        bool model_ready = model_.loaded();
        struct stat ps;
        bool has_patch = (::stat(kPatchPath, &ps) == 0);

        char buf[512];
        snprintf(buf, sizeof(buf),
            "--- STATUS ----------------------------------------\n"
            "Mode         : %s\n"
            "Model        : %s\n"
            "Training n   : %d  mean=%.2f  std=%.2f C\n"
            "Readings     : %d\n"
            "Last temp    : %.2f C\n"
            "Last score   : %.4f\n"
            "Anomaly      : %s\n"
            "Pending patch: %s\n"
            "---------------------------------------------------",
            mode_str,
            model_ready ? "TFLite autoencoder" : "not loaded",
            z_n, (double)z_mean, (double)z_std,
            readings, (double)last_temp, (double)last_score,
            anom ? "YES ***" : "no",
            has_patch ? "YES" : "no");
        return buf;
    }

    std::string menu() {
        return std::string("Commands:  t=train  i=infer  u=update  s=status  ?=help\n"
                           "(Patch update: adb push patch.tmd ") + kPatchPath
             + ", then send 'u')";
    }
};

// =============================================================================
// main
// =============================================================================

// Background thread: read single-char commands from a named FIFO.
static void fifo_thread(DemoApp &app) {
    mkfifo(kCmdFifo, 0666);
    LOG_INFO("Command FIFO: %s", kCmdFifo);
    while (true) {
        int fd = open(kCmdFifo, O_RDONLY);
        if (fd < 0) { sleep(1); continue; }
        char c;
        while (read(fd, &c, 1) == 1) {
            if (c == '\n' || c == '\r' || c == ' ') continue;
            std::string cmd(1, c);
            std::string resp = app.handle_cmd(cmd);
            if (!resp.empty()) LOG_INFO("CMD response: %s", resp.c_str());
        }
        close(fd);
    }
}

int main() {
    LOG_INFO("TinyMLDelta demo app starting");
    LOG_INFO("Model  : %s", kModelPath);
    LOG_INFO("Socket : %s", kRouterSock);

    DemoApp      app;
    RouterClient client(kRouterSock, app);

    if (!client.connect_retry(10, 2)) {
        LOG_ERROR("Cannot connect to arduino-router after 10 attempts");
        return 1;
    }

    client.register_method("demo/tick");
    client.register_method("demo/cmd");

    // Start FIFO command thread for direct ADB control
    std::thread(fifo_thread, std::ref(app)).detach();

    LOG_INFO("Ready. Waiting for STM32 sensor data and commands...");
    client.run_forever();
    LOG_WARN("Service exiting — router connection lost");
    return 0;
}
