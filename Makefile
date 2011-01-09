FFMPEG_LDFLAGS := \
	$(shell pkg-config --libs libavformat) \
	$(shell pkg-config --libs libavcodec) \
	$(shell pkg-config --libs libavutil) \
	$(shell pkg-config --libs libswscale) \

FFMPEG_CFLAGS := \
	$(shell pkg-config --cflags libavformat) \
	$(shell pkg-config --cflags libavcodec) \
	$(shell pkg-config --cflags libavutil) \
	$(shell pkg-config --cflags libswscale) \

TOPDIR=$(shell pwd)
SRCS = main.c

PROG = vrec
CFLAGS += -g -Wall -Werror -O2 -Wno-deprecated-declarations

CFLAGS += $(FFMPEG_CFLAGS)
LIBS   += $(FFMPEG_LDFLAGS) -lasound -lm -lz -lXv -lX11 -lpthread

.OBJDIR=        obj
DEPFLAG = -M

OBJS = $(patsubst %.c,%.o, $(SRCS))
DEPS= ${OBJS:%.o=%.d}

prefix ?= $(INSTALLPREFIX)
INSTDIR= $(prefix)/bin

all:	$(PROG)

install:
	mkdir -p $(INSTDIR)
	cd $(.OBJDIR) && install -s ${PROG} $(INSTDIR)

${PROG}: $(.OBJDIR) $(OBJS) Makefile
	cd $(.OBJDIR) && $(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

$(.OBJDIR):
	mkdir $(.OBJDIR)

.c.o:	Makefile
	cd $(.OBJDIR) && $(CC) -MD $(CFLAGS) -c -o $@ $(CURDIR)/$<

clean:
	rm -rf $(.OBJDIR) *~ core*

vpath %.o ${.OBJDIR}
vpath %.S ${.OBJDIR}
vpath ${PROG} ${.OBJDIR}
vpath ${PROGBIN} ${.OBJDIR}

# include dependency files if they exist
$(addprefix ${.OBJDIR}/, ${DEPS}): ;
-include $(addprefix ${.OBJDIR}/, ${DEPS})
