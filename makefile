#
# FILE            makefile
#
# AUTHOR          Ken Zangelin
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
#
# Every library in this stack is a SIBLING repo - `-I..` and `../<name>/lib<name>.a`
# is the layout, and it is part of the build contract rather than a convenience.
#
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

#
# The k-libs this library links against, by path rather than by -L/-l: the
# sibling checkout is the source of truth, and a -l would happily find an older
# copy installed somewhere on the system.
#
LIBS          = ../kalloc/libkalloc.a ../kbase/libkbase.a -lpthread

TEST          = corHttpTest

all: $(LIB) $(LIB_SO) $(TEST)

#
# corHttpTest - a server that answers, for exercising the library alone
#
$(TEST): corHttpTest.c $(LIB)
	$(CC) $(CFLAGS) -o $@ corHttpTest.c $(LIB) $(LIBS)

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

#
# Removed first: `ar r` replaces and adds but never removes, so an object that
# is no longer built stays in the archive forever, and the next link quietly
# uses code that is not in the tree any more.
#
$(LIB): $(OBJECTS)
	@rm -f $@
	ar rcs $@ $(OBJECTS)

$(LIB_SO): $(OBJECTS)
	$(CC) -shared -o $@ $(OBJECTS)

#
# install - NOT a copy into /usr/local, and deliberately so.
#
# Nothing in this stack installs headers or libraries system-wide: consumers
# compile with `-I..` and link `../corHttp/libcorHttp.a` straight out of the
# checkout, so `all` has already put the artefacts where every consumer looks
# for them. What is left is the sibling convention - the test binary goes into
# bin/, the same as every other lib here - and `make install` needing sudo to
# succeed would be a bug, not a policy.
#
install: all
	@if [ ! -d bin ]; then mkdir bin; fi
	cp $(TEST) bin/

di: all install

ci: clean install

clean:
	rm -rf obj $(LIB) $(LIB_SO) $(TEST) *.o *.d *.gcno *.gcda

FORCE:

.PHONY: all install di ci clean FORCE

-include $(DEPS)
