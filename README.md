# lintel


Lintel is a Python module that can be used to decode videos, and return a byte
array of all of the frames in the video, using the FFmpeg C interface directly.

Lintel was created for the purpose of developing machine learning algorithms
using video datasets such as the
[NTU RGB+D](http://rose1.ntu.edu.sg/Datasets/actionRecognition.asp),
[Kinetics](https://deepmind.com/research/open-source/open-source-datasets/kinetics/)
and [Charades](http://allenai.org/plato/charades/) action recognition datasets.

The foremost advantages of Lintel are:

1. Lintel provides a simple and fast Python interface to video decoding that
   can be dropped into existing machine learning training scripts.

2. By decoding video on the fly in a data processing pipeline, input pipelines
   can be made to circumvent an I/O bottleneck that would become a problem if
   data were stored as frames in an encoded image format such as JPEG.

3. Decoding videos on the fly using the FFmpeg C API provides a high degree of
   control over the input. For example, video can be decoded at dynamic
   framerates, with no loss in efficiency.

   An example of this control is in the implementation of `loadvid_frame_nums`,
   where a list of frame indices is passed to indicate which specific frames
   are to be decoded.

4. By using the FFmpeg C API directly, as opposed to piping input to the
   `ffmpeg` command line tool, a number of issues and complications surrounding
   interfacing the `ffmpeg` command line tool from a performance-intensive
   machine learning application are completely avoided.


# Pre-requisites

Python 3 is required. To use Python 2, I believe a (potentially small) amount
of code would have to be changed in `lintel/py_ext/lintelmodule.c`.

A version of FFmpeg that supports the `avcodec_send_packet()` and
`avcodec_receive_frame()` API for decoding video is required. For example,
FFmpeg version 3.3.6 should work fine, as should downloading and installing the
latest development version of FFmpeg.

If the version of FFmpeg distributed with your system is too old, see the
Installing FFmpeg from Source section below to install a newer version.


# Installation

Run the following to install a locally editable version of the library, with pip:

`pip3 install --editable . --user`


# Testing Lintel

1. After installing, run:

   `lintel_test --filename <video-filename> --width <width> --height <height>`

   Pass criteria: decoded frames from the video should show up without
   distortion, decoding each clip in < 500ms.

2. Run:

   `lintel_test --filename <video-filename> --width <width> --height <height> --frame-nums --should-seek`

   to test the frame number API.

Passing `--width 0 --height 0` will test the dynamic resizing.


# Usage in a data processing pipeline

The `lintel.loadvid` interface can be used in a Python input pipeline as
follows:

```python
def _sample_frame_sequence_to_4darray(video, dataset, should_random_seek, fps_cap):
    """Called to extract a frame sequence `dataset.num_frames` long, sampled
    uniformly from inside `video`, to a 4D numpy array.
.
    Args:
        video: Encoded video.
        dataset: Dataset meta-info, e.g., width and height.
        should_random_seek: If set to `True`, then `lintel.loadvid` will start
            decoding from a uniformly random seek point in the video (with
            enough space to decode the requested number of frames).

            The seek distance will be returned, so that if the label of the
            data depends on the timestamp, then the label can be dynamically
            set.
        fps_cap: The _maximum_ framerate that will be captured from the video.
            Excess frames will be dropped, i.e., if `fps_cap` is 30 for a video
            with a 60 fps framerate, every other frame will be dropped.

    Returns:
        A tuple (frames, seek_distance) where `frames` is a 4-D numpy array
        loaded from the byte array returned by `lintel.loadvid`, and
        `seek_distance` is the number of seconds into `video` that decoding
        started from.

    Note that the random seeking can be turned off.

    Use _sample_frame_sequence_to_4darray in your PyTorch Dataset object, which
    subclasses torch.utils.data.Dataset. Call _sample_frame_sequence_to_4darray
    in __getitem__. This means that for every minibatch, for each example, a
    random keyframe in the video is seeked to and num_frames frames are decoded
    from there. num_frames would normally tend to be small (if you were going
    to use them as input to a 3D ConvNet or optical flow algorithm), e.g., 32
    frames.
    """
    video, seek_distance = lintel.loadvid(
        video,
        should_random_seek=should_random_seek,
        width=dataset.width,
        height=dataset.height,
        num_frames=dataset.num_frames,
        fps_cap=fps_cap)
    video = np.frombuffer(video, dtype=np.uint8)
    video = np.reshape(
        video, newshape=(dataset.num_frames, dataset.height, dataset.width, 3))

    return video, seek_distance
```

The `lintel.loadvid_frame_nums` API can be used similarly:

```python
def _load_frame_nums_to_4darray(video, dataset, frame_nums):
    """Decodes a specific set of frames from `video` to a 4D numpy array.
    
    Args:
        video: Encoded video.
        dataset: Dataset meta-info, e.g., width and height.
        frame_nums: Indices of specific frame indices to decode, e.g.,
            [1, 10, 30, 35] will return four frames: the first, 10th, 30th and
            35 frames in `video`. Indices must be in strictly increasing order.

    Returns:
        A numpy array, loaded from the byte array returned by
        `lintel.loadvid_frame_nums`, containing the specified frames, decoded.
    """
    decoded_frames = lintel.loadvid_frame_nums(video,
                                               frame_nums=frame_nums,
                                               width=dataset.width,
                                               height=dataset.height)
    decoded_frames = np.frombuffer(decoded_frames, dtype=np.uint8)
    decoded_frames = np.reshape(
        decoded_frames,
        newshape=(dataset.num_frames, dataset.height, dataset.width, 3))

    return decoded_frames
```

Both APIs can be used without passing a width and height, in which case the
width and height of the video will be determined by `libavcodec` and returned
in the result tuple.

```python
decoded_frames, width, height = lintel.loadvid_frame_nums(
    video, frame_nums=frame_nums)

video, width, height, seek_distance = lintel.loadvid(
    video,
    should_random_seek=should_random_seek,
    num_frames=dataset.num_frames,
    fps_cap=fps_cap)
```


# Installing FFmpeg from Source

It may be necessary to compile FFmpeg from source, e.g. if there is no way to
get the development FFmpeg files from the package manager. To do so, nasm, x264
and FFmpeg must all be installed.

Note in the following I assume that you have created a directory `$HOME/.local`
for local installations, and that `$HOME/.local/include`, `$HOME/.local/bin`
and `$HOME/.local/lib` are in your `CPATH`, `PATH` and `LD_LIBRARY_PATH` (as
well as `LIBRARY_PATH`) environment variables, respectively.

1. Download and install nasm:

```
wget http://www.nasm.us/pub/nasm/releasebuilds/2.13.01/nasm-2.13.01.tar.bz2

tar xvjf nasm-2.13.01.tar.bz2 && cd nasm-2.13.01

./configure --prefix=$HOME/.local/

make -j$(nproc) && make install
```

2. Download and install x264:

```
git clone git://git.videolan.org/x264.git && cd x264

./configure --enable-static --enable-shared --prefix=$HOME/.local

make -j$(nproc) && make install
```

3. Download and install FFmpeg:

```
git clone https://github.com/FFmpeg/FFmpeg.git && cd FFmpeg

./configure --enable-shared --enable-gpl --enable-libx264 --enable-pic --enable-runtime-cpudetect --cc="gcc -fPIC" --prefix=$HOME/.local

make -j$(nproc) && make install
```


## Installation Debugging

The following error:

`ImportError: <lintel-path>/_lintel.cpython-36m-x86_64-linux-gnu.so: undefined symbol: avcodec_receive_frame`

can be debugged as follows.

One way to see what shared objects a binary is linking to is using ldd:
`LD_DEBUG=libs ldd <binary-name>`.

E.g.,

`LD_DEBUG=libs ldd <lintel-path>/_lintel.cpython-36m-x86_64-linux-gnu.so`

It should spit out a bunch of information, including a line like this:
libavcodec.so.57 => /export/mlrg/bduke/.local/lib/libavcodec.so.57
(0x00007ff4b997a000). This libavcodec.so.57 => line should point to the new
libavcodec.so that you compiled and installed.

It is possible that this issue may occur if `LIBRARY_PATH` (different from
`LD_LIBRARY_PATH`) is not set during compile time of lintel. `LIBRARY_PATH`
should also point to wherever libavcodec.so lives, the same place as
`LD_LIBRARY_PATH`, but `LIBRARY_PATH` is used at compile time instead of
runtime (i.e., be sure that `LIBRARY_PATH` includes a directory with your new
libavcodec.so in it when you run `pip install` on lintel). I suspect that the
libavcodec.so.57 symbol name is baked into the lintel CPython shared object at
compile time.


# Citing

If you find Lintel useful for an academic publication, then please use the
following BibTeX to cite it:

```
@misc{lintel,
  author = {Duke, Brendan},
  title = {Lintel: Python Video Decoding},
  year = {2018},
  publisher = {GitHub},
  journal = {GitHub repository},
  howpublished = {\url{https://github.com/dukebw/lintel}},
}
```
