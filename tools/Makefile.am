bin_PROGRAMS = wyoming-piper-server

AM_CFLAGS = -std=c99 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
	-Wall -Wextra -pedantic \
	-I$(top_srcdir)/include \
	-I$(top_srcdir)/src/cJSON \
	-DLIBWYOMING_GIT_HASH=\"$(GIT_HASH)\"

wyoming_piper_server_SOURCES = wyoming-piper-server.c
wyoming_piper_server_LDADD = $(top_builddir)/src/libwyoming.la -lm
