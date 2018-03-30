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

        assert((video_stream->duration > 0) && (video_stream->nb_frames > 0));

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

static PyObject *
loadvid_frame_nums(PyObject *UNUSED(dummy), PyObject *args, PyObject *kw)
{
        PyObject *result = NULL;
        const char *video_bytes;
        Py_ssize_t in_size_bytes;
        PyObject *frame_nums = NULL;
        uint32_t width = 256;
        uint32_t height = 256;
        static char *kwlist[] = {"encoded_video",
                                 "frame_nums",
                                 "width",
                                 "height",
                                 0};

        if (!PyArg_ParseTupleAndKeywords(args,
                                         kw,
                                         "y#|$OII:loadvid_frame_nums",
                                         kwlist,
                                         &video_bytes,
                                         &in_size_bytes,
                                         &frame_nums,
                                         &width,
                                         &height))
                return NULL;

        if (!PySequence_Check(frame_nums)) {
                PyErr_SetString(PyExc_TypeError,
                                "frame_nums needs to be a sequence");
                return NULL;
        }

        const Py_ssize_t num_frames = PySequence_Size(frame_nums);
        PyByteArrayObject *frames = alloc_pyarray(num_frames*width*height*3);
        if (PyErr_Occurred() || (frames == NULL))
                return (PyObject *)frames;

        struct video_stream_context vid_ctx;
        struct buffer_data input_buf = {.ptr = video_bytes,
                                        .offset_bytes = 0,
                                        .total_size_bytes = in_size_bytes};
        int32_t status = setup_vid_stream_context(&vid_ctx, &input_buf);
        if (status != LOADVID_SUCCESS) {
                if (status == LOADVID_ERR_STREAM_INDEX)
                        return (PyObject *)frames;

                return NULL;
        }

        assert(((uint32_t)vid_ctx.codec_context->width == width) &&
               ((uint32_t)vid_ctx.codec_context->height == height));

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
                                     frame_nums_buf);

        PyMem_RawFree(frame_nums_buf);

clean_up:
        clean_up_vid_ctx(&vid_ctx);

        if (result != (PyObject *)frames)
                Py_DECREF(frames);

        return result;
}

static PyObject *
loadvid(PyObject *UNUSED(dummy), PyObject *args, PyObject *kw)
{
        PyObject *result = NULL;
        const char *video_bytes;
        Py_ssize_t in_size_bytes;
        bool should_random_seek = true;
        uint32_t width = 256;
        uint32_t height = 256;
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

        PyByteArrayObject *frames = alloc_pyarray(num_frames*width*height*3);
        if (PyErr_Occurred() || (frames == NULL))
                return (PyObject *)frames;

        struct video_stream_context vid_ctx;
        struct buffer_data input_buf = {.ptr = video_bytes,
                                        .offset_bytes = 0,
                                        .total_size_bytes = in_size_bytes};
        int32_t status = setup_vid_stream_context(&vid_ctx, &input_buf);
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
                                         vid_ctx.format_context,
                                         vid_ctx.video_stream_index,
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
        result = Py_BuildValue("Of", frames, seek_distance);
        Py_DECREF(frames);

        return result;
}

static PyMethodDef lintel_methods[] = {
        {"loadvid",
         (PyCFunction)loadvid,
         METH_VARARGS | METH_KEYWORDS,
         PyDoc_STR("loadvid(encoded_video, should_random_seek, width, height, num_frames, fps_cap) -> "
                   "tuple(decoded video ByteArray object, seek_distance)")},
        {"loadvid_frame_nums",
         (PyCFunction)loadvid_frame_nums,
         METH_VARARGS | METH_KEYWORDS,
         PyDoc_STR("loadvid_frame_nums(encoded_video, frame_nums, width, height) -> "
                   "decoded video ByteArray object")},
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
