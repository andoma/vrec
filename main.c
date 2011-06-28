#include <assert.h>
#include <pthread.h>

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>

#include <sys/ipc.h>
#include <sys/shm.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xos.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xv.h>
#include <X11/extensions/Xvlib.h>



#define __user
#include <linux/videodev2.h>


#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

#include "vrec.h"

/**
 * Global
 */
pthread_mutex_t rec_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  rec_cond  = PTHREAD_COND_INITIALIZER;
const char *output_basename;
struct rec_msg_queue rec_msgs;


/**
 * Local
 */
static int g_wide;
static int v_pixfmt;
static int v_width;
static int v_height;
static int v_fd;
static int recording;
static void *buf_ptr[256];
static struct SwsContext *rec_sws;

static Display *v_display;
static Window v_window;
static int v_screen;
static GC v_gc;

static int v_xv_port;
static XShmSegmentInfo v_shminfo;
static XvImage *v_xvimage;

static int dpy_width;
static int dpy_height;
static int v_refill;

/**
 *
 */
int64_t
get_ts(void)
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}


static void start_output(void);

static void stop_output(void);




struct fmt_map {
    enum PixelFormat ff_fmt;
    int32_t v4l2_fmt;
};

static struct fmt_map fmt_conversion_table[] = {
    {
        .ff_fmt = PIX_FMT_YUV420P,
        .v4l2_fmt = V4L2_PIX_FMT_YUV420,
    },
    {
        .ff_fmt = PIX_FMT_YUV422P,
        .v4l2_fmt = V4L2_PIX_FMT_YUV422P,
    },
    {
        .ff_fmt = PIX_FMT_YUYV422,
        .v4l2_fmt = V4L2_PIX_FMT_YUYV,
    },
    {
        .ff_fmt = PIX_FMT_UYVY422,
        .v4l2_fmt = V4L2_PIX_FMT_UYVY,
    },
    {
        .ff_fmt = PIX_FMT_YUV411P,
        .v4l2_fmt = V4L2_PIX_FMT_YUV411P,
    },
    {
        .ff_fmt = PIX_FMT_YUV410P,
        .v4l2_fmt = V4L2_PIX_FMT_YUV410,
    },
    {
        .ff_fmt = PIX_FMT_RGB555,
        .v4l2_fmt = V4L2_PIX_FMT_RGB555,
    },
    {
        .ff_fmt = PIX_FMT_RGB565,
        .v4l2_fmt = V4L2_PIX_FMT_RGB565,
    },
    {
        .ff_fmt = PIX_FMT_BGR24,
        .v4l2_fmt = V4L2_PIX_FMT_BGR24,
    },
    {
        .ff_fmt = PIX_FMT_RGB24,
        .v4l2_fmt = V4L2_PIX_FMT_RGB24,
    },
    {
        .ff_fmt = PIX_FMT_BGRA,
        .v4l2_fmt = V4L2_PIX_FMT_BGR32,
    },
    {
        .ff_fmt = PIX_FMT_GRAY8,
        .v4l2_fmt = V4L2_PIX_FMT_GREY,
    },
};



#if 0
static uint32_t fmt_ff2v4l(enum PixelFormat pix_fmt)
{
    int i;

    for (i = 0; i < FF_ARRAY_ELEMS(fmt_conversion_table); i++) {
        if (fmt_conversion_table[i].ff_fmt == pix_fmt) {
            return fmt_conversion_table[i].v4l2_fmt;
        }
    }

    return 0;
}
#endif

static enum PixelFormat fmt_v4l2ff(uint32_t pix_fmt)
{
    int i;

    for (i = 0; i < FF_ARRAY_ELEMS(fmt_conversion_table); i++) {
        if (fmt_conversion_table[i].v4l2_fmt == pix_fmt) {
            return fmt_conversion_table[i].ff_fmt;
        }
    }

    return PIX_FMT_NONE;
}




/**
 *
 */
static int
opendev(void)
{
  int r, fd, i;
  struct v4l2_capability caps;
  struct v4l2_fmtdesc fmtdesc;
  struct v4l2_format fmt;
  struct v4l2_requestbuffers rb;
  enum PixelFormat pixfmt;
  enum v4l2_buf_type type;
  struct v4l2_input input;
  struct v4l2_standard standard;
  int hs, vs;

  fd = open("/dev/video0", O_RDWR);
  if(fd == -1)
    return -1;

  r = ioctl(fd, VIDIOC_QUERYCAP, &caps);
  if(r < 0)
    return -1;
  
  v_fd = fd;

  i = 0;
  memset(&fmtdesc, 0, sizeof(fmtdesc));
  while(1) {
    fmtdesc.index = i;
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    r = ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc);
    if(r < 0)
      break;

    v_pixfmt = fmtdesc.pixelformat;
    break;
    i++;
  }

  if(v_pixfmt == 0)
    return -1;

  pixfmt = fmt_v4l2ff(v_pixfmt);

  avcodec_get_chroma_sub_sample(pixfmt, &hs, &vs);
  
  memset(&fmt, 0, sizeof(struct v4l2_format));
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = 720;
  fmt.fmt.pix.height = 576;
  fmt.fmt.pix.pixelformat = v_pixfmt;
  fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;
  r = ioctl(fd, VIDIOC_S_FMT, &fmt);

  if(r < 0)
    return -1;

  v_width  = fmt.fmt.pix.width;
  v_height = fmt.fmt.pix.height;


  memset(&rb, 0, sizeof(struct v4l2_requestbuffers));
  rb.count = 32;
  rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  rb.memory = V4L2_MEMORY_MMAP;
  r = ioctl(fd, VIDIOC_REQBUFS, &rb);
  if(r < 0) {
    if (errno == EINVAL) {
      fprintf(stderr, "Device does not support mmap\n");
    } else {
      fprintf(stderr, "ioctl(VIDIOC_REQBUFS)\n");
    }
    return -1;
  }

  for (i = 0; i < rb.count; i++) {
    struct v4l2_buffer buf;

    memset(&buf, 0, sizeof(struct v4l2_buffer));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;
    r = ioctl(fd, VIDIOC_QUERYBUF, &buf);
    if (r < 0) {
      fprintf(stderr, "ioctl(VIDIOC_QUERYBUF)\n");
      return -1;
    }

    buf_ptr[i] = mmap(NULL, buf.length,
		      PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
    if(buf_ptr[i] == MAP_FAILED) {

      fprintf(stderr, "mmap: %s\n", strerror(errno));
      return -1;
    }
  }


  int channel = 1;

  /* set tv video input */
  memset (&input, 0, sizeof (input));
  input.index = channel;
  if(ioctl(fd, VIDIOC_ENUMINPUT, &input) < 0) {
    fprintf(stderr, "The V4L2 driver ioctl enum input failed:\n");
    return -1;
  }
  
  fprintf(stderr, "The V4L2 driver set input_id: %d, input: %s\n",
	  channel, input.name);
  fprintf(stderr, "  Associated audio: %08x\n", input.audioset);

  if(ioctl(fd, VIDIOC_S_INPUT, &input.index) < 0 ) {
    fprintf(stderr, "The V4L2 driver ioctl set input(%d) failed\n", channel);
    return -1;
  }


  for(i=0;;i++) {
    standard.index = i;
    if(ioctl(fd, VIDIOC_ENUMSTD, &standard) < 0) {
      break;
    }

    if(!strcmp("PAL", (const char *)standard.name)) {
      if(ioctl(fd, VIDIOC_S_STD, &standard.id) < 0) {
	fprintf(stderr, "Unable to set standard\n");
	return -1;
      }
      break;
    }
  }

  
  for (i = 0; i < rb.count; i++) {
    struct v4l2_buffer buf;

    memset(&buf, 0, sizeof(struct v4l2_buffer));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index  = i;

    r = ioctl(fd, VIDIOC_QBUF, &buf);
    if (r < 0) {
      fprintf(stderr, "ioctl(VIDIOC_QBUF): %s\n", strerror(errno));
      return -1;
    }
  }

  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  r = ioctl(fd, VIDIOC_STREAMON, &type);
  if (r < 0) {
    fprintf(stderr, "ioctl(VIDIOC_STREAMON): %s\n", strerror(errno));
    return -1;
  }

  return 0;
}





static void
keypress(XEvent *event)

{
  char str[16];
  KeySym keysym;
  XComposeStatus composestatus;
  int len;

  len = XLookupString(&event->xkey, str, sizeof(str), 
		      &keysym, &composestatus); 

  switch(keysym) {
  case XK_F1:
    start_output();
    break;
  case XK_F2:
    stop_output();
    break;
  }
}

static void
do_x_stuff(void)
{
  XEvent event;
  int w, h;
  while(XCheckWindowEvent(v_display, v_window, 0xffffffff, &event)) {
    switch(event.type) {
      
    case KeyPress:
      keypress(&event);
      break;
    case Expose:
      v_refill = 1;
      break;
    case VisibilityNotify:
      v_refill = 1;
      break;

    case ConfigureNotify:
      w = event.xconfigure.width;
      h = event.xconfigure.height;
      if(dpy_width != w || dpy_height != h) {
	dpy_width = w;
	dpy_height = h;
	v_refill = 1;
      }
      break;
    }
  }
}


/**
 *
 */
static Bool 
WaitForNotify(Display *d, XEvent *e, char *arg) 
{
   return (e->type == MapNotify) && (e->xmap.window == (Window)arg);
}

/**
 *
 */
static int
opendisplay(void)
{
  Window rootwin;
  XVisualInfo vinfo;
  XWindowAttributes attribs;
  Colormap cmap;
  XSetWindowAttributes swa;
  XvAdaptorInfo *ai = NULL;
  unsigned int adaptors;
  XGCValues xgcv;
  XEvent event;

  dpy_width = 720;
  dpy_height = 576;

  v_display = XOpenDisplay(NULL);

  if(!XShmQueryExtension(v_display)) {
    fprintf(stderr, "No SHM Extension enabled\n");
    return -1;
  }


  v_screen = DefaultScreen(v_display);
  rootwin = RootWindow(v_display, v_screen);

  XGetWindowAttributes(v_display, DefaultRootWindow(v_display), &attribs);
  XMatchVisualInfo(v_display, v_screen, attribs.depth, TrueColor, &vinfo);

  cmap = XCreateColormap(v_display, rootwin, vinfo.visual, AllocNone);

  memset(&swa, 0, sizeof(swa));
  
  swa.colormap = cmap;
  swa.border_pixel = 0;
  swa.event_mask = StructureNotifyMask | KeyPressMask | VisibilityChangeMask | ExposureMask;

  v_window = XCreateWindow(v_display, rootwin, 
			   0, 0, 
			   dpy_width,
			   dpy_height,
			   0,
			   vinfo.depth, 
			   InputOutput,
			   CopyFromParent,
			   CWBorderPixel |
			   CWColormap | 
			   CWEventMask, 
			   &swa);

  XMapWindow(v_display, v_window);

  XIfEvent(v_display, &event, WaitForNotify, (char *)v_window);
  
  v_gc = XCreateGC(v_display, v_window, 0, &xgcv);

  if(XvQueryAdaptors(v_display, DefaultRootWindow(v_display), 
		     &adaptors, &ai) != Success) {
    fprintf(stderr, "Unable to query for Xvideo adaptors\n");
    return -1;
  }

  if(adaptors == 0) {
    fprintf(stderr, "No Xvideo adaptors\n");
    return -1;
  }

  v_xv_port = ai[0].base_id;

  if(XvGrabPort(v_display, v_xv_port, CurrentTime) != Success) {
    fprintf(stderr, "Unable to grab port\n");
    return -1;
  }
#if 0
  XvSetPortAttribute(v_display, v_xv_port, 
		     XInternAtom(v_display, 
				 "XV_DOUBLE_BUFFER", True), 1);
#endif

  uint32_t xv_format = 0x32595559; // YV12

  v_xvimage = XvShmCreateImage(v_display, v_xv_port,
			       xv_format, NULL, 
			       v_width, v_height, 
			       &v_shminfo);

  v_shminfo.shmid = shmget(IPC_PRIVATE, v_xvimage->data_size, 
			   IPC_CREAT | 0777);
  v_shminfo.shmaddr = (char *)shmat(v_shminfo.shmid, 0, 0);
  v_shminfo.readOnly = False;

  v_xvimage->data = v_shminfo.shmaddr;

  XShmAttach(v_display, &v_shminfo);

  XSync(v_display, False);
  shmctl(v_shminfo.shmid, IPC_RMID, 0);
    
  memset(v_xvimage->data, 128, v_xvimage->data_size);
  return 0;
}


/**
 *
 */
static int
readvideoframes(void)
{
  struct v4l2_buffer buf;
  int r;
  int fd = v_fd;
  const uint8_t *src;
  uint8_t *dst;
  int64_t pts, pts_start = AV_NOPTS_VALUE;
  const uint8_t *data[4];
  int linesize[4];
  

  memset(&buf, 0, sizeof(struct v4l2_buffer));
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;

  while(1) {
    do_x_stuff();

    r = ioctl(fd, VIDIOC_DQBUF, &buf);
    if(r < 0)
      break;

    dst = (uint8_t *)v_xvimage->data;
    src = buf_ptr[buf.index];

    data[0] = (void *)src;
    linesize[0] = 720 * 2;

    pts = buf.timestamp.tv_sec * INT64_C(1000000) + buf.timestamp.tv_usec;

    if(pts_start == AV_NOPTS_VALUE) {
      pts_start = pts;
      pts = 0;
    } else {
      pts -= pts_start;
    }

    float a = (float)dpy_width / (dpy_height * (float)v_width / v_height);

    if(g_wide)
	a = a / 1.3333;

    int frame_width, frame_height;

    if(a > 1) {
      frame_width  = dpy_width / a;
      frame_height = dpy_height;
    } else {
      frame_width = dpy_width;
      frame_height = dpy_height * a;
    }

    memcpy(dst, src, v_xvimage->data_size);

    if(v_refill) {
      XFillRectangle(v_display, v_window, v_gc, 0, 0, dpy_width, dpy_height);
      v_refill = 0;
    }

    XvShmPutImage(v_display, v_xv_port, v_window, 
		  v_gc, v_xvimage, 
		  0, 0, v_width, v_height, 
		  (dpy_width - frame_width) / 2,
		  (dpy_height - frame_height) / 2,
		  frame_width, frame_height,
		  False);

    XFlush(v_display);
    XSync(v_display, False);

    if(recording) {
      rec_msg_t *rm = calloc(1, sizeof(rec_msg_t));
      rm->type = REC_PICTURE;
      avpicture_alloc(&rm->picture, PIX_FMT_YUV422P, v_width, v_height);

      sws_scale(rec_sws, data, linesize, 0, 576,
		rm->picture.data, rm->picture.linesize);

      pthread_mutex_lock(&rec_mutex);
      TAILQ_INSERT_TAIL(&rec_msgs, rm, link);
      pthread_cond_signal(&rec_cond);
      pthread_mutex_unlock(&rec_mutex);
    }

    ioctl(fd, VIDIOC_QBUF, &buf);
  }
  return 0;
}



/**
 *
 */
static void
send_rec_msg(int m)
{
  rec_msg_t *rm = calloc(1, sizeof(rec_msg_t));
  rm->type = m;

  TAILQ_INSERT_TAIL(&rec_msgs, rm, link);
  pthread_cond_signal(&rec_cond);
}


static void
start_output(void)
{
  pthread_mutex_lock(&rec_mutex);

  rec_sws = sws_getContext(v_width, v_height, PIX_FMT_YUYV422, 
			   v_width, v_height, PIX_FMT_YUV422P,
			   SWS_BICUBIC | SWS_PRINT_INFO |
			   SWS_CPU_CAPS_MMX | SWS_CPU_CAPS_MMX2,
			   NULL, NULL, NULL);


  send_rec_msg(REC_START);
  recording = 1;
  pthread_mutex_unlock(&rec_mutex);
}

static void
stop_output(void)
{
  pthread_mutex_lock(&rec_mutex);

  send_rec_msg(REC_STOP);
  recording = 0;
  sws_freeContext(rec_sws);
  rec_sws = NULL;
  pthread_mutex_unlock(&rec_mutex);

}

/**
 *
 */
int
main(int argc, char **argv)
{
  int c;

  TAILQ_INIT(&rec_msgs);

  while((c = getopt(argc, argv, "f:w")) != -1) {
    switch(c) {
    case 'f':
      output_basename = optarg;
      break;
    case 'w':
      g_wide = 1;
      break;
    }
  }


  av_log_set_level(AV_LOG_DEBUG);
  av_register_all();

  init_recorder();
  
  if(opendev())
    exit(1);

  if(opendisplay())
    exit(1);

  readvideoframes();

  return 0;
}


