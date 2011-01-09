#ifndef VREC_H__
#define VREC_H__

#include <sys/queue.h>
#include <pthread.h>
#include <libavcodec/avcodec.h>

extern pthread_mutex_t rec_mutex;
extern pthread_cond_t rec_cond;
extern const char *output_basename;

TAILQ_HEAD(rec_msg_queue, rec_msg);

extern struct rec_msg_queue rec_msgs;

typedef struct rec_msg {

  TAILQ_ENTRY(rec_msg) link;

  enum {
    REC_START,
    REC_STOP,
    REC_PICTURE,
  } type;

  AVPicture picture;
  int64_t pts;
} rec_msg_t;

void init_recorder(void);

#endif // VREC_H__
