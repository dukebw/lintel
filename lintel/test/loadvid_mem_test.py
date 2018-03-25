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

"""Test for memory leaks."""
import argparse

import h5py
import psutil

import loadvid


def loadvid_mem_test(hdf5_filename):
    """Prints memory usage (by this process) in megabytes every iteration."""
    p = psutil.Process()
    with h5py.File(hdf5_filename, mode='r') as h5_data:
        print('Number of example: {}'.format(h5_data.attrs['num_examples']))
        for i in range(h5_data.attrs['num_examples']):
            loadvid.loadvid(h5_data['video'][i],
                            should_random_seek=True,
                            width=256,
                            height=256,
                            num_frames=32)

            loadvid.loadvid_frame_nums(
                h5_data['video'][i],
                frame_nums=[i for i in range(32)],
                width=256,
                height=256)

            print('{}'.format(p.memory_info().vms/10**6))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(
        description="""Unit test for decode H264 op.""")

    parser.add_argument('--video-hdf5-file',
                        type=str,
                        default=None,
                        help=('Test video dataset HDF5 filename.'))

    args = parser.parse_args()

    loadvid_mem_test(args.video_hdf5_file)
