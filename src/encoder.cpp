#include "encoder.h"

#include <spdlog/spdlog.h>

extern "C" {
#include <libavutil/opt.h>
}

#include "avutils.h"
#include "conversions.h"

Encoder::Encoder()
    : ofmt_ctx_(nullptr), enc_ctx_(nullptr), out_vstream_idx_(-1), stream_map_(nullptr) {}

Encoder::~Encoder() {
    if (enc_ctx_) {
        avcodec_free_context(&enc_ctx_);
    }
    if (ofmt_ctx_) {
        if (!(ofmt_ctx_->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&ofmt_ctx_->pb);
        }
        avformat_free_context(ofmt_ctx_);
    }
    if (stream_map_) {
        av_free(stream_map_);
    }
}

int Encoder::init(
    AVBufferRef *hw_ctx,
    const std::filesystem::path &out_fpath,
    AVFormatContext *ifmt_ctx,
    AVCodecContext *dec_ctx,
    EncoderConfig *encoder_config,
    int in_vstream_idx
) {
    int ret;

    // Allocate the output format context
    avformat_alloc_output_context2(&ofmt_ctx_, nullptr, nullptr, out_fpath.u8string().c_str());
    if (!ofmt_ctx_) {
        spdlog::error("Could not create output context");
        return AVERROR_UNKNOWN;
    }

    // Find the encoder
    const AVCodec *encoder = avcodec_find_encoder(encoder_config->codec);
    if (!encoder) {
        spdlog::error(
            "Required video encoder not found for codec {}", avcodec_get_name(encoder_config->codec)
        );
        return AVERROR_ENCODER_NOT_FOUND;
    }

    // Create a new video stream in the output file
    AVStream *out_vstream = avformat_new_stream(ofmt_ctx_, nullptr);
    if (!out_vstream) {
        spdlog::error("Failed to allocate the output video stream");
        return AVERROR_UNKNOWN;
    }
    out_vstream_idx_ = out_vstream->index;

    // Allocate the encoder context
    enc_ctx_ = avcodec_alloc_context3(encoder);
    if (!enc_ctx_) {
        spdlog::error("Failed to allocate the encoder context");
        return AVERROR(ENOMEM);
    }

    // Set hardware device context
    if (hw_ctx != nullptr) {
        enc_ctx_->hw_device_ctx = av_buffer_ref(hw_ctx);
    }

    // Set encoding parameters
    enc_ctx_->height = encoder_config->out_height;
    enc_ctx_->width = encoder_config->out_width;
    enc_ctx_->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;
    enc_ctx_->bit_rate = encoder_config->bit_rate;

    // Set the color properties
    enc_ctx_->color_range = dec_ctx->color_range;
    enc_ctx_->color_primaries = dec_ctx->color_primaries;
    enc_ctx_->color_trc = dec_ctx->color_trc;
    enc_ctx_->colorspace = dec_ctx->colorspace;
    enc_ctx_->chroma_sample_location = dec_ctx->chroma_sample_location;

    // Set the pixel format
    if (encoder_config->pix_fmt != AV_PIX_FMT_NONE) {
        // Use the specified pixel format
        enc_ctx_->pix_fmt = encoder_config->pix_fmt;
    } else {
        // Automatically select the pixel format
        enc_ctx_->pix_fmt = get_encoder_default_pix_fmt(encoder, dec_ctx->pix_fmt);
        if (enc_ctx_->pix_fmt == AV_PIX_FMT_NONE) {
            spdlog::error("Could not get the default pixel format for the encoder");
            return AVERROR(EINVAL);
        }
        spdlog::debug("Auto-selected pixel format: {}", av_get_pix_fmt_name(enc_ctx_->pix_fmt));
    }

    // Set the output video's time base
    if (dec_ctx->time_base.num > 0 && dec_ctx->time_base.den > 0) {
        enc_ctx_->time_base = dec_ctx->time_base;
    } else {
        enc_ctx_->time_base = av_inv_q(av_guess_frame_rate(ifmt_ctx, out_vstream, nullptr));
    }

    // Set the output video's frame rate
    if (dec_ctx->framerate.num > 0 && dec_ctx->framerate.den > 0) {
        enc_ctx_->framerate = dec_ctx->framerate;
    } else {
        enc_ctx_->framerate = av_guess_frame_rate(ifmt_ctx, out_vstream, nullptr);
    }

    // Set the CRF and preset for any codecs that support it
    std::string crf_str = std::to_string(encoder_config->crf);
    av_opt_set(enc_ctx_->priv_data, "crf", crf_str.c_str(), 0);
    av_opt_set(enc_ctx_->priv_data, "preset", encoder_config->preset, 0);

    // Use global headers if necessary
    if (ofmt_ctx_->oformat->flags & AVFMT_GLOBALHEADER) {
        enc_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    // Open the encoder
    if ((ret = avcodec_open2(enc_ctx_, encoder, nullptr)) < 0) {
        spdlog::error("Cannot open video encoder");
        return ret;
    }

    // Copy encoder parameters to output video stream
    ret = avcodec_parameters_from_context(out_vstream->codecpar, enc_ctx_);
    if (ret < 0) {
        spdlog::error("Failed to copy encoder parameters to output video stream");
        return ret;
    }

    out_vstream->time_base = enc_ctx_->time_base;
    out_vstream->avg_frame_rate = enc_ctx_->framerate;
    out_vstream->r_frame_rate = enc_ctx_->framerate;

    // Copy other streams if necessary
    if (encoder_config->copy_streams) {
        // Allocate the stream mape frame o
        stream_map_ =
            reinterpret_cast<int *>(av_malloc_array(ifmt_ctx->nb_streams, sizeof(*stream_map_)));
        if (!stream_map_) {
            spdlog::error("Could not allocate stream mapping");
            return AVERROR(ENOMEM);
        }

        // Map each input stream to an output stream
        for (int i = 0; i < static_cast<int>(ifmt_ctx->nb_streams); i++) {
            AVStream *in_stream = ifmt_ctx->streams[i];
            AVCodecParameters *in_codecpar = in_stream->codecpar;

            // Skip the input video stream as it's already processed
            if (i == in_vstream_idx) {
                stream_map_[i] = out_vstream_idx_;
                continue;
            }

            // Map only audio and subtitle streams (skip other types)
            if (in_codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
                in_codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
                stream_map_[i] = -1;
                spdlog::warn("Skipping unsupported stream type at index: {}", i);
                continue;
            }

            // Create corresponding output stream for audio and subtitle streams
            AVStream *out_stream = avformat_new_stream(ofmt_ctx_, nullptr);
            if (!out_stream) {
                spdlog::error("Failed allocating output stream");
                return AVERROR_UNKNOWN;
            }

            // Copy codec parameters from input to output
            ret = avcodec_parameters_copy(out_stream->codecpar, in_codecpar);
            if (ret < 0) {
                spdlog::error("Failed to copy codec parameters");
                return ret;
            }
            out_stream->codecpar->codec_tag = 0;

            // Copy time base
            out_stream->time_base = in_stream->time_base;

            // Map input stream index to output stream index
            spdlog::debug("Stream mapping: {} (in) -> {} (out)", i, out_stream->index);
            stream_map_[i] = out_stream->index;
        }
    }

    // Open the output file
    if (!(ofmt_ctx_->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&ofmt_ctx_->pb, out_fpath.u8string().c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            spdlog::error("Could not open output file '{}'", out_fpath.u8string());
            return ret;
        }
    }

    return 0;
}

int Encoder::write_frame(AVFrame *frame, int64_t frame_idx) {
    AVFrame *converted_frame = nullptr;
    int ret;

    // Set the frame's presentation timestamp if not set
    if (frame->pts <= 0) {
        frame->pts = frame_idx;
    }

    // Convert the frame to the encoder's pixel format if needed
    if (frame->format != enc_ctx_->pix_fmt) {
        converted_frame = convert_avframe_pix_fmt(frame, enc_ctx_->pix_fmt);
        if (!converted_frame) {
            spdlog::error("Error converting frame to encoder's pixel format");
            return AVERROR_EXTERNAL;
        }
        converted_frame->pts = frame->pts;
    }

    AVPacket *enc_pkt = av_packet_alloc();
    if (!enc_pkt) {
        spdlog::error("Could not allocate AVPacket");
        return AVERROR(ENOMEM);
    }

    // Send the frame to the encoder
    if (converted_frame != nullptr) {
        ret = avcodec_send_frame(enc_ctx_, converted_frame);
        av_frame_free(&converted_frame);
    } else {
        ret = avcodec_send_frame(enc_ctx_, frame);
    }
    if (ret < 0) {
        spdlog::error("Error sending frame to encoder");
        av_packet_free(&enc_pkt);
        return ret;
    }

    // Receive packets from the encoder
    while (ret >= 0) {
        ret = avcodec_receive_packet(enc_ctx_, enc_pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_packet_unref(enc_pkt);
            break;
        } else if (ret < 0) {
            spdlog::error("Error encoding frame");
            av_packet_free(&enc_pkt);
            return ret;
        }

        // Rescale packet timestamps
        av_packet_rescale_ts(
            enc_pkt, enc_ctx_->time_base, ofmt_ctx_->streams[out_vstream_idx_]->time_base
        );
        enc_pkt->stream_index = out_vstream_idx_;

        // Write the packet
        ret = av_interleaved_write_frame(ofmt_ctx_, enc_pkt);
        av_packet_unref(enc_pkt);
        if (ret < 0) {
            spdlog::error("Error muxing packet");
            av_packet_free(&enc_pkt);
            return ret;
        }
    }

    av_packet_free(&enc_pkt);
    return 0;
}

int Encoder::flush() {
    int ret;
    AVPacket *enc_pkt = av_packet_alloc();
    if (!enc_pkt) {
        spdlog::error("Could not allocate AVPacket");
        return AVERROR(ENOMEM);
    }

    // Send a NULL frame to signal the encoder to flush
    ret = avcodec_send_frame(enc_ctx_, nullptr);
    if (ret < 0) {
        spdlog::error("Error sending NULL frame to encoder during flush");
        av_packet_free(&enc_pkt);
        return ret;
    }

    // Receive and write packets until flushing is complete
    while (true) {
        ret = avcodec_receive_packet(enc_ctx_, enc_pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_packet_unref(enc_pkt);
            break;
        } else if (ret < 0) {
            spdlog::error("Error encoding packet during flush");
            av_packet_free(&enc_pkt);
            return ret;
        }

        // Rescale packet timestamps
        av_packet_rescale_ts(
            enc_pkt, enc_ctx_->time_base, ofmt_ctx_->streams[out_vstream_idx_]->time_base
        );
        enc_pkt->stream_index = out_vstream_idx_;

        // Write the packet
        ret = av_interleaved_write_frame(ofmt_ctx_, enc_pkt);
        av_packet_unref(enc_pkt);
        if (ret < 0) {
            spdlog::error("Error muxing packet during flush");
            av_packet_free(&enc_pkt);
            return ret;
        }
    }

    av_packet_free(&enc_pkt);
    return 0;
}

AVCodecContext *Encoder::get_encoder_context() const {
    return enc_ctx_;
}

AVFormatContext *Encoder::get_format_context() const {
    return ofmt_ctx_;
}

int Encoder::get_output_video_stream_index() const {
    return out_vstream_idx_;
}

int *Encoder::get_stream_map() const {
    return stream_map_;
}
