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

        bool was_key_frame;

        av_init_packet(&packet);

        status = avcodec_receive_frame(vid_ctx->codec_context,
                                       vid_ctx->frame);
        if (status == 0)
                return VID_DECODE_SUCCESS;
        else if (status == AVERROR_EOF)
                return VID_DECODE_EOF;
        else if (status != AVERROR(EAGAIN))
                return VID_DECODE_FFMPEG_ERR;

        was_frame_received = false;
        was_key_frame = false;
        while (!was_frame_received &&
               (av_read_frame(vid_ctx->format_context, &packet) == 0))
        {
                if (packet.stream_index == vid_ctx->video_stream_index && (packet.flags == AV_PKT_FLAG_KEY || was_key_frame))
                {
                        status = avcodec_send_packet(vid_ctx->codec_context,
                                                     &packet);
                        if (status != 0)
                        {
                                av_packet_unref(&packet);
                                return VID_DECODE_FFMPEG_ERR;
                        }

                        status = avcodec_receive_frame(vid_ctx->codec_context,
                                                       vid_ctx->frame);
                        if (status == 0)
                        {
                                was_frame_received = true;
                        }
                        else if (status != AVERROR(EAGAIN))
                        {
                                av_packet_unref(&packet);
                                return VID_DECODE_FFMPEG_ERR;
                        }
                        else
                        {
                                was_key_frame = true;
                                av_packet_unref(&packet);
                        }
                }
                av_packet_unref(&packet);
        }

        if (was_frame_received)
                return VID_DECODE_SUCCESS;

        /**
         * NOTE(brendan): Flush/drain the codec. After this, subsequent calls
         * to receive_frame will return frames until EOF.
         *
         * See FFmpeg's libavcodec/avcodec.h.
         */
        av_init_packet(&packet);
        packet.data = NULL;
        packet.size = 0;

        status = avcodec_send_packet(vid_ctx->codec_context,
                                     &packet);
        if (status == 0)
        {
                status = avcodec_receive_frame(vid_ctx->codec_context,
                                               vid_ctx->frame);
                if (status == 0)
                {
                        av_packet_unref(&packet);

                        return VID_DECODE_SUCCESS;
                }
        }

        av_packet_unref(&packet);

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
        if (status < 0)
        {
                av_frame_free(&frame_rgb);
                return NULL;
        }

        return frame_rgb;
}

static AVFrame *
allocate_rgb_image2(AVCodecContext *codec_context, uint32_t rewidth, uint32_t reheight)
{
        int32_t status;
        AVFrame *frame_rgb;

        frame_rgb = av_frame_alloc();
        if (frame_rgb == NULL)
                return NULL;

        // frame_rgb->format = AV_PIX_FMT_RGB24;
        // frame_rgb->width = codec_context->width;
        // frame_rgb->height = codec_context->height;

        // resize
        frame_rgb->format = AV_PIX_FMT_BGR24;  // frame mode
        frame_rgb->width = rewidth;
        frame_rgb->height = reheight;

        status = av_image_alloc(frame_rgb->data,
                                frame_rgb->linesize,
                                frame_rgb->width,
                                frame_rgb->height,
                                AV_PIX_FMT_RGB24,
                                32);
        if (status < 0)
        {
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
                  (const uint8_t *const *)(frame->data),
                  frame->linesize,
                  0,
                  codec_context->height,
                  frame_rgb->data,
                  frame_rgb->linesize);

        uint8_t *next_row = frame_rgb->data[0];
        for (int32_t row_index = 0;
             row_index < frame_rgb->height;
             ++row_index)
        {
                memcpy(dest + copied_bytes, next_row, bytes_per_row);

                next_row += frame_rgb->linesize[0];
                copied_bytes += bytes_per_row;
        }

        return copied_bytes;
}

// static AVFrame *
// crop_frame(const AVFrame *in,
//                 int32_t left,
//                 int32_t top,
//                 int32_t right,
//                 int32_t bottom)
// {
//     AVFilterContext *buffersink_ctx;
//     AVFilterContext *buffersrc_ctx;
//     AVFilterGraph *filter_graph = avfilter_graph_alloc();
//     AVFrame *f = av_frame_alloc();
//     AVFilterInOut *inputs = NULL, *outputs = NULL;
//     char args[512];
//     int32_t ret;
//     snprintf(args, sizeof(args),
//              "buffer=video_size=%dx%d:pix_fmt=%d:time_base=1/1:pixel_aspect=0/1[in];"
//              "[in]crop=x=%d:y=%d:out_w=in_w-x-%d:out_h=in_h-y-%d[out];"
//              "[out]buffersink",
//              in->width, in->height, in->format,
//              left, top, right, bottom);

//     ret = avfilter_graph_parse2(filter_graph, args, &inputs, &outputs);
//     if (ret < 0) return NULL;
//     assert(inputs == NULL && outputs == NULL);
//     ret = avfilter_graph_config(filter_graph, NULL);
//     if (ret < 0) return NULL;

//     buffersrc_ctx = avfilter_graph_get_filter(filter_graph, "Parsed_buffer_0");
//     buffersink_ctx = avfilter_graph_get_filter(filter_graph, "Parsed_buffersink_2");
//     assert(buffersrc_ctx != NULL);
//     assert(buffersink_ctx != NULL);

//     av_frame_ref(f, in);
//     ret = av_buffersrc_add_frame(buffersrc_ctx, f);
//     if (ret < 0) return NULL;
//     ret = av_buffersink_get_frame(buffersink_ctx, f);
//     if (ret < 0) return NULL;

//     avfilter_graph_free(&filter_graph);

//     return f;
// }

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
        // fprintf(stderr, "Ran out of frames. Looping.\n");
        if (frame_number == 0)
        {
                // fprintf(stderr, "No frames received after seek.\n");
                return;
        }

        uint32_t bytes_to_copy = copied_bytes;
        int32_t remaining_frames = (num_requested_frames - frame_number);
        while (remaining_frames > 0)
        {
                if (remaining_frames < frame_number)
                        bytes_to_copy = remaining_frames * bytes_per_frame;

                memcpy(dest + copied_bytes, dest, bytes_to_copy);

                remaining_frames -= frame_number;
                copied_bytes += bytes_to_copy;
        }
}

void decode_video_to_out_buffer(uint8_t *dest,
                                struct video_stream_context *vid_ctx,
                                int32_t num_requested_frames)
{
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

        const uint32_t bytes_per_row = 3 * frame_rgb->width;
        const uint32_t bytes_per_frame = bytes_per_row * frame_rgb->height;
        uint32_t copied_bytes = 0;
        for (int32_t frame_number = 0;
             frame_number < num_requested_frames;
             ++frame_number)
        {
                int32_t status = receive_frame(vid_ctx);
                if (status == VID_DECODE_EOF)
                {
                        loop_to_buffer_end(dest,
                                           copied_bytes,
                                           frame_number,
                                           bytes_per_frame,
                                           num_requested_frames);
                        break;
                }
                assert(status == VID_DECODE_SUCCESS);

                copied_bytes = copy_next_frame(dest,
                                               vid_ctx->frame,
                                               frame_rgb,
                                               codec_context,
                                               sws_context,
                                               copied_bytes,
                                               bytes_per_row);
        }

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

        switch (whence)
        {
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
             ++stream_index)
        {
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
                                             format_context->iformat,
                                             NULL);
        if (status < 0)
        {
                fprintf(stderr,
                        "AVERROR: %d, message: %s\n",
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

int32_t
setup_format_context2(AVFormatContext **format_context_ptr,
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
                                             format_context->iformat,
                                             NULL);
        if (status < 0)
        {
                fprintf(stderr,
                        "AVERROR: %d, message: %s\n",
                        status,
#ifdef __cplusplus
                        "");
#else
                        av_err2str(status));
#endif // __cplusplus
                return VID_DECODE_FFMPEG_ERR;
        }

        //status = avformat_find_stream_info(format_context, NULL);
        //assert(status >= 0);

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
        if (status != 0)
        {
                avcodec_free_context(&codec_context);
                return NULL;
        }

        status = avcodec_open2(codec_context, video_codec, NULL);
        if (status != 0)
        {
                avcodec_free_context(&codec_context);
                return NULL;
        }

        return codec_context;
}

int64_t
seek_to_closest_keypoint(float *seek_distance_out,
                         struct video_stream_context *vid_ctx,
                         bool should_random_seek,
                         uint32_t num_requested_frames)
{
        if (!should_random_seek)
                return 0;

        int64_t start_time;
        AVStream *video_stream =
            vid_ctx->format_context->streams[vid_ctx->video_stream_index];
        /**
         * TODO(brendan): Do something smarter to guess the start time, if the
         * container doesn't have it?
         */
        if (video_stream->start_time != AV_NOPTS_VALUE)
                start_time = video_stream->start_time;
        else
                start_time = 0;

        int64_t valid_seek_frame_limit = (vid_ctx->nb_frames -
                                          num_requested_frames);
        if (valid_seek_frame_limit <= 0)
                return AV_NOPTS_VALUE;

        /**
         * NOTE(brendan): skip_past_timestamp looks at the PTS of each frame
         * until it crosses timestamp. Therefore if the video has N frames and
         * we request one, timestamp should be in {0, 1, ..., N - 2}, because
         * the PTS corresponding to timestamp will be dropped (i.e., frame
         * N - 2 could be dropped, leaving N - 1).
         */
        int64_t timestamp = rand() % (valid_seek_frame_limit + 1);
        if (timestamp == 0)
                /* NOTE(brendan): Use AV_NOPTS_VALUE to represent no skip. */
                return AV_NOPTS_VALUE;
        else
                timestamp -= 1;

        timestamp = av_rescale_rnd(timestamp,
                                   vid_ctx->duration,
                                   vid_ctx->nb_frames,
                                   AV_ROUND_DOWN);
        timestamp += start_time;

        /**
         * NOTE(brendan): Convert seek distance from stream timebase units to
         * seconds.
         */
        int64_t tb_num = video_stream->time_base.num;
        int64_t tb_den = video_stream->time_base.den;
        /**
         * TODO(brendan): This seek distance is off by one frame...
         */
        float seek_distance = ((double)timestamp * tb_num) / tb_den;
        if (seek_distance_out != NULL)
                *seek_distance_out = seek_distance;

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
        if (timestamp == AV_NOPTS_VALUE)
                return VID_DECODE_SUCCESS;

        do
        {
                int32_t status = receive_frame(vid_ctx);
                if (status < 0)
                {
                        // fprintf(stderr, "Ran out of frames during seek.\n");
                        return status;
                }
                assert(status == VID_DECODE_SUCCESS);
        } while (vid_ctx->frame->pts < timestamp);

        return VID_DECODE_SUCCESS;
}

int32_t
count_frames(struct video_stream_context *vid_ctx)
{
        AVPacket packet;
        av_init_packet(&packet);
        int32_t gop_num = 0;

        while ((av_read_frame(vid_ctx->format_context, &packet) == 0))
        {
                if (packet.stream_index == vid_ctx->video_stream_index && packet.flags == AV_PKT_FLAG_KEY)
                {
                        ++gop_num;
                        //av_packet_unref(&packet);
                }
                av_packet_unref(&packet);
        }
        av_packet_unref(&packet);
        return gop_num;

        // while (av_read_frame(vid_ctx->format_context, &packet) >= 0){
        //
        //        if(packet.size==0 || packet.stream_index != vid_ctx->video_stream_index)
        //        {
        //                av_packet_unref(&packet);
        //                av_init_packet(&packet);
        //                continue;
        //        }
        //        if (packet.stream_index == vid_ctx->video_stream_index && (packet.flags & AV_PKT_FLAG_KEY)) {
        //                ++(gop_num);
        //        }
        //        av_packet_unref(&packet);
        //        av_init_packet(&packet);
        //}
        //return gop_num;
}

static int32_t
receive_frame2(struct video_stream_context *vid_ctx,
                int32_t current_frame,
                int32_t target_frame)
{
        AVPacket packet;
        int32_t status;
        bool was_frame_received;

        bool was_key_frame;

        av_init_packet(&packet);

        status = avcodec_receive_frame(vid_ctx->codec_context,
                                       vid_ctx->frame);
        if (status == 0)
                return VID_DECODE_SUCCESS;
        else if (status == AVERROR_EOF)
                return VID_DECODE_EOF;
        else if (status != AVERROR(EAGAIN))
                return VID_DECODE_FFMPEG_ERR;

        was_frame_received = false;
        while (!was_frame_received &&
               (av_read_frame(vid_ctx->format_context, &packet) == 0))
        {
                if (packet.stream_index == vid_ctx->video_stream_index && (packet.flags == AV_PKT_FLAG_KEY))
                {
                    if (current_frame == target_frame) {


                                status = avcodec_send_packet(vid_ctx->codec_context,
                                                     &packet);
                                if (status != 0) {
                                av_packet_unref(&packet);
                                return VID_DECODE_FFMPEG_ERR;}

                                status = avcodec_receive_frame(vid_ctx->codec_context,
                                                               vid_ctx->frame);
                                if (status == 0) {
                                        was_frame_received = true;
                                }
                                else if (status != AVERROR(EAGAIN)) {
                                        av_packet_unref(&packet);
                                        return VID_DECODE_FFMPEG_ERR;
                                }
                    }
                    else {
                        was_frame_received = true;
                    }
                }
                av_packet_unref(&packet);
        }

        if (was_frame_received)
                return VID_DECODE_SUCCESS;

        /**
         * NOTE(brendan): Flush/drain the codec. After this, subsequent calls
         * to receive_frame will return frames until EOF.
         *
         * See FFmpeg's libavcodec/avcodec.h.
         */
        av_init_packet(&packet);
        packet.data = NULL;
        packet.size = 0;

        status = avcodec_send_packet(vid_ctx->codec_context,
                                     &packet);
        if (status == 0)
        {
                status = avcodec_receive_frame(vid_ctx->codec_context,
                                               vid_ctx->frame);
                if (status == 0)
                {
                        av_packet_unref(&packet);
                        return VID_DECODE_SUCCESS;
                }
        }

        av_packet_unref(&packet);

        return VID_DECODE_EOF;
}


void decode_video_from_frame_nums(uint8_t *dest,
                                  struct video_stream_context *vid_ctx,
                                  int32_t num_requested_frames,
                                  const int32_t *frame_numbers,
                                  uint32_t *rewidth,
                                  uint32_t *reheight,
                                  bool should_seek)
{
        if (num_requested_frames <= 0)
                return;

        AVCodecContext *codec_context = vid_ctx->codec_context;
        struct SwsContext *sws_context = sws_getContext(codec_context->width,
                                                        codec_context->height,
                                                        codec_context->pix_fmt,
                                                        //codec_context->width,
                                                        //codec_context->height,
                                                        *rewidth,
                                                        *reheight,
                                                        AV_PIX_FMT_BGR24,  // frame mode
                                                        //AV_PIX_FMT_RGB24,
                                                        SWS_FAST_BILINEAR,
                                                        //SWS_BILINEAR,
                                                        //SWS_POINT,
                                                        NULL,
                                                        NULL,
                                                        NULL);
        assert(sws_context != NULL);

        // AVFrame *frame_rgb = allocate_rgb_image(codec_context);
        // resize
        AVFrame *frame_rgb = allocate_rgb_image2(codec_context, *rewidth, *reheight);
        assert(frame_rgb != NULL);

        int32_t status;
        uint32_t copied_bytes = 0;
        const uint32_t bytes_per_row = 3 * frame_rgb->width;
        const uint32_t bytes_per_frame = bytes_per_row * frame_rgb->height;
        int32_t current_frame_index = 0;
        int32_t out_frame_index = 0;
        int64_t prev_pts = 0;
        if (should_seek)
        {
                /**
                 * NOTE(brendan): Convert from frame number to video stream
                 * time base by multiplying by the _average_ time (in
                 * video_stream->time_base units) per frame.
                 */
                int32_t avg_frame_duration = (vid_ctx->duration /
                                              vid_ctx->nb_frames);
                int64_t timestamp = frame_numbers[0] * avg_frame_duration;
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

                current_frame_index = vid_ctx->frame->pts / avg_frame_duration;
                assert(current_frame_index <= frame_numbers[0]);

                /**
                 * NOTE(brendan): Handle the chance that the seek brought the
                 * stream exactly to the first desired frame index.
                 */
                if (current_frame_index == frame_numbers[0])
                {
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
             ++out_frame_index)
        {
                int32_t desired_frame_num = frame_numbers[out_frame_index];
                assert((desired_frame_num >= current_frame_index) &&
                       (desired_frame_num >= 0));
                /* Loop frames instead of aborting if we asked for too many. */
                if (desired_frame_num > vid_ctx->nb_frames)
                {
                        loop_to_buffer_end(dest,
                                           copied_bytes,
                                           out_frame_index,
                                           bytes_per_frame,
                                           num_requested_frames);
                        goto out_free_frame_rgb_and_sws;
                }
                //status = receive_frame2(vid_ctx, current_frame_index, desired_frame_num);
                while (current_frame_index <= desired_frame_num)
                {
                        status = receive_frame2(vid_ctx, current_frame_index, desired_frame_num);
                        if (status == VID_DECODE_EOF)
                        {
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
                        ++current_frame_index;
                        // if (vid_ctx->frame->pts > prev_pts)
                        // {
                        //         ++current_frame_index;
                        //         prev_pts = vid_ctx->frame->pts;
                        // }
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
