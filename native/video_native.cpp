#include <quidra/native_extension.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}

namespace {

constexpr int dtype_int64 = QCORE_DTYPE_INT64;
constexpr int dtype_int8 = QCORE_DTYPE_INT8;
constexpr int dtype_int16 = QCORE_DTYPE_INT16;
constexpr int dtype_int32 = QCORE_DTYPE_INT32;
constexpr int dtype_uint8 = QCORE_DTYPE_UINT8;
constexpr int dtype_uint16 = QCORE_DTYPE_UINT16;
constexpr int dtype_uint32 = QCORE_DTYPE_UINT32;
constexpr int dtype_uint64 = QCORE_DTYPE_UINT64;
constexpr int dtype_float64 = QCORE_DTYPE_FLOAT64;
constexpr int dtype_float32 = QCORE_DTYPE_FLOAT32;

std::string ffmpeg_message(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    if (av_strerror(code, buffer, sizeof(buffer)) == 0) return buffer;
    return "FFmpeg error " + std::to_string(code);
}



struct VideoResource {
    const std::uint8_t* data{};
    std::int64_t size{};
};

struct AvioState {
    std::shared_ptr<VideoResource> resource;
    std::int64_t position{};
};

int video_read_packet(void* opaque, std::uint8_t* buffer, int requested) {
    auto* state = static_cast<AvioState*>(opaque);
    if (!state || !state->resource || !buffer || requested <= 0)
        return AVERROR(EINVAL);
    if (state->position >= state->resource->size) return AVERROR_EOF;
    const auto remaining = state->resource->size - state->position;
    const auto amount = static_cast<std::int64_t>(
        std::min<std::int64_t>(remaining, requested));
    if (amount <= 0) return AVERROR_EOF;
    std::memcpy(
        buffer,
        state->resource->data + static_cast<std::size_t>(state->position),
        static_cast<std::size_t>(amount));
    state->position += amount;
    return static_cast<int>(amount);
}

std::int64_t video_seek(void* opaque, std::int64_t offset, int whence) {
    auto* state = static_cast<AvioState*>(opaque);
    if (!state || !state->resource) return AVERROR(EINVAL);
    if (whence & AVSEEK_SIZE) return state->resource->size;
    const int origin = whence & ~AVSEEK_FORCE;
    std::int64_t base = 0;
    if (origin == SEEK_SET) base = 0;
    else if (origin == SEEK_CUR) base = state->position;
    else if (origin == SEEK_END) base = state->resource->size;
    else return AVERROR(EINVAL);
    if ((offset > 0 && base > std::numeric_limits<std::int64_t>::max() - offset) ||
        (offset < 0 && base < std::numeric_limits<std::int64_t>::min() - offset))
        return AVERROR(EINVAL);
    const auto target = base + offset;
    if (target < 0 || target > state->resource->size) return AVERROR(EINVAL);
    state->position = target;
    return target;
}

struct Session {
    AVFormatContext* format{};
    AVIOContext* io{};
    AvioState* io_state{};
    AVCodecContext* codec{};
    AVPacket* packet{};
    AVFrame* frame{};
    SwsContext* scaler{};
    int stream{-1};
    bool draining{};

    ~Session() {
        if (scaler) sws_freeContext(scaler);
        if (frame) av_frame_free(&frame);
        if (packet) av_packet_free(&packet);
        if (codec) avcodec_free_context(&codec);
        if (format) avformat_close_input(&format);
        if (io) {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
        delete io_state;
    }
};

std::unique_ptr<Session> open_session(
    const std::shared_ptr<VideoResource>& resource) {
    if (!resource || !resource->data || resource->size == 0)
        throw std::invalid_argument("invalid video source");
    auto session = std::make_unique<Session>();
    session->io_state = new AvioState{resource, 0};
    constexpr int io_buffer_size = 64 * 1024;
    auto* io_buffer = static_cast<unsigned char*>(
        av_malloc(static_cast<std::size_t>(io_buffer_size)));
    if (!io_buffer) throw std::bad_alloc();
    session->io = avio_alloc_context(
        io_buffer, io_buffer_size, 0, session->io_state,
        video_read_packet, nullptr, video_seek);
    if (!session->io) {
        av_free(io_buffer);
        throw std::bad_alloc();
    }
    session->format = avformat_alloc_context();
    if (!session->format) throw std::bad_alloc();
    session->format->pb = session->io;
    session->format->flags |= AVFMT_FLAG_CUSTOM_IO;

    int code = avformat_open_input(
        &session->format, nullptr, nullptr, nullptr);
    if (code < 0) {
        throw std::runtime_error("cannot open video: " + ffmpeg_message(code));
    }
    code = avformat_find_stream_info(session->format, nullptr);
    if (code < 0) {
        throw std::runtime_error(
            "cannot read video stream metadata: " + ffmpeg_message(code));
    }

#if LIBAVFORMAT_VERSION_MAJOR >= 59
    const AVCodec* decoder = nullptr;
    session->stream = av_find_best_stream(
        session->format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
#else
    // FFmpeg 4.4 / libavformat 58 predates the const-correct decoder_ret
    // signature introduced by FFmpeg 5. Keep one source compatible with both.
    AVCodec* legacy_decoder = nullptr;
    session->stream = av_find_best_stream(
        session->format, AVMEDIA_TYPE_VIDEO, -1, -1, &legacy_decoder, 0);
    const AVCodec* decoder = legacy_decoder;
#endif
    if (session->stream < 0 || !decoder) {
        throw std::runtime_error("video contains no decodable video stream");
    }

    session->codec = avcodec_alloc_context3(decoder);
    if (!session->codec) throw std::bad_alloc();
    code = avcodec_parameters_to_context(
        session->codec, session->format->streams[session->stream]->codecpar);
    if (code < 0) {
        throw std::runtime_error(
            "cannot configure video decoder: " + ffmpeg_message(code));
    }
    code = avcodec_open2(session->codec, decoder, nullptr);
    if (code < 0) {
        throw std::runtime_error(
            "cannot initialize video decoder: " + ffmpeg_message(code));
    }

    session->packet = av_packet_alloc();
    session->frame = av_frame_alloc();
    if (!session->packet || !session->frame) throw std::bad_alloc();
    return session;
}

int decode_next(Session& session) {
    for (;;) {
        const int received = avcodec_receive_frame(session.codec, session.frame);
        if (received == 0) return 1;
        if (received == AVERROR_EOF) return 0;
        if (received != AVERROR(EAGAIN)) {
            throw std::runtime_error(
                "video decode failed: " + ffmpeg_message(received));
        }

        bool supplied = false;
        while (!supplied) {
            const int read = av_read_frame(session.format, session.packet);
            if (read == AVERROR_EOF) {
                if (!session.draining) {
                    const int sent = avcodec_send_packet(session.codec, nullptr);
                    if (sent < 0 && sent != AVERROR_EOF) {
                        throw std::runtime_error(
                            "video decoder flush failed: " + ffmpeg_message(sent));
                    }
                    session.draining = true;
                    supplied = true;
                    continue;
                }
                return 0;
            }
            if (read < 0) {
                throw std::runtime_error(
                    "video demux read failed: " + ffmpeg_message(read));
            }
            if (session.packet->stream_index != session.stream) {
                av_packet_unref(session.packet);
                continue;
            }
            const int sent = avcodec_send_packet(session.codec, session.packet);
            av_packet_unref(session.packet);
            if (sent == AVERROR(EAGAIN)) {
                throw std::runtime_error(
                    "video decoder packet backpressure invariant failed");
            }
            if (sent < 0) {
                throw std::runtime_error(
                    "video packet decode failed: " + ffmpeg_message(sent));
            }
            supplied = true;
        }
    }
}


struct Metadata {
    long long width{};
    long long height{};
    double fps{-1.0};
    long long frames{-1};
    double duration{-1.0};
};

Metadata describe(const Session& session) {
    Metadata result;
    result.width = session.codec->width;
    result.height = session.codec->height;
    if (result.width <= 0 || result.height <= 0)
        throw std::runtime_error("video has invalid dimensions");
    AVStream* stream = session.format->streams[session.stream];
    AVRational rate = stream->avg_frame_rate;
    if (rate.num <= 0 || rate.den <= 0) rate = stream->r_frame_rate;
    if (rate.num > 0 && rate.den > 0) {
        const double value = av_q2d(rate);
        if (std::isfinite(value) && value > 0.0) result.fps = value;
    }
    if (stream->nb_frames > 0)
        result.frames = static_cast<long long>(stream->nb_frames);
    if (stream->duration != AV_NOPTS_VALUE && stream->duration >= 0) {
        const double value =
            static_cast<double>(stream->duration) * av_q2d(stream->time_base);
        if (std::isfinite(value) && value >= 0.0) result.duration = value;
    } else if (session.format->duration != AV_NOPTS_VALUE &&
               session.format->duration >= 0) {
        const double value =
            static_cast<double>(session.format->duration) /
            static_cast<double>(AV_TIME_BASE);
        if (std::isfinite(value) && value >= 0.0) result.duration = value;
    }
    return result;
}

void advance_to(Session& session, long long frame) {
    for (long long index = 0; index < frame; ++index) {
        av_frame_unref(session.frame);
        if (decode_next(session) != 1)
            throw std::out_of_range("video frame is beyond end of stream");
    }
}

std::shared_ptr<VideoResource> borrowed_resource(
    const void* data, unsigned long long size) {
    if (!data || size == 0 ||
        size > static_cast<unsigned long long>(
            std::numeric_limits<std::int64_t>::max())) {
        throw std::invalid_argument("video source bytes are empty or too large");
    }
    auto resource = std::make_shared<VideoResource>();
    resource->data = static_cast<const std::uint8_t*>(data);
    resource->size = static_cast<std::int64_t>(size);
    return resource;
}

std::size_t checked_product(std::size_t a, std::size_t b) {
    if (a != 0 && b > std::numeric_limits<std::size_t>::max() / a) {
        throw std::overflow_error("video frame size overflow");
    }
    return a * b;
}

int component_depth(const AVFrame& frame) {
    const auto* descriptor =
        av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame.format));
    if (!descriptor) return 8;
    int depth = 0;
    for (int index = 0; index < descriptor->nb_components; ++index) {
        depth = std::max(depth, static_cast<int>(descriptor->comp[index].depth));
    }
    return depth > 0 ? depth : 8;
}

template <typename SourceSample, typename TargetSample>
std::vector<std::uint8_t> convert_samples(
    const std::vector<std::uint8_t>& input) {
    if (input.size() % sizeof(SourceSample) != 0) {
        throw std::runtime_error("invalid decoded video sample storage");
    }
    const std::size_t count = input.size() / sizeof(SourceSample);
    std::vector<std::uint8_t> output(count * sizeof(TargetSample));
    for (std::size_t index = 0; index < count; ++index) {
        SourceSample value{};
        std::memcpy(
            &value, input.data() + index * sizeof(SourceSample),
            sizeof(SourceSample));
        if constexpr (std::is_integral_v<TargetSample>) {
            if (static_cast<unsigned long long>(value) >
                static_cast<unsigned long long>(
                    std::numeric_limits<TargetSample>::max())) {
                throw std::range_error(
                    "video dtype conversion would change an integer value");
            }
        }
        const TargetSample converted = static_cast<TargetSample>(value);
        std::memcpy(
            output.data() + index * sizeof(TargetSample),
            &converted, sizeof(TargetSample));
    }
    return output;
}

template <typename SourceSample>
std::vector<std::uint8_t> convert_to(
    const std::vector<std::uint8_t>& input, int dtype) {
    switch (dtype) {
        case dtype_int8: return convert_samples<SourceSample, std::int8_t>(input);
        case dtype_int16: return convert_samples<SourceSample, std::int16_t>(input);
        case dtype_int32: return convert_samples<SourceSample, std::int32_t>(input);
        case dtype_int64: return convert_samples<SourceSample, std::int64_t>(input);
        case dtype_uint8: return convert_samples<SourceSample, std::uint8_t>(input);
        case dtype_uint16: return convert_samples<SourceSample, std::uint16_t>(input);
        case dtype_uint32: return convert_samples<SourceSample, std::uint32_t>(input);
        case dtype_uint64: return convert_samples<SourceSample, std::uint64_t>(input);
        case dtype_float32: return convert_samples<SourceSample, float>(input);
        case dtype_float64: return convert_samples<SourceSample, double>(input);
        default: throw std::invalid_argument("unsupported video target dtype");
    }
}

std::vector<std::uint8_t> convert_dtype(
    const std::vector<std::uint8_t>& input, int source_dtype, int target_dtype) {
    if (source_dtype == target_dtype) return input;
    if (source_dtype == dtype_uint8) return convert_to<std::uint8_t>(input, target_dtype);
    if (source_dtype == dtype_uint16) return convert_to<std::uint16_t>(input, target_dtype);
    throw std::invalid_argument("unsupported decoded video dtype");
}

void require_extent(long long actual, long long expected, const char* axis) {
    if (expected < 0) return;
    if (actual != expected) {
        throw std::invalid_argument(
            std::string("video ") + axis +
            " does not match the expected tensor shape constraint");
    }
}



int fill_frame(Session& session, long long channels, void* output) {
    if (!output || qcore_native_abi_version() != QUIDRA_NATIVE_ABI_VERSION)
        return -2;
    if (channels != 1 && channels != 3 && channels != 4) return -3;
    if (qcore_tensor_device(output) != -1 ||
        !qcore_tensor_is_contiguous(output) ||
        qcore_tensor_rank(output) != 3) return -4;

    AVFrame& frame = *session.frame;
    const int depth = component_depth(frame);
    if (depth > 16) return -5;
    const int source_dtype = depth <= 8 ? dtype_uint8 : dtype_uint16;
    const std::size_t sample_bytes = source_dtype == dtype_uint8 ? 1U : 2U;
    const AVPixelFormat output_format =
        sample_bytes == 1
            ? (channels == 1 ? AV_PIX_FMT_GRAY8
                             : channels == 3 ? AV_PIX_FMT_RGB24
                                             : AV_PIX_FMT_RGBA)
            : (channels == 1 ? AV_PIX_FMT_GRAY16
                             : channels == 3 ? AV_PIX_FMT_RGB48
                                             : AV_PIX_FMT_RGBA64);

    session.scaler = sws_getCachedContext(
        session.scaler,
        frame.width, frame.height, static_cast<AVPixelFormat>(frame.format),
        frame.width, frame.height, output_format,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!session.scaler) return -6;

    const auto width = static_cast<std::size_t>(frame.width);
    const auto height = static_cast<std::size_t>(frame.height);
    const auto pixels = checked_product(width, height);
    const auto samples = checked_product(
        pixels, static_cast<std::size_t>(channels));
    const auto bytes = checked_product(samples, sample_bytes);
    const auto row_bytes = checked_product(
        checked_product(width, static_cast<std::size_t>(channels)), sample_bytes);
    if (row_bytes > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return -7;

    std::uint8_t* destination[4]{nullptr, nullptr, nullptr, nullptr};
    int destination_linesize[4]{0, 0, 0, 0};
    const int allocated = av_image_alloc(
        destination, destination_linesize,
        frame.width, frame.height, output_format, 64);
    if (allocated < 0) return -8;
    struct ImageBufferGuard {
        std::uint8_t** data{};
        ~ImageBufferGuard() {
            if (data && data[0]) av_freep(&data[0]);
        }
    } guard{destination};

    if (destination_linesize[0] < 0 ||
        static_cast<std::size_t>(destination_linesize[0]) < row_bytes)
        return -9;
    const int scaled = sws_scale(
        session.scaler, frame.data, frame.linesize, 0, frame.height,
        destination, destination_linesize);
    if (scaled != frame.height) return -10;

    std::vector<std::uint8_t> chw(bytes);
    for (std::size_t y = 0; y < height; ++y) {
        const auto* row = destination[0] +
            y * static_cast<std::size_t>(destination_linesize[0]);
        for (std::size_t x = 0; x < width; ++x) {
            for (std::size_t channel = 0;
                 channel < static_cast<std::size_t>(channels); ++channel) {
                const auto source =
                    (x * static_cast<std::size_t>(channels) + channel) *
                    sample_bytes;
                const auto target =
                    ((channel * height + y) * width + x) * sample_bytes;
                std::memcpy(chw.data() + target, row + source, sample_bytes);
            }
        }
    }

    const int final_dtype = qcore_tensor_dtype(output);
    if (final_dtype < dtype_int64 || final_dtype > dtype_float32) return -11;
    if (qcore_tensor_extent(output, 0) != channels ||
        qcore_tensor_extent(output, 1) != frame.height ||
        qcore_tensor_extent(output, 2) != frame.width) return -12;

    auto converted = convert_dtype(chw, source_dtype, final_dtype);
    void* output_data = qcore_tensor_cpu_data(output);
    if (!output_data) return -13;
    if (!converted.empty())
        std::memcpy(output_data, converted.data(), converted.size());
    return 0;
}

} // namespace

extern "C" std::int32_t video_native_probe(
    const void* data, unsigned long long size, void* metadata) {
    try {
        if (qcore_native_abi_version() != QUIDRA_NATIVE_ABI_VERSION ||
            !metadata || qcore_tensor_dtype(metadata) != QCORE_DTYPE_FLOAT64 ||
            qcore_tensor_device(metadata) != -1 ||
            !qcore_tensor_is_contiguous(metadata) ||
            qcore_tensor_rank(metadata) != 1 ||
            qcore_tensor_extent(metadata, 0) < 5) return -1;
        auto resource = borrowed_resource(data, size);
        auto session = open_session(resource);
        const auto info = describe(*session);
        auto* values = static_cast<double*>(qcore_tensor_cpu_data(metadata));
        if (!values) return -2;
        values[0] = static_cast<double>(info.width);
        values[1] = static_cast<double>(info.height);
        values[2] = info.fps;
        values[3] = static_cast<double>(info.frames);
        values[4] = info.duration;
        return 0;
    } catch (...) {
        return -3;
    }
}

extern "C" std::int32_t video_native_decode(
    const void* data, unsigned long long size,
    long long frame, long long channels, void* output) {
    try {
        if (frame < 0) return -1;
        auto resource = borrowed_resource(data, size);
        auto session = open_session(resource);
        advance_to(*session, frame);
        av_frame_unref(session->frame);
        if (decode_next(*session) == 0) return 1;
        return static_cast<std::int32_t>(
            fill_frame(*session, channels, output));
    } catch (const std::out_of_range&) {
        return 1;
    } catch (...) {
        return -20;
    }
}
