#
# FILE            makefile
#
# AUTHOR          Ken Zangelin
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
#
# The k-libs are collected into the corLibs umbrella, NEXT TO this repo - the
# same layout every other lib in the stack builds against.
#
# := and not ?=/= : MAKEFILE_LIST grows as make reads more files, so evaluating
# it lazily inside a rule resolves it against whatever was included last.
#
THIS_DIR     := $(dir $(lastword $(MAKEFILE_LIST)))
KLIB_DIR     ?= $(abspath $(THIS_DIR)../corLibs/lib)

LIB_SO        = libcorHttp.so
LIB           = libcorHttp.a
CC            = gcc
INCLUDE       = -I..
DFLAGS        =
#
# EXTRA_CFLAGS - the hook for a caller that needs to ADD flags to this build.
# Not DFLAGS: `make DFLAGS=...` REPLACES it, and a `DFLAGS +=` here would be
# ignored along with it, so a caller adding one flag would drop every default.
#
CFLAGS        = -O2 -Wall -Werror -Wundef -fPIC -Wno-unused-function -fstack-protector-all $(DFLAGS) $(INCLUDE) -MMD -MP $(EXTRA_CFLAGS)

LIB_SOURCES   = corHttpConn.c     \
                corHttpParse.c    \
                corHttpResponse.c \
                corHttpServer.c

BUILD        ?= debug
OBJDIR        = obj/$(BUILD)
OBJECTS       = $(LIB_SOURCES:%.c=$(OBJDIR)/%.o)
DEPS          = $(OBJECTS:.o=.d)

PREFIX       ?= /usr/local
INC_DIR       = $(PREFIX)/include/corHttp
LIB_DIR       = $(PREFIX)/lib

all: $(LIB) $(LIB_SO)

#
# corHttpTest - a server that answers, for exercising the library alone
#
corHttpTest: corHttpTest.c $(LIB)
	$(CC) $(CFLAGS) -o $@ corHttpTest.c $(LIB) -L$(KLIB_DIR) -lkalloc -lkbase -lpthread

#
# $(OBJDIR)/.flags - rebuild when the COMPILE LINE changes
#
# A flag change is invisible to every timestamp: the sources are older than the
# objects and make sees nothing to do, so the build silently keeps objects
# compiled with the previous flags. This records them and makes the objects
# depend on the record.
#
$(OBJDIR)/.flags: FORCE
	@mkdir -p $(OBJDIR)
	@echo '$(CFLAGS)' | cmp -s - $@ || echo '$(CFLAGS)' > $@

$(OBJDIR)/%.o: %.c $(OBJDIR)/.flags
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(OBJECTS)
	ar rcs $@ $(OBJECTS)

$(LIB_SO): $(OBJECTS)
	$(CC) -shared -o $@ $(OBJECTS)

install: all
	mkdir -p $(INC_DIR) $(LIB_DIR)
	# Replace the header set rather than adding to it: `cp *.h` never removes a
	# header that was deleted here, and one has survived its own deletion before.
	rm -rf $(INC_DIR)
	mkdir -p $(INC_DIR)
	cp *.h $(INC_DIR)/
	cp $(LIB) $(LIB_SO) $(LIB_DIR)/

di: all install

clean:
	rm -rf obj $(LIB) $(LIB_SO) corHttpTest *.o *.d *.gcno *.gcda

FORCE:

.PHONY: all install di clean FORCE

-include $(DEPS)
