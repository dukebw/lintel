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

"""Installs Lintel, the video decoding Python module."""
import distutils.core
import setuptools


lintel_module = distutils.core.Extension(
    '_lintel',
    define_macros=[('MAJOR_VERSION', '1'), ('MINOR_VERSION', '0')],
    undef_macros=['NDEBUG'],
    include_dirs=['/usr/include/ffmpeg', 'lintel'],
    libraries=['avformat', 'avcodec', 'swscale', 'avutil', 'swresample'],
    sources=['lintel/py_ext/lintelmodule.c',
             'lintel/core/video_decode.c'])


setuptools.setup(author='Brendan Duke',
                 author_email='brendanw.duke@gmail.com',
                 name='Lintel',
                 description='Video decoding package.',
                 long_description="""
                     Extension for loading video from Python, by directly
                     using the FFmpeg C API.
                 """,
                 entry_points="""
                     [console_scripts]
                     lintel_test=lintel.test.loadvid_test:loadvid_test
                 """,
                 install_requires=['Click', 'numpy'],
                 ext_modules=[lintel_module],
                 packages=setuptools.find_packages(),
                 py_modules=['lintel.test.loadvid_test'],
                 url='https://brendanduke.ca',
                 version='1.0')
