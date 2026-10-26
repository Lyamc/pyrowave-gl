/* pyrowave_gl.h -- C ABI for the OpenGL PyroWave encoder.
 * Struct layouts match reference pyrowave.h where the records are shared
 * with the Vulkan decoder (packet, cpu buffer, rate control).
 */
#ifndef PYROWAVE_GL_H
#define PYROWAVE_GL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PWGL_API_VERSION 1

#if defined(_WIN32)
#  if defined(PWGL_BUILD_DLL)
#    define PWGL_API __declspec(dllexport)
#  else
#    define PWGL_API
#  endif
#else
#  if defined(PWGL_BUILD_DLL)
#    define PWGL_API __attribute__((visibility("default")))
#  else
#    define PWGL_API
#  endif
#endif

typedef enum pwgl_result {
    PWGL_SUCCESS = 0,
    PWGL_TIMEOUT = 1,
    PWGL_ERROR_GENERIC = -1,
    PWGL_ERROR_INVALID_ARGUMENT = -2,
    PWGL_ERROR_OUT_OF_HOST_MEMORY = -3,
    PWGL_ERROR_OUT_OF_DEVICE_MEMORY = -4,
    PWGL_ERROR_NO_GL = -5,
    PWGL_ERROR_NOT_IMPLEMENTED = -6,
    PWGL_ERROR_INT_MAX = 0x7fffffff
} pwgl_result;

/* pyrowave.h pyrowave_chroma_subsampling */
typedef enum pwgl_chroma_subsampling {
    PWGL_CHROMA_SUBSAMPLING_420 = 0,
    PWGL_CHROMA_SUBSAMPLING_444 = 1,
    PWGL_CHROMA_SUBSAMPLING_INT_MAX = 0x7fffffff
} pwgl_chroma_subsampling;

/* pyrowave.h pyrowave_cpu_buffer_format. 8-bit planar only in this build. */
typedef enum pwgl_cpu_buffer_format {
    PWGL_CPU_BUFFER_FORMAT_YUV420P = 1,
    PWGL_CPU_BUFFER_FORMAT_YUV444P = 2,
    PWGL_CPU_BUFFER_FORMAT_INT_MAX = 0x7fffffff
} pwgl_cpu_buffer_format;

typedef struct pwgl_device_t *pwgl_device;
typedef struct pwgl_encoder_t *pwgl_encoder;

/* pyrowave.h packet: two size_t fields. */
typedef struct pwgl_packet {
    size_t offset;
    size_t size;
} pwgl_packet;

/* pyrowave.h cpu buffer. 8-bit planes, tightly or strided. */
typedef struct pwgl_cpu_buffer {
    void *data[3];
    size_t row_stride_in_bytes[3];
    size_t plane_size_in_bytes[3];
    int width;
    int height;
    pwgl_cpu_buffer_format format;
} pwgl_cpu_buffer;

typedef struct pwgl_rate_control {
    size_t maximum_bitstream_size;
} pwgl_rate_control;

typedef struct pwgl_device_create_info {
    uint32_t struct_size;
    int32_t adapter_index; /* -1: first device with a GL 4.3 core context */
    uint32_t flags;        /* bit0: accept a software renderer */
} pwgl_device_create_info;

typedef struct pwgl_adapter_info {
    char name[128];
    uint32_t vendor_id;
    uint32_t device_id;
    uint64_t dedicated_video_memory;
} pwgl_adapter_info;

typedef struct pwgl_encoder_create_info {
    pwgl_device device;
    int width;
    int height;
    pwgl_chroma_subsampling chroma;
} pwgl_encoder_create_info;

#ifdef __cplusplus
static_assert(sizeof(pwgl_packet) == 16 || sizeof(void *) != 8, "pwgl_packet");
static_assert(sizeof(pwgl_rate_control) == 8 || sizeof(void *) != 8, "pwgl_rate_control");
static_assert(sizeof(pwgl_cpu_buffer) == 88 || sizeof(void *) != 8, "pwgl_cpu_buffer");
static_assert(sizeof(pwgl_adapter_info) == 144, "pwgl_adapter_info");
#endif

PWGL_API uint32_t pwgl_get_api_version(void);
PWGL_API const char *pwgl_last_error(void);

PWGL_API pwgl_result pwgl_create_device(const pwgl_device_create_info *info, pwgl_device *out);
PWGL_API pwgl_result pwgl_device_get_adapter_info(pwgl_device dev, pwgl_adapter_info *out);
PWGL_API void pwgl_device_destroy(pwgl_device dev);

PWGL_API pwgl_result pwgl_encoder_create(const pwgl_encoder_create_info *info, pwgl_encoder *out);
PWGL_API pwgl_result pwgl_encoder_encode_cpu(pwgl_encoder enc, const pwgl_cpu_buffer *buf,
                                            const pwgl_rate_control *rc);
PWGL_API pwgl_result pwgl_encoder_compute_num_packets(pwgl_encoder enc, size_t packet_boundary,
                                                     size_t *num_packets);
PWGL_API pwgl_result pwgl_encoder_packetize(pwgl_encoder enc, pwgl_packet *packets,
                                           size_t packet_boundary, size_t *num_packets,
                                           void *bitstream, size_t size);
PWGL_API void pwgl_encoder_destroy(pwgl_encoder enc);

#ifdef __cplusplus
}
#endif

#endif /* PYROWAVE_GL_H */
