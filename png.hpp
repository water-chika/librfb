#pragma once

// Shared PNG writer. Lives in a header so the capture tool and the viewer's
// control port encode screenshots with exactly one implementation, rather
// than growing a second, subtly different one.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace rfb_png {

// Encodes the decoded BGRA framebuffer to a PNG file using libavcodec's PNG
// encoder, so no extra image library is needed.
inline void write_png_direct(const char* path, const uint8_t* bgra, int width, int height) {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (!codec) {
        throw std::runtime_error("png encoder not found in this ffmpeg build");
    }
    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        throw std::runtime_error("could not allocate png codec context");
    }
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    SwsContext* sws_ctx = nullptr;
    auto cleanup = [&]() {
        if (sws_ctx) sws_freeContext(sws_ctx);
        if (pkt) av_packet_free(&pkt);
        if (frame) av_frame_free(&frame);
        if (codec_ctx) avcodec_free_context(&codec_ctx);
    };
    try {
        codec_ctx->width = width;
        codec_ctx->height = height;
        codec_ctx->pix_fmt = AV_PIX_FMT_RGB24;
        codec_ctx->time_base = AVRational{1, 1};
        if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
            throw std::runtime_error("could not open png encoder");
        }
        frame = av_frame_alloc();
        frame->format = AV_PIX_FMT_RGB24;
        frame->width = width;
        frame->height = height;
        if (av_frame_get_buffer(frame, 32) < 0) {
            throw std::runtime_error("could not allocate png frame buffer");
        }
        sws_ctx = sws_getContext(
                width, height, AV_PIX_FMT_BGRA,
                width, height, AV_PIX_FMT_RGB24,
                SWS_POINT, nullptr, nullptr, nullptr);
        if (!sws_ctx) {
            throw std::runtime_error("could not create BGRA->RGB24 scale context");
        }
        const uint8_t* src_slices[1] = {bgra};
        int src_stride[1] = {width * 4};
        sws_scale(sws_ctx, src_slices, src_stride, 0, height, frame->data, frame->linesize);

        pkt = av_packet_alloc();
        if (avcodec_send_frame(codec_ctx, frame) < 0) {
            throw std::runtime_error("error sending frame to png encoder");
        }
        if (avcodec_send_frame(codec_ctx, nullptr) < 0) {
            throw std::runtime_error("error flushing png encoder");
        }
        if (avcodec_receive_packet(codec_ctx, pkt) < 0) {
            throw std::runtime_error("error receiving png packet");
        }
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            throw std::runtime_error(std::format("could not open output file: {}", path));
        }
        out.write(reinterpret_cast<const char*>(pkt->data), pkt->size);
        if (!out) {
            throw std::runtime_error(std::format("could not write output file: {}", path));
        }
    }
    catch (...) {
        cleanup();
        throw;
    }
    cleanup();
}

// Write beside the destination and publish only a complete, closed PNG. Rename
// is atomic on the supported filesystems; Windows requires removing an existing
// destination first, but a failed encode still always preserves it.
inline void write_png(const char* path, const uint8_t* bgra, int width, int height) {
    namespace fs = std::filesystem;
    fs::path destination{path};
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path temporary = destination;
    temporary += std::format(".tmp.{}", stamp);
    try {
        write_png_direct(temporary.string().c_str(), bgra, width, height);
#ifdef _WIN32
        if (!::MoveFileExW(temporary.c_str(), destination.c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error{std::format(
                "could not replace output file: {} ({})", path, GetLastError())};
#else
        fs::rename(temporary, destination);
#endif
    }
    catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}

} // namespace rfb_png
