# Copyright 2018 Brendan Duke.
#
# This file is part of Lintel.
#
# Lintel is free software: you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later
# version.
#
# Lintel is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
# A PARTICULAR PURPOSE. See the GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along with
# Lintel. If not, see <http://www.gnu.org/licenses/>.

"""Unit test for loadvid."""
import random
import time
import os

import click
import numpy as np
import matplotlib.pyplot as plt

import lintel


def _loadvid_test_frame_nums(filename):
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

    num_frames = 64
    for _ in range(10):
        start = time.perf_counter()

        i = 0
        frame_nums = []
        for _ in range(num_frames):
            i += int(random.uniform(1, 4))
            frame_nums.append(i)

        decoded_frames,w ,h = lintel.loadvid_frame_nums(encoded_video,
                                                                  frame_nums=frame_nums)
        print(w, h)
        decoded_frames = np.frombuffer(decoded_frames, dtype=np.uint8)
        decoded_frames = np.reshape(decoded_frames,
                                    newshape=(num_frames, h, w, 3))
        end = time.perf_counter()

        print('time: {}'.format(end - start))
        for i in range(num_frames):
            plt.imshow(decoded_frames[i, ...])
            plt.show()
            
@click.command()
@click.option('--filename',
              default=None,
              type=str,
              help='Name of the input video.')
def dynamic_test(filename):
    _loadvid_test_frame_nums(filename)
