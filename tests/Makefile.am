check_PROGRAMS = test_libwyoming

test_libwyoming_SOURCES = \
	test_main.c \
	test_event.c \
	test_client_server.c

test_libwyoming_CFLAGS = \
	-std=c99 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
	-Wall -Wextra -pedantic \
	-I$(top_srcdir)/include \
	-I$(top_srcdir)/src/cJSON \
	-DLIBWYOMING_GIT_HASH=\"$(GIT_HASH)\"

test_libwyoming_LDADD = $(top_builddir)/src/libwyoming.la -lm -lpthread

TESTS = test_libwyoming
