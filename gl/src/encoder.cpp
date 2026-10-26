#include "pyrowave_gl.h"
#include "gl_api.hpp"

#include "dwt.glsl.inl"
#include "quant.glsl.inl"
#include "pack.glsl.inl"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace {

char g_error[512];

void set_error(const char *msg)
{
    std::snprintf(g_error, sizeof(g_error), "%s", msg ? msg : "");
}

struct Buffer {
    GLuint id = 0;
    size_t bytes = 0;
};

Buffer make_buffer(size_t bytes, const void *data)
{
    Buffer b;
    b.bytes = bytes;
    gl.GenBuffers(1, &b.id);
    gl.BindBuffer(GL_SHADER_STORAGE_BUFFER, b.id);
    gl.BufferData(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(bytes), data, GL_DYNAMIC_DRAW);
    return b;
}

void free_buffer(Buffer &b)
{
    if (b.id) gl.DeleteBuffers(1, &b.id);
    b = {};
}

void zero_buffer(const Buffer &b)
{
    if (!b.id || b.bytes == 0) return;
    GLuint z = 0;
    gl.BindBuffer(GL_SHADER_STORAGE_BUFFER, b.id);
    gl.ClearBufferSubData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, 0, GLsizeiptr(b.bytes),
                          GL_RED_INTEGER, GL_UNSIGNED_INT, &z);
}

bool read_buffer(const Buffer &b, void *dst, size_t bytes)
{
    if (bytes > b.bytes) return false;
    gl.MemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    gl.Finish();
    gl.BindBuffer(GL_SHADER_STORAGE_BUFFER, b.id);
    void *p = gl.MapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, GLsizeiptr(bytes), GL_MAP_READ_BIT);
    if (p) {
        std::memcpy(dst, p, bytes);
        gl.UnmapBuffer(GL_SHADER_STORAGE_BUFFER);
        return true;
    }
    gl.GetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, GLsizeiptr(bytes), dst);
    return gl.GetError() == 0;
}

GLuint build_program(const char *src, std::string *log)
{
    GLuint sh = gl.CreateShader(GL_COMPUTE_SHADER);
    gl.ShaderSource(sh, 1, &src, nullptr);
    gl.CompileShader(sh);
    GLint ok = 0;
    gl.GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint n = 0;
        gl.GetShaderiv(sh, GL_INFO_LOG_LENGTH, &n);
        std::string msg(size_t(n > 0 ? n : 1), '\0');
        gl.GetShaderInfoLog(sh, n, nullptr, msg.data());
        if (log) *log = msg;
        gl.DeleteShader(sh);
        return 0;
    }
    GLuint prog = gl.CreateProgram();
    gl.AttachShader(prog, sh);
    gl.LinkProgram(prog);
    gl.DeleteShader(sh);
    gl.GetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint n = 0;
        gl.GetProgramiv(prog, GL_INFO_LOG_LENGTH, &n);
        std::string msg(size_t(n > 0 ? n : 1), '\0');
        gl.GetProgramInfoLog(prog, n, nullptr, msg.data());
        if (log) *log = msg;
        gl.DeleteProgram(prog);
        return 0;
    }
    return prog;
}

struct DwtPush {
    int32_t rx, ry;
    float ix, iy;
    int32_t ax, ay;
    uint32_t dc;
    float rs;
    uint32_t phase;
    uint32_t groups_x;
    uint32_t slot_stride;
    uint32_t coeff_pitch;
    uint32_t src_pitch;
    uint32_t src_float;
    uint32_t pad0, pad1;
};
static_assert(sizeof(DwtPush) == 64, "DwtPush");

struct QuantPush {
    int32_t rx, ry;
    int32_t bx, by;
    float ix, iy;
    float input_layer;
    float quant_resolution;
    int32_t block_offset;
    int32_t block_stride;
    float rdo;
    uint32_t coeff_pitch;
};
static_assert(sizeof(QuantPush) == 48, "QuantPush");

struct PackPush {
    int32_t rx, ry;
    int32_t b32x, b32y;
    int32_t b8x, b8y;
    uint32_t quant_code;
    uint32_t seq;
    int32_t off32, stride32;
    int32_t off8, stride8;
};
static_assert(sizeof(PackPush) == 48, "PackPush");

/* pyrowave_common.hpp. GCC and Clang pack these the same way the Vulkan decoder reads them. */
struct BitstreamPacket {
    uint32_t offset_u32;
    uint32_t num_words;
};
struct BitstreamHeader {
    uint16_t ballot;
    uint16_t payload_words : 12;
    uint16_t sequence : 3;
    uint16_t extended : 1;
    uint32_t quant_code : 8;
    uint32_t block_index : 24;
};
struct BitstreamSequenceHeader {
    uint32_t width_minus_1 : 14;
    uint32_t height_minus_1 : 14;
    uint32_t sequence : 3;
    uint32_t extended : 1;
    uint32_t total_blocks : 24;
    uint32_t code : 2;
    uint32_t chroma_resolution : 1;
    uint32_t color_primaries : 1;
    uint32_t transfer_function : 1;
    uint32_t ycbcr_transform : 1;
    uint32_t ycbcr_range : 1;
    uint32_t chroma_siting : 1;
};
static_assert(sizeof(BitstreamPacket) == 8, "BitstreamPacket");
static_assert(sizeof(BitstreamHeader) == 8, "BitstreamHeader");
static_assert(sizeof(BitstreamSequenceHeader) == 8, "BitstreamSequenceHeader");

uint8_t encode_quant_code(float decoder_q_scale)
{
    uint32_t v = 0;
    std::memcpy(&v, &decoder_q_scale, sizeof(v));
    int e = int((v >> 23) & 0xff) - 127 - 4;
    int m = int((v >> 20) & 0x7);
    e = -e;
    if (e < 0) e = 0;
    if (e > 20) e = 20;
    return uint8_t((e << 3) | m);
}

float decode_quant_code(uint8_t quant_code)
{
    int e = 4 - int(quant_code >> 3);
    int m = int(quant_code & 0x7);
    int shift = 20 + e;
    float mag = float(8 + m);
    if (shift >= 0) mag *= float(1 << shift);
    else mag /= float(1 << -shift);
    return mag / (8.0f * 1024.0f * 1024.0f);
}

float quant_resolution_for(int level, int component, int band)
{
    int bits = 8;
    if (band == 0) bits += 2;
    else if (band < 3) bits += 1;
    bits += level;
    if (component != 0) bits--;
    float quant_res = float(1 << bits);
    if (quant_res > 4096.0f) quant_res = 4096.0f;
    uint8_t code = encode_quant_code(1.0f / quant_res);
    float decoded = decode_quant_code(code);
    if (decoded == 0.0f) return quant_res;
    return 1.0f / decoded;
}

unsigned quant_code_for_band(int level, int component, int band)
{
    int bits = 8;
    if (band == 0) bits += 2;
    else if (band < 3) bits += 1;
    bits += level;
    if (component != 0) bits--;
    float quant_res = float(1 << bits);
    if (quant_res > 4096.0f) quant_res = 4096.0f;
    uint32_t v = 0;
    float inv = 1.0f / quant_res;
    std::memcpy(&v, &inv, sizeof(v));
    int e = int((v >> 23) & 0xff) - 127 - 4;
    int m = int((v >> 20) & 0x7);
    e = -e;
    if (e < 0) e = 0;
    if (e > 20) e = 20;
    return unsigned((e << 3) | m);
}

struct Band {
    Buffer coeff;
    int w = 0, h = 0, pitch = 0, pitch_h = 0;
    int level = 0, component = 0;
};

struct BandJob {
    int band_index = 0;
    int level = 0, component = 0, band = 0;
    int w = 0, h = 0, pitch = 0;
    int off8 = 0, stride8 = 0;
    int off32 = 0, stride32 = 0;
};

struct Device {
    char name[128];
};

struct Encoder {
    pwgl_device device = nullptr;
    int width = 0, height = 0;
    pwgl_chroma_subsampling chroma = PWGL_CHROMA_SUBSAMPLING_420;
    int aw = 0, ah = 0;
    int pw[3] = {}, ph[3] = {};

    GLuint dwt_prog = 0, quant_prog = 0, pack_prog = 0;
    GLuint ubo = 0;

    Buffer planes[3];
    Buffer scratch;
    Buffer dummy;
    Band bands[3][5];
    bool band_ok[3][5] = {};

    Buffer meta, stats, payload, quant, bitstream, meta_out;
    int total8 = 0, total32 = 0;
    size_t payload_bytes = 0, bitstream_bytes = 0, meta_bytes = 0;

    std::vector<uint8_t> host_bitstream;
    std::vector<uint8_t> host_meta;
    int block_count = 0;
    uint32_t sequence_code = 1;
};

GLuint g_ubo = 0;

void upload_ubo(const void *data, size_t bytes)
{
    gl.BindBuffer(GL_UNIFORM_BUFFER, g_ubo);
    gl.BufferSubData(GL_UNIFORM_BUFFER, 0, GLsizeiptr(bytes), data);
    gl.BindBufferBase(GL_UNIFORM_BUFFER, 0, g_ubo);
}

void barrier()
{
    gl.MemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
}

int align32(int v)
{
    int a = (v + 31) & ~31;
    return a < 128 ? 128 : a;
}

pwgl_result ensure_programs(Encoder *enc)
{
    if (enc->dwt_prog) return PWGL_SUCCESS;
    std::string log;
    enc->dwt_prog = build_program(kDwtComp, &log);
    if (!enc->dwt_prog) {
        set_error(log.c_str());
        return PWGL_ERROR_GENERIC;
    }
    enc->quant_prog = build_program(kQuantComp, &log);
    if (!enc->quant_prog) {
        set_error(log.c_str());
        return PWGL_ERROR_GENERIC;
    }
    enc->pack_prog = build_program(kPackComp, &log);
    if (!enc->pack_prog) {
        set_error(log.c_str());
        return PWGL_ERROR_GENERIC;
    }
    gl.GenBuffers(1, &g_ubo);
    gl.BindBuffer(GL_UNIFORM_BUFFER, g_ubo);
    gl.BufferData(GL_UNIFORM_BUFFER, 256, nullptr, GL_DYNAMIC_DRAW);
    enc->ubo = g_ubo;
    uint32_t zero = 0;
    enc->dummy = make_buffer(256, nullptr);
    zero_buffer(enc->dummy);
    (void)zero;
    return PWGL_SUCCESS;
}

pwgl_result upload_planes(Encoder *enc, const pwgl_cpu_buffer *buf)
{
    for (int c = 0; c < 3; c++) {
        int stride = int(buf->row_stride_in_bytes[c]);
        if (stride <= 0) stride = enc->pw[c];
        size_t tight = size_t(enc->pw[c]) * size_t(enc->ph[c]);
        size_t bytes = (tight + 3u) & ~size_t(3);
        std::vector<uint8_t> packed(bytes, 0);
        const auto *src = static_cast<const uint8_t *>(buf->data[c]);
        if (!src) {
            set_error("plane pointer is null");
            return PWGL_ERROR_INVALID_ARGUMENT;
        }
        for (int y = 0; y < enc->ph[c]; y++) {
            std::memcpy(packed.data() + size_t(y) * size_t(enc->pw[c]),
                        src + size_t(y) * size_t(stride), size_t(enc->pw[c]));
        }
        free_buffer(enc->planes[c]);
        enc->planes[c] = make_buffer(bytes, packed.data());
    }
    return PWGL_SUCCESS;
}

pwgl_result create_bands(Encoder *enc)
{
    const uint32_t tile = 20u * 64u;
    uint32_t gx = uint32_t(enc->aw / 32);
    uint32_t gy = uint32_t(enc->ah / 32);
    size_t floats = size_t(gx) * gy * tile * 2u * 2u;
    if (enc->scratch.bytes < floats * 4u) {
        free_buffer(enc->scratch);
        enc->scratch = make_buffer(floats * 4u, nullptr);
        zero_buffer(enc->scratch);
    }

    for (int c = 0; c < 3; c++) {
        int start = (enc->chroma == PWGL_CHROMA_SUBSAMPLING_420 && c > 0) ? 1 : 0;
        for (int l = 0; l < 5; l++) {
            if (l < start) {
                enc->band_ok[c][l] = false;
                continue;
            }
            int w = enc->aw >> (l + 1);
            int h = enc->ah >> (l + 1);
            int pitch = (w + 31) & ~31;
            int pitch_h = (h + 31) & ~31;
            size_t elems = size_t(4) * size_t(pitch) * size_t(pitch_h);
            if (!enc->band_ok[c][l] || enc->bands[c][l].w != w || enc->bands[c][l].h != h) {
                free_buffer(enc->bands[c][l].coeff);
                enc->bands[c][l].coeff = make_buffer(elems * 4u, nullptr);
                enc->bands[c][l].w = w;
                enc->bands[c][l].h = h;
                enc->bands[c][l].pitch = pitch;
                enc->bands[c][l].pitch_h = pitch_h;
                enc->bands[c][l].level = l;
                enc->bands[c][l].component = c;
                enc->band_ok[c][l] = enc->bands[c][l].coeff.id != 0;
            }
            zero_buffer(enc->bands[c][l].coeff);
        }
    }
    return PWGL_SUCCESS;
}

void dispatch_dwt(const DwtPush &pc, GLuint src_bytes, GLuint src_float, GLuint scratch, GLuint coeff,
                  GLuint gx, GLuint gy)
{
    for (uint32_t phase = 0; phase < 4; phase++) {
        DwtPush p = pc;
        p.phase = phase;
        upload_ubo(&p, sizeof(p));
        gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, src_bytes);
        gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, scratch);
        gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, coeff);
        gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, src_float);
        gl.DispatchCompute(gx, gy, 1);
        barrier();
    }
}

pwgl_result run_dwt(Encoder *enc)
{
    gl.UseProgram(enc->dwt_prog);
    const uint32_t tile = 20u * 64u;

    auto run = [&](GLuint bytes, GLuint floats, uint32_t src_pitch, uint32_t src_float,
                   const Band &dst, int rx, int ry, int ax, int ay, bool dc) {
        DwtPush pc = {};
        pc.rx = rx;
        pc.ry = ry;
        pc.ix = 1.0f / float(rx);
        pc.iy = 1.0f / float(ry);
        pc.ax = ax;
        pc.ay = ay;
        pc.dc = dc ? 1u : 0u;
        pc.rs = 1.0f;
        pc.groups_x = uint32_t((ax + 31) / 32);
        pc.slot_stride = pc.groups_x * uint32_t((ay + 31) / 32) * tile;
        pc.coeff_pitch = uint32_t(dst.pitch);
        pc.src_pitch = src_pitch;
        pc.src_float = src_float;
        GLuint gx = pc.groups_x;
        GLuint gy = GLuint((ay + 31) / 32);
        dispatch_dwt(pc, bytes, floats, enc->scratch.id, dst.coeff.id, gx, gy);
    };

    run(enc->planes[0].id, enc->dummy.id, uint32_t(enc->pw[0]), 0u, enc->bands[0][0],
        enc->pw[0], enc->ph[0], enc->aw, enc->ah, true);

    for (int l = 1; l < 5; l++) {
        for (int c = 0; c < 3; c++) {
            bool chroma_l1 = c > 0 && l == 1 && enc->chroma == PWGL_CHROMA_SUBSAMPLING_420;
            if (chroma_l1) {
                run(enc->planes[c].id, enc->dummy.id, uint32_t(enc->pw[c]), 0u, enc->bands[c][l],
                    enc->pw[c], enc->ph[c], enc->aw >> 1, enc->ah >> 1, true);
            } else {
                const Band &prev = enc->bands[c][l - 1];
                run(enc->dummy.id, prev.coeff.id, uint32_t(prev.pitch), 1u, enc->bands[c][l],
                    prev.w, prev.h, prev.w, prev.h, false);
            }
        }
    }
    return PWGL_SUCCESS;
}

std::vector<BandJob> build_jobs(Encoder *enc)
{
    std::vector<BandJob> jobs;
    int count8 = 0;
    int count32 = 0;
    for (int l = 4; l >= 0; --l) {
        for (int c = 0; c < 3; c++) {
            if (l == 0 && c > 0 && enc->chroma == PWGL_CHROMA_SUBSAMPLING_420) continue;
            if (!enc->band_ok[c][l]) continue;
            const Band &b = enc->bands[c][l];
            if (b.w <= 0 || b.h <= 0) continue;
            int b8x = (b.w + 7) / 8;
            int b8y = (b.h + 7) / 8;
            int b32x = (b.w + 31) / 32;
            int b32y = (b.h + 31) / 32;
            int first = (l == 4) ? 0 : 1;
            for (int band = first; band < 4; band++) {
                BandJob j;
                j.level = l;
                j.component = c;
                j.band = band;
                j.w = b.w;
                j.h = b.h;
                j.pitch = b.pitch;
                j.off8 = count8;
                j.stride8 = b8x;
                j.off32 = count32;
                j.stride32 = b32x;
                jobs.push_back(j);
            }
            count8 += b8x * b8y;
            count32 += b32x * b32y;
        }
    }
    enc->total8 = count8;
    enc->total32 = count32;
    return jobs;
}

pwgl_result alloc_coding(Encoder *enc, size_t cap)
{
    if (enc->total8 <= 0 || enc->total32 <= 0) {
        set_error("no coding blocks");
        return PWGL_ERROR_INVALID_ARGUMENT;
    }
    size_t meta_b = size_t(enc->total8) * 8u;
    size_t stats_b = size_t(enc->total8) * 64u;
    size_t payload = size_t(enc->aw) * size_t(enc->ah) * 8u + size_t(enc->total8) * 64u + 256u;
    payload = (payload + 15u) & ~size_t(15);
    size_t meta_out = size_t(enc->total32) * 8u;
    size_t bits = cap + meta_out;
    size_t floor_bits = size_t(enc->total32) * 640u * 4u;
    if (bits < floor_bits) bits = floor_bits;
    bits = (bits + 3u) & ~size_t(3);

    auto grow = [](Buffer &b, size_t n) {
        if (b.bytes == n && b.id) return;
        free_buffer(b);
        b = make_buffer(n, nullptr);
    };
    grow(enc->meta, meta_b);
    grow(enc->stats, stats_b);
    grow(enc->payload, payload);
    grow(enc->quant, size_t(enc->total32) * 4u);
    grow(enc->bitstream, bits);
    grow(enc->meta_out, meta_out);
    enc->payload_bytes = payload;
    enc->bitstream_bytes = bits;
    enc->meta_bytes = meta_out;
    return PWGL_SUCCESS;
}

pwgl_result run_quant(Encoder *enc, const std::vector<BandJob> &jobs)
{
    zero_buffer(enc->meta);
    zero_buffer(enc->stats);
    zero_buffer(enc->payload);
    gl.UseProgram(enc->quant_prog);
    for (const BandJob &j : jobs) {
        QuantPush pc = {};
        pc.rx = j.w;
        pc.ry = j.h;
        pc.bx = (j.w + 7) / 8;
        pc.by = (j.h + 7) / 8;
        pc.ix = 1.0f / float(j.w);
        pc.iy = 1.0f / float(j.h);
        pc.input_layer = float(j.band);
        pc.quant_resolution = quant_resolution_for(j.level, j.component, j.band);
        pc.block_offset = j.off8;
        pc.block_stride = j.stride8;
        pc.rdo = 1.0f / 256.0f;
        pc.coeff_pitch = uint32_t(j.pitch);
        upload_ubo(&pc, sizeof(pc));
        gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, enc->bands[j.component][j.level].coeff.id);
        gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, enc->meta.id);
        gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, enc->stats.id);
        gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, enc->payload.id);
        gl.DispatchCompute(GLuint(pc.bx), GLuint(pc.by), 1);
        barrier();
    }
    return PWGL_SUCCESS;
}

pwgl_result run_pack(Encoder *enc, const std::vector<BandJob> &jobs)
{
    /* Rate control stays at quant 0, matching pyrowave-dx11 v0.1.1. */
    zero_buffer(enc->quant);
    zero_buffer(enc->bitstream);
    zero_buffer(enc->meta_out);
    gl.UseProgram(enc->pack_prog);
    for (const BandJob &j : jobs) {
        PackPush pc = {};
        pc.rx = j.w;
        pc.ry = j.h;
        pc.b32x = (j.w + 31) / 32;
        pc.b32y = (j.h + 31) / 32;
        pc.b8x = (j.w + 7) / 8;
        pc.b8y = (j.h + 7) / 8;
        pc.quant_code = quant_code_for_band(j.level, j.component, j.band);
        pc.seq = enc->sequence_code;
        pc.off32 = j.off32;
        pc.stride32 = j.stride32;
        pc.off8 = j.off8;
        pc.stride8 = j.stride8;
        upload_ubo(&pc, sizeof(pc));
        gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, enc->bitstream.id);
        gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, enc->meta_out.id);
        gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, enc->meta.id);
        gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, enc->payload.id);
        gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, enc->quant.id);
        gl.DispatchCompute(GLuint((pc.b32x + 1) / 2), GLuint((pc.b32y + 1) / 2), 1);
        barrier();
    }
    return PWGL_SUCCESS;
}

int packetize_blocks(const Encoder *enc, size_t packet_boundary,
                     pwgl_packet *packets, uint8_t *output, size_t output_size)
{
    if (enc->block_count <= 0) return -1;
    if (enc->host_meta.size() < size_t(enc->block_count) * sizeof(BitstreamPacket)) return -1;
    const auto *meta = reinterpret_cast<const BitstreamPacket *>(enc->host_meta.data());
    const uint8_t *bits = enc->host_bitstream.data();
    const size_t bits_size = enc->host_bitstream.size();

    auto block_bytes = [&](int i, const uint8_t **src) -> size_t {
        size_t words = meta[i].num_words;
        if (!words) return 0;
        size_t off = size_t(meta[i].offset_u32) * sizeof(uint32_t);
        size_t nbytes = words * sizeof(uint32_t);
        if (off > bits_size || nbytes > bits_size - off) return size_t(-1);
        *src = bits + off;
        return nbytes;
    };

    uint32_t sequence = 0;
    const uint8_t *first = nullptr;
    size_t first_n = block_bytes(0, &first);
    if (first_n == size_t(-1)) return -1;
    if (first_n >= sizeof(BitstreamHeader) && first) {
        sequence = reinterpret_cast<const BitstreamHeader *>(first)->sequence;
    }

    if (output && output_size < sizeof(BitstreamSequenceHeader)) return -1;
    BitstreamSequenceHeader header = {};
    header.width_minus_1 = uint32_t(enc->width - 1);
    header.height_minus_1 = uint32_t(enc->height - 1);
    header.sequence = sequence;
    header.extended = 1;
    header.code = 0;
    size_t nonzero = 0;
    for (int i = 0; i < enc->block_count; i++) {
        if (meta[i].num_words) nonzero++;
    }
    header.total_blocks = uint32_t(nonzero);
    header.chroma_resolution = enc->chroma == PWGL_CHROMA_SUBSAMPLING_444 ? 1u : 0u;

    size_t num_packets = 0;
    size_t size_in_packet = 0;
    size_t packet_offset = 0;
    size_t output_offset = 0;
    if (output) std::memcpy(output, &header, sizeof(header));
    output_offset += sizeof(header);
    size_in_packet += sizeof(header);

    for (int i = 0; i < enc->block_count; i++) {
        const uint8_t *src = nullptr;
        size_t nbytes = block_bytes(i, &src);
        if (nbytes == size_t(-1)) return -1;
        if (!nbytes) continue;
        if (size_in_packet + nbytes > packet_boundary) {
            if (packets) packets[num_packets] = {packet_offset, size_in_packet};
            num_packets++;
            size_in_packet = 0;
            packet_offset = output_offset;
        }
        if (output) {
            if (output_offset + nbytes > output_size) return -1;
            std::memcpy(output + output_offset, src, nbytes);
        }
        output_offset += nbytes;
        size_in_packet += nbytes;
    }
    if (size_in_packet) {
        if (packets) packets[num_packets] = {packet_offset, size_in_packet};
        num_packets++;
    }
    return int(num_packets);
}

} // namespace

uint32_t pwgl_get_api_version(void) { return PWGL_API_VERSION; }

const char *pwgl_last_error(void) { return g_error; }

pwgl_result pwgl_create_device(const pwgl_device_create_info *info, pwgl_device *out)
{
    if (!info || !out || info->struct_size < sizeof(pwgl_device_create_info)) {
        set_error("device create info");
        return PWGL_ERROR_INVALID_ARGUMENT;
    }
    char renderer[128] = {};
    char err[256] = {};
    int allow_sw = (info->flags & 1u) ? 1 : 0;
    if (gl_init(info->adapter_index, allow_sw, renderer, sizeof(renderer), err, sizeof(err)) != 0) {
        set_error(err[0] ? err : "OpenGL init failed");
        gl_shutdown();
        return PWGL_ERROR_NO_GL;
    }
    auto *dev = new (std::nothrow) Device();
    if (!dev) return PWGL_ERROR_OUT_OF_HOST_MEMORY;
    std::snprintf(dev->name, sizeof(dev->name), "%s", renderer);
    *out = reinterpret_cast<pwgl_device>(dev);
    g_error[0] = 0;
    return PWGL_SUCCESS;
}

pwgl_result pwgl_device_get_adapter_info(pwgl_device dev_h, pwgl_adapter_info *out)
{
    auto *dev = reinterpret_cast<Device *>(dev_h);
    if (!dev || !out) return PWGL_ERROR_INVALID_ARGUMENT;
    std::memset(out, 0, sizeof(*out));
    std::snprintf(out->name, sizeof(out->name), "%s", dev->name);
    return PWGL_SUCCESS;
}

void pwgl_device_destroy(pwgl_device dev_h)
{
    delete reinterpret_cast<Device *>(dev_h);
    gl_shutdown();
    g_ubo = 0;
}

pwgl_result pwgl_encoder_create(const pwgl_encoder_create_info *info, pwgl_encoder *out)
{
    if (!info || !out || !info->device) return PWGL_ERROR_INVALID_ARGUMENT;
    if (info->width < 16 || info->height < 16) return PWGL_ERROR_INVALID_ARGUMENT;
    if ((info->width & 1) || (info->height & 1)) return PWGL_ERROR_INVALID_ARGUMENT;
    if (info->chroma != PWGL_CHROMA_SUBSAMPLING_420 && info->chroma != PWGL_CHROMA_SUBSAMPLING_444)
        return PWGL_ERROR_INVALID_ARGUMENT;

    auto *enc = new (std::nothrow) Encoder();
    if (!enc) return PWGL_ERROR_OUT_OF_HOST_MEMORY;
    enc->device = info->device;
    enc->width = info->width;
    enc->height = info->height;
    enc->chroma = info->chroma;
    enc->aw = align32(info->width);
    enc->ah = align32(info->height);
    enc->pw[0] = info->width;
    enc->ph[0] = info->height;
    if (info->chroma == PWGL_CHROMA_SUBSAMPLING_420) {
        enc->pw[1] = enc->pw[2] = info->width / 2;
        enc->ph[1] = enc->ph[2] = info->height / 2;
    } else {
        enc->pw[1] = enc->pw[2] = info->width;
        enc->ph[1] = enc->ph[2] = info->height;
    }
    pwgl_result hr = ensure_programs(enc);
    if (hr != PWGL_SUCCESS) {
        pwgl_encoder_destroy(reinterpret_cast<pwgl_encoder>(enc));
        return hr;
    }
    *out = reinterpret_cast<pwgl_encoder>(enc);
    return PWGL_SUCCESS;
}

pwgl_result pwgl_encoder_encode_cpu(pwgl_encoder enc_h, const pwgl_cpu_buffer *buf, const pwgl_rate_control *rc)
{
    auto *enc = reinterpret_cast<Encoder *>(enc_h);
    if (!enc || !buf || !rc) return PWGL_ERROR_INVALID_ARGUMENT;
    while (gl.GetError() != 0) {}
    if (buf->width != enc->width || buf->height != enc->height) {
        set_error("buffer size does not match the encoder");
        return PWGL_ERROR_INVALID_ARGUMENT;
    }
    if (enc->chroma == PWGL_CHROMA_SUBSAMPLING_420 && buf->format != PWGL_CPU_BUFFER_FORMAT_YUV420P) {
        set_error("420 encoder expects YUV420P");
        return PWGL_ERROR_INVALID_ARGUMENT;
    }
    if (enc->chroma == PWGL_CHROMA_SUBSAMPLING_444 && buf->format != PWGL_CPU_BUFFER_FORMAT_YUV444P) {
        set_error("444 encoder expects YUV444P");
        return PWGL_ERROR_INVALID_ARGUMENT;
    }

    pwgl_result hr = upload_planes(enc, buf);
    if (hr != PWGL_SUCCESS) return hr;
    hr = create_bands(enc);
    if (hr != PWGL_SUCCESS) return hr;
    hr = run_dwt(enc);
    if (hr != PWGL_SUCCESS) return hr;

    std::vector<BandJob> jobs = build_jobs(enc);
    hr = alloc_coding(enc, rc->maximum_bitstream_size);
    if (hr != PWGL_SUCCESS) return hr;
    hr = run_quant(enc, jobs);
    if (hr != PWGL_SUCCESS) return hr;
    hr = run_pack(enc, jobs);
    if (hr != PWGL_SUCCESS) return hr;

    enc->host_bitstream.resize(enc->bitstream_bytes);
    enc->host_meta.resize(enc->meta_bytes);
    if (!read_buffer(enc->bitstream, enc->host_bitstream.data(), enc->bitstream_bytes) ||
        !read_buffer(enc->meta_out, enc->host_meta.data(), enc->meta_bytes)) {
        set_error("readback failed");
        return PWGL_ERROR_GENERIC;
    }
    enc->block_count = enc->total32;
    enc->sequence_code = (enc->sequence_code + 1u) & 7u;
    if (enc->sequence_code == 0u) enc->sequence_code = 1u;

    GLenum ge = gl.GetError();
    if (ge != 0) {
        std::snprintf(g_error, sizeof(g_error), "GL error 0x%x after encode", unsigned(ge));
        return PWGL_ERROR_GENERIC;
    }
    return PWGL_SUCCESS;
}

pwgl_result pwgl_encoder_compute_num_packets(pwgl_encoder enc_h, size_t packet_boundary, size_t *num_packets)
{
    auto *enc = reinterpret_cast<Encoder *>(enc_h);
    if (!enc || !num_packets || packet_boundary < sizeof(BitstreamSequenceHeader))
        return PWGL_ERROR_INVALID_ARGUMENT;
    int count = packetize_blocks(enc, packet_boundary, nullptr, nullptr, 0);
    if (count < 0) return PWGL_ERROR_GENERIC;
    *num_packets = size_t(count);
    return PWGL_SUCCESS;
}

pwgl_result pwgl_encoder_packetize(pwgl_encoder enc_h, pwgl_packet *packets, size_t packet_boundary,
                                  size_t *num_packets, void *bitstream, size_t size)
{
    auto *enc = reinterpret_cast<Encoder *>(enc_h);
    if (!enc || !num_packets || !bitstream) return PWGL_ERROR_INVALID_ARGUMENT;
    if (packet_boundary < sizeof(BitstreamSequenceHeader)) return PWGL_ERROR_INVALID_ARGUMENT;
    int count = packetize_blocks(enc, packet_boundary, packets, static_cast<uint8_t *>(bitstream), size);
    if (count < 0) return PWGL_ERROR_GENERIC;
    *num_packets = size_t(count);
    return PWGL_SUCCESS;
}

void pwgl_encoder_destroy(pwgl_encoder enc_h)
{
    auto *enc = reinterpret_cast<Encoder *>(enc_h);
    if (!enc) return;
    if (enc->dwt_prog) gl.DeleteProgram(enc->dwt_prog);
    if (enc->quant_prog) gl.DeleteProgram(enc->quant_prog);
    if (enc->pack_prog) gl.DeleteProgram(enc->pack_prog);
    if (enc->ubo) {
        gl.DeleteBuffers(1, &enc->ubo);
        if (g_ubo == enc->ubo) g_ubo = 0;
    }
    for (int c = 0; c < 3; c++) free_buffer(enc->planes[c]);
    free_buffer(enc->scratch);
    free_buffer(enc->dummy);
    for (int c = 0; c < 3; c++)
        for (int l = 0; l < 5; l++) free_buffer(enc->bands[c][l].coeff);
    free_buffer(enc->meta);
    free_buffer(enc->stats);
    free_buffer(enc->payload);
    free_buffer(enc->quant);
    free_buffer(enc->bitstream);
    free_buffer(enc->meta_out);
    delete enc;
}
