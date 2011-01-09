#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#include "vrec.h"

static AVCodecContext *v_ctx;
static AVStream *v_st;
static int video_outbuf_size;
static uint8_t *video_outbuf;
static AVFormatContext *outformat;
static int do_output;



/**
 *
 */
static void
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
    return;
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
    return;
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

  if (av_set_parameters(oc, NULL) < 0) {
    fprintf(stderr, "Invalid output format parameters\n");
    return;
  }

  dump_format(oc, 0, filename, 1);

  c = avcodec_find_encoder(avctx->codec_id);
  if(avcodec_open(avctx, c)) {
    fprintf(stderr, "Unable to open codec\n");
    return;
  }
  v_ctx = avctx;
  v_st = st;

  if(url_fopen(&oc->pb, filename, URL_WRONLY) < 0) {
    fprintf(stderr, "Could not open '%s'\n", filename);
    return;
  }

  /* write the stream header, if any */
  av_write_header(oc);

  outformat = oc;

  video_outbuf_size = 600000;
  video_outbuf = av_malloc(video_outbuf_size);

  do_output = 1;
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
write_video(rec_msg_t *rm)
{
  int r, i;
  AVPacket pkt;

  AVFrame *frame = avcodec_alloc_frame();

  for(i = 0; i < 4; i++) {
    frame->data[i] = rm->picture.data[i];
    frame->linesize[i] = rm->picture.linesize[i];
  }

  frame->pts = rm->pts;

  r = avcodec_encode_video(v_ctx, video_outbuf, video_outbuf_size, frame);

  if(r) {

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
  av_free(frame);
}

/**
 *
 */
static void *
rec_thread(void *aux)
{
  rec_msg_t *rm;

  pthread_mutex_lock(&rec_mutex);

  while(1) {

    while((rm = TAILQ_FIRST(&rec_msgs)) == NULL)
      pthread_cond_wait(&rec_cond, &rec_mutex);

    TAILQ_REMOVE(&rec_msgs, rm, link);
    pthread_mutex_unlock(&rec_mutex);

    switch(rm->type) {

    case REC_START:
      openoutput();
      break;
    case REC_STOP:
      closeoutput();
      break;
    case REC_PICTURE:
      write_video(rm);
      avpicture_free(&rm->picture);
      break;
    }
    free(rm);

    pthread_mutex_lock(&rec_mutex);
  }
  return NULL;
}

/**
 *
 */
void
init_recorder(void)
{
  pthread_t id;
  pthread_create(&id, NULL, rec_thread, NULL);
}

