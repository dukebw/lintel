"""Installs Lintel, the video decoding Python module."""
import distutils.core
import setuptools


lintel_module = distutils.core.Extension(
    '_lintel',
    define_macros=[('MAJOR_VERSION', '0'), ('MINOR_VERSION', '0')],
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
                 url='https://brendanduke.ca',
                 version='0.1')
