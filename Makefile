CC = $(CROSS_COMPILE)gcc

OFLAGS = -O3 -fomit-frame-pointer
DFLAGS = # -g

CFLAGS = -Wall -Werror $(DFLAGS) $(OFLAGS)
CFLAGS += $(shell pkg-config --cflags libbsd)
CFLAGS += $(shell pkg-config --cflags libcap-ng)

LFLAGS += $(shell pkg-config --libs libbsd)
LFLAGS += $(shell pkg-config --libs libcap-ng)

TARGET = mcuxeq

SRCS += $(wildcard *.c)
OBJS += $(subst .c,.o,$(SRCS))
HDRS += $(wildcard *.h)

ifneq ($(V),1)
Q = @
endif

all:		$(TARGET)

.PHONY:		all clean

$(TARGET):	$(OBJS)
		@echo LD $@
		$(Q)$(CC) -o $@ $(OBJS) $(LFLAGS)

%.o:		%.c $(HDRS)
		@echo CC $<
		$(Q)$(CC) -c $(CFLAGS) -o $@ $<

clean:
		@echo CLEAN
		$(Q)$(RM) $(TARGET) $(OBJS)
