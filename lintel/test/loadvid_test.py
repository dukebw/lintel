# Copyright 2018 Brendan Duke.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Unit test for loadvid."""
import random
import time

import click
import numpy as np
import matplotlib.pyplot as plt

import lintel


def _loadvid_test_vanilla(filename, width, height):
    """Tests the usual loadvid call.

    The input file, an encoded video corresponding to `filename`, is repeatedly
    decoded (with a random seek). The first and last of the returned frames are
    plotted using `matplotlib.pyplot`.
    """
    with open(filename, 'rb') as f:
        encoded_video = f.read()

    num_frames = 32
    for _ in range(10):
        start = time.perf_counter()
        result = lintel.loadvid(encoded_video,
                                should_random_seek=True,
                                width=width,
                                height=height,
                                num_frames=num_frames)

        # NOTE(brendan): dynamic size returns (frames, width, height,
        # seek_distance).
        if (width == 0) and (height == 0):
            decoded_frames, width, height, _ = result
        else:
            decoded_frames, _ = result

        decoded_frames = np.frombuffer(decoded_frames, dtype=np.uint8)
        decoded_frames = np.reshape(decoded_frames,
                                    newshape=(num_frames, height, width, 3))
        end = time.perf_counter()

        print('time: {}'.format(end - start))
        plt.imshow(decoded_frames[0, ...])
        plt.show()
        plt.imshow(decoded_frames[-1, ...])
        plt.show()


def _loadvid_test_frame_nums(filename,
                             width,
                             height,
                             start_frame,
                             should_seek):
    """Tests loadvid_frame_nums Python extension.

    `loadvid_frame_nums` takes a list of (strictly increasing, and not
    repeated) frame indices to decode from the encoded video corresponding to
    `filename`.

    This function randomly selects frames to decode, in a loop, decodes the
    chosen frames with `loadvid_frame_nums`, and visualizes the resulting
    frames (all of them) using `matplotlib.pyplot`.
    """
    with open(filename, 'rb') as f:
        encoded_video = f.read()

    num_frames = 32
    for _ in range(10):
        start = time.perf_counter()

        i = start_frame
        frame_nums = []
        for _ in range(num_frames):
            frame_nums.append(i)
            i += int(random.uniform(1, 4))

        result = lintel.loadvid_frame_nums(encoded_video,
                                           frame_nums=frame_nums,
                                           width=width,
                                           height=height,
                                           should_seek=should_seek)

        if (width == 0) and (height == 0):
            decoded_frames, width, height = result
        else:
            decoded_frames = result

        decoded_frames = np.frombuffer(decoded_frames, dtype=np.uint8)
        decoded_frames = np.reshape(decoded_frames,
                                    newshape=(num_frames, height, width, 3))
        end = time.perf_counter()

        print('time: {}'.format(end - start))
        for i in range(num_frames):
            plt.imshow(decoded_frames[i, ...])
            plt.show()


@click.command()
@click.option('--dynamic-size/--no-dynamic-size',
              default=False,
              help='Whether lintel should dynamically find video size.')
@click.option('--filename',
              default=None,
              type=str,
              help='Name of the input video.')
@click.option('--height',
              default=None,
              type=int,
              help='The _exact_ height of the input video.')
@click.option('--width',
              default=None,
              type=int,
              help='The _exact_ width of the input video.')
@click.option('--frame-nums',
              'test_name',
              flag_value='frame_nums',
              default=True)
@click.option('--loadvid',
              'test_name',
              flag_value='loadvid')
@click.option('--should-seek/--no-should-seek',
              default=False,
              help='Whether to use the potentially frame-inaccurate seek.')
@click.option('--start-frame',
              default=0,
              type=int,
              help='Which frame to start decoding from.')
def loadvid_test(dynamic_size,
                 filename,
                 width,
                 height,
                 test_name,
                 should_seek,
                 start_frame):
    """Tests the lintel.loadvid Python extension.

    This program will run tests to sanity check -- visually, by using
    matplotlib to plot individual frames -- that Lintel is working.

    This program also acts as a sample use case for the APIs provided by
    Lintel.
    """
    if dynamic_size:
        width = 0
        height = 0

    if test_name == 'loadvid':
        _loadvid_test_vanilla(filename, width, height)
    elif test_name == 'frame_nums':
        _loadvid_test_frame_nums(filename,
                                 width,
                                 height,
                                 start_frame,
                                 should_seek)
