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


#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

static void start_output(void);

static void stop_output(void);



static pthread_mutex_t outputmutex = PTHREAD_MUTEX_INITIALIZER;

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



static int v_pixfmt;
static int v_width;
static int v_height;
static int v_fd;

static void *buf_ptr[256];

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


static Display *v_display;
static Window v_window;
static int v_screen;
static GC v_gc;

static pthread_t xevent_thread_id;
static int v_xv_port;
static XShmSegmentInfo v_shminfo;
static XvImage *v_xvimage;


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




static void *
xevent_thread(void *aux)
{
  XEvent event;

  while(1) {
    XNextEvent(v_display, &event);

    switch(event.type) {
      
    case KeyPress:
      keypress(&event);
      break;
    }
  }
  return NULL;
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

  XInitThreads();

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
  swa.event_mask = StructureNotifyMask | KeyPressMask;

  v_window = XCreateWindow(v_display, rootwin, 
			   0, 0, 
			   720,
			   576,
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


  pthread_create(&xevent_thread_id, NULL, xevent_thread, NULL);



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

const char *output_basename;
static AVFrame out_videoframe;
static AVCodecContext *v_ctx;
static AVStream *v_st;
static int video_outbuf_size;
static uint8_t *video_outbuf;
static AVFormatContext *outformat;
struct SwsContext *v_sws;

static int do_output;

/**
 *
 */
static int
openoutput(void)
{
  AVOutputFormat *fmt;
  AVFormatContext *oc;
  AVStream *st;
  struct stat sbuf;
  AVCodecContext *avctx;
  AVCodec *c;
  const char *postfix = "dv";
  char filename[256];
  int i = 1;

  if(output_basename == NULL) {
    fprintf(stderr, "No output filename given\n");
    return -1;
  }

  while(1) {
    snprintf(filename, sizeof(filename), "%s.%d.%s", 
	     output_basename, i, postfix);
    if(stat(filename, &sbuf))
      break;
    i++;
  }

  fprintf(stderr, "Opening output to %s\n", filename);


  fmt = guess_format(postfix, NULL, NULL);

  assert(fmt != NULL);

  oc = avformat_alloc_context();
  if(!oc) {
    fprintf(stderr, "Memory error\n");
    return -1;
  }
 
  oc->oformat = fmt;
  snprintf(oc->filename, sizeof(oc->filename), "%s", filename);

  
  st = av_new_stream(oc, 0);
  avctx = st->codec;
  avctx->codec_type = CODEC_TYPE_VIDEO;

  avctx->codec_id = CODEC_ID_DVVIDEO;
//  avctx->codec_id = CODEC_ID_MPEG2VIDEO;

  avctx->bit_rate = 1000000;
  avctx->width = 720;
  avctx->height = 576;
  avctx->time_base.den = 25;
  avctx->time_base.num = 1;
  avctx->pix_fmt = PIX_FMT_YUV422P;

#if 0
  avctx->mb_decision = FF_MB_DECISION_RD;
  avctx->trellis = 2;
  avctx->me_cmp = 2;
  avctx->me_sub_cmp = 2;
  avctx->flags |= CODEC_FLAG_QP_RD;
  avctx->gop_size = 12;
  avctx->max_b_frames = 2;
#endif


  avctx->flags |= CODEC_FLAG_INTERLACED_DCT;
  avctx->flags |= CODEC_FLAG_INTERLACED_ME;

  avpicture_alloc((AVPicture *)&out_videoframe, PIX_FMT_YUV422P,
		  v_width, v_height);

  if (av_set_parameters(oc, NULL) < 0) {
    fprintf(stderr, "Invalid output format parameters\n");
    return -1;
  }

  dump_format(oc, 0, filename, 1);

  c = avcodec_find_encoder(avctx->codec_id);
  if(avcodec_open(avctx, c)) {
    fprintf(stderr, "Unable to open codec\n");
    return -1;
  }
  v_ctx = avctx;
  v_st = st;

  if(url_fopen(&oc->pb, filename, URL_WRONLY) < 0) {
    fprintf(stderr, "Could not open '%s'\n", filename);
    return -1;
  }

  /* write the stream header, if any */
  av_write_header(oc);

  outformat = oc;

  video_outbuf_size = 600000;
  video_outbuf = av_malloc(video_outbuf_size);

  v_sws = sws_getContext(v_width, v_height, PIX_FMT_YUYV422, 
			 v_width, v_height, PIX_FMT_YUV422P,
			 SWS_BICUBIC | SWS_PRINT_INFO |
			 SWS_CPU_CAPS_MMX | SWS_CPU_CAPS_MMX2,
			 NULL, NULL, NULL);

  do_output = 1;


  return 0;
}

/*
 *
 */
static int
closeoutput(void)
{
  AVStream *st;
  int i;

  do_output = 0;

  fprintf(stderr, "Closing output\n");

  sws_freeContext(v_sws);
  v_sws = NULL;


  free(video_outbuf);

  av_write_trailer(outformat);

  
  for(i = 0; i < outformat->nb_streams; i++) {
    st = outformat->streams[i];
    avcodec_close(st->codec);
    free(st->codec);
    free(st);
  }


  url_fclose(outformat->pb);
  free(outformat);
  return 0;
}



/**
 *
 */
static void
write_video(void)
{
  int r;
  AVPacket pkt;

  r = avcodec_encode_video(v_ctx, video_outbuf, video_outbuf_size, 
			   &out_videoframe);

  if(r == 0)
    return;

   av_init_packet(&pkt);

  if(v_ctx->coded_frame->pts != AV_NOPTS_VALUE)
    pkt.pts = av_rescale_q(v_ctx->coded_frame->pts, 
			   AV_TIME_BASE_Q, v_st->time_base);

  if(v_ctx->coded_frame->key_frame)
    pkt.flags |= PKT_FLAG_KEY;

  pkt.stream_index= v_st->index;
  pkt.data= video_outbuf;
  pkt.size= r;
  
  /* write the compressed frame in the media file */
  r = av_interleaved_write_frame(outformat, &pkt);

  if (r != 0) {
    fprintf(stderr, "Error while writing video frame\n");
  }
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
  uint8_t *data[4];
  int linesize[4];
  

  memset(&buf, 0, sizeof(struct v4l2_buffer));
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;

  while(1) {
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
    out_videoframe.pts = pts;

    memcpy(dst, src, v_xvimage->data_size);

    XvShmPutImage(v_display, v_xv_port, v_window, 
		  v_gc, v_xvimage, 
		  0, 0, v_width, v_height, 
		  0, 0, v_width, v_height, 
		  False);

    XFlush(v_display);
    XSync(v_display, False);

    pthread_mutex_lock(&outputmutex);

    if(v_sws) {
      sws_scale(v_sws, data, linesize, 0, 576,
		out_videoframe.data, out_videoframe.linesize);
    }

    pthread_mutex_unlock(&outputmutex);

    ioctl(fd, VIDIOC_QBUF, &buf);

    pthread_mutex_lock(&outputmutex);

    if(do_output)
      write_video();

    pthread_mutex_unlock(&outputmutex);

  }

  return 0;
}


/**
 *
 */
int
main(int argc, char **argv)
{
  int c;

  while((c = getopt(argc, argv, "f:")) != -1) {
    switch(c) {
    case 'f':
      output_basename = optarg;
      break;
    }
  }


  av_log_set_level(AV_LOG_DEBUG);
  avdevice_register_all();
  av_register_all();

  if(opendev())
    exit(1);

  if(opendisplay())
    exit(1);

  readvideoframes();

  return 0;
}


static void
start_output(void)
{
  pthread_mutex_lock(&outputmutex);
  if(!do_output) {
    openoutput();
  }
  pthread_mutex_unlock(&outputmutex);
}

static void
stop_output(void)
{
  pthread_mutex_lock(&outputmutex);
  if(do_output) {
    closeoutput();
  }
  pthread_mutex_unlock(&outputmutex);
}
