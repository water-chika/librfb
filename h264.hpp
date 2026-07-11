#pragma once

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
                    for (int x_ = 0; x_ < width; x_++) {
                        for (int y_ = 0; y_ < height; y_++) {
                            uint8_t Y = frame->data[0][y_*frame->linesize[0] + x_];
                            uint8_t Cb = frame->data[1][y_/2*frame->linesize[1] + x_/2];
                            uint8_t Cr = frame->data[2][y_/2*frame->linesize[2] + x_/2];
                            auto clamp = [](int v) {
                                if (v > 255) return 255;
                                else if (v < 0) return 0;
                                else return v;
                            };
                            auto R = clamp(Y + 1.402*(Cr-128));
                            auto G = clamp(Y - 0.34414*(Cb-128) - 0.71414*(Cr-128));
                            auto B = clamp(Y + 1.772*(Cb-128));
                            framebuffer[(sy+y_)*fb_width + (sx+x_)] =
                                (R<<(2*8)) |
                                (G<<(1*8)) |
                                (B<<(0*8)) |
                                0;
                        }
                    }
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
                    for (int x_ = 0; x_ < width; x_++) {
                        for (int y_ = 0; y_ < height; y_++) {
                            uint8_t Y = frame->data[0][y_*frame->linesize[0] + x_];
                            uint8_t Cb = frame->data[1][y_/2*frame->linesize[1] + x_/2];
                            uint8_t Cr = frame->data[2][y_/2*frame->linesize[2] + x_/2];
                            auto clamp = [](int v) {
                                if (v > 255) return 255;
                                else if (v < 0) return 0;
                                else return v;
                            };
                            auto R = clamp(Y + 1.402*(Cr-128));
                            auto G = clamp(Y - 0.34414*(Cb-128) - 0.71414*(Cr-128));
                            auto B = clamp(Y + 1.772*(Cb-128));
                            framebuffer[(sy+y_)*fb_width + (sx+x_)] =
                                (R<<(2*8)) |
                                (G<<(1*8)) |
                                (B<<(0*8)) |
                                0;
                        }
                    }
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
