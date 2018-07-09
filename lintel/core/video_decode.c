/**
 * Copyright 2018 Brendan Duke.
 *
 * This file is part of Lintel.
 *
 * Lintel is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * Lintel is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * Lintel. If not, see <http://www.gnu.org/licenses/>.
 */
#include "video_decode.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/**
 * Gets the frame rate for a passed video stream.
 *
 * @param format_context Format context to read from.
 * @param video_stream Video stream to open codec context for.
 *
 * @return Frame rate of video stream, in seconds.
 */
static double
get_frame_rate(AVFormatContext *format_context, AVStream *video_stream)
{
        return av_q2d(av_guess_frame_rate(format_context, video_stream, NULL));
}

/**
 * Receives a complete frame from the video stream in format_context that
 * corresponds to video_stream_index.
 *
 * @param vid_ctx Context needed to decode frames from the video stream.
 *
 * @return SUCCESS on success, VID_DECODE_EOF if no frame was received, and
 * VID_DECODE_FFMPEG_ERR if an FFmpeg error occurred..
 */
static int32_t
receive_frame(struct video_stream_context *vid_ctx)
{
        AVPacket packet;
        int32_t status;
        bool was_frame_received;

        status = avcodec_receive_frame(vid_ctx->codec_context,
                                       vid_ctx->frame);
        if (status == 0)
                return VID_DECODE_SUCCESS;
        else if (status != AVERROR(EAGAIN))
                return VID_DECODE_FFMPEG_ERR;

        was_frame_received = false;
        while (!was_frame_received &&
               (av_read_frame(vid_ctx->format_context, &packet) == 0)) {
                if (packet.stream_index == vid_ctx->video_stream_index) {
                        status = avcodec_send_packet(vid_ctx->codec_context,
                                                     &packet);
                        if (status != 0) {
                                av_packet_unref(&packet);
                                return VID_DECODE_FFMPEG_ERR;
                        }

                        status = avcodec_receive_frame(vid_ctx->codec_context,
                                                       vid_ctx->frame);
                        if (status == 0) {
                                was_frame_received = true;
                        } else if (status != AVERROR(EAGAIN)) {
                                av_packet_unref(&packet);
                                return VID_DECODE_FFMPEG_ERR;
                        }
                }

                av_packet_unref(&packet);
        }

        if (was_frame_received)
                return VID_DECODE_SUCCESS;

        return VID_DECODE_EOF;
}

/**
 * Allocates an RGB image frame.
 *
 * @param codec_context Decoder context from the video stream, from which the
 * RGB frame will get its dimensions.
 *
 * @return The allocated RGB frame on success, NULL on failure.
 */
static AVFrame *
allocate_rgb_image(AVCodecContext *codec_context)
{
        int32_t status;
        AVFrame *frame_rgb;

        frame_rgb = av_frame_alloc();
        if (frame_rgb == NULL)
                return NULL;

        frame_rgb->format = AV_PIX_FMT_RGB24;
        frame_rgb->width = codec_context->width;
        frame_rgb->height = codec_context->height;

        status = av_image_alloc(frame_rgb->data,
                                frame_rgb->linesize,
                                frame_rgb->width,
                                frame_rgb->height,
                                AV_PIX_FMT_RGB24,
                                32);
        if (status < 0) {
                av_frame_free(&frame_rgb);
                return NULL;
        }

        return frame_rgb;
}

/**
 * Copies the received frame in `frame` to `dest`, using `frame_rgb` as
 * temporary storage for `sws_scale`.
 *
 * @param dest Destination buffer for RGB24 frame.
 * @param frame Received frame.
 * @param frame_rgb Temporary RGB frame.
 * @param sws_context Context to use for sws_scale operation.
 * @param copied_bytes Number of bytes already copied into dest from the video.
 * @param bytes_per_row Number of bytes per row in the video.
 *
 * @return Number of bytes copied to `dest`, including the frame copied over by
 * this function.
 */
static uint32_t
copy_next_frame(uint8_t *dest,
                AVFrame *frame,
                AVFrame *frame_rgb,
                AVCodecContext *codec_context,
                struct SwsContext *sws_context,
                uint32_t copied_bytes,
                const uint32_t bytes_per_row)
{
        sws_scale(sws_context,
                  (const uint8_t * const *)(frame->data),
                  frame->linesize,
                  0,
                  codec_context->height,
                  frame_rgb->data,
                  frame_rgb->linesize);

        uint8_t *next_row = frame_rgb->data[0];
        for (int32_t row_index = 0;
             row_index < frame_rgb->height;
             ++row_index) {
                memcpy(dest + copied_bytes, next_row, bytes_per_row);

                next_row += frame_rgb->linesize[0];
                copied_bytes += bytes_per_row;
        }

        return copied_bytes;
}

/**
 * Loops the frames already received in `dest` until the `num_requested_frames`
 * have been satisfied.
 *
 * @param dest Output RGB24 frame buffer.
 * @param copied_bytes Number of bytes already copied into `dest`.
 * @param frame_number The number of the next frame to copy into `dest`.
 * @param bytes_per_frame The number of bytes per frame (3*width*height).
 * @param num_requested_frames The number of frames that were requested.
 */
static void
loop_to_buffer_end(uint8_t *dest,
                   uint32_t copied_bytes,
                   int32_t frame_number,
                   uint32_t bytes_per_frame,
                   int32_t num_requested_frames)
{
        fprintf(stderr, "Ran out of frames. Looping.\n");
        if (frame_number == 0) {
                fprintf(stderr, "No frames received after seek.\n");
                return;
        }

        uint32_t bytes_to_copy = copied_bytes;
        int32_t remaining_frames = (num_requested_frames - frame_number);
        while (remaining_frames > 0) {
                if (remaining_frames < frame_number)
                        bytes_to_copy = remaining_frames*bytes_per_frame;

                memcpy(dest + copied_bytes, dest, bytes_to_copy);

                remaining_frames -= frame_number;
                copied_bytes += bytes_to_copy;
        }
}

void
decode_video_to_out_buffer(uint8_t *dest,
                           struct video_stream_context *vid_ctx,
                           int32_t num_requested_frames,
                           uint32_t fps_cap)
{
        AVStream *video_stream =
                vid_ctx->format_context->streams[vid_ctx->video_stream_index];

        AVCodecContext *codec_context = vid_ctx->codec_context;
        struct SwsContext *sws_context = sws_getContext(codec_context->width,
                                                        codec_context->height,
                                                        codec_context->pix_fmt,
                                                        codec_context->width,
                                                        codec_context->height,
                                                        AV_PIX_FMT_RGB24,
                                                        SWS_BILINEAR,
                                                        NULL,
                                                        NULL,
                                                        NULL);
        assert(sws_context != NULL);

        AVFrame *frame_rgb = allocate_rgb_image(codec_context);
        assert(frame_rgb != NULL);

        double fps_ratio = get_frame_rate(vid_ctx->format_context,
                                          video_stream);

        fps_ratio = fps_ratio/fps_cap;

        const uint32_t bytes_per_row = 3*frame_rgb->width;
        const uint32_t bytes_per_frame = bytes_per_row*frame_rgb->height;
        uint32_t copied_bytes = 0;
        float accumulated_extra_frames = 0.0f;
        float per_frame_extra = (float)fps_ratio - 1.0f;
        for (int32_t frame_number = 0;
             frame_number < num_requested_frames;
             ++frame_number) {
                int32_t status = receive_frame(vid_ctx);
                if (status == VID_DECODE_EOF) {
                        loop_to_buffer_end(dest,
                                           copied_bytes,
                                           frame_number,
                                           bytes_per_frame,
                                           num_requested_frames);
                        break;
                }
                assert(status == VID_DECODE_SUCCESS);

                /**
                 * NOTE(brendan): if fps_ratio > 0.0, then for each frame we
                 * received from the video stream, we have received
                 * (stream_fps/desired_fps) too many frames.
                 *
                 * Therefore, the fractional component of
                 * stream_fps/desired_fps is accumulated until it is greater
                 * than 1.0, i.e. we have received at least one frame too many,
                 * at which point frames are dropped until this extra frame
                 * component (accumulated_extra_frames) is again below 1.0.
                 */
                accumulated_extra_frames += per_frame_extra;
                while (accumulated_extra_frames >= 1.0f) {
                        status = receive_frame(vid_ctx);
                        if (status == VID_DECODE_EOF) {
                                loop_to_buffer_end(dest,
                                                   copied_bytes,
                                                   frame_number,
                                                   bytes_per_frame,
                                                   num_requested_frames);
                                goto out_free_frame_rgb_and_sws;
                        }
                        assert(status == VID_DECODE_SUCCESS);

                        accumulated_extra_frames -= 1.0f;
                }

                copied_bytes = copy_next_frame(dest,
                                               vid_ctx->frame,
                                               frame_rgb,
                                               codec_context,
                                               sws_context,
                                               copied_bytes,
                                               bytes_per_row);
        }

out_free_frame_rgb_and_sws:
        av_freep(frame_rgb->data);
        av_frame_free(&frame_rgb);

        sws_freeContext(sws_context);
}

int32_t read_memory(void *opaque, uint8_t *buffer, int32_t buf_size_bytes)
{
        struct buffer_data *input_buf = (struct buffer_data *)opaque;
        int32_t bytes_remaining = (input_buf->total_size_bytes -
                                   input_buf->offset_bytes);
        if (bytes_remaining < buf_size_bytes)
                buf_size_bytes = bytes_remaining;

        memcpy(buffer,
               input_buf->ptr + input_buf->offset_bytes,
               buf_size_bytes);

        input_buf->offset_bytes += buf_size_bytes;

        return buf_size_bytes;
}

int64_t seek_memory(void *opaque, int64_t offset64, int32_t whence)
{
        struct buffer_data *input_buf = (struct buffer_data *)opaque;
        int32_t offset = (int32_t)offset64;

        switch (whence) {
        case SEEK_CUR:
                input_buf->offset_bytes += offset;
                break;
        case SEEK_END:
                input_buf->offset_bytes = (input_buf->total_size_bytes -
                                           offset);
                break;
        case SEEK_SET:
                input_buf->offset_bytes = offset;
                break;
        case AVSEEK_SIZE:
                return input_buf->total_size_bytes;
        default:
                break;
        }

        return input_buf->offset_bytes;
}

/**
 * Probes the input video and returns the resulting guessed file format.
 *
 * @param input_buf Buffer tracking the input byte string.
 * @param buffer_size Size allocated for the AV I/O context.
 *
 * @return The guessed file format of the video.
 */
static AVInputFormat *
probe_input_format(struct buffer_data *input_buf, const uint32_t buffer_size)
{
        const int32_t probe_buf_size_bytes = (buffer_size +
                                              AVPROBE_PADDING_SIZE);
        AVProbeData probe_data = {NULL,
                                  (uint8_t *)av_malloc(probe_buf_size_bytes),
                                  probe_buf_size_bytes,
                                  NULL};
        assert(probe_data.buf != NULL);

        memset(probe_data.buf, 0, probe_buf_size_bytes);

        read_memory((void *)input_buf, probe_data.buf, buffer_size);
        input_buf->offset_bytes = 0;

        AVInputFormat *io_format = av_probe_input_format(&probe_data, 1);
        av_freep(&probe_data.buf);

        return io_format;
}

/**
 * Finds a video stream for the AV format context and returns the associated
 * stream index.
 *
 * @param format_context AV format where video streams should be searched for.
 *
 * @return Index of video stream on success, negative error code on failure.
 */
static int32_t
find_video_stream_index(AVFormatContext *format_context)
{
        AVStream *video_stream;
        uint32_t stream_index;

        for (stream_index = 0;
             stream_index < format_context->nb_streams;
             ++stream_index) {
                video_stream = format_context->streams[stream_index];

                if (video_stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
                        break;
        }

        if (stream_index >= format_context->nb_streams)
                return VID_DECODE_FFMPEG_ERR;

        return stream_index;
}

int32_t
setup_format_context(AVFormatContext **format_context_ptr,
                     AVIOContext *avio_ctx,
                     struct buffer_data *input_buf,
                     const uint32_t buffer_size)
{
        AVFormatContext *format_context = *format_context_ptr;

        format_context->pb = avio_ctx;
        format_context->flags |= AVFMT_FLAG_CUSTOM_IO;
        format_context->iformat = probe_input_format(input_buf, buffer_size);

        int32_t status = avformat_open_input(format_context_ptr,
                                             "",
                                             NULL,
                                             NULL);
        if (status < 0) {
                printf("AVERROR: %d, message: %s\n",
                       status,
#ifdef __cplusplus
                       "");
#else
                       av_err2str(status));
#endif // __cplusplus
                return VID_DECODE_FFMPEG_ERR;
        }

        status = avformat_find_stream_info(format_context, NULL);
        assert(status >= 0);

        return find_video_stream_index(format_context);
}

AVCodecContext *open_video_codec_ctx(AVStream *video_stream)
{
        int32_t status;
        AVCodecContext *codec_context;
        AVCodec *video_codec;

        video_codec = avcodec_find_decoder(video_stream->codecpar->codec_id);
        if (video_codec == NULL)
                return NULL;

        codec_context = avcodec_alloc_context3(video_codec);
        if (codec_context == NULL)
                return NULL;

        status = avcodec_parameters_to_context(codec_context,
                                               video_stream->codecpar);
        if (status != 0) {
                avcodec_free_context(&codec_context);
                return NULL;
        }

        status = avcodec_open2(codec_context, video_codec, NULL);
        if (status != 0) {
                avcodec_free_context(&codec_context);
                return NULL;
        }

        return codec_context;
}

int64_t
seek_to_closest_keypoint(float *seek_distance_out,
                         struct video_stream_context *vid_ctx,
                         bool should_random_seek,
                         uint32_t num_requested_frames,
                         uint32_t fps_cap)
{
        if (!should_random_seek)
                return 0;

        AVStream *video_stream =
                vid_ctx->format_context->streams[vid_ctx->video_stream_index];
        assert(video_stream->start_time == 0);

        int64_t tb_num = video_stream->time_base.num*(int64_t)AV_TIME_BASE;
        int64_t tb_den = video_stream->time_base.den;
        enum AVRounding rnd = (enum AVRounding)(AV_ROUND_DOWN |
                                                AV_ROUND_PASS_MINMAX);

        int64_t duration = av_rescale_rnd(vid_ctx->duration, tb_num, tb_den, rnd);
        double duration_seconds = ((double)duration)/AV_TIME_BASE;

        double frame_rate = get_frame_rate(vid_ctx->format_context, video_stream);
        if (frame_rate <= 0.0)
                return 0;

        if (frame_rate > fps_cap)
                frame_rate = fps_cap;

        double valid_seek_limit = (duration_seconds -
                                   ((double)num_requested_frames)/frame_rate);
        if (valid_seek_limit <= 0.0)
                return 0;

        float seek_distance = (float)(((double)rand()/RAND_MAX)*valid_seek_limit);
        if (seek_distance_out != NULL)
                *seek_distance_out = seek_distance;

        int64_t timestamp = (int64_t)(seek_distance*AV_TIME_BASE + 0.5);
        timestamp = av_rescale_rnd(timestamp, tb_den, tb_num, rnd);

        int32_t status = av_seek_frame(vid_ctx->format_context,
                                       vid_ctx->video_stream_index,
                                       timestamp,
                                       AVSEEK_FLAG_BACKWARD);
        assert(status >= 0);

        return timestamp;
}

int32_t
skip_past_timestamp(struct video_stream_context *vid_ctx, int64_t timestamp)
{
        do {
                int32_t status = receive_frame(vid_ctx);
                if (status < 0) {
                        fprintf(stderr, "Ran out of frames during seek.\n");
                        return status;
                }
                assert(status == VID_DECODE_SUCCESS);
        } while (vid_ctx->frame->pts < timestamp);

        return VID_DECODE_SUCCESS;
}

void
decode_video_from_frame_nums(uint8_t *dest,
                             struct video_stream_context *vid_ctx,
                             int32_t num_requested_frames,
                             const int32_t *frame_numbers,
                             bool should_seek)
{
        if (num_requested_frames <= 0)
                return;

        AVCodecContext *codec_context = vid_ctx->codec_context;
        struct SwsContext *sws_context = sws_getContext(codec_context->width,
                                                        codec_context->height,
                                                        codec_context->pix_fmt,
                                                        codec_context->width,
                                                        codec_context->height,
                                                        AV_PIX_FMT_RGB24,
                                                        SWS_BILINEAR,
                                                        NULL,
                                                        NULL,
                                                        NULL);
        assert(sws_context != NULL);

        AVFrame *frame_rgb = allocate_rgb_image(codec_context);
        assert(frame_rgb != NULL);

        int32_t status;
        uint32_t copied_bytes = 0;
        const uint32_t bytes_per_row = 3*frame_rgb->width;
        const uint32_t bytes_per_frame = bytes_per_row*frame_rgb->height;
        int32_t current_frame_index = 0;
        int32_t out_frame_index = 0;
        int64_t prev_pts = 0;
        if (should_seek) {
                /**
                 * NOTE(brendan): Convert from frame number to video stream
                 * time base by multiplying by the _average_ time (in
                 * video_stream->time_base units) per frame.
                 */
                int32_t avg_frame_duration = (vid_ctx->duration /
                                              vid_ctx->nb_frames);
                int64_t timestamp = frame_numbers[0]*avg_frame_duration;
                status = av_seek_frame(vid_ctx->format_context,
                                       vid_ctx->video_stream_index,
                                       timestamp,
                                       AVSEEK_FLAG_BACKWARD);
                assert(status >= 0);

                /**
                 * NOTE(brendan): Here we are handling seeking, where we need
                 * to decode the first frame in order to get the current PTS in
                 * the video stream.
                 *
                 * Most likely, the seek brought the video stream to a keyframe
                 * before the first desired frame, in which case we need to:
                 *
                 * 1. Determine which frame the video stream is at, by decoding
                 * the first frame and using its PTS and using the average
                 * frame duration approximation again.
                 *
                 * 2. Possibly copy this decoded frame into the output buffer,
                 * if by chance the frame seeked to is the first desired frame.
                 */
                status = receive_frame(vid_ctx);
                if (status == VID_DECODE_EOF)
                        goto out_free_frame_rgb_and_sws;
                assert(status == VID_DECODE_SUCCESS);

                current_frame_index = vid_ctx->frame->pts/avg_frame_duration;
                assert(current_frame_index <= frame_numbers[0]);

                /**
                 * NOTE(brendan): Handle the chance that the seek brought the
                 * stream exactly to the first desired frame index.
                 */
                if (current_frame_index == frame_numbers[0]) {
                        copied_bytes = copy_next_frame(dest,
                                                       vid_ctx->frame,
                                                       frame_rgb,
                                                       codec_context,
                                                       sws_context,
                                                       copied_bytes,
                                                       bytes_per_row);
                        ++out_frame_index;
                }
                ++current_frame_index;

                prev_pts = vid_ctx->frame->pts;
        }

        for (;
             out_frame_index < num_requested_frames;
             ++out_frame_index) {
                int32_t desired_frame_num = frame_numbers[out_frame_index];
                assert((desired_frame_num >= current_frame_index) &&
                       (desired_frame_num >= 0));

                /* Loop frames instead of aborting if we asked for too many. */
                if (desired_frame_num > vid_ctx->nb_frames) {
                        loop_to_buffer_end(dest,
                                           copied_bytes,
                                           out_frame_index,
                                           bytes_per_frame,
                                           num_requested_frames);
                        goto out_free_frame_rgb_and_sws;
                }

                while (current_frame_index <= desired_frame_num) {
                        status = receive_frame(vid_ctx);
                        if (status == VID_DECODE_EOF) {
                                loop_to_buffer_end(dest,
                                                   copied_bytes,
                                                   out_frame_index,
                                                   bytes_per_frame,
                                                   num_requested_frames);
                                goto out_free_frame_rgb_and_sws;
                        }
                        assert(status == VID_DECODE_SUCCESS);

                        /**
                         * NOTE(brendan): Only advance the frame index if the
                         * current frame's PTS is greater than the previous
                         * frame's PTS. This is to workaround an FFmpeg oddity
                         * where the first frame decoded gets duplicated.
                         */
                        if (vid_ctx->frame->pts > prev_pts) {
                                ++current_frame_index;
                                prev_pts = vid_ctx->frame->pts;
                        }
                }

                copied_bytes = copy_next_frame(dest,
                                               vid_ctx->frame,
                                               frame_rgb,
                                               codec_context,
                                               sws_context,
                                               copied_bytes,
                                               bytes_per_row);
        }

out_free_frame_rgb_and_sws:
        av_freep(frame_rgb->data);
        av_frame_free(&frame_rgb);
        sws_freeContext(sws_context);
}
