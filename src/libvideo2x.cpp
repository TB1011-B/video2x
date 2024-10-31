#include "libvideo2x.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>

#include <spdlog/spdlog.h>
#include <opencv2/videoio.hpp>

#include "decoder.h"
#include "encoder.h"
#include "filter.h"
#include "libplacebo_filter.h"
#include "realesrgan_filter.h"

/**
 * @brief Process frames using the selected filter.
 *
 * @param[in] encoder_config Encoder configurations
 * @param[in,out] proc_ctx Struct containing the processing context
 * @param[in] ifmt_ctx Input format context
 * @param[in] ofmt_ctx Output format context
 * @param[in] dec_ctx Decoder context
 * @param[in] enc_ctx Encoder context
 * @param[in] filter Filter instance
 * @param[in] vstream_idx Index of the video stream in the input format context
 * @param[in] stream_map Array mapping input stream indexes to output stream indexes
 * @param[in] benchmark Flag to enable benchmarking mode
 * @return int 0 on success, negative value on error
 */
static int process_frames(
    EncoderConfig *encoder_config,
    VideoProcessingContext *proc_ctx,
    AVFormatContext *ifmt_ctx,
    AVFormatContext *ofmt_ctx,
    AVCodecContext *dec_ctx,
    AVCodecContext *enc_ctx,
    Filter *filter,
    int vstream_idx,
    int *stream_map,
    bool benchmark = false
) {
    int ret;
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    std::vector<AVFrame *> flushed_frames;

    // Get the total number of frames in the video with OpenCV
    spdlog::debug("Reading total number of frames with OpenCV");
    cv::VideoCapture cap(ifmt_ctx->url);
    if (!cap.isOpened()) {
        spdlog::error("Failed to open video file with OpenCV");
        return -1;
    }
    proc_ctx->total_frames = static_cast<int64_t>(cap.get(cv::CAP_PROP_FRAME_COUNT));
    cap.release();

    // Check if the total number of frames is still 0
    if (proc_ctx->total_frames == 0) {
        spdlog::warn("Unable to determine total number of frames");
    } else {
        spdlog::debug("{} frames to process", proc_ctx->total_frames);
    }

    // Get start time
    proc_ctx->start_time = time(NULL);
    if (proc_ctx->start_time == -1) {
        perror("time");
    }

    AVFrame *frame = av_frame_alloc();
    if (frame == nullptr) {
        ret = AVERROR(ENOMEM);
        return ret;
    }

    AVPacket *packet = av_packet_alloc();
    if (packet == nullptr) {
        spdlog::error("Could not allocate AVPacket");
        av_frame_free(&frame);
        return AVERROR(ENOMEM);
    }

    // Lambda function for cleaning up resources
    auto cleanup = [&]() {
        av_frame_free(&frame);
        av_packet_free(&packet);
        for (AVFrame *&flushed_frame : flushed_frames) {
            if (flushed_frame) {
                av_frame_free(&flushed_frame);
                flushed_frame = nullptr;
            }
        }
    };

    // Read frames from the input file
    while (!proc_ctx->abort) {
        ret = av_read_frame(ifmt_ctx, packet);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                spdlog::debug("Reached end of file");
                break;
            }
            av_strerror(ret, errbuf, sizeof(errbuf));
            spdlog::error("Error reading packet: {}", errbuf);
            cleanup();
            return ret;
        }

        if (packet->stream_index == vstream_idx) {
            ret = avcodec_send_packet(dec_ctx, packet);
            if (ret < 0) {
                av_strerror(ret, errbuf, sizeof(errbuf));
                spdlog::error("Error sending packet to decoder: {}", errbuf);
                av_packet_unref(packet);
                cleanup();
                return ret;
            }

            while (!proc_ctx->abort) {
                if (proc_ctx->pause) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }

                ret = avcodec_receive_frame(dec_ctx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    spdlog::debug("Frame not ready");
                    break;
                } else if (ret < 0) {
                    av_strerror(ret, errbuf, sizeof(errbuf));
                    spdlog::error("Error decoding video frame: {}", errbuf);
                    av_packet_unref(packet);
                    cleanup();
                    return ret;
                }

                AVFrame *processed_frame = nullptr;
                ret = filter->process_frame(frame, &processed_frame);
                if (ret < 0 && ret != AVERROR(EAGAIN)) {
                    av_strerror(ret, errbuf, sizeof(errbuf));
                    av_frame_free(&processed_frame);
                    av_packet_unref(packet);
                    cleanup();
                    return ret;
                } else if (ret == 0 && processed_frame != nullptr) {
                    if (!benchmark) {
                        ret = write_frame(processed_frame, enc_ctx, ofmt_ctx, vstream_idx);
                        if (ret < 0) {
                            av_strerror(ret, errbuf, sizeof(errbuf));
                            spdlog::error("Error encoding/writing frame: {}", errbuf);
                            av_frame_free(&processed_frame);
                            av_packet_unref(packet);
                            cleanup();
                            return ret;
                        }
                    }
                    av_frame_free(&processed_frame);
                    proc_ctx->processed_frames++;
                }

                av_frame_unref(frame);
                spdlog::debug(
                    "Processed frame {}/{}", proc_ctx->processed_frames, proc_ctx->total_frames
                );
            }
        } else if (encoder_config->copy_streams && stream_map[packet->stream_index] >= 0) {
            AVStream *in_stream = ifmt_ctx->streams[packet->stream_index];
            int out_stream_index = stream_map[packet->stream_index];
            AVStream *out_stream = ofmt_ctx->streams[out_stream_index];

            av_packet_rescale_ts(packet, in_stream->time_base, out_stream->time_base);
            packet->stream_index = out_stream_index;

            ret = av_interleaved_write_frame(ofmt_ctx, packet);
            if (ret < 0) {
                av_strerror(ret, errbuf, sizeof(errbuf));
                spdlog::error("Error muxing packet: {}", errbuf);
                av_packet_unref(packet);
                cleanup();
                return ret;
            }
        }
        av_packet_unref(packet);
    }

    // Flush the filter
    ret = filter->flush(flushed_frames);
    if (ret < 0) {
        av_strerror(ret, errbuf, sizeof(errbuf));
        spdlog::error("Error flushing filter: {}", errbuf);
        cleanup();
        return ret;
    }

    // Encode and write all flushed frames
    for (AVFrame *&flushed_frame : flushed_frames) {
        ret = write_frame(flushed_frame, enc_ctx, ofmt_ctx, vstream_idx);
        if (ret < 0) {
            av_strerror(ret, errbuf, sizeof(errbuf));
            spdlog::error("Error encoding/writing flushed frame: {}", errbuf);
            av_frame_free(&flushed_frame);
            flushed_frame = nullptr;
            cleanup();
            return ret;
        }
        av_frame_free(&flushed_frame);
        flushed_frame = nullptr;
        proc_ctx->processed_frames++;
    }

    // Flush the encoder
    ret = flush_encoder(enc_ctx, ofmt_ctx);
    if (ret < 0) {
        av_strerror(ret, errbuf, sizeof(errbuf));
        spdlog::error("Error flushing encoder: {}", errbuf);
        cleanup();
        return ret;
    }

    cleanup();
    return ret;
}

/**
 * @brief Process a video file using the selected filter and encoder settings.
 *
 * @param[in] in_fname Path to the input video file
 * @param[in] out_fname Path to the output video file
 * @param[in] log_level Log level
 * @param[in] benchmark Flag to enable benchmarking mode
 * @param[in] hw_type Hardware device type
 * @param[in] filter_config Filter configurations
 * @param[in] encoder_config Encoder configurations
 * @param[in,out] proc_ctx Video processing context
 * @return int 0 on success, non-zero value on error
 */
extern "C" int process_video(
    const char *in_fname,
    const char *out_fname,
    Libvideo2xLogLevel log_level,
    bool benchmark,
    AVHWDeviceType hw_type,
    const FilterConfig *filter_config,
    EncoderConfig *encoder_config,
    VideoProcessingContext *proc_ctx
) {
    AVFormatContext *ifmt_ctx = nullptr;
    AVFormatContext *ofmt_ctx = nullptr;
    AVCodecContext *dec_ctx = nullptr;
    AVCodecContext *enc_ctx = nullptr;
    AVBufferRef *hw_ctx = nullptr;
    int *stream_map = nullptr;
    Filter *filter = nullptr;
    int vstream_idx = -1;
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    int ret = 0;

    // Lambda function for cleaning up resources
    auto cleanup = [&]() {
        if (ifmt_ctx) {
            avformat_close_input(&ifmt_ctx);
            ifmt_ctx = nullptr;
        }
        if (ofmt_ctx && !(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&ofmt_ctx->pb);
            ofmt_ctx->pb = nullptr;
        }
        if (ofmt_ctx) {
            avformat_free_context(ofmt_ctx);
            ofmt_ctx = nullptr;
        }
        if (dec_ctx) {
            avcodec_free_context(&dec_ctx);
            dec_ctx = nullptr;
        }
        if (enc_ctx) {
            avcodec_free_context(&enc_ctx);
            enc_ctx = nullptr;
        }
        if (hw_ctx) {
            av_buffer_unref(&hw_ctx);
            hw_ctx = nullptr;
        }
        if (stream_map) {
            av_free(stream_map);
            stream_map = nullptr;
        }
        if (filter) {
            delete filter;
            filter = nullptr;
        }
    };

    // Set the log level for FFmpeg and spdlog (libvideo2x)
    switch (log_level) {
        case LIBVIDEO2X_LOG_LEVEL_TRACE:
            av_log_set_level(AV_LOG_TRACE);
            spdlog::set_level(spdlog::level::trace);
            break;
        case LIBVIDEO2X_LOG_LEVEL_DEBUG:
            av_log_set_level(AV_LOG_DEBUG);
            spdlog::set_level(spdlog::level::debug);
            break;
        case LIBVIDEO2X_LOG_LEVEL_INFO:
            av_log_set_level(AV_LOG_INFO);
            spdlog::set_level(spdlog::level::info);
            break;
        case LIBVIDEO2X_LOG_LEVEL_WARNING:
            av_log_set_level(AV_LOG_WARNING);
            spdlog::set_level(spdlog::level::warn);
            break;
        case LIBVIDEO2X_LOG_LEVEL_ERROR:
            av_log_set_level(AV_LOG_ERROR);
            spdlog::set_level(spdlog::level::err);
            break;
        case LIBVIDEO2X_LOG_LEVEL_CRITICAL:
            av_log_set_level(AV_LOG_FATAL);
            spdlog::set_level(spdlog::level::critical);
            break;
        case LIBVIDEO2X_LOG_LEVEL_OFF:
            av_log_set_level(AV_LOG_QUIET);
            spdlog::set_level(spdlog::level::off);
            break;
        default:
            av_log_set_level(AV_LOG_INFO);
            spdlog::set_level(spdlog::level::info);
            break;
    }

    // Initialize hardware device context
    if (hw_type != AV_HWDEVICE_TYPE_NONE) {
        ret = av_hwdevice_ctx_create(&hw_ctx, hw_type, NULL, NULL, 0);
        if (ret < 0) {
            av_strerror(ret, errbuf, sizeof(errbuf));
            spdlog::error("Error initializing hardware device context: {}", errbuf);
            cleanup();
            return ret;
        }
    }

    // Initialize input
    ret = init_decoder(hw_type, hw_ctx, in_fname, &ifmt_ctx, &dec_ctx, &vstream_idx);
    if (ret < 0) {
        av_strerror(ret, errbuf, sizeof(errbuf));
        spdlog::error("Failed to initialize decoder: {}", errbuf);
        cleanup();
        return ret;
    }

    // Initialize output dimensions based on filter configuration
    int output_width = 0, output_height = 0;
    switch (filter_config->filter_type) {
        case FILTER_BACKEND_LIBPLACEBO:
            output_width = filter_config->config.libplacebo.out_width;
            output_height = filter_config->config.libplacebo.out_height;
            break;
        case FILTER_BACKEND_REALESRGAN:
            output_width = dec_ctx->width * filter_config->config.realesrgan.scaling_factor;
            output_height = dec_ctx->height * filter_config->config.realesrgan.scaling_factor;
            break;
        default:
            spdlog::error("Unknown filter type");
            cleanup();
            return -1;
    }
    spdlog::info("Output video dimensions: {}x{}", output_width, output_height);

    // Initialize output encoder
    encoder_config->out_width = output_width;
    encoder_config->out_height = output_height;
    ret = init_encoder(
        hw_ctx,
        out_fname,
        ifmt_ctx,
        &ofmt_ctx,
        &enc_ctx,
        dec_ctx,
        encoder_config,
        vstream_idx,
        &stream_map
    );
    if (ret < 0) {
        av_strerror(ret, errbuf, sizeof(errbuf));
        spdlog::error("Failed to initialize encoder: {}", errbuf);
        cleanup();
        return ret;
    }

    // Write the output file header
    ret = avformat_write_header(ofmt_ctx, NULL);
    if (ret < 0) {
        av_strerror(ret, errbuf, sizeof(errbuf));
        spdlog::error("Error occurred when opening output file: {}", errbuf);
        cleanup();
        return ret;
    }

    // Create and initialize the appropriate filter
    if (filter_config->filter_type == FILTER_BACKEND_LIBPLACEBO) {
        const auto &config = filter_config->config.libplacebo;
        if (!config.shader_path) {
            spdlog::error("Shader path must be provided for the libplacebo filter");
            cleanup();
            return -1;
        }
        filter = new LibplaceboFilter{
            config.out_width, config.out_height, std::filesystem::path(config.shader_path)
        };
    } else if (filter_config->filter_type == FILTER_BACKEND_REALESRGAN) {
        const auto &config = filter_config->config.realesrgan;
        if (!config.model) {
            spdlog::error("Model name must be provided for the RealESRGAN filter");
            cleanup();
            return -1;
        }
        filter = new RealesrganFilter{
            config.gpuid, config.tta_mode, config.scaling_factor, config.model
        };
    } else {
        spdlog::error("Unknown filter type");
        cleanup();
        return -1;
    }

    // Check if the filter instance was created successfully
    if (filter == nullptr) {
        spdlog::error("Failed to create filter instance");
        cleanup();
        return -1;
    }

    // Initialize the filter
    ret = filter->init(dec_ctx, enc_ctx, hw_ctx);
    if (ret < 0) {
        spdlog::error("Failed to initialize filter");
        cleanup();
        return ret;
    }

    // Process frames
    ret = process_frames(
        encoder_config,
        proc_ctx,
        ifmt_ctx,
        ofmt_ctx,
        dec_ctx,
        enc_ctx,
        filter,
        vstream_idx,
        stream_map,
        benchmark
    );
    if (ret < 0) {
        av_strerror(ret, errbuf, sizeof(errbuf));
        spdlog::error("Error processing frames: {}", errbuf);
        cleanup();
        return ret;
    }

    // Write the output file trailer
    av_write_trailer(ofmt_ctx);

    // Cleanup before returning
    cleanup();

    if (ret < 0 && ret != AVERROR_EOF) {
        av_strerror(ret, errbuf, sizeof(errbuf));
        spdlog::error("Error occurred: {}", errbuf);
        return ret;
    }
    return 0;
}
