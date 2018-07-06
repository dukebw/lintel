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

/**
 * Load video data.
 */
#include "core/video_decode.h"
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <Python.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>

#define UNUSED(x) x __attribute__ ((__unused__))

#define LOADVID_SUCCESS 0
#define LOADVID_ERR (-1)
#define LOADVID_ERR_STREAM_INDEX (-2)

PyDoc_STRVAR(module_doc, "Module for loading video data.");

/**
 * Allocates a PyByteArrayObject, and `out_size_bytes` of buffer for that
 * object.
 *
 * If the memory allocation fails, the references will be cleaned up by this
 * function. If a reference to a PyByteArrayObject object is returned, that
 * reference is owned by the caller.
 */
static PyByteArrayObject *
alloc_pyarray(const uint32_t out_size_bytes)
{
        PyByteArrayObject *frames = PyObject_New(PyByteArrayObject,
                                                 &PyByteArray_Type);
        if (frames == NULL)
                return NULL;

        frames->ob_bytes = PyObject_Malloc(out_size_bytes);
        if (frames->ob_bytes == NULL) {
                Py_DECREF(frames);
                return (PyByteArrayObject *)PyErr_NoMemory();
        }

        Py_SIZE(frames) = out_size_bytes;
        frames->ob_alloc = out_size_bytes;
        frames->ob_start = frames->ob_bytes;
        frames->ob_exports = 0;

        return frames;
}

/**
 * setup_vid_stream_context() - Fills in the members of `vid_ctx` by allocating
 * and setting up FFmpeg contexts through libavformat and libavcodec.
 * @vid_ctx: Output video_stream_context to be filled in.
 * @input_buf: buffer_data structure injected into `vid_ctx`, which should have
 * the same lifetime as `vid_ctx`.
 *
 * LOADVID_ERR_STREAM_INDEX is returned if the video corresponding to
 * `input_buf`'s stream index was not found. For other errors, LOADVID_ERR is
 * returned. LOADVID_SUCCESS is returned on success.
 */
static int32_t
setup_vid_stream_context(struct video_stream_context *vid_ctx,
                         struct buffer_data *input_buf)
{
        const uint32_t buffer_size = 32*1024;
        uint8_t *avio_ctx_buffer = av_malloc(buffer_size);
        if (avio_ctx_buffer == NULL)
                return LOADVID_ERR;

        AVIOContext *avio_ctx = avio_alloc_context(avio_ctx_buffer,
                                                   buffer_size,
                                                   0,
                                                   (void *)input_buf,
                                                   &read_memory,
                                                   NULL,
                                                   &seek_memory);
        if (avio_ctx == NULL)
                goto clean_up_avio_ctx_buffer;

        vid_ctx->format_context = avformat_alloc_context();
        if (vid_ctx->format_context == NULL)
                goto clean_up_avio_ctx;

        vid_ctx->video_stream_index =
                setup_format_context(&vid_ctx->format_context,
                                     avio_ctx,
                                     input_buf,
                                     buffer_size);
        if (vid_ctx->video_stream_index < 0) {
                fprintf(stderr, "Stream index not found.\n");

                if (vid_ctx->video_stream_index == VID_DECODE_FFMPEG_ERR)
                        /**
                         * NOTE(brendan): Return a unique error code here so
                         * that if there is no video stream a garbage buffer
                         * can be returned.
                         *
                         * format_context, avio_ctx, and avio_ctx_buffer have
                         * already been cleaned up (see setup_format_context
                         * comment).
                         */
                        return LOADVID_ERR_STREAM_INDEX;

                goto clean_up_format_context;
        }

        AVStream *video_stream =
                vid_ctx->format_context->streams[vid_ctx->video_stream_index];
        vid_ctx->codec_context = open_video_codec_ctx(video_stream);
        if (vid_ctx->codec_context == NULL)
                goto clean_up_format_context;

        if ((video_stream->duration <= 0) || (video_stream->nb_frames <= 0)) {
                /**
                 * Some video containers (e.g., webm) contain indices of only
                 * frames-of-interest, e.g., keyframes, and therefore the whole
                 * file must be parsed to get the number of frames (nb_frames
                 * will be zero).
                 *
                 * Also, for webm only the duration of the entire file is
                 * specified in the header (as opposed to the stream duration),
                 * so the duration must be taken from the AVFormatContext, not
                 * the AVStream.
                 *
                 * See this SO answer: https://stackoverflow.com/a/32538549
                 */

                /**
                 * Compute nb_frames from fmt ctx duration (microseconds) and
                 * stream FPS (frames/second).
                 */
                assert(video_stream->avg_frame_rate.den > 0);

                enum AVRounding rnd = (enum AVRounding)(AV_ROUND_DOWN |
                                                        AV_ROUND_PASS_MINMAX);
                int64_t fps_num = video_stream->avg_frame_rate.num;
                int64_t fps_den =
                        video_stream->avg_frame_rate.den*(int64_t)AV_TIME_BASE;
                vid_ctx->nb_frames =
                        av_rescale_rnd(vid_ctx->format_context->duration,
                                       fps_num,
                                       fps_den,
                                       rnd);

                /**
                 * NOTE(brendan): fmt ctx duration in microseconds =>
                 *
                 * fmt ctx duration == (stream duration)*(stream timebase)*1e6
                 *
                 * since stream timebase is in units of
                 * seconds / (stream timestamp). The rest of the code expects
                 * the duration in stream timestamps, so do the conversion
                 * here.
                 *
                 * Multiply the timebase numerator by AV_TIME_BASE to get a
                 * more accurate rounded duration by doing the rounding in the
                 * higher precision units.
                 */
                int64_t tb_num = video_stream->time_base.num*(int64_t)AV_TIME_BASE;
                int64_t tb_den = video_stream->time_base.den;
                vid_ctx->duration =
                        av_rescale_rnd(vid_ctx->format_context->duration,
                                       tb_den,
                                       tb_num,
                                       rnd);
        } else {
                vid_ctx->duration = video_stream->duration;
                vid_ctx->nb_frames = video_stream->nb_frames;
        }

        vid_ctx->frame = av_frame_alloc();
        if (vid_ctx->frame == NULL)
                goto clean_up_avcodec;

        return LOADVID_SUCCESS;

clean_up_avcodec:
        avcodec_close(vid_ctx->codec_context);
        avcodec_free_context(&vid_ctx->codec_context);
clean_up_format_context:
        avformat_close_input(&vid_ctx->format_context);
clean_up_avio_ctx:
        av_freep(&avio_ctx);
clean_up_avio_ctx_buffer:
        av_freep(&avio_ctx_buffer);

        return LOADVID_ERR;
}

static void
clean_up_vid_ctx(struct video_stream_context *vid_ctx)
{
        av_frame_free(&vid_ctx->frame);
        avcodec_close(vid_ctx->codec_context);
        avcodec_free_context(&vid_ctx->codec_context);
        av_freep(&vid_ctx->format_context->pb->buffer);
        av_freep(&vid_ctx->format_context->pb);
        avformat_close_input(&vid_ctx->format_context);
}

/**
 * get_vid_width_height() - Sets `width` and `height` dynamically based on the
 * video's `AVCodecContext` if they are not already set.
 * @width: In/out pointer to width (unchecked for NULL).
 * @height: In/out pointer to height (unchecked for NULL).
 * @codec_context: Already-opened video `AVCodecContext`.
 *
 * Returns true iff the size has been set dynamically.
 * Also, checks that the width/height matches the AVCodecContext regardless
 * (via assert).
 */
static bool
get_vid_width_height(uint32_t *width,
                     uint32_t *height,
                     AVCodecContext *codec_context)
{
        /* NOTE(brendan): If no size is passed, dynamically find size. */
        bool is_size_dynamic = (*width == 0) && (*height == 0);
        if (is_size_dynamic) {
                *width = codec_context->width;
                *height = codec_context->height;
        }

        assert(((uint32_t)codec_context->width == *width) &&
               ((uint32_t)codec_context->height == *height));

        return is_size_dynamic;
}

static PyObject *
loadvid_frame_nums(PyObject *UNUSED(dummy), PyObject *args, PyObject *kw)
{
        PyObject *result = NULL;
        const char *video_bytes = NULL;
        Py_ssize_t in_size_bytes = 0;
        PyObject *frame_nums = NULL;
        uint32_t width = 0;
        uint32_t height = 0;
        /* NOTE(brendan): should_seek must be int (not bool) because Python. */
        int32_t should_seek = false;
        static char *kwlist[] = {"encoded_video",
                                 "frame_nums",
                                 "width",
                                 "height",
                                 "should_seek",
                                 0};

        if (!PyArg_ParseTupleAndKeywords(args,
                                         kw,
                                         "y#|$OIIp:loadvid_frame_nums",
                                         kwlist,
                                         &video_bytes,
                                         &in_size_bytes,
                                         &frame_nums,
                                         &width,
                                         &height,
                                         &should_seek))
                return NULL;

        if (!PySequence_Check(frame_nums)) {
                PyErr_SetString(PyExc_TypeError,
                                "frame_nums needs to be a sequence");
                return NULL;
        }

        struct video_stream_context vid_ctx;
        struct buffer_data input_buf = {.ptr = video_bytes,
                                        .offset_bytes = 0,
                                        .total_size_bytes = in_size_bytes};
        int32_t status = setup_vid_stream_context(&vid_ctx, &input_buf);

        bool is_size_dynamic = get_vid_width_height(&width,
                                                    &height,
                                                    vid_ctx.codec_context);

        /**
         * TODO(brendan): There is a hole in the logic here, where a bad status
         * could be returned from `setup_vid_stream_context`, but the width and
         * height from `codec_context` is still used to allocate `frames`.
         *
         * It is safer to pass the width and height as arguments, if there is a
         * possibility that videos in the dataset have no video stream.
         */
        const Py_ssize_t num_frames = PySequence_Size(frame_nums);
        PyByteArrayObject *frames = alloc_pyarray(num_frames*width*height*3);
        if (PyErr_Occurred() || (frames == NULL))
                return (PyObject *)frames;


        if (status != LOADVID_SUCCESS) {
                if (status == LOADVID_ERR_STREAM_INDEX)
                        return (PyObject *)frames;

                return NULL;
        }

        int32_t *frame_nums_buf = PyMem_RawMalloc(num_frames*sizeof(int32_t));
        if (frame_nums_buf == NULL)
                return PyErr_NoMemory();

        for (int32_t i = 0;
             i < num_frames;
             ++i) {
                PyObject *item = PySequence_GetItem(frame_nums, i);
                if (item == NULL)
                        goto clean_up;

                frame_nums_buf[i] = PyLong_AsLong(item);
                Py_DECREF(item);
                if (PyErr_Occurred())
                        goto clean_up;
        }

        result = (PyObject *)frames;

        decode_video_from_frame_nums((uint8_t *)(frames->ob_bytes),
                                     &vid_ctx,
                                     num_frames,
                                     frame_nums_buf,
                                     should_seek);

        PyMem_RawFree(frame_nums_buf);

clean_up:
        clean_up_vid_ctx(&vid_ctx);

        if (result != (PyObject *)frames) {
                Py_CLEAR(frames);
                return result;
        }

        if (!is_size_dynamic)
                return (PyObject *)frames;

        result = Py_BuildValue("Oii", frames, width, height);
        Py_DECREF(frames);

        return result;
}

static PyObject *
loadvid(PyObject *UNUSED(dummy), PyObject *args, PyObject *kw)
{
        PyObject *result = NULL;
        const char *video_bytes = NULL;
        Py_ssize_t in_size_bytes = 0;
        bool should_random_seek = true;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t num_frames = 32;
        float seek_distance = 0.0f;
        float fps_cap = 25.0f;
        static char *kwlist[] = {"encoded_video",
                                 "should_random_seek",
                                 "width",
                                 "height",
                                 "num_frames",
                                 "fps_cap",
                                 0};

        if (!PyArg_ParseTupleAndKeywords(args,
                                         kw,
                                         "y#|$pIIIf:loadvid",
                                         kwlist,
                                         &video_bytes,
                                         &in_size_bytes,
                                         &should_random_seek,
                                         &width,
                                         &height,
                                         &num_frames,
                                         &fps_cap))
                return NULL;

        struct video_stream_context vid_ctx;
        struct buffer_data input_buf = {.ptr = video_bytes,
                                        .offset_bytes = 0,
                                        .total_size_bytes = in_size_bytes};
        int32_t status = setup_vid_stream_context(&vid_ctx, &input_buf);

        bool is_size_dynamic = get_vid_width_height(&width,
                                                    &height,
                                                    vid_ctx.codec_context);

        PyByteArrayObject *frames = alloc_pyarray(num_frames*width*height*3);
        if (PyErr_Occurred() || (frames == NULL))
                return (PyObject *)frames;

        if (status != LOADVID_SUCCESS) {
                /**
                 * NOTE(brendan): In case there was a stream index error,
                 * return a garbage buffer.
                 */
                if (status == LOADVID_ERR_STREAM_INDEX)
                          goto return_frames;

                return NULL;
        }

        int64_t timestamp =
                seek_to_closest_keypoint(&seek_distance,
                                         &vid_ctx,
                                         should_random_seek,
                                         num_frames,
                                         fps_cap);

        /*
         * NOTE(brendan): after this point, the only possible errors are due to
         * not having enough frames in the video stream past the initial seek
         * point. All other errors are covered by asserts.
         *
         * Therefore we return the frames buffer regardless, and it is a
         * feature to return garbage in the decoded video output buffer, rather
         * than returning an error, if there weren't any frames to decode in
         * the first place.
         */
        result = (PyObject *)frames;

        status = skip_past_timestamp(&vid_ctx, timestamp);
        if (status != VID_DECODE_SUCCESS)
                goto clean_up_av_frame;

        decode_video_to_out_buffer((uint8_t *)(frames->ob_bytes),
                                   &vid_ctx,
                                   num_frames,
                                   fps_cap);

clean_up_av_frame:
        clean_up_vid_ctx(&vid_ctx);

        if (result != (PyObject *)frames) {
                Py_CLEAR(frames);
                return result;
        }

return_frames:
        if (!is_size_dynamic)
                result = Py_BuildValue("Of", frames, seek_distance);
        else
                result = Py_BuildValue("Oiif",
                                       frames,
                                       width,
                                       height,
                                       seek_distance);
        Py_DECREF(frames);

        return result;
}

static PyMethodDef lintel_methods[] = {
        {"loadvid",
         (PyCFunction)loadvid,
         METH_VARARGS | METH_KEYWORDS,
         PyDoc_STR("loadvid(encoded_video, should_random_seek, width, height, num_frames, fps_cap) -> "
                   "tuple(decoded video ByteArray object, seek_distance) or\n"
                   "tuple(decoded video ByteArray object, width, height, seek_distance)\n"
                   "if width and height are not passed as arguments.")},
        {"loadvid_frame_nums",
         (PyCFunction)loadvid_frame_nums,
         METH_VARARGS | METH_KEYWORDS,
         PyDoc_STR("loadvid_frame_nums(encoded_video, frame_nums, width, height, should_seek) -> "
                   "decoded video ByteArray object or\n"
                   "tuple(decoded video ByteArray object, width, height)\n"
                   "if width and height are not passed as arguments.")},
        {NULL, NULL, 0, NULL}
};

static struct PyModuleDef
lintelmodule = {
        PyModuleDef_HEAD_INIT,
        "_lintel",
        module_doc,
        0,
        lintel_methods,
        NULL,
        NULL,
        NULL,
        NULL
};

PyMODINIT_FUNC
PyInit__lintel(void)
{
        av_register_all();
        av_log_set_level(AV_LOG_ERROR);
        srand(time(NULL));

        return PyModuleDef_Init(&lintelmodule);
}
