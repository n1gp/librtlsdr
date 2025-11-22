CC = gcc

CFLAGS = -Wall -O3 #-ffast-math -funroll-loops -ftree-vectorize -march=armv8-a+fp+simd #-g

LIBS = -lrt -lm -lpthread -lrtlsdr -lliquid

INCLUDES =

TARGET = rtl_hpsdr

DEPENDDIR = .
DEPENDFLAGS = -M

SRCS = rtl_hpsdr.c

OBJS = $(SRCS:.c=.o)

all: $(TARGET)

DEPS = $(patsubst %.o,$(DEPENDDIR)/%.d,$(OBJS))
-include $(DEPS)

$(DEPENDDIR)/%.d: %.c $(DEPENDDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPENDFLAGS) $< >$@

$(DEPENDDIR):
	@[ ! -d $(DEPENDDIR) ] && mkdir -p $(DEPENDDIR)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) $(INCLUDES) $(LIBS) -o $(TARGET)

clean:
	rm -f $(DEPS) $(OBJS) $(TARGET)

.PHONY: all clean
