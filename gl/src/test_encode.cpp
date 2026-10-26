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

/* Walk the packet record produced by pwgl_encoder_encode_cpu + packetize.
 * Sequence header layout matches pyrowave_common.hpp BitstreamSequenceHeader.
 * Each emitted block starts with BitstreamHeader: payload_words in bits 16..27
 * of the first word, sequence in bits 28..30.
 */
static int check_record(const char *label, int W, int H, int expect_chroma,
                        const std::vector<uint8_t> &bits, size_t nbytes, size_t packets)
{
    if (nbytes < 8) {
        std::fprintf(stderr, "%s: record shorter than a sequence header\n", label);
        return 1;
    }
    uint32_t w0 = 0, w1 = 0;
    std::memcpy(&w0, bits.data(), 4);
    std::memcpy(&w1, bits.data() + 4, 4);
    int width = int(w0 & 0x3fffu) + 1;
    int height = int((w0 >> 14) & 0x3fffu) + 1;
    int extended = int((w0 >> 31) & 1u);
    int blocks = int(w1 & 0xffffffu);
    int chroma = int((w1 >> 26) & 1u);
    std::printf("%s %dx%d packets=%zu width=%d height=%d chroma=%d blocks=%d extended=%d\n",
                label, W, H, packets, width, height, chroma, blocks, extended);
    if (width != W || height != H || chroma != expect_chroma || extended != 1 || blocks <= 0) {
        std::fprintf(stderr, "%s: sequence header does not describe the frame\n", label);
        return 1;
    }

    size_t off = 8;
    int seen = 0;
    while (off + 8 <= nbytes) {
        uint32_t b0 = 0;
        std::memcpy(&b0, bits.data() + off, 4);
        unsigned words = (b0 >> 16) & 0xfffu;
        unsigned seq = (b0 >> 28) & 7u;
        std::printf(" block words=%u seq=%u\n", words, seq);
        if (words < 2 || words > 640 || seq == 0) {
            std::fprintf(stderr, "%s: block %d header words=%u seq=%u\n", label, seen, words, seq);
            return 1;
        }
        size_t nbytes_block = size_t(words) * 4u;
        if (off + nbytes_block > nbytes) {
            std::fprintf(stderr, "%s: block %d overruns the record\n", label, seen);
            return 1;
        }
        off += nbytes_block;
        seen++;
    }
    if (off != nbytes || seen != blocks) {
        std::fprintf(stderr, "%s: walked %d blocks and %zu bytes, header says %d blocks and record is %zu\n",
                     label, seen, off, blocks, nbytes);
        return 1;
    }
    return 0;
}

static int encode_one(pwgl_device dev, int W, int H, pwgl_chroma_subsampling chroma)
{
    const bool is444 = chroma == PWGL_CHROMA_SUBSAMPLING_444;
    const char *label = is444 ? "444" : "420";
    size_t cw = is444 ? size_t(W) : size_t(W / 2);
    size_t ch = is444 ? size_t(H) : size_t(H / 2);
    std::vector<uint8_t> y(size_t(W) * size_t(H));
    std::vector<uint8_t> u(cw * ch);
    std::vector<uint8_t> v(cw * ch);
    for (int y0 = 0; y0 < H; y0++) {
        for (int x = 0; x < W; x++) {
            y[size_t(y0) * size_t(W) + size_t(x)] = uint8_t((x * 3 + y0 * 5) & 255);
        }
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
    ei.chroma = chroma;
    pwgl_encoder enc = nullptr;
    if (pwgl_encoder_create(&ei, &enc) != PWGL_SUCCESS) return fail("encoder_create");

    pwgl_cpu_buffer buf = {};
    buf.data[0] = y.data();
    buf.data[1] = u.data();
    buf.data[2] = v.data();
    buf.row_stride_in_bytes[0] = size_t(W);
    buf.row_stride_in_bytes[1] = cw;
    buf.row_stride_in_bytes[2] = cw;
    buf.width = W;
    buf.height = H;
    buf.format = is444 ? PWGL_CPU_BUFFER_FORMAT_YUV444P : PWGL_CPU_BUFFER_FORMAT_YUV420P;

    pwgl_rate_control rc = {};
    rc.maximum_bitstream_size = size_t(W) * size_t(H) * (is444 ? 3u : 2u);
    if (rc.maximum_bitstream_size < 65536) rc.maximum_bitstream_size = 65536;

    /* The shipped encode entry point. Packetize only slices the record it returns. */
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
    std::vector<uint8_t> bits(rc.maximum_bitstream_size * 4u + 65536u);
    size_t got = 0;
    pwgl_result pr = pwgl_encoder_packetize(enc, packets.data(), boundary, &got, bits.data(), bits.size());
    pwgl_encoder_destroy(enc);
    if (pr != PWGL_SUCCESS || got != np) return fail("packetize");

    size_t nbytes = 0;
    for (size_t i = 0; i < got; i++) nbytes += packets[i].size;
    return check_record(label, W, H, is444 ? 1 : 0, bits, nbytes, got);
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

    int rc = encode_one(dev, 64, 64, PWGL_CHROMA_SUBSAMPLING_420);
    if (rc == 0) rc = encode_one(dev, 64, 64, PWGL_CHROMA_SUBSAMPLING_444);
    pwgl_device_destroy(dev);
    if (rc == 0) std::printf("pass\n");
    return rc;
}
