#pragma once

// Server side H264 encoder, encoding 50.
//
// Only compiled when the build was configured with -DRFB_SERVER_H264=ON, which
// is what defines RFB_SERVER_HAS_H264 and links libavcodec/libavutil/
// libswscale. The default build stays ffmpeg free: that is the whole reason
// rfb_server does not link the rfb client library.
//
// Wire format of one rectangle, after the usual 12 byte rect header carrying
// encoding 50:
//
//     u32 length   bytes of H264 that follow
//     u32 flags    see h264_flags below
//     u8  data[length]   Annex B byte stream fed straight to avcodec
//
// The payload is one access unit: everything the encoder produced for a single
// captured frame, in order, so a viewer can hand it to avcodec_send_packet()
// unmodified.

#include <cstdint>
#include <cstring>
#include <vector>

#include "server_platform.hpp"

#ifdef RFB_SERVER_HAS_H264

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#endif

namespace rfb::server {

// The flags word of an H264 rectangle. Everything not listed is reserved and
// sent as zero.
enum h264_flags : uint32_t {
    // The payload starts with an IDR: a viewer that just joined, or that lost
    // packets, can start decoding here.
    h264_flag_keyframe = 1u << 0,
    // A new H264 sequence begins, with its own SPS/PPS and possibly new
    // dimensions. A viewer should flush its decoder before feeding this.
    h264_flag_stream_reset = 1u << 1,
};

#ifdef RFB_SERVER_HAS_H264

inline constexpr bool h264_encoder_available = true;

// One H264 encoder per session: libx264 keeps per stream state, and sessions
// join and leave independently.
class h264_encoder {
public:
    h264_encoder() = default;
    h264_encoder(const h264_encoder&) = delete;
    h264_encoder& operator=(const h264_encoder&) = delete;
    ~h264_encoder() { close(); }

    // Encodes the whole framebuffer. Returns false when the encoder gave back
    // nothing for this frame, in which case there is no rectangle to send and
    // the caller should keep the update request pending.
    //
    // up_to_date says whether the packets describe the frame just handed in.
    // An encoder with output delay answers with an older picture instead, and
    // the caller must not then record this frame as one the client has seen.

    // Constant-quality level in H.264 QP units. Takes effect the next time the
    // encoder is opened, which a change here forces.
    void set_quality(int quality) {
        quality = std::clamp(quality, 0, 51);
        if (quality == m_quality) {
            return;
        }
        m_quality = quality;
        close();
    }

    bool encode(const frame_buffer& source, bool force_keyframe,
                std::vector<uint8_t>& payload, uint32_t& flags,
                bool& up_to_date) {
        up_to_date = false;
        if (source.width <= 0 || source.height <= 0) {
            return false;
        }
        if (m_context == nullptr || source.width != m_width ||
            source.height != m_height) {
            close();
            open(source.width, source.height);
            force_keyframe = true;
        }

        // The encoder keeps a reference to every frame submitted, so the
        // buffer has to be unshared before anything is written into it, not
        // after.
        if (av_frame_make_writable(m_frame) < 0) {
            throw std::runtime_error{"h264 frame not writable"};
        }
        convert(source);

        m_frame->pict_type =
            force_keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        // A millisecond time base with real elapsed time keeps the rate
        // control honest about the irregular pace screen updates arrive at.
        auto now = std::chrono::steady_clock::now();
        auto pts = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now - m_opened_at)
                       .count();
        if (pts <= m_last_pts) {
            pts = m_last_pts + 1;
        }
        m_last_pts = pts;
        m_frame->pts = pts;

        if (avcodec_send_frame(m_context, m_frame) < 0) {
            throw std::runtime_error{"h264 send frame fail"};
        }

        payload.clear();
        flags = 0;
        while (true) {
            int ret = avcodec_receive_packet(m_context, m_packet);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                throw std::runtime_error{"h264 receive packet fail"};
            }
            if ((m_packet->flags & AV_PKT_FLAG_KEY) != 0) {
                flags |= h264_flag_keyframe;
            }
            // Packets come back stamped with the pts of the frame they
            // describe, which is how a delayed encoder gives itself away.
            up_to_date = m_packet->pts == pts;
            payload.insert(payload.end(), m_packet->data,
                           m_packet->data + m_packet->size);
            av_packet_unref(m_packet);
        }
        if (payload.empty()) {
            return false;
        }
        if (m_stream_reset) {
            flags |= h264_flag_stream_reset;
            m_stream_reset = false;
        }
        return true;
    }

    // The encoded picture is rounded up to even dimensions for YUV420P; the
    // rectangle the client is told about stays the real screen size, which is
    // never larger, so the extra column or row is simply never displayed.
    int width() const { return m_width; }
    int height() const { return m_height; }
    const std::string& encoder_name() const { return m_encoder_name; }

private:
    // Every encoder spells "constant quality" differently, and getting this
    // wrong is what makes a screen stream look like a bad video call: with no
    // rate control configured at all, both libx264 and h264_amf fall back to a
    // fixed bitrate far too low for a 4K desktop, so text smears.
    void apply_encoder_options(const AVCodec* codec) {
        const std::string_view name{codec->name};
        const std::string quality = std::to_string(m_quality);
        // A bitrate target and constant quality are mutually exclusive; the
        // encoders below only honour the latter when no bitrate is asked for.
        m_context->bit_rate = 0;
        m_context->rc_max_rate = 0;
        m_context->rc_buffer_size = 0;
        if (name == "libx264") {
            // Every encoder names its speed/latency knobs differently, so
            // these are set per encoder rather than blindly: h264_amf rejects
            // libx264's preset values outright.
            av_opt_set(m_context->priv_data, "preset", "veryfast", 0);
            av_opt_set(m_context->priv_data, "tune", "zerolatency", 0);
            // x264's CRF is in QP units, so the same number means much the
            // same thing here as it does for the hardware encoders.
            av_opt_set(m_context->priv_data, "crf", quality.c_str(), 0);
            return;
        }
        if (name == "libopenh264") {
            // openh264 has no CRF; its quality follows the bitrate, so a
            // generous one is set instead and the QP scale is mapped onto it.
            av_opt_set(m_context->priv_data, "rc_mode", "quality", 0);
            av_opt_set(m_context->priv_data, "allow_skip_frames", "1", 0);
            // Roughly 40 Mbit at the best level down to 2 Mbit at the worst,
            // scaled by pixel count against 1080p.
            double pixels = static_cast<double>(m_encoded_width) *
                m_encoded_height / (1920.0 * 1080.0);
            double scale = static_cast<double>(39 - m_quality) / 27.0;
            m_context->bit_rate = static_cast<int64_t>(
                (2000000.0 + 38000000.0 * std::clamp(scale, 0.0, 1.0)) *
                std::max(0.1, pixels));
            return;
        }
        if (name.ends_with("_amf")) {
            // AMF defaults to a latency-tuned VBR; constant QP is what keeps
            // static text sharp instead of letting it settle over seconds.
            av_opt_set(m_context->priv_data, "usage", "ultralowlatency", 0);
            // AMF's speed knob is called "quality", not "preset".
            av_opt_set(m_context->priv_data, "quality", "quality", 0);
            av_opt_set(m_context->priv_data, "rc", "cqp", 0);
            av_opt_set(m_context->priv_data, "qp_i", quality.c_str(), 0);
            av_opt_set(m_context->priv_data, "qp_p", quality.c_str(), 0);
            av_opt_set(m_context->priv_data, "qp_b", quality.c_str(), 0);
            // AMF's own defaults re-insert IDRs on a timer, which wastes
            // bandwidth on a screen that mostly does not change.
            av_opt_set(m_context->priv_data, "enforce_hrd", "0", 0);
            return;
        }
        if (name.ends_with("_qsv") || name.ends_with("_nvenc")) {
            av_opt_set(m_context->priv_data, "rc", "cqp", 0);
            av_opt_set(m_context->priv_data, "cq", quality.c_str(), 0);
            av_opt_set(m_context->priv_data, "qp", quality.c_str(), 0);
            return;
        }
        // Anything else (libopenh264, the native encoders): the generic
        // global_quality path, which is in FF_QP2LAMBDA units.
        m_context->flags |= AV_CODEC_FLAG_QSCALE;
        m_context->global_quality = FF_QP2LAMBDA * m_quality;
    }

    // Tries every H264 encoder the build has, in preference order, and keeps
    // the first that actually opens. A hardware encoder is only usable on the
    // matching hardware -- h264_amf is present on any machine with the AMD
    // runtime installed but fails to open on a VM with no AMD GPU -- and that
    // is only discoverable by opening it, so a build with both a hardware and
    // a software encoder must not commit to one by name alone.
    void open(int width, int height) {
        std::vector<const AVCodec*> candidates{};
        auto add = [&candidates](const AVCodec* codec) {
            if (codec != nullptr &&
                std::find(candidates.begin(), candidates.end(), codec) ==
                    candidates.end()) {
                candidates.push_back(codec);
            }
        };
        // Hardware first: it costs the host almost nothing. libx264 is the
        // best of the software encoders, libopenh264 the fallback, and
        // whatever avcodec considers the default H264 encoder comes last.
        for (const char* name : {"h264_amf", "h264_qsv", "h264_nvenc",
                                 "libx264", "libopenh264"}) {
            add(avcodec_find_encoder_by_name(name));
        }
        add(avcodec_find_encoder(AV_CODEC_ID_H264));
        if (candidates.empty()) {
            throw std::runtime_error{
                "h264 encoder not found in this ffmpeg build"};
        }
        std::string errors{};
        for (const AVCodec* codec : candidates) {
            try {
                open_with(codec, width, height);
                return;
            }
            catch (const std::exception& e) {
                if (!errors.empty()) {
                    errors += ", ";
                }
                errors += std::string{codec->name} + ": " + e.what();
                close();
            }
        }
        throw std::runtime_error{"no usable h264 encoder (" + errors + ")"};
    }

    void open_with(const AVCodec* codec, int width, int height) {
        m_width = width;
        m_height = height;
        // YUV420P subsamples chroma by two, so odd dimensions have to be
        // padded rather than scaled: scaling would resample the whole image
        // to save one column.
        m_encoded_width = width + (width & 1);
        m_encoded_height = height + (height & 1);

        m_context = avcodec_alloc_context3(codec);
        if (m_context == nullptr) {
            throw std::runtime_error{"h264 context allocation fail"};
        }
        m_context->width = m_encoded_width;
        m_context->height = m_encoded_height;
        m_context->time_base = AVRational{1, 1000};
        m_context->framerate = AVRational{0, 1};
        m_context->pix_fmt = AV_PIX_FMT_YUV420P;
        // Interactive streaming: no reordering, and a keyframe only when one
        // is asked for. Screen content is mostly static, so periodic IDRs
        // would be pure waste.
        m_context->max_b_frames = 0;
        m_context->has_b_frames = 0;
        m_context->flags |= AV_CODEC_FLAG_LOW_DELAY;
        m_context->gop_size = 250;
        // Both ends have to agree on the colour space or the picture comes
        // back tinted, so it is pinned here and signalled in the VUI, which is
        // where the client reads it back from. Limited range rather than full:
        // full range makes avcodec hand the viewer the deprecated YUVJ420P
        // pixel format, which viewers routinely fail to recognise.
        m_context->colorspace = AVCOL_SPC_BT709;
        m_context->color_range = AVCOL_RANGE_MPEG;
        m_context->color_primaries = AVCOL_PRI_BT709;
        m_context->color_trc = AVCOL_TRC_BT709;
        apply_encoder_options(codec);
        if (avcodec_open2(m_context, codec, nullptr) < 0) {
            throw std::runtime_error{"open fail"};
        }
        m_encoder_name = codec->name;

        m_frame = av_frame_alloc();
        if (m_frame == nullptr) {
            throw std::runtime_error{"h264 frame allocation fail"};
        }
        m_frame->format = AV_PIX_FMT_YUV420P;
        m_frame->width = m_encoded_width;
        m_frame->height = m_encoded_height;
        m_frame->colorspace = AVCOL_SPC_BT709;
        m_frame->color_range = AVCOL_RANGE_MPEG;
        m_frame->color_primaries = AVCOL_PRI_BT709;
        m_frame->color_trc = AVCOL_TRC_BT709;
        if (av_frame_get_buffer(m_frame, 32) < 0) {
            throw std::runtime_error{"h264 frame buffer allocation fail"};
        }
        m_packet = av_packet_alloc();
        if (m_packet == nullptr) {
            throw std::runtime_error{"h264 packet allocation fail"};
        }

        m_scaler = sws_getContext(m_encoded_width, m_encoded_height,
                                  AV_PIX_FMT_BGRA, m_encoded_width,
                                  m_encoded_height, AV_PIX_FMT_YUV420P,
                                  SWS_BICUBIC | SWS_FULL_CHR_H_INT |
                                      SWS_FULL_CHR_H_INP | SWS_ACCURATE_RND,
                                  nullptr, nullptr, nullptr);
        if (m_scaler == nullptr) {
            throw std::runtime_error{"BGRA to YUV420P scaler creation fail"};
        }
        // The source is BGRA, which is always full range; the destination is
        // the limited range BT.709 the encoder announces.
        const int* coefficients = sws_getCoefficients(SWS_CS_ITU709);
        sws_setColorspaceDetails(m_scaler, coefficients, 1, coefficients, 0, 0,
                                 1 << 16, 1 << 16);

        if (m_encoded_width != m_width || m_encoded_height != m_height) {
            m_padded.assign(static_cast<size_t>(m_encoded_width) *
                                m_encoded_height * 4,
                            0);
        }
        else {
            m_padded.clear();
            m_padded.shrink_to_fit();
        }

        m_opened_at = std::chrono::steady_clock::now();
        m_last_pts = -1;
        m_stream_reset = true;
    }

    void convert(const frame_buffer& source) {
        const uint8_t* data = source.pixels.data();
        int stride = static_cast<int>(source.stride());
        if (!m_padded.empty()) {
            pad(source);
            data = m_padded.data();
            stride = m_encoded_width * 4;
        }
        const uint8_t* slices[1] = {data};
        int strides[1] = {stride};
        sws_scale(m_scaler, slices, strides, 0, m_encoded_height, m_frame->data,
                  m_frame->linesize);
    }

    // Copies the frame into an even sized buffer, duplicating the last column
    // and row into the padding so the encoder does not have to invent an edge.
    void pad(const frame_buffer& source) {
        auto row_bytes = static_cast<size_t>(m_width) * 4;
        auto padded_stride = static_cast<size_t>(m_encoded_width) * 4;
        for (int row = 0; row < m_height; row++) {
            const uint8_t* src =
                source.pixels.data() + static_cast<size_t>(row) * source.stride();
            uint8_t* dst = m_padded.data() + static_cast<size_t>(row) * padded_stride;
            std::memcpy(dst, src, row_bytes);
            if (m_encoded_width != m_width) {
                std::memcpy(dst + row_bytes, src + row_bytes - 4, 4);
            }
        }
        if (m_encoded_height != m_height) {
            std::memcpy(m_padded.data() + static_cast<size_t>(m_height) * padded_stride,
                        m_padded.data() + static_cast<size_t>(m_height - 1) * padded_stride,
                        padded_stride);
        }
    }

    void close() {
        if (m_scaler != nullptr) {
            sws_freeContext(m_scaler);
            m_scaler = nullptr;
        }
        if (m_packet != nullptr) {
            av_packet_free(&m_packet);
        }
        if (m_frame != nullptr) {
            av_frame_free(&m_frame);
        }
        if (m_context != nullptr) {
            avcodec_free_context(&m_context);
        }
        m_width = 0;
        m_height = 0;
        m_encoder_name.clear();
    }

    AVCodecContext* m_context{};
    AVFrame* m_frame{};
    AVPacket* m_packet{};
    SwsContext* m_scaler{};
    std::vector<uint8_t> m_padded{};
    std::chrono::steady_clock::time_point m_opened_at{};
    int64_t m_last_pts{-1};
    int m_width{};
    int m_height{};
    int m_encoded_width{};
    int m_encoded_height{};
    bool m_stream_reset{true};
    int m_quality{18};
    std::string m_encoder_name{};
};

#else

inline constexpr bool h264_encoder_available = false;

// A stand-in so the session can hold one unconditionally. Nothing ever calls
// encode(): negotiation never picks encoding 50 when the encoder is missing.
class h264_encoder {
public:
    void set_quality(int) {}
    const std::string& encoder_name() const {
        static const std::string none{};
        return none;
    }
    bool encode(const frame_buffer&, bool, std::vector<uint8_t>&, uint32_t&,
                bool&) {
        return false;
    }
};

#endif

} // namespace rfb::server
