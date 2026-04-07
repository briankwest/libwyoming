bin_PROGRAMS = wyoming-server wyoming-describe

AM_CFLAGS = -std=c99 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
	-Wall -Wextra -pedantic \
	-I$(top_srcdir)/include \
	-I$(top_srcdir)/src \
	-I$(top_srcdir)/src/cJSON \
	-DLIBWYOMING_GIT_HASH=\"$(GIT_HASH)\"

wyoming_server_SOURCES = wyoming-server.c $(top_srcdir)/src/wyoming_config.c
wyoming_server_LDADD = $(top_builddir)/src/libwyoming.la -lm
if HAVE_SHERPA
wyoming_server_CFLAGS = $(AM_CFLAGS) $(SHERPA_CFLAGS)
wyoming_server_LDADD += $(SHERPA_LIBS)
endif

wyoming_describe_SOURCES = wyoming-describe.c
wyoming_describe_LDADD = $(top_builddir)/src/libwyoming.la
