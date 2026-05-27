CC      ?= musl-gcc
CFLAGS  = -O2 -static -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
           -Wno-sign-compare -Wno-missing-field-initializers
LDFLAGS = -static

TARGET  = oinit
SRCS    = main.c init.c shell.c applets.c util.c
OBJS    = $(SRCS:.c=.o)

# fall back to gcc if musl-gcc not available
ifeq ($(shell which musl-gcc 2>/dev/null),)
  CC = gcc
endif

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c oinit.h
	$(CC) $(CFLAGS) -c -o $@ $<

# create install symlinks in a directory (make links ROOT=/path)
ROOT ?= .
links: $(TARGET)
	cp $(TARGET) $(ROOT)/oinit
	for applet in sh ash bash cat ls cp mv rm mkdir rmdir ln chmod chown touch stat \
	              echo printf test true false grep sed cut tr wc head tail sort uniq tee \
	              find xargs mount umount sync ps kill env uname hostname pwd date sleep \
	              seq yes id whoami basename dirname readlink realpath which dd hexdump \
	              reboot poweroff shutdown dmesg free df du mkfifo mknod tty stty nohup nice; do \
		ln -sf oinit $(ROOT)/$$applet; \
	done
	ln -sf oinit $(ROOT)/init

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean links
