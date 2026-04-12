from setuptools import setup, Extension
from distutils.command.build_ext import build_ext

module = Extension(
    '_hrl',
    sources=['hrl_wrap.cxx'],   # le wrapper généré par SWIG
    include_dirs=['../../src'],              # où est hrl.h
    library_dirs=['../../build'],             # où est hrl.lib / hrl.dll
    libraries=['hrldll'],                   # nom du .lib sans extension
    extra_link_args=['-lmsvcrt'],  # remplace msvcr70 par msvcrt moderne
)

setup(name='hrl', ext_modules=[module])
