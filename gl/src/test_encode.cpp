#include "pyrowave_gl.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

static int fail(const char *what)
{
    std::fprintf(stderr, "FAIL %s: %s\n", what, pwgl_last_error());
    return 1;
}

static int encode_one(pwgl_device dev, int W, int H)
{
    std::vector<uint8_t> y(size_t(W) * size_t(H));
    std::vector<uint8_t> u(size_t(W) * size_t(H) / 4);
    std::vector<uint8_t> v(size_t(W) * size_t(H) / 4);
    for (int y0 = 0; y0 < H; y0++) {
        for (int x = 0; x < W; x++) {
            y[size_t(y0) * W + x] = uint8_t((x * 3 + y0 * 5) & 255);
        }
    }
    for (size_t i = 0; i < u.size(); i++) {
        u[i] = uint8_t(96 + (i & 31));
        v[i] = uint8_t(160 - (i & 15));
    }

    pwgl_encoder_create_info ei = {};
    ei.device = dev;
    ei.width = W;
    ei.height = H;
    ei.chroma = PWGL_CHROMA_SUBSAMPLING_420;
    pwgl_encoder enc = nullptr;
    if (pwgl_encoder_create(&ei, &enc) != PWGL_SUCCESS) return fail("encoder_create");

    pwgl_cpu_buffer buf = {};
    buf.data[0] = y.data();
    buf.data[1] = u.data();
    buf.data[2] = v.data();
    buf.row_stride_in_bytes[0] = size_t(W);
    buf.row_stride_in_bytes[1] = size_t(W / 2);
    buf.row_stride_in_bytes[2] = size_t(W / 2);
    buf.width = W;
    buf.height = H;
    buf.format = PWGL_CPU_BUFFER_FORMAT_YUV420P;

    pwgl_rate_control rc = {};
    rc.maximum_bitstream_size = size_t(W) * size_t(H);
    if (rc.maximum_bitstream_size < 65536) rc.maximum_bitstream_size = 65536;
    if (pwgl_encoder_encode_cpu(enc, &buf, &rc) != PWGL_SUCCESS) {
        pwgl_encoder_destroy(enc);
        return fail("encode");
    }

    const size_t boundary = 1200;
    size_t np = 0;
    if (pwgl_encoder_compute_num_packets(enc, boundary, &np) != PWGL_SUCCESS || np == 0) {
        pwgl_encoder_destroy(enc);
        return fail("num_packets");
    }
    std::vector<pwgl_packet> packets(np);
    std::vector<uint8_t> bits(rc.maximum_bitstream_size * 4u + 4096u);
    size_t got = 0;
    pwgl_result pr = pwgl_encoder_packetize(enc, packets.data(), boundary, &got, bits.data(), bits.size());
    pwgl_encoder_destroy(enc);
    if (pr != PWGL_SUCCESS || got != np) return fail("packetize");

    uint32_t w0 = 0, w1 = 0;
    std::memcpy(&w0, bits.data(), 4);
    std::memcpy(&w1, bits.data() + 4, 4);
    int width = int(w0 & 0x3fff) + 1;
    int height = int((w0 >> 14) & 0x3fff) + 1;
    int extended = int((w0 >> 31) & 1);
    int blocks = int(w1 & 0xffffff);
    std::printf("%dx%d packets=%zu header=%08x %08x decoded=%dx%d extended=%d blocks=%d first=%zu bytes\n",
                W, H, got, w0, w1, width, height, extended, blocks, packets[0].size);
    uint32_t b0 = 0, b1 = 0;
    std::memcpy(&b0, bits.data() + 8, 4);
    std::memcpy(&b1, bits.data() + 12, 4);
    unsigned words = (b0 >> 16) & 0xfffu;
    unsigned seq = (b0 >> 28) & 7u;
    unsigned block_index = b1 >> 8;
    std::printf("  block0 words=%u seq=%u index=%u\n", words, seq, block_index);
    if (words < 2 || words > 640 || seq == 0) {
        std::fprintf(stderr, "first block header is not a PyroWave packet\n");
        return 1;
    }
    if (width != W || height != H || extended != 1 || blocks <= 0) {
        std::fprintf(stderr, "sequence header does not describe the frame\n");
        return 1;
    }
    size_t payload = 0;
    for (size_t i = 0; i < got; i++) payload += packets[i].size;
    if (payload <= 8) {
        std::fprintf(stderr, "bitstream has no block payload\n");
        return 1;
    }
    return 0;
}

int main()
{
    pwgl_device_create_info di = {};
    di.struct_size = sizeof(di);
    di.adapter_index = -1;
    di.flags = 0;
    pwgl_device dev = nullptr;
    if (pwgl_create_device(&di, &dev) != PWGL_SUCCESS) return fail("create_device");
    pwgl_adapter_info info = {};
    pwgl_device_get_adapter_info(dev, &info);
    std::printf("renderer: %s\n", info.name);

    int rc = encode_one(dev, 64, 64);
    if (rc == 0) rc = encode_one(dev, 320, 240);
    pwgl_device_destroy(dev);
    return rc;
}
