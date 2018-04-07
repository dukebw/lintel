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
#ifndef _VIDEO_DECODE_H_
#define _VIDEO_DECODE_H_

/**
 * A set of helper functions for decoding video by linking against FFmpeg.
 */

#ifdef __cplusplus
extern "C" {
#endif
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#ifdef __cplusplus
};
#endif
#include <stdint.h>
#include <stdbool.h>

#define VID_DECODE_FFMPEG_ERR (-2)
#define VID_DECODE_EOF (-1)
#define VID_DECODE_SUCCESS 0

struct buffer_data {
        const char *ptr;
        int32_t offset_bytes;
        int32_t total_size_bytes;
};

/**
 * struct video_stream_context - Context needed to decode and receive frames
 * from a video stream.
 * @frame: Output frame to be received.
 * @format_context: Format context to read from.
 * @codec_context: Context of decoder used to decode video stream packets.
 * @video_stream_index: Index of video stream that frames will be read from.
 */
struct video_stream_context {
        AVFrame *frame;
        AVCodecContext *codec_context;
        AVFormatContext *format_context;
        int32_t video_stream_index;
};

/**
 * A function for refilling the buffer from a `struct buffer_data` instance.
 *
 * @param opaque Pointer to the `struct buffer_data` instance.
 * @param buffer Pointer to the buffer to fill.
 * @param buf_size_bytes The size of `buffer`, in bytes.
 *
 * @return The number of bytes written to `buffer`.
 */
int32_t read_memory(void *opaque, uint8_t *buffer, int32_t buf_size_bytes);

/**
 * A function for seeking to a specified byte position in a
 * `struct buffer_data` instance.
 *
 * @param opaque Pointer to the `struct buffer_data` instance.
 * @param offset64 Offset to seek.
 * @param whence One of `SEEK_CUR`, `SEEK_END`, `SEEK_SET` or `AVSEEK_SIZE`.
 *
 * @return The new offset in the `struct buffer_data` instance after seeking.
 */
int64_t seek_memory(void *opaque, int64_t offset64, int32_t whence);

/**
 * Sets up the `AVFormatContext` pointed to by `format_context_ptr`, and finds
 * the first video stream index for `format_context`.
 *
 * IMPORTANT(brendan): If `setup_format_context` fails with
 * VID_DECODE_FFMPEG_ERR, then avformat_open_input() has failed and already
 * freed format_context_ptr, avio_ctx and avio_ctx->buffer.
 *
 * @param format_context_ptr Pointer to the (pointer to) the AVFormatContext to
 * be setup.
 * @param avio_ctx Byte-stream I/O context.
 * @param input_buf Buffer tracking the input byte string.
 * @param buffer_size Size allocated for the AV I/O context.
 *
 * @return Index of the video stream corresponding to `format_context`, or a
 * negative error code on failure.
 */
int32_t
setup_format_context(AVFormatContext **format_context_ptr,
                     AVIOContext *avio_ctx,
                     struct buffer_data *input_buf,
                     const uint32_t buffer_size);

/**
 * Allocates a codec context for video_stream, and opens it.  We cannot call
 * avcodec_open2 on an av_stream's codec context directly.
 *
 * @param video_stream Video stream to open codec context for.
 *
 * @warning If successful, codec_context must be freed with
 * avcodec_free_context, and closed with avcodec_close.
 *
 * @return Opened copy of codec_context on success, NULL on failure.
 */
AVCodecContext *open_video_codec_ctx(AVStream *video_stream);

/**
 * Seeks the video stream corresponding to `video_stream_index` in
 * `format_context->streams` to the closest keypoint frame that comes before
 * a chosen seek distance into the video.
 *
 * The caller should skip frames until a frame with a timestamp past the
 * returned value from this function is received.
 *
 * If `should_random_seek` is set, then the video decoding code will attempt to
 * do a random seek within the valid range of the video, i.e. the range for
 * which `num_requested_frames` can still be grabbed.
 *
 * @param seek_distance_out Output seek_distance variable. Only set if random
 * seeking occurred, therefore this output parameter should be initialized to
 * 0.0f by the caller.
 * @param format_context The format context with the video stream in it.
 * @param video_stream_index Index of the video stream to seek.
 * @param should_random_seek Should a random seek in the video be performed?
 * @param num_requested_frames Number of requested frames to be extracted
 * starting from the seek point.
 *
 * @return The timestamp, in the video stream's `time_base`, corresponding to
 * the seek distance. The seek distance is output in seek_distance_out.
 */
int64_t
seek_to_closest_keypoint(float *seek_distance_out,
                         AVFormatContext *format_context,
                         int32_t video_stream_index,
                         bool should_random_seek,
                         uint32_t num_requested_frames,
                         uint32_t fps_cap);

/**
 * Skips frames until a frame that is past `timestamp` has been reached.
 *
 * @param vid_ctx Context needed to decode frames from the video stream.
 *
 * @return 0 on success, a negative value on failure.
 */
int32_t
skip_past_timestamp(struct video_stream_context *vid_ctx, int64_t timestamp);

/**
 * Decodes video from the video stream corresponding to `video_stream_index`,
 * into raw RGB24 frames in `dest`.
 *
 * If less than `num_requested_frames` are sent from the video stream, then
 * however many frames were received are looped until `num_requested_frames`,
 * unless no frames were received (in which case the output buffer is garbage
 * data).
 *
 * The framerate is capped to a hardcoded value of `FPS_CAP`. Framerates lower
 * than `FPS_CAP` are allowed.
 *
 * TODO(brendan): Support fixing the framerate?
 *
 * @param dest Output RGB24 frame buffer.
 * @param vid_ctx Context needed to decode frames from the video stream.
 * @param num_requested_frames Number of frames requested to fill into `dest`.
 * @param fps_cap Maximum framerate to capture. Higher framerates will be
 * dropped.
 */
void
decode_video_to_out_buffer(uint8_t *dest,
                           struct video_stream_context *vid_ctx,
                           int32_t num_requested_frames,
                           uint32_t fps_cap);

/**
 * decode_video_from_frame_nums() - Decodes video from exactly the frames
 * numbered by `frame_numbers`.
 * @dest: Destination output buffer for decoded frames.
 * @vid_ctx: Context needed to decode frames from the video stream.
 * @num_requested_frames: Number of frames requested to fill into `dest`.
 * @frame_numbers: A list of frame numbers to extract.
 * @should_seek: If false, decoding will be frame-accurate by starting from the
 * first frame in the video and counting frames. However, this method may be
 * slow.
 * Therefore, this `should_seek` flag can be set to true to cause a seek to the
 * closest keyframe before the first desired frame index. Note that this makes
 * the assumption of a fixed FPS, and for variable framerate videos the
 * approximation of average PTS duration per frame is made to do the seek.
 *
 * If there are less than `num_requested_frames` to decode from the video
 * stream, then the initial frames are looped repeatedly until the end of the
 * buffer.
 */
void
decode_video_from_frame_nums(uint8_t *dest,
                             struct video_stream_context *vid_ctx,
                             int32_t num_requested_frames,
                             const int32_t *frame_numbers,
                             bool should_seek);

#endif // _VIDEO_DECODE_H_
