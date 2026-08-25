#pragma once

#include <numeric>
#include <execution>

extern "C" {
#include <libavcodec/avcodec.h>
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
template<typename T>
class add_yuv_to_rgb : public T {
public:
    using parent = T;
    add_yuv_to_rgb(const configure auto& conf) : parent{conf} {
    }
    struct yuv_to_rgb_coefficients {
        float y_scale;
        float y_offset;
        float r_cr;
        float g_cb;
        float g_cr;
        float b_cb;
    };
    // H.264 streams are limited range (Y 16-235, C 16-240) and use BT.709 for HD
    // resolutions, so the matrix has to come from the frame metadata rather than
    // being hardcoded to full range BT.601.
    static yuv_to_rgb_coefficients get_coefficients(const AVFrame* frame) {
        auto colorspace = frame->colorspace;
        if (colorspace == AVCOL_SPC_UNSPECIFIED) {
            colorspace = frame->height >= 720 ? AVCOL_SPC_BT709 : AVCOL_SPC_BT470BG;
        }
        double kr = 0.299, kb = 0.114;
        switch (colorspace) {
        case AVCOL_SPC_BT709:
            kr = 0.2126; kb = 0.0722; break;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            kr = 0.2627; kb = 0.0593; break;
        case AVCOL_SPC_SMPTE240M:
            kr = 0.212; kb = 0.087; break;
        default:
            break;
        }
        double kg = 1.0 - kr - kb;
        bool full_range = frame->color_range == AVCOL_RANGE_JPEG;
        double y_scale = full_range ? 1.0 : 255.0/219.0;
        double c_scale = full_range ? 1.0 : 255.0/224.0;
        return yuv_to_rgb_coefficients{
            .y_scale = static_cast<float>(y_scale),
            .y_offset = full_range ? 0.0f : 16.0f,
            .r_cr = static_cast<float>(2*(1-kr)*c_scale),
            .g_cb = static_cast<float>(2*(1-kb)*kb/kg*c_scale),
            .g_cr = static_cast<float>(2*(1-kr)*kr/kg*c_scale),
            .b_cb = static_cast<float>(2*(1-kb)*c_scale),
        };
    }
    void yuv_to_rgb(
            auto& framebuffer, auto fb_width, auto fb_height,
            auto& frame, auto width, auto height,
            auto sx, auto sy
            ) {
        auto coefficients = get_coefficients(frame);
        auto x_indices = std::vector<uint32_t>(width);
        std::iota(x_indices.begin(), x_indices.end(), 0);
        auto y_indices = std::vector<uint32_t>(height);
        std::iota(y_indices.begin(), y_indices.end(), 0);
        std::for_each(std::execution::par, y_indices.begin(), y_indices.end(),
                [&x_indices, frame=frame, &framebuffer, fb_width, sx, sy, coefficients](auto y_) {
                    std::for_each(std::execution::seq, x_indices.begin(), x_indices.end(),
                            [y_, frame, &framebuffer, fb_width, sx, sy, coefficients](auto x_) {
                                uint8_t Y = frame->data[0][y_*frame->linesize[0] + x_];
                                uint8_t Cb = frame->data[1][y_/2*frame->linesize[1] + x_/2];
                                uint8_t Cr = frame->data[2][y_/2*frame->linesize[2] + x_/2];
                                auto clamp = [](float v) {
                                    if (v > 255.0f) return 255;
                                    else if (v < 0.0f) return 0;
                                    else return static_cast<int>(v + 0.5f);
                                };
                                float y = (Y - coefficients.y_offset) * coefficients.y_scale;
                                float cb = Cb - 128.0f;
                                float cr = Cr - 128.0f;
                                auto R = clamp(y + coefficients.r_cr*cr);
                                auto G = clamp(y - coefficients.g_cb*cb - coefficients.g_cr*cr);
                                auto B = clamp(y + coefficients.b_cb*cb);
                                framebuffer[(sy+y_)*fb_width + (sx+x_)] =
                                    (R<<(2*8)) |
                                    (G<<(1*8)) |
                                    (B<<(0*8)) |
                                    0;
                            }
                            );
                }
        );
    }
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
                if (frame->format == AV_PIX_FMT_YUV420P) {
                    parent::yuv_to_rgb(framebuffer, fb_width, fb_height,
                            frame, width, height,
                            sx, sy);
                }
            }
        }
        std::cout << "h264_decode end" << std::endl;
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
        //if (packet)
            //av_packet_free(&packet);
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
                if (frame->format == AV_PIX_FMT_YUV420P) {
                    parent::yuv_to_rgb(framebuffer, fb_width, fb_height,
                            frame, width, height,
                            sx, sy);
                }
            }
        }
        std::cout << "h264_decode end" << std::endl;
    }
 private:
    const AVCodec* codec;
    AVCodecContext* context;
    AVPacket* packet;
    AVFrame* frame;
};
}
