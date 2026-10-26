#include "pyrowave_gl.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

static double ms_since(std::chrono::steady_clock::time_point a)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
}

/* Time pwgl_encoder_encode_cpu + compute_num_packets + packetize.
 * Frame matches the 720p/1080p measurement: luma (x*3+y*5)&255, chroma 16/235 checkerboard. */
static int run_size(pwgl_device dev, int W, int H, int warmup, int iters)
{
    size_t cw = size_t(W / 2), ch = size_t(H / 2);
    std::vector<uint8_t> y(size_t(W) * size_t(H));
    std::vector<uint8_t> u(cw * ch), v(cw * ch);
    for (int y0 = 0; y0 < H; y0++) {
        for (int x = 0; x < W; x++)
            y[size_t(y0) * size_t(W) + x] = uint8_t((x * 3 + y0 * 5) & 255);
    }
    for (size_t y0 = 0; y0 < ch; y0++) {
        for (size_t x = 0; x < cw; x++) {
            u[y0 * cw + x] = ((x ^ y0) & 1u) ? uint8_t(16) : uint8_t(235);
            v[y0 * cw + x] = ((x + y0) & 1u) ? uint8_t(16) : uint8_t(235);
        }
    }

    pwgl_encoder_create_info ei = {};
    ei.device = dev;
    ei.width = W;
    ei.height = H;
    ei.chroma = PWGL_CHROMA_SUBSAMPLING_420;
    pwgl_encoder enc = nullptr;
    if (pwgl_encoder_create(&ei, &enc) != PWGL_SUCCESS) {
        std::fprintf(stderr, "encoder_create %dx%d: %s\n", W, H, pwgl_last_error());
        return 1;
    }

    pwgl_cpu_buffer buf = {};
    buf.data[0] = y.data();
    buf.data[1] = u.data();
    buf.data[2] = v.data();
    buf.row_stride_in_bytes[0] = size_t(W);
    buf.row_stride_in_bytes[1] = cw;
    buf.row_stride_in_bytes[2] = cw;
    buf.width = W;
    buf.height = H;
    buf.format = PWGL_CPU_BUFFER_FORMAT_YUV420P;

    const double scale = (double(W) * double(H)) / (1920.0 * 1080.0);
    size_t cap = size_t(400000.0 * scale);
    if (cap < 65536) cap = 65536;
    pwgl_rate_control rc = {};
    rc.maximum_bitstream_size = cap;

    std::vector<uint8_t> bits(size_t(W) * size_t(H) * 8u + 1048576u);
    const size_t boundary = 1200;
    double sum = 0;
    size_t nbytes = 0, np = 0;
    for (int i = 0; i < warmup + iters; i++) {
        auto t0 = std::chrono::steady_clock::now();
        if (pwgl_encoder_encode_cpu(enc, &buf, &rc) != PWGL_SUCCESS) {
            std::fprintf(stderr, "encode %dx%d: %s\n", W, H, pwgl_last_error());
            pwgl_encoder_destroy(enc);
            return 1;
        }
        size_t got_np = 0;
        if (pwgl_encoder_compute_num_packets(enc, boundary, &got_np) != PWGL_SUCCESS || got_np == 0) {
            std::fprintf(stderr, "num_packets %dx%d: %s\n", W, H, pwgl_last_error());
            pwgl_encoder_destroy(enc);
            return 1;
        }
        std::vector<pwgl_packet> packets(got_np);
        size_t got = 0;
        if (pwgl_encoder_packetize(enc, packets.data(), boundary, &got, bits.data(), bits.size()) != PWGL_SUCCESS) {
            std::fprintf(stderr, "packetize %dx%d: %s\n", W, H, pwgl_last_error());
            pwgl_encoder_destroy(enc);
            return 1;
        }
        double ms = ms_since(t0);
        if (i < warmup) continue;
        sum += ms;
        nbytes = 0;
        for (size_t p = 0; p < got; p++) nbytes += packets[p].size;
        np = got;
    }
    pwgl_encoder_destroy(enc);
    std::printf("%dx%d avg_ms=%.3f bytes=%zu packets=%zu\n", W, H, sum / double(iters), nbytes, np);
    return 0;
}

int main()
{
    pwgl_device_create_info di = {};
    di.struct_size = sizeof(di);
    di.adapter_index = -1;
    di.flags = 0;
    pwgl_device dev = nullptr;
    if (pwgl_create_device(&di, &dev) != PWGL_SUCCESS) {
        std::fprintf(stderr, "create_device: %s\n", pwgl_last_error());
        return 1;
    }
    pwgl_adapter_info info = {};
    pwgl_device_get_adapter_info(dev, &info);
    std::printf("renderer: %s\n", info.name);
    int rc = run_size(dev, 1280, 720, 2, 8);
    if (rc == 0) rc = run_size(dev, 1920, 1080, 1, 5);
    pwgl_device_destroy(dev);
    return rc;
}
