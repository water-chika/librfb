#pragma once

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

namespace rfb {

template<typename T>
class add_avcodec_decoder : public T {
public:
    using parent = T;
    add_avcodec_decoder(const configure auto& conf) : parent{conf} {
        decoder = avcodec_find_decoder(AV_CODEC_ID_H264);
    }
    auto get_codec() {
        return decoder;
    }
private:
    const AVCodec* decoder;
};
template<typename T>
class add_avcodec_context : public T {
public:
    using parent = T;
    add_avcodec_context(const configure auto& conf) : parent{conf},
        context{avcodec_alloc_context3(parent::get_codec())}
    {
        if (avcodec_open2(context, parent::get_codec(), NULL) < 0) {
            throw std::runtime_error{"codec open fail"};
        }
    }
    ~add_avcodec_context() {
        if (context) avcodec_free_context(&context);
    }
    auto get_context() {
        return context;
    }
private:
    AVCodecContext* context;
};
// Converts the decoded YUV420P picture into the BGRA framebuffer.
//
// This used to be a hand written per pixel loop doing float arithmetic; on a
// 4K frame that cost several milliseconds of pure latency on every update.
// libswscale does the same job with SIMD, and being told the frame's real
// colorspace and range it also gets BT.601 vs BT.709 and limited vs full range
// right without the matrix having to be derived here.
//
// The conversion is cut into horizontal bands run in parallel. Each band also
// converts a few of its neighbours' rows and discards them, because the chroma
// upsample filter reads beyond the 2x2 group and a band converted in isolation
// would otherwise show a seam at its edges.
template<typename T>
class add_yuv_to_rgb : public T {
public:
    using parent = T;
    add_yuv_to_rgb(const configure auto& conf) : parent{conf} {
    }
    ~add_yuv_to_rgb() {
        release();
    }
    void yuv_to_rgb(
            auto& framebuffer, auto fb_width, auto fb_height,
            auto& frame, auto width, auto height,
            auto sx, auto sy
            ) {
        int w = static_cast<int>(width);
        int h = static_cast<int>(height);
        if (w <= 0 || h <= 0) {
            return;
        }
        prepare(frame, w, h);

        auto* out = reinterpret_cast<uint8_t*>(framebuffer);
        int out_stride = static_cast<int>(fb_width) * 4;
        auto convert_band = [&](band& entry) {
            const uint8_t* planes[4] = {
                frame->data[0] +
                    static_cast<size_t>(entry.y - entry.above) * frame->linesize[0],
                frame->data[1] +
                    static_cast<size_t>((entry.y - entry.above) / 2) * frame->linesize[1],
                frame->data[2] +
                    static_cast<size_t>((entry.y - entry.above) / 2) * frame->linesize[2],
                nullptr};
            // The band writes straight into the framebuffer at its own offset,
            // so only the overlap rows need somewhere else to go: they are
            // written above the destination and then overwritten by the band
            // that owns them, which is why each band converts into scratch.
            uint8_t* dst[4] = {entry.scratch.data(), nullptr, nullptr, nullptr};
            int dst_stride[4] = {m_scratch_stride, 0, 0, 0};
            sws_scale(entry.scaler, planes, frame->linesize, 0,
                      entry.above + entry.height + entry.below, dst, dst_stride);
            for (int row = 0; row < entry.height; row++) {
                std::memcpy(
                    out + static_cast<size_t>(sy + entry.y + row) * out_stride +
                        static_cast<size_t>(sx) * 4,
                    entry.scratch.data() +
                        static_cast<size_t>(entry.above + row) * m_scratch_stride,
                    static_cast<size_t>(m_width) * 4);
            }
        };
        if (m_bands.size() == 1) {
            convert_band(m_bands.front());
            return;
        }
        std::vector<std::thread> workers{};
        workers.reserve(m_bands.size() - 1);
        // Joins whatever was started even when a later thread cannot be
        // created: letting ~vector<thread> destroy a joinable thread calls
        // std::terminate, whereas a propagating exception is recoverable.
        struct joiner {
            std::vector<std::thread>& threads;
            ~joiner() {
                for (std::thread& worker : threads) {
                    if (worker.joinable()) {
                        worker.join();
                    }
                }
            }
        } join_all{workers};
        for (size_t i = 0; i + 1 < m_bands.size(); i++) {
            workers.emplace_back(convert_band, std::ref(m_bands[i]));
        }
        convert_band(m_bands.back());
    }
private:
    static constexpr int overlap_rows = 8;

    struct band {
        SwsContext* scaler{};
        std::vector<uint8_t> scratch{};
        int y{};
        int height{};
        int above{};
        int below{};
    };

    void release() {
        for (band& entry : m_bands) {
            if (entry.scaler != nullptr) {
                sws_freeContext(entry.scaler);
            }
        }
        m_bands.clear();
    }

    // Rebuilds the band set when the picture, its pixel format or its
    // colorimetry changes; otherwise the contexts are reused frame to frame.
    void prepare(const AVFrame* frame, int width, int height) {
        auto format = static_cast<AVPixelFormat>(frame->format);
        if (!m_bands.empty() && width == m_width && height == m_height &&
            format == m_format && frame->colorspace == m_colorspace &&
            frame->color_range == m_range) {
            return;
        }
        release();
        m_scratch_stride = 0;
        int scratch_stride = width * 4;

        // Unspecified is common in the wild; H264 at HD sizes means BT.709.
        auto colorspace = frame->colorspace;
        if (colorspace == AVCOL_SPC_UNSPECIFIED) {
            colorspace = height >= 720 ? AVCOL_SPC_BT709 : AVCOL_SPC_BT470BG;
        }
        int table = SWS_CS_ITU601;
        switch (colorspace) {
        case AVCOL_SPC_BT709: table = SWS_CS_ITU709; break;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL: table = SWS_CS_BT2020; break;
        case AVCOL_SPC_SMPTE240M: table = SWS_CS_SMPTE240M; break;
        default: break;
        }
        // YUVJ420P is the deprecated full range spelling; either way the range
        // is what color_range says, defaulting to the limited range every H264
        // screen stream uses.
        int src_range = (frame->color_range == AVCOL_RANGE_JPEG ||
                         format == AV_PIX_FMT_YUVJ420P)
                            ? 1
                            : 0;

        unsigned cores = std::thread::hardware_concurrency();
        int count = static_cast<int>(std::clamp<unsigned>(cores, 1u, 8u));
        constexpr int min_rows = 64;
        count = std::clamp(count, 1, std::max(1, height / min_rows));
        int rows = ((height + count - 1) / count + 1) & ~1;

        const int* coefficients = sws_getCoefficients(table);
        // Built to the side and committed only once whole. Recording the cache
        // key up front would, if an allocation below threw, leave a key
        // claiming to describe the entire picture next to a band list covering
        // only its top -- and every later frame would then silently convert
        // just that slice, leaving the rest of the framebuffer stale forever.
        std::vector<band> bands{};
        bands.reserve(static_cast<size_t>(count) + 1);
        try {
            for (int y = 0; y < height; y += rows) {
                band entry{};
                entry.y = y;
                entry.height = std::min(rows, height - y);
                entry.above = std::min(overlap_rows, y);
                entry.below =
                    std::min(overlap_rows, (height - (y + entry.height)) & ~1);
                int converted = entry.above + entry.height + entry.below;
                entry.scaler = sws_getContext(width, converted, format, width,
                                              converted, AV_PIX_FMT_BGRA,
                                              SWS_BILINEAR, nullptr, nullptr,
                                              nullptr);
                if (entry.scaler == nullptr) {
                    throw std::runtime_error{
                        "YUV to BGRA scaler creation fail"};
                }
                // Handed to the reserved vector before anything else can throw,
                // so the context is owned and cannot leak past the catch.
                bands.push_back(std::move(entry));
                band& stored = bands.back();
                sws_setColorspaceDetails(stored.scaler, coefficients, src_range,
                                         sws_getCoefficients(SWS_CS_DEFAULT), 1,
                                         0, 1 << 16, 1 << 16);
                stored.scratch.assign(
                    static_cast<size_t>(scratch_stride) * converted, 0);
            }
        } catch (...) {
            for (band& entry : bands) {
                if (entry.scaler != nullptr) {
                    sws_freeContext(entry.scaler);
                }
            }
            throw;
        }
        m_bands = std::move(bands);
        m_width = width;
        m_height = height;
        m_format = format;
        m_colorspace = frame->colorspace;
        m_range = frame->color_range;
        m_scratch_stride = scratch_stride;
    }

    std::vector<band> m_bands{};
    int m_width{};
    int m_height{};
    int m_scratch_stride{};
    AVPixelFormat m_format{AV_PIX_FMT_NONE};
    AVColorSpace m_colorspace{AVCOL_SPC_UNSPECIFIED};
    AVColorRange m_range{AVCOL_RANGE_UNSPECIFIED};
};

template<typename T>
class add_hw_decode_h264 : public T {
public:
    using parent = T;
    add_hw_decode_h264(const configure auto& conf) : parent{conf} {
        decoder = avcodec_find_decoder(AV_CODEC_ID_H264);
        context = avcodec_alloc_context3(decoder);
        config = avcodec_get_hw_config(decoder, 0);
        auto type = AV_HWDEVICE_TYPE_VULKAN;
        for (int i = 0;; i++) {
            config = avcodec_get_hw_config(decoder, i);
            if (!config) {
                fprintf(stderr, "Decoder %s does not support device type %s.\n",
                        decoder->name, av_hwdevice_get_type_name(type));
            }
            if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
                config->device_type == type) {
                //hw_pix_fmt = config->pix_fmt;
                break;
            }
        }
        context->get_format = get_hw_format;
        context->thread_type = FF_THREAD_SLICE;
        context->flags |= AV_CODEC_FLAG_LOW_DELAY;
        if (hw_decoder_init(context, config->device_type) < 0) {
            throw std::runtime_error{"hw decoder init fail"};
        }
        if (avcodec_open2(context, decoder, NULL) < 0) {
            throw std::runtime_error{"codec open fail"};
        }
        frame = av_frame_alloc();
    }
    int hw_decoder_init(AVCodecContext *ctx, const enum AVHWDeviceType type)
    {
        int err = 0;
        if ((err = av_hwdevice_ctx_create(&hw_device_ctx, type,
                                          NULL, NULL, 0)) < 0) {
            fprintf(stderr, "Failed to create specified HW device.\n");
            return err;
        }
        ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
        return err;
    }

    static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
                                            const enum AVPixelFormat *pix_fmts)
    {
        const enum AVPixelFormat *p;

        for (p = pix_fmts; *p != -1; p++) {
            if (*p == AV_PIX_FMT_YUV420P)
                return *p;
        }
        fprintf(stderr, "Failed to get HW surface format.\n");
        return AV_PIX_FMT_NONE;
    }
    void h264_decode(std::span<uint8_t> h264_data, int sx, int sy, int width, int height, std::span<uint8_t> frame_u8) {
        auto data = reinterpret_cast<uint8_t*>(av_malloc(h264_data.size()+AV_INPUT_BUFFER_PADDING_SIZE));
        auto data_size = h264_data.size();
        memcpy(data, h264_data.data(), data_size);
        memset(data+data_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        av_packet_from_data(packet, data, data_size);
        if (packet->size) {
            int ret = avcodec_send_packet(context, packet);
            if (ret < 0) {
                throw std::runtime_error{"h264 send packet fail"};
            }
            while (ret >= 0) {
                ret = avcodec_receive_frame(context, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    //throw std::runtime_error{"receive frame fail"};
                    return;
                }

                auto fb_width = parent::get_width();
                auto fb_height = parent::get_height();
                auto framebuffer = reinterpret_cast<uint32_t*>(frame_u8.data());
                // YUVJ420P is the deprecated full range spelling of the same
                // planar layout; ffmpeg still produces it for streams whose
                // VUI says full range, and it decodes identically here
                // because the range comes from frame->color_range.
                if (frame->format == AV_PIX_FMT_YUV420P ||
                    frame->format == AV_PIX_FMT_YUVJ420P) {
                    parent::yuv_to_rgb(framebuffer, fb_width, fb_height,
                            frame, width, height,
                            sx, sy);
                }
            }
        }
    }
private:
    const AVCodec* decoder;
    const AVCodecHWConfig* config;
    AVBufferRef *hw_device_ctx = NULL;
    AVCodecContext* context;
    AVPacket* packet;
    AVFrame* frame;
};
template<typename T>
class add_decode_h264 : public T {
public:
    using parent = T;
    add_decode_h264(const configure auto& conf) : parent{conf} {
        codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        context = avcodec_alloc_context3(codec);
        // Frame level threading holds finished pictures back by roughly the
        // thread count, which for a live stream means the display lags behind
        // and a session that starts with a single keyframe shows nothing at
        // all. Slice threading keeps the parallelism without the delay.
        context->thread_type = FF_THREAD_SLICE;
        context->flags |= AV_CODEC_FLAG_LOW_DELAY;
        if (avcodec_open2(context, codec, NULL) < 0) {
            throw std::runtime_error{"codec open fail"};
        }
        frame = av_frame_alloc();
        packet = av_packet_alloc();
    }
    ~add_decode_h264() {
        if (context)
            avcodec_free_context(&context);
        if (frame)
            av_frame_free(&frame);
        if (packet)
            av_packet_free(&packet);
    }
    void h264_decode(std::span<uint8_t> h264_data, int sx, int sy, int width, int height, std::span<uint8_t> frame_u8) {
        auto data = reinterpret_cast<uint8_t*>(av_malloc(h264_data.size()+AV_INPUT_BUFFER_PADDING_SIZE));
        auto data_size = h264_data.size();
        memcpy(data, h264_data.data(), data_size);
        memset(data+data_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        av_packet_from_data(packet, data, data_size);
        // avcodec_send_packet only takes a reference, so the buffer this
        // packet owns has to be released again on every path out of here --
        // otherwise the decoder leaks one compressed frame per update.
        struct packet_guard {
            AVPacket* p;
            ~packet_guard() { av_packet_unref(p); }
        } guard{packet};
        if (packet->size) {
            int ret = avcodec_send_packet(context, packet);
            if (ret < 0) {
                throw std::runtime_error{"h264 send packet fail"};
            }
            while (ret >= 0) {
                ret = avcodec_receive_frame(context, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    //throw std::runtime_error{"receive frame fail"};
                    return;
                }

                auto fb_width = parent::get_width();
                auto fb_height = parent::get_height();
                auto framebuffer = reinterpret_cast<uint32_t*>(frame_u8.data());
                // YUVJ420P is the deprecated full range spelling of the same
                // planar layout; ffmpeg still produces it for streams whose
                // VUI says full range, and it decodes identically here
                // because the range comes from frame->color_range.
                if (frame->format == AV_PIX_FMT_YUV420P ||
                    frame->format == AV_PIX_FMT_YUVJ420P) {
                    parent::yuv_to_rgb(framebuffer, fb_width, fb_height,
                            frame, width, height,
                            sx, sy);
                }
            }
        }
    }
 private:
    const AVCodec* codec;
    AVCodecContext* context;
    AVPacket* packet;
    AVFrame* frame;
};
}
