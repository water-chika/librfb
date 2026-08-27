// rfb_recorder: connects to an RFB/VNC server and records the framebuffer
// updates it receives into an H.264-encoded video file (e.g. mp4), using
// FFmpeg's libavformat/libavcodec/libswscale. No GPU/Wayland dependency —
// everything here runs on the CPU so it can be used headlessly.

#include "rfb.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
}

#include <chrono>
#include <csignal>
#include <cstring>
#include <ctime>
#include <format>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

using cpp_helper::empty_configurable_class;
using cpp_helper::configure;

namespace {

volatile std::sig_atomic_t g_stop = 0;
void handle_stop_signal(int) {
    g_stop = 1;
}

// RFB protocol stack: identical to the one used by rfb_window_demo
// (window.cpp), minus everything Wayland/HIP/DRM related, since we only
// need the decoded BGRA framebuffer, not a displayed window.
template<typename T>
using add_recorder_rfb =
    rfb::add_rfb<
    rfb::add_process_framebuffer_update<
    rfb::add_decode_h264<
    rfb::add_yuv_to_rgb<
    rfb::add_zrle<
    rfb::init_rfb<
    rfb::add_server_cut_text<
    rfb::add_client_cut_text<
    rfb::add_set_encodings<
    rfb::set_supported_encodings_from_configure<
    rfb::add_set_format<
    rfb::add_server_init<
    rfb::add_client_init<
    rfb::add_connection<
    rfb::set_port<
    rfb::set_address<
    T
    >>>>>>>>>>>>>>>>
;

struct config : public cpp_helper::empty_configure {
    const char* address;
    uint16_t port;
    std::vector<uint32_t> supported_encodings;
};

using recorder_rfb = add_recorder_rfb<empty_configurable_class>;

// Splits "some/dir/name.mp4" into stem="some/dir/name" and ext=".mp4"
// (ext is empty if there's no '.' after the last '/'). Used to build
// per-segment file names like "some/dir/name_20260824_182322.mp4".
std::pair<std::string, std::string> split_stem_ext(const std::string& path) {
    auto slash = path.find_last_of('/');
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
        return {path, ""};
    }
    return {path.substr(0, dot), path.substr(dot)};
}

std::string make_segment_path(const std::string& output_path, bool segmented) {
    if (!segmented) {
        return output_path;
    }
    auto [stem, ext] = split_stem_ext(output_path);
    auto t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char timestamp[32];
    std::strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &tm);
    return stem + "_" + timestamp + (ext.empty() ? ".mp4" : ext);
}

// Encodes BGRA frames (as produced by librfb's framebuffer decoder) into an
// H.264 video file with a variable frame rate: each frame's presentation
// timestamp is the real wall-clock time (in milliseconds) since recording
// started, so playback speed matches real time regardless of how
// irregularly the RFB server sends updates.
class video_writer {
public:
    video_writer(const char* path, int width, int height) {
        avformat_alloc_output_context2(&fmt_ctx, nullptr, nullptr, path);
        if (!fmt_ctx) {
            throw std::runtime_error("could not create output context for path: " + std::string(path));
        }
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!codec) {
            throw std::runtime_error("h264 encoder not found (is a libx264-enabled ffmpeg installed?)");
        }
        stream = avformat_new_stream(fmt_ctx, nullptr);
        if (!stream) {
            throw std::runtime_error("could not create output stream");
        }
        codec_ctx = avcodec_alloc_context3(codec);
        if (!codec_ctx) {
            throw std::runtime_error("could not allocate codec context");
        }
        codec_ctx->width = width;
        codec_ctx->height = height;
        // Millisecond time base to support the variable frame rate coming
        // from RFB framebuffer updates (they don't arrive on a fixed clock).
        codec_ctx->time_base = AVRational{1, 1000};
        codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        codec_ctx->gop_size = 60;
        if (fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
            codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        av_opt_set(codec_ctx->priv_data, "preset", "veryfast", 0);
        av_opt_set(codec_ctx->priv_data, "tune", "zerolatency", 0);
        if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
            throw std::runtime_error("could not open h264 encoder");
        }
        avcodec_parameters_from_context(stream->codecpar, codec_ctx);
        stream->time_base = codec_ctx->time_base;
        if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
            if (avio_open(&fmt_ctx->pb, path, AVIO_FLAG_WRITE) < 0) {
                throw std::runtime_error("could not open output file: " + std::string(path));
            }
        }
        if (avformat_write_header(fmt_ctx, nullptr) < 0) {
            throw std::runtime_error("could not write output file header");
        }

        sws_ctx = sws_getContext(
                width, height, AV_PIX_FMT_BGRA,
                width, height, AV_PIX_FMT_YUV420P,
                SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_ctx) {
            throw std::runtime_error("could not create BGRA->YUV420P scale context");
        }

        frame = av_frame_alloc();
        frame->format = AV_PIX_FMT_YUV420P;
        frame->width = width;
        frame->height = height;
        if (av_frame_get_buffer(frame, 32) < 0) {
            throw std::runtime_error("could not allocate frame buffer");
        }
        pkt = av_packet_alloc();

        start_time = std::chrono::steady_clock::now();
    }

    ~video_writer() {
        if (fmt_ctx) {
            if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE) && fmt_ctx->pb) {
                avio_closep(&fmt_ctx->pb);
            }
            avformat_free_context(fmt_ctx);
        }
        if (codec_ctx) {
            avcodec_free_context(&codec_ctx);
        }
        if (frame) {
            av_frame_free(&frame);
        }
        if (pkt) {
            av_packet_free(&pkt);
        }
        if (sws_ctx) {
            sws_freeContext(sws_ctx);
        }
    }

    void write_frame(const uint8_t* bgra_data, int width, int height) {
        const uint8_t* src_slices[1] = {bgra_data};
        int src_stride[1] = {width * 4};
        if (av_frame_make_writable(frame) < 0) {
            throw std::runtime_error("frame not writable");
        }
        sws_scale(sws_ctx, src_slices, src_stride, 0, height, frame->data, frame->linesize);
        auto now = std::chrono::steady_clock::now();
        frame->pts = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();
        encode(frame);
    }

    void finish() {
        encode(nullptr); // flush encoder
        av_write_trailer(fmt_ctx);
    }

private:
    void encode(AVFrame* input_frame) {
        int ret = avcodec_send_frame(codec_ctx, input_frame);
        if (ret < 0) {
            throw std::runtime_error("error sending frame to encoder");
        }
        while (ret >= 0) {
            ret = avcodec_receive_packet(codec_ctx, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            else if (ret < 0) {
                throw std::runtime_error("error receiving packet from encoder");
            }
            av_packet_rescale_ts(pkt, codec_ctx->time_base, stream->time_base);
            pkt->stream_index = stream->index;
            av_interleaved_write_frame(fmt_ctx, pkt);
            av_packet_unref(pkt);
        }
    }

    AVFormatContext* fmt_ctx{};
    AVStream* stream{};
    AVCodecContext* codec_ctx{};
    SwsContext* sws_ctx{};
    AVFrame* frame{};
    AVPacket* pkt{};
    std::chrono::steady_clock::time_point start_time{};
};

} // namespace

int main(int argc, const char* argv[]) {
  try {
    auto usage_string =
                "Usage: rfb_recorder <address> <port> <encodings> --output <path> [--segment-seconds <n>]\n"
                "--output <path>          output video file path (required, e.g. out.mp4)\n"
                "--segment-seconds <n>    (optional) rotate to a new output file every\n"
                "                         <n> seconds, for continuous/indefinite recording.\n"
                "                         <path> is then used as a prefix: files are named\n"
                "                         <stem>_<YYYYmmdd_HHMMSS><ext>, e.g. out_20260824_182322.mp4\n"
                "<encodings>              comma/space separated RFB encodings, by name or ID,\n"
                "                         e.g. h264,zrle,raw or 50,16,0\n"
                "                         (0=Raw, 16=ZRLE, 50=H264; default: 50,16,0)\n"
                ;
    if (argc < 3) {
        throw std::logic_error(usage_string);
    }
    const char* address = "127.0.0.1";
    uint16_t port = 5900;
    std::vector<uint32_t> encodings{};
    const char* output_path = nullptr;
    long segment_seconds = 0; // 0 = disabled, single continuous file
    for (int i = 1, pos_arg = 0; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == '-') {
            if (strcmp(&argv[i][2], "output") == 0) {
                output_path = argv[i+1];
                i += 1;
            }
            else if (strcmp(&argv[i][2], "segment-seconds") == 0) {
                segment_seconds = strtol(argv[i+1], NULL, 10);
                i += 1;
            }
            else {
                throw std::logic_error(usage_string);
            }
        }
        else {
            if (pos_arg == 0) {
                address = argv[i];
            }
            else if (pos_arg == 1) {
                port = strtol(argv[i], NULL, 10);
            }
            else if (pos_arg == 2) {
                auto parsed = rfb::parse_encodings(argv[i]);
                if (!parsed) {
                    throw std::runtime_error{
                        std::string{"unknown encoding: "} + argv[i]};
                }
                for (auto v : *parsed) {
                    encodings.emplace_back(
                        rfb::to_big_endian(static_cast<uint32_t>(v)));
                }
            }
            else {
                throw std::logic_error(usage_string);
            }
            pos_arg += 1;
        }
    }
    if (encodings.size() == 0) {
        encodings = {rfb::to_big_endian(50), rfb::to_big_endian(16), 0};
    }
    if (output_path == nullptr) {
        throw std::logic_error(usage_string);
    }

    std::signal(SIGINT, handle_stop_signal);
    std::signal(SIGTERM, handle_stop_signal);

    auto conf = config{
        .address = address, .port = port,
        .supported_encodings = std::move(encodings),
    };
    recorder_rfb client{conf};

    int width = client.get_width();
    int height = client.get_height();
    bool segmented = segment_seconds > 0;
    std::cerr << "connected: " << width << "x" << height
        << ", recording to " << output_path
        << (segmented
                ? std::format(" (rotating every {}s, Ctrl-C to stop)", segment_seconds)
                : " (Ctrl-C to stop)")
        << std::endl;

    auto open_segment = [&]() {
        auto path = make_segment_path(output_path, segmented);
        std::cerr << "starting segment: " << path << std::endl;
        return std::make_unique<video_writer>(path.c_str(), width, height);
    };

    auto writer = open_segment();
    auto segment_start = std::chrono::steady_clock::now();

    client.framebuffer_update_request(0, 0, width, height, false);
    while (!g_stop) {
        client.process_server_message();
        if (client.is_frame_updated()) {
            client.reset_frame_updated();
            auto frame_data = client.get_frame();
            writer->write_frame(frame_data.data(), width, height);
        }
        if (segmented) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - segment_start).count();
            if (elapsed >= segment_seconds) {
                writer->finish();
                std::cerr << "segment finished" << std::endl;
                writer = open_segment();
                segment_start = now;
            }
        }
        client.framebuffer_update_request(0, 0, width, height, true);
    }

    writer->finish();
    std::cerr << "recording finished" << std::endl;
  }
  catch (std::exception& e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }
  return 0;
}
