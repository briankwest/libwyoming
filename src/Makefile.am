lib_LTLIBRARIES = libwyoming.la

libwyoming_la_SOURCES = \
	event.c \
	client.c \
	server.c \
	cJSON/cJSON.c

if HAVE_PIPER
libwyoming_la_SOURCES += piper.c
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
libwyoming_la_LIBADD += -lpiper -lonnxruntime
endif
