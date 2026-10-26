/* Same measurement as pyrowave-dx11 tools/pwcmpbench.cpp, calling the
 * OpenGL encoder. Windows build (x64 Native Tools):
 *
 *   cl /nologo /O2 /EHsc /std:c++17 /MT /W4 /D_CRT_SECURE_NO_WARNINGS /DPWGL_BUILD_DLL ^
 *      /Iinclude /Isrc src\gl_api.cpp src\encoder.cpp /LD /Fe:build\pyrowave_gl.dll ^
 *      /link opengl32.lib user32.lib gdi32.lib
 *   cl /nologo /O2 /EHsc /std:c++17 /MT /W4 /D_CRT_SECURE_NO_WARNINGS ^
 *      /Iinclude src\cmp_bench.cpp /Fe:build\pwgl_cmp.exe /link psapi.lib
 *
 *   pwgl_cmp build\pyrowave_gl.dll 1280 720
 *   pwgl_cmp build\pyrowave_gl.dll 1920 1080
 *
 * 20 warmup frames, then at least 200 frames and 5 seconds, capped at
 * 30,000 frames. The frame is the pwbench synthetic I420 pattern. The rate
 * cap passed to the encoder is max(W*H*3/16, 32768). The packet buffer is
 * 64 MiB and the packet boundary is that whole buffer: the quant-0 pyramid
 * is larger than the cap plus 1 MiB.
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "pyrowave_gl.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/* Discrete GPU on an Optimus host. Must be exported by this exe. */
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

struct Stats {
    double min_v = 0, max_v = 0, sum = 0, sumsq = 0;
    int n = 0;
    void add(double x)
    {
        if (n == 0 || x < min_v) min_v = x;
        if (n == 0 || x > max_v) max_v = x;
        sum += x;
        sumsq += x * x;
        n++;
    }
    double avg() const { return n ? sum / n : 0; }
    double stdev() const
    {
        if (n < 2) return 0;
        double v = (sumsq - (sum * sum) / n) / (n - 1);
        if (v < 0) v = 0;
        return std::sqrt(v);
    }
};

static double qpc_ms()
{
    static LARGE_INTEGER freq = []() {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) * 1000.0 / double(freq.QuadPart);
}

static uint64_t filetime_100ns(FILETIME f)
{
    ULARGE_INTEGER u;
    u.LowPart = f.dwLowDateTime;
    u.HighPart = f.dwHighDateTime;
    return u.QuadPart;
}

static uint64_t process_cpu_100ns()
{
    FILETIME create_t, exit_t, kernel_t, user_t;
    if (!GetProcessTimes(GetCurrentProcess(), &create_t, &exit_t, &kernel_t, &user_t)) return 0;
    return filetime_100ns(kernel_t) + filetime_100ns(user_t);
}

struct MemSample {
    double cpu_machine_pct;
    double cpu_onecore_pct;
    double ws_mib;
    double priv_mib;
};

static int g_procs = 1;
static std::atomic<int> g_sample{0};
static std::mutex g_sample_mu;
static std::vector<MemSample> g_samples;

static void read_memory(double *ws_mib, double *priv_mib)
{
    PROCESS_MEMORY_COUNTERS_EX pmc;
    std::memset(&pmc, 0, sizeof(pmc));
    pmc.cb = sizeof(pmc);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof(pmc))) {
        *ws_mib = 0;
        *priv_mib = 0;
        return;
    }
    *ws_mib = double(pmc.WorkingSetSize) / (1024.0 * 1024.0);
    *priv_mib = double(pmc.PrivateUsage) / (1024.0 * 1024.0);
}

static void sampler_main()
{
    uint64_t prev_cpu = process_cpu_100ns();
    double prev_t = qpc_ms();
    while (g_sample.load(std::memory_order_relaxed)) {
        Sleep(50);
        if (!g_sample.load(std::memory_order_relaxed)) break;
        uint64_t now_cpu = process_cpu_100ns();
        double now_t = qpc_ms();
        double wall = now_t - prev_t;
        double onecore = 0;
        if (wall > 0) onecore = double(now_cpu - prev_cpu) / (wall * 10000.0) * 100.0;
        double ws = 0, priv = 0;
        read_memory(&ws, &priv);
        MemSample s;
        s.cpu_onecore_pct = onecore;
        s.cpu_machine_pct = onecore / double(g_procs);
        s.ws_mib = ws;
        s.priv_mib = priv;
        {
            std::lock_guard<std::mutex> lock(g_sample_mu);
            g_samples.push_back(s);
        }
        prev_cpu = now_cpu;
        prev_t = now_t;
    }
}

static void print_stats(const char *key, const Stats &s, const char *unit)
{
    std::printf("%s min=%.4f avg=%.4f max=%.4f std=%.4f n=%d unit=%s\n",
                key, s.min_v, s.avg(), s.max_v, s.stdev(), s.n, unit);
}

static std::vector<uint8_t> make_plane(int w, int h, uint32_t seed)
{
    std::vector<uint8_t> p(size_t(w) * size_t(h));
    uint32_t s = seed;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            s = s * 1664525u + 1013904223u;
            float v = 128.f + 60.f * std::sin(x * 0.031f + float(seed)) * std::cos(y * 0.017f) +
                      0.15f * float(x - w / 2) + (((x / 37) + (y / 23)) & 1 ? 25.f : -25.f) +
                      float(int(s >> 27)) - 16.f;
            if (v < 0.f) v = 0.f;
            if (v > 255.f) v = 255.f;
            p[size_t(y) * size_t(w) + size_t(x)] = uint8_t(v);
        }
    }
    return p;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        std::fprintf(stderr, "usage: pwgl_cmp <pyrowave_gl.dll> W H\n");
        return 2;
    }
    g_procs = int(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    if (g_procs < 1) g_procs = 1;
    HMODULE mod = LoadLibraryExA(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!mod) {
        std::fprintf(stderr, "LoadLibrary failed %lu\n", GetLastError());
        return 1;
    }
    auto create = (pwgl_result(__cdecl *)(const pwgl_device_create_info *, pwgl_device *))GetProcAddress(mod, "pwgl_create_device");
    auto adapter = (pwgl_result(__cdecl *)(pwgl_device, pwgl_adapter_info *))GetProcAddress(mod, "pwgl_device_get_adapter_info");
    auto destroy_dev = (void(__cdecl *)(pwgl_device))GetProcAddress(mod, "pwgl_device_destroy");
    auto enc_create = (pwgl_result(__cdecl *)(const pwgl_encoder_create_info *, pwgl_encoder *))GetProcAddress(mod, "pwgl_encoder_create");
    auto encode = (pwgl_result(__cdecl *)(pwgl_encoder, const pwgl_cpu_buffer *, const pwgl_rate_control *))GetProcAddress(mod, "pwgl_encoder_encode_cpu");
    auto numpk = (pwgl_result(__cdecl *)(pwgl_encoder, size_t, size_t *))GetProcAddress(mod, "pwgl_encoder_compute_num_packets");
    auto packetize = (pwgl_result(__cdecl *)(pwgl_encoder, pwgl_packet *, size_t, size_t *, void *, size_t))GetProcAddress(mod, "pwgl_encoder_packetize");
    auto enc_destroy = (void(__cdecl *)(pwgl_encoder))GetProcAddress(mod, "pwgl_encoder_destroy");
    auto last_err = (const char *(__cdecl *)(void))GetProcAddress(mod, "pwgl_last_error");
    if (!create || !adapter || !destroy_dev || !enc_create || !encode || !numpk || !packetize || !enc_destroy) {
        std::fprintf(stderr, "dll is missing an export\n");
        return 1;
    }

    int W = std::atoi(argv[2]);
    int H = std::atoi(argv[3]);
    pwgl_device_create_info di = {};
    di.struct_size = sizeof(di);
    di.adapter_index = -1;
    di.flags = 0;
    pwgl_device dev = nullptr;
    if (create(&di, &dev) != PWGL_SUCCESS) {
        std::fprintf(stderr, "create_device: %s\n", last_err ? last_err() : "");
        return 1;
    }
    pwgl_adapter_info info = {};
    adapter(dev, &info);
    std::printf("renderer: %s\n", info.name);
    std::fflush(stdout);

    pwgl_encoder_create_info ei = {};
    ei.device = dev;
    ei.width = W;
    ei.height = H;
    ei.chroma = PWGL_CHROMA_SUBSAMPLING_420;
    pwgl_encoder enc = nullptr;
    if (enc_create(&ei, &enc) != PWGL_SUCCESS) {
        std::fprintf(stderr, "encoder_create: %s\n", last_err ? last_err() : "");
        destroy_dev(dev);
        return 1;
    }

    auto y = make_plane(W, H, 1);
    auto cb = make_plane(W / 2, H / 2, 2);
    auto cr = make_plane(W / 2, H / 2, 3);
    pwgl_cpu_buffer buf = {};
    buf.data[0] = y.data();
    buf.data[1] = cb.data();
    buf.data[2] = cr.data();
    buf.row_stride_in_bytes[0] = size_t(W);
    buf.row_stride_in_bytes[1] = size_t(W / 2);
    buf.row_stride_in_bytes[2] = size_t(W / 2);
    buf.plane_size_in_bytes[0] = y.size();
    buf.plane_size_in_bytes[1] = cb.size();
    buf.plane_size_in_bytes[2] = cr.size();
    buf.width = W;
    buf.height = H;
    buf.format = PWGL_CPU_BUFFER_FORMAT_YUV420P;
    size_t budget = size_t(W) * size_t(H) * 3 / 16;
    if (budget < 32768) budget = 32768;
    pwgl_rate_control rc = {};
    rc.maximum_bitstream_size = budget;
    /* The quant-0 pyramid can be larger than budget + 1 MiB. The other
     * encoders fit in that buffer. This one gets 64 MiB, and the packet
     * boundary is that whole buffer. */
    std::vector<uint8_t> out(64u << 20);
    std::vector<pwgl_packet> pk(65536);

    auto step = [&](size_t *last) -> int {
        if (encode(enc, &buf, &rc) != PWGL_SUCCESS) return -1;
        size_t n = 0, np = 0;
        if (numpk(enc, out.size(), &n) != PWGL_SUCCESS) return -2;
        if (n > pk.size()) return -100;
        if (packetize(enc, pk.data(), out.size(), &np, out.data(), out.size()) != PWGL_SUCCESS) return -3;
        size_t bytes = 0;
        if (np && np <= pk.size()) bytes = pk[np - 1].offset + pk[np - 1].size;
        *last = bytes;
        return 0;
    };

    const int warmup = 20;
    const int min_frames = 200;
    const double min_sec = 5;
    const double max_sec = 60;
    const int max_frames = 30000;
    size_t last_bytes = 0;
    for (int i = 0; i < warmup; i++) {
        int r = step(&last_bytes);
        if (r != 0) {
            std::fprintf(stderr, "warmup failed frame %d rc=%d %s\n", i, r, last_err ? last_err() : "");
            return 1;
        }
    }
    Stats lat, cpu, onecore, ws, priv;
    g_sample.store(1);
    std::thread sampler(sampler_main);
    double t_begin = qpc_ms();
    int frames = 0;
    double seconds = 0;
    for (int i = 0; i < max_frames; i++) {
        double t0 = qpc_ms();
        int r = step(&last_bytes);
        double t1 = qpc_ms();
        if (r != 0) {
            std::fprintf(stderr, "encode failed frame %d rc=%d %s\n", i, r, last_err ? last_err() : "");
            g_sample.store(0);
            sampler.join();
            return 1;
        }
        lat.add(t1 - t0);
        frames++;
        seconds = (t1 - t_begin) / 1000.0;
        if (frames >= min_frames && seconds >= min_sec) break;
        if (seconds >= max_sec) break;
    }
    g_sample.store(0);
    sampler.join();
    {
        std::lock_guard<std::mutex> lock(g_sample_mu);
        for (const MemSample &s : g_samples) {
            cpu.add(s.cpu_machine_pct);
            onecore.add(s.cpu_onecore_pct);
            ws.add(s.ws_mib);
            priv.add(s.priv_mib);
        }
    }
    char host[256] = {};
    DWORD n = sizeof(host);
    GetComputerNameA(host, &n);
    std::printf("HOST %s procs=%d\n", host, g_procs);
    std::printf("RESULT backend=gl W=%d H=%d budget=%zu device=\"%s\" frames=%d seconds=%.3f last_bytes=%zu encode_rc=0 procs=%d\n",
                W, H, budget, info.name, frames, seconds, last_bytes, g_procs);
    print_stats("LAT_MS", lat, "ms");
    print_stats("CPU_MACHINE_PCT", cpu, "pct_machine");
    print_stats("CPU_ONECORE_PCT", onecore, "pct_onecore");
    print_stats("WS_MIB", ws, "MiB");
    print_stats("PRIV_MIB", priv, "MiB");
    enc_destroy(enc);
    destroy_dev(dev);
    return 0;
}
