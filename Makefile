.PHONY: all install clean distclean

ifeq ($(CC),)
CC       = cc
endif

HEADERS  = $(wildcard *.h)
OBJECTS  = $(patsubst %.c,%.o,$(wildcard *.c))
VERSION  = `git describe --tag | sed -e 's/-g/-/'`

CFLAGS	?= \
	-g3 -pipe -fPIC -std=c99 \
	-D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 \
	-D_POSIX_C_SOURCE=200809L -D_BSD_SOURCE=1 -D_XOPEN_SOURCE=600 \
	-ffunction-sections -fdata-sections -fstack-protector-all -Wempty-body \
	-Wall -Winit-self -Wswitch-enum -Wundef \
	-Waddress -Wmissing-field-initializers  \
	-Wredundant-decls -Wvla -Wstack-protector -ftabstop=4 -Wshadow \
	-Wpointer-arith -Wtype-limits
LDFLAGS	+= -lc

all: reader

reader: $(OBJECTS) $(HEADERS) Makefile
	$(CC) $(CFLAGS) $(OBJECTS) $(LDFLAGS) -o $@

install: $(NTC_CTL)
	install -D reader $(EXEC_DIR)

clean:
	rm -f *.o reader

distclean: clean

