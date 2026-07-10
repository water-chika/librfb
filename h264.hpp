#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace rfb {

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
                 for (int x_ = 0; x_ < width; x_++) {
                     for (int y_ = 0; y_ < height; y_++) {
                         auto update = frame->data[0];
                         framebuffer[(sy+y_)*fb_width + (sx+x_)] =
                             //(static_cast<uint32_t>(frame->data[2][y_/2*frame->linesize[2] + x_/2]) << (2*8)) |
                             (static_cast<uint32_t>(frame->data[1][y_/2*frame->linesize[1] + x_/2]) << (1*8)) |
                             //(static_cast<uint32_t>(frame->data[0][y_*frame->linesize[0] + x_*frame->linesize[0]/width]) << (0*8)) |
                             0;
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
