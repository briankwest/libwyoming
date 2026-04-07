lib_LTLIBRARIES = libwyoming.la

libwyoming_la_SOURCES = \
	event.c \
	client.c \
	server.c \
	cJSON/cJSON.c

if HAVE_PIPER
libwyoming_la_SOURCES += piper.c piper_bridge.cpp
endif

if HAVE_SHERPA
libwyoming_la_SOURCES += sherpa.c
endif

libwyoming_la_CFLAGS = \
	-std=c99 -D_POSIX_C_SOURCE=200809L \
	-Wall -Wextra -pedantic \
	-I$(top_srcdir)/include \
	-I$(srcdir)/cJSON \
	-DLIBWYOMING_GIT_HASH=\"$(GIT_HASH)\"

libwyoming_la_LDFLAGS = -version-info 0:0:0
libwyoming_la_LIBADD = -lm

if HAVE_PIPER
libwyoming_la_LIBADD += -lpiper_phonemize -lonnxruntime -lespeak-ng -lstdc++
endif

# C++ flags for piper bridge
libwyoming_la_CXXFLAGS = -std=c++17 -Wall -Wextra \
	-I$(top_srcdir)/include \
	-I$(srcdir) \
	-I$(srcdir)/cJSON \
	-DLIBWYOMING_GIT_HASH=\"$(GIT_HASH)\"

if HAVE_SHERPA
libwyoming_la_CFLAGS += $(SHERPA_CFLAGS)
libwyoming_la_LIBADD += $(SHERPA_LIBS)
endif
