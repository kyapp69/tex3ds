# Makefile.am -- Process this file with automake to produce Makefile.in

AUTOMAKE_OPTIONS = subdir-objects

bin_PROGRAMS = tex3ds

tex3ds_SOURCES = source/atlas.cpp \
                 source/encode.cpp \
                 source/huff.cpp \
                 source/lzss.cpp \
                 source/magick_compat.cpp \
                 source/main.cpp \
                 source/rg_etc1.cpp \
                 source/rle.cpp \
                 include/atlas.h \
                 include/compress.h \
                 include/encode.h \
                 include/magick_compat.h \
                 include/quantum.h \
                 include/rg_etc1.h \
                 include/subimage.h

tex3ds_LDADD = $(ImageMagick_LIBS)
AM_CPPFLAGS   = -I$(srcdir)/include -D_GNU_SOURCE $(ImageMagick_CFLAGS)
EXTRA_DIST = autogen.sh
