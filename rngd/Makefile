#!/usr/bin/make -f
# Makefile for libquantisusb
# Requirements:
# - GNU Make (Tested with 3.81)
# - pkg-config (Tested with 0.26)

prefix:=/usr/local
bindir:=$(prefix)/bin

# Build type
# coverage
BUILD_TYPE:=normal

PKG_CONFIG:=pkg-config
CC:=gcc
LD:=$(CC)
RM:=rm -f
PKG_CONFIG_LIBS:=libusb-1.0

INCS:=$(shell $(PKG_CONFIG) --cflags $(PKG_CONFIG_LIBS))
CFLAGS:=-O2 -g -std=c99 -pedantic -Wall -Wconversion -Wformat-security -Werror -fstrict-aliasing -fPIE -fstack-protector-all -fvisibility=hidden $(INCS)
LDFLAGS:=-z relro -z now -pie
LIBS:=$(shell $(PKG_CONFIG) --libs $(PKG_CONFIG_LIBS)) -lm

ifeq ($(BUILD_TYPE),coverage)
  CFLAGS += -fprofile-arcs -ftest-coverage
  LIBS += -lgcov
endif

LIB_HEADERS:=quantisusb.h version.h
LIB_SRCS:= quantisusb.c
LIB_OBJS:= $(LIB_SRCS:.c=.o)

DAEMON_SRCS:= databuf.c quantisusb-rngd.c
DAEMON_HEADERS:= databuf.h
DAEMON_OBJS:= $(DAEMON_SRCS:.c=.o)

READER_SRCS:=quantisusb-reader.c
READER_OBJS:=$(READER_SRCS:.c=.o)

ANALYSIS_OBJS:=$(LIB_SRCS:.c=.plist) $(DAEMON_SRCS:.c=.plist) $(READER_SRCS:.c=.plist)


all: quantisusb-reader quantisusb-rngd

analyze: $(ANALYSIS_OBJS)

quantisusb-rngd: $(LIB_OBJS) $(DAEMON_OBJS)
	$(LD) $(LDFLAGS) -o $@ $^ $(LIBS)

quantisusb-reader: $(LIB_OBJS) $(READER_OBJS)
	$(LD) $(LDFLAGS) -o $@ $^ $(LIBS)

clean:
	$(RM) $(ANALYSIS_OBJS)
	$(RM) *.gcov *.gcda *.gcno
	$(RM) $(LIB_OBJS)
	$(RM) $(DAEMON_OBJS)
	$(RM) $(READER_OBJS)
	$(RM) quantisusb-rngd
	$(RM) quantisusb-reader

install: quantisusb-rngd quantisusb-reader
	mkdir -p $(DESTDIR)$(bindir)
	install -s -p -t $(DESTDIR)$(bindir) quantisusb-rngd quantisusb-reader

%.o: %.c $(LIB_HEADERS)
	$(CC) $(CFLAGS) -c $<

%.plist: %.c
	clang --analyze $(INCS) $<
