/*
 * Copyright (C) 2015
 * Gilles Mazoyer <mazoyer.gilles@omega.ovh>
 *
 * This is free software, licensed under the GNU General Public License v2.
 * See /LICENSE for more information.
 */

/* Documentation about API of Asterisk 11 is available at
   http://doxygen.asterisk.org/trunk/ */

/* Documentation about API of ALSA PCM is available at
   http://www.alsa-project.org/alsa-doc/alsa-lib/pcm.html */

#if ((!defined AST_VERSION) || ((18 != AST_VERSION) && (110 != AST_VERSION) && (130 != AST_VERSION)))
#error "Preprocessor define AST_VERSION not defined or not equal to 18, 110 or 130"
#endif

/* To be included first */
#include <asterisk.h>

ASTERISK_FILE_VERSION(__FILE__, "$Revision: 1 $")

#include <linux/input.h>
#include <linux/types.h>
#include <fcntl.h>
#include <math.h>
#include <sys/ioctl.h>
#include <sys/time.h>

#define ALSA_PCM_NEW_HW_PARAMS_API
#define ALSA_PCM_NEW_SW_PARAMS_API
#include <alsa/asoundlib.h>

#include <asterisk/abstract_jb.h>
#include <asterisk/ast_version.h>
#include <asterisk/channel.h>
#include <asterisk/callerid.h>
#include <asterisk/causes.h>
#include <asterisk/cli.h>
#include <asterisk/config.h>
#include <asterisk/dsp.h>
#include <asterisk/endian.h>
#if (AST_VERSION > 110)
#include <asterisk/format_cache.h>
#endif /* (AST_VERSION > 110) */
#if (AST_VERSION >= 110)
#include <asterisk/format_cap.h>
#endif /* (AST_VERSION >= 110) */
#include <asterisk/frame.h>
#include <asterisk/linkedlists.h>
#include <asterisk/lock.h>
#include <asterisk/logger.h>
#include <asterisk/module.h>
#include <asterisk/musiconhold.h>
#include <asterisk/pbx.h>
#include <asterisk/strings.h>
#include <asterisk/utils.h>

/*
 D'apres ce que je comprends le deroulement est le suivant
 * Si l'appel est initie par l'utilisateur :
 celui ci decroche le telephone, ce qui est detecte par le moniteur,
 ce dernier attend ou pas qu'une extension soit tape, appelle alsa_input_new()
 ce qui cree un channel dans l'etat AST_STATE_RING, et appelle ast_pbx_start()
 pour la creation d'un thread pour le channel precedemment cree.
 Asterisk appelle ensuite alsa_input_chan_answer() ce qui fait passer le channel
 dans l'etat AST_STATE_UP et passe la ligne en mode conversation.
 En fin de conversation hangup() est appelle : le channel est detache
 de la ligne.
 * Si l'appel est recu de l'exterieur :
 Asterisk appelle alsa_input_chan_request() qui appelle alsa_input_new() ce qui
 cree un channel dans l'etat AST_STATE_DOWN. Pas de thread demarre, c'est
 Asterisk qui s'en chargera plus tard.
 Asterisk appelle ensuite alsa_input_chan_call() pour faire sonner le telephone.
 Lorsque l'utilisateur decroche la ligne passe en mode conversation.
 En fin de conversation hangup() est appelle : le channel est detache de
 la ligne.
*/

#define DEBUG

#undef alsa_input_assert
#undef alsa_input_pr_debug
#ifdef DEBUG
#define alsa_input_assert(cond) if (!(cond)) { ast_log(AST_LOG_DEBUG, "condition '%s' is false\n", #cond); }
#define alsa_input_pr_debug(fmt, args...) ast_log(AST_LOG_DEBUG, fmt, ## args)
#else /* !DEBUG */
#define alsa_input_assert(cond)
#define alsa_input_pr_debug(fmt, args...)
#endif /* !DEBUG */

#if !defined(__cplusplus) && !defined(c_plusplus)
typedef int bool;

#define false 0
#define true 1
#endif /* __cplusplus */

#if (AST_VERSION < 110)
typedef format_t alsa_input_ast_format;
#else /* (AST_VERSION >= 110) */
typedef struct ast_format alsa_input_ast_format;
#endif /* (AST_VERSION >= 110) */

#if (AST_VERSION < 110)
typedef format_t alsa_input_ast_format_cap;
#else /* (AST_VERSION >= 110) */
typedef struct ast_format_cap alsa_input_ast_format_cap;
#endif /* (AST_VERSION >= 110) */

#if (AST_VERSION <= 110)
/*
 * If a new format is added, functions alsa_input_init_cache_ast_format and
 * alsa_input_ast_format_cap_get_best_by_type should be updated
 */
static alsa_input_ast_format *ast_format_slin;
#endif /* (AST_VERSION <= 110) */

static void alsa_input_init_cache_ast_format(void)
{
#if (AST_VERSION < 110)
   static format_t format_slin = AST_FORMAT_SLINEAR;
   ast_format_slin = &(format_slin);
#endif /* (AST_VERSION < 110) */
#if (110 == AST_VERSION)
   static struct ast_format format_slin;
   ast_format_slin = ast_getformatbyname("slin", &(format_slin));
   alsa_input_assert(NULL != ast_format_slin);
#endif /* (110 == AST_VERSION) */
}

static inline const char *alsa_input_ast_format_get_name(
   const alsa_input_ast_format *format)
{
   alsa_input_assert(NULL != format);
#if (AST_VERSION < 110)
   return (ast_getformatname(*format));
#endif /* (110 == AST_VERSION) */
#if (110 == AST_VERSION)
   return (ast_getformatname(format));
#endif /* (110 == AST_VERSION) */
#if (AST_VERSION > 110)
   return (ast_format_get_name(format));
#endif /* (AST_VERSION > 110) */
}

static inline int alsa_input_ast_formats_are_equal(
   const alsa_input_ast_format *format1, const alsa_input_ast_format *format2)
{
   alsa_input_assert((NULL != format1) && (NULL != format2));
#if (AST_VERSION < 110)
   return (*format1 == *format2);
#else /* (AST_VERSION >= 110) */
   return (AST_FORMAT_CMP_EQUAL == ast_format_cmp(format1, format2));
#endif /* (AST_VERSION > =110) */
}

static inline alsa_input_ast_format_cap *alsa_input_ast_format_cap_alloc(void)
{
#if (AST_VERSION < 110)
   alsa_input_assert(false);
   return (NULL);
#endif /* (AST_VERSION < 110) */
#if (110 == AST_VERSION)
   return (ast_format_cap_alloc());
#endif /* (110 == AST_VERSION) */
#if (AST_VERSION > 110)
   return (ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_DEFAULT));
#endif /* (AST_VERSION > 110) */
}

static inline void alsa_input_ast_format_cap_destroy(alsa_input_ast_format_cap *cap)
{
   alsa_input_assert(NULL != cap);
#if (AST_VERSION < 110)
   alsa_input_assert(false);
#endif /* (AST_VERSION < 110) */
#if (110 == AST_VERSION)
   ast_format_cap_destroy(cap);
#endif /* (110 == AST_VERSION) */
#if (AST_VERSION > 110)
   ao2_ref(cap, -1);
#endif /* (AST_VERSION > 110) */
}

#if (AST_VERSION <= 110)
enum ast_media_type {
   AST_MEDIA_TYPE_UNKNOWN,
   AST_MEDIA_TYPE_AUDIO,
};
#endif /* (AST_VERSION <= 110) */

static inline void alsa_input_ast_format_cap_remove_by_type(
   alsa_input_ast_format_cap *cap, enum ast_media_type type)
{
   alsa_input_assert((NULL != cap) &&
      ((AST_MEDIA_TYPE_AUDIO == type) || (AST_MEDIA_TYPE_UNKNOWN == type)));
   if (AST_MEDIA_TYPE_AUDIO == type) {
#if (AST_VERSION < 110)
      *cap &= (~(AST_FORMAT_AUDIO_MASK));
#endif /* (110 == AST_VERSION) */
#if (110 == AST_VERSION)
      ast_format_cap_remove_bytype(cap, AST_FORMAT_TYPE_AUDIO);
#endif /* (110 == AST_VERSION) */
#if (AST_VERSION > 110)
      ast_format_cap_remove_by_type(cap, type);
#endif /* (AST_VERSION > 110) */
   }
   else if (AST_MEDIA_TYPE_UNKNOWN == type) {
#if (AST_VERSION < 110)
      *cap = 0;
#endif /* (110 == AST_VERSION) */
#if (110 == AST_VERSION)
      ast_format_cap_remove_all(cap);
#endif /* (110 == AST_VERSION) */
#if (AST_VERSION > 110)
      ast_format_cap_remove_by_type(cap, type);
#endif /* (AST_VERSION > 110) */
   }
   else {
      alsa_input_assert(false);
   }
}

static inline void alsa_input_ast_format_cap_append_format(
   alsa_input_ast_format_cap *cap, alsa_input_ast_format *format)
{
   alsa_input_assert((NULL != cap) && (NULL != format));
#if (AST_VERSION < 110)
   *cap |= *format;
#endif /* (AST_VERSION < 110) */
#if (110 == AST_VERSION)
   ast_format_cap_add(cap, format);
#endif /* (110 == AST_VERSION) */
#if (AST_VERSION > 110)
   ast_format_cap_append(cap, format, 0);
#endif /* (AST_VERSION > 110) */
}

static inline int alsa_input_ast_format_cap_iscompatible_cap(
   const alsa_input_ast_format_cap *cap1, const alsa_input_ast_format_cap *cap2)
{
   alsa_input_assert((NULL != cap1) && (NULL != cap2));
#if (AST_VERSION < 110)
   return (*cap1 & *cap2);
#endif /* (AST_VERSION < 110) */
#if (110 == AST_VERSION)
   return (ast_format_cap_has_joint(cap1, cap2));
#endif /* (110 == AST_VERSION) */
#if (AST_VERSION > 110)
   return (ast_format_cap_iscompatible(cap1, cap2));
#endif /* (AST_VERSION > 110) */
}

static const char *alsa_input_ast_channel_name(const struct ast_channel *chan)
{
   alsa_input_assert(NULL != chan);
#if (AST_VERSION < 110)
   return (chan->name);
#else /* (AST_VERSION >= 110) */
   return (ast_channel_name(chan));
#endif /* (AST_VERSION >= 110) */
}

static inline enum ast_channel_state alsa_input_ast_channel_state(const struct ast_channel *chan)
{
   alsa_input_assert(NULL != chan);
#if (AST_VERSION < 110)
   return (chan->_state);
#else /* (AST_VERSION >= 110) */
   return (ast_channel_state(chan));
#endif /* (AST_VERSION >= 110) */
}

static inline const char *alsa_input_ast_channel_linkedid(const struct ast_channel *chan)
{
   alsa_input_assert(NULL != chan);
#if (AST_VERSION < 110)
   return (chan->linkedid);
#else /* (AST_VERSION >= 110) */
   return (ast_channel_linkedid(chan));
#endif /* (AST_VERSION >= 110) */
}

static inline struct ast_party_caller *alsa_input_ast_channel_caller(struct ast_channel *chan)
{
   alsa_input_assert(NULL != chan);
#if (AST_VERSION < 110)
   return (&(chan->caller));
#else /* (AST_VERSION >= 110) */
   return (ast_channel_caller(chan));
#endif /* (AST_VERSION >= 110) */
}

static inline struct ast_party_connected_line *alsa_input_ast_channel_connected(struct ast_channel *chan)
{
   alsa_input_assert(NULL != chan);
#if (AST_VERSION < 110)
   return (&(chan->connected));
#else /* (AST_VERSION >= 110) */
   return (ast_channel_connected(chan));
#endif /* (AST_VERSION >= 110) */
}

static inline void alsa_input_ast_channel_tech_set(struct ast_channel *chan,
   const struct ast_channel_tech *chan_tech)
{
#if (AST_VERSION < 110)
   chan->tech = chan_tech;
#else /* (AST_VERSION >= 110) */
   ast_channel_tech_set(chan, chan_tech);
#endif /* (AST_VERSION >= 110) */
}

static inline void *alsa_input_ast_channel_tech_pvt(struct ast_channel *chan)
{
   alsa_input_assert(NULL != chan);
#if (AST_VERSION < 110)
   return (chan->tech_pvt);
#else /* (AST_VERSION >= 110) */
   return (ast_channel_tech_pvt(chan));
#endif /* (AST_VERSION >= 110) */
}

static inline void alsa_input_ast_channel_tech_pvt_set(
   struct ast_channel *chan, void *value)
{
   alsa_input_assert(NULL != chan);
#if (AST_VERSION < 110)
   chan->tech_pvt = value;
#else /* (AST_VERSION >= 110) */
   ast_channel_tech_pvt_set(chan, value);
#endif /* (AST_VERSION >= 110) */
}

/* Beware : value must not be allocated on the stack */
static inline void alsa_input_ast_channel_nativeformats_set(
   struct ast_channel *chan, alsa_input_ast_format_cap *value)
{
   alsa_input_assert((NULL != chan) && (NULL != value));
#if (AST_VERSION < 110)
   chan->nativeformats = *value;
#endif /* (AST_VERSION < 110) */
#if (110 == AST_VERSION)
   struct ast_format_cap *tmpcap = ast_channel_nativeformats(chan);
   if (NULL == tmpcap) {
      tmpcap = ast_format_cap_alloc();
   }
   if (NULL != tmpcap) {
      ast_format_cap_remove_all(tmpcap);
      ast_format_cap_copy(tmpcap, value);
   }
   else {
      alsa_input_assert(false);
   }
#endif /* (110 == AST_VERSION) */
#if (AST_VERSION > 110)
   ast_channel_nativeformats_set(chan, value);
#endif /* (AST_VERSION > 110) */
}

static alsa_input_ast_format *alsa_input_ast_channel_rawreadformat(
   struct ast_channel *chan)
{
   alsa_input_assert(NULL != chan);
#if (AST_VERSION < 110)
   return (&(chan->rawreadformat));
#else /* (AST_VERSION >= 110) */
   return (ast_channel_rawreadformat(chan));
#endif /* (AST_VERSION >= 110) */
}

static inline void alsa_input_ast_channel_set_rawreadformat(
   struct ast_channel *chan, alsa_input_ast_format *format)
{
   alsa_input_assert((NULL != chan) && (NULL != format));
#if (AST_VERSION < 110)
   chan->rawreadformat = *format;
#endif /* (AST_VERSION < 110) */
#if (110 == AST_VERSION)
   ast_format_copy(ast_channel_rawreadformat(chan), format);
#endif /* (110 == AST_VERSION) */
#if (AST_VERSION > 110)
   ast_channel_set_rawreadformat(chan, format);
#endif /* (AST_VERSION > 110) */
}

static inline void alsa_input_ast_channel_set_rawwriteformat(
   struct ast_channel *chan, alsa_input_ast_format *format)
{
   alsa_input_assert((NULL != chan) && (NULL != format));
#if (AST_VERSION < 110)
   chan->rawwriteformat = *format;
#endif /* (AST_VERSION < 110) */
#if (110 == AST_VERSION)
   ast_format_copy(ast_channel_rawwriteformat(chan), format);
#endif /* (110 == AST_VERSION) */
#if (AST_VERSION > 110)
   ast_channel_set_rawwriteformat(chan, format);
#endif /* (AST_VERSION > 110) */
}

static inline void alsa_input_ast_channel_set_readformat(
   struct ast_channel *chan, alsa_input_ast_format *format)
{
   alsa_input_assert((NULL != chan) && (NULL != format));
#if (AST_VERSION < 110)
   chan->readformat = *format;
#endif /* (AST_VERSION < 110) */
#if (110 == AST_VERSION)
   ast_format_copy(ast_channel_readformat(chan), format);
#endif /* (110 == AST_VERSION) */
#if (AST_VERSION > 110)
   ast_channel_set_readformat(chan, format);
#endif /* (AST_VERSION > 110) */
}

static inline void alsa_input_ast_channel_set_writeformat(
   struct ast_channel *chan, alsa_input_ast_format *format)
{
   alsa_input_assert((NULL != chan) && (NULL != format));
#if (AST_VERSION < 110)
   chan->writeformat = *format;
#endif /* (AST_VERSION < 110) */
#if (110 == AST_VERSION)
   ast_format_copy(ast_channel_writeformat(chan), format);
#endif /* (110 == AST_VERSION) */
#if (AST_VERSION > 110)
   ast_channel_set_writeformat(chan, format);
#endif /* (AST_VERSION > 110) */
}

static inline void alsa_input_ast_channel_context_set(
   struct ast_channel *chan, const char *value)
{
   alsa_input_assert((NULL != chan) && (NULL != value));
#if (AST_VERSION < 110)
   ast_copy_string(chan->context, value, sizeof(chan->context));
#else /* (AST_VERSION >= 110) */
   ast_channel_context_set(chan, value);
#endif /* (AST_VERSION >= 110) */
}

static inline void alsa_input_ast_channel_exten_set(
   struct ast_channel *chan, const char *value)
{
   alsa_input_assert((NULL != chan) && (NULL != value));
#if (AST_VERSION < 110)
   ast_copy_string(chan->exten, value, sizeof(chan->exten));
#else /* (AST_VERSION >= 110) */
   ast_channel_exten_set(chan, value);
#endif /* (AST_VERSION >= 110) */
}

static inline void alsa_input_ast_channel_language_set(
   struct ast_channel *chan, const char *value)
{
   alsa_input_assert((NULL != chan) && (NULL != value));
#if (AST_VERSION < 110)
   ast_string_field_set(chan, language, value);
#else /* (AST_VERSION >= 110) */
   ast_channel_language_set(chan, value);
#endif /* (AST_VERSION >= 110) */
}

static inline void alsa_input_ast_channel_rings_set(
   struct ast_channel *chan, int value)
{
   alsa_input_assert(NULL != chan);
#if (AST_VERSION < 110)
   chan->rings = value;
#else /* (AST_VERSION >= 110) */
   ast_channel_rings_set(chan, value);
#endif /* (AST_VERSION >= 110) */
}

static inline alsa_input_ast_format_cap *alsa_input_get_chan_tech_cap(
   struct ast_channel_tech *chan_tech)
{
   alsa_input_assert(NULL != chan_tech);
#if (AST_VERSION < 110)
   return (&(chan_tech->capabilities));
#else /* (AST_VERSION >= 110) */
   return (chan_tech->capabilities);
#endif /* (AST_VERSION >= 110) */
}

static inline const alsa_input_ast_format *alsa_input_ast_get_frame_format(
   const struct ast_frame *frame)
{
   alsa_input_assert(NULL != frame);
#if (AST_VERSION < 110)
   return (&(frame->subclass.codec));
#endif /* (AST_VERSION < 110) */
#if (110 == AST_VERSION)
   return (&(frame->subclass.format));
#endif /* (110 == AST_VERSION) */
#if (AST_VERSION > 110)
   return (frame->subclass.format);
#endif /* (AST_VERSION > 110) */
}

static inline void alsa_input_ast_set_frame_format(
   struct ast_frame *frame, alsa_input_ast_format *format)
{
   alsa_input_assert((NULL != frame) && (NULL != format));
#if (AST_VERSION < 110)
   frame->subclass.codec = *format;
#endif /* (AST_VERSION < 110) */
#if (110 == AST_VERSION)
   ast_format_copy(&(frame->subclass.format), format);
#endif /* (110 == AST_VERSION) */
#if (AST_VERSION > 110)
   frame->subclass.format = format;
#endif /* (AST_VERSION > 110) */
}

/* The two following values are from the Asterisk code */
#define MIN_DTMF_DURATION 100
#define MIN_TIME_BETWEEN_DTMF 45

#define DELAY_AUTO_HOOK_ON 10000

#define RING_CADENCE_OFF 4000
#define RING_CADENCE_ON 2000

#define AST_MODULE alsa_input_chan_type

typedef struct {
   unsigned int freq1;
   unsigned int freq2;
   unsigned int time;
   unsigned int modulate:1;
   unsigned int midinote:1;
} alsa_input_tone_part_t;

typedef struct {
   int fac1;
   int init_v2_1;
   int init_v3_1;
   int fac2;
   int init_v2_2;
   int init_v3_2;
   int modulate;
   unsigned int duration;
} alsa_input_tone_item_t;

#define MAX_ITEM_PER_PLAYTONE 2

typedef struct {
   int reppos;
   int nitems;
   alsa_input_tone_item_t items[MAX_ITEM_PER_PLAYTONE];
} alsa_input_tone_def_t;

typedef struct {
   int v1_1;
   int v2_1;
   int v3_1;
   int v1_2;
   int v2_2;
   int v3_2;
   int reppos;
   size_t nitems;
   const alsa_input_tone_item_t *items;
   size_t npos;
   size_t oldnpos;
   size_t pos;
} alsa_input_tone_state_t;

typedef enum {
   AI_STATUS_DISCONNECTED,
   AI_STATUS_ON_HOOK,
   AI_STATUS_OFF_HOOK,
} alsa_input_status_t;

typedef enum {
   AI_TONE_NONE,
   /*
    Phone is off hook and emits a tone that signals that we are
    waiting for the user to dial one or more digits
   */
   AI_TONE_WAITING_DIAL,
   /* Phone is off hook and emits a tone that signals a problem */
   AI_TONE_INVALID,
   /*
    Phone is off hook and emits a tone that signals that the called
    phone is busy
   */
   AI_TONE_BUSY,
   /* Phone is off hook and emits a DTMF tone */
   AI_TONE_DTMF_0,
   AI_TONE_DTMF_1,
   AI_TONE_DTMF_2,
   AI_TONE_DTMF_3,
   AI_TONE_DTMF_4,
   AI_TONE_DTMF_5,
   AI_TONE_DTMF_6,
   AI_TONE_DTMF_7,
   AI_TONE_DTMF_8,
   AI_TONE_DTMF_9,
   AI_TONE_DTMF_ASTER,
   AI_TONE_DTMF_POUND,
   AI_TONE_DTMF_A,
   AI_TONE_DTMF_B,
   AI_TONE_DTMF_C,
   AI_TONE_DTMF_D,
} alsa_input_tone_t;

/*
- The events that can occur on a line
-*/
typedef enum {
   /* The user hooks off the phone */
   AI_EV_OFF_HOOK,
   /* The user hooks on the phone */
   AI_EV_ON_HOOK,
   /* The phone is disconnected */
   AI_EV_DISCONNECTED,
   /* An extension has been found */
   AI_EV_EXT_FOUND,
   /* No extension can be found */
   AI_EV_NO_EXT_CAN_BE_FOUND,
   /* Internal error */
   AI_EV_INTERNAL_ERROR,
   /* Asterisk calls alsa_input_chan_request() */
   AI_EV_AST_REQUEST,
   /* Asterisk calls alsa_input_chan_call() */
   AI_EV_AST_CALL,
   /* Asterisk calls alsa_input_chan_answer() */
   AI_EV_AST_ANSWER,
   /* Asterisk calls alsa_input_chan_hangup() */
   AI_EV_AST_HANGUP,
} alsa_input_event_t;

/*
 The logical states of a line
*/
typedef enum {
   /* Phone is disconnected */
   AI_ST_DISCONNECTED,
   /* Phone is on hook */
   AI_ST_ON_IDLE,
   /* Phone is on hook and Asterisk calls alsa_input_chan_request() */
   AI_ST_ON_PRE_RINGING,
   /*
    Phone is on hook and while we were in state AI_ST_ON_PRE_RINGING,
    Asterisk calls alsa_input_chan_call()
   */
   AI_ST_ON_RINGING,
   /*
    Phone is off hook and while we were in state AI_ST_ON_IDLE,
    the user hook off the phone. This state is entered only if
    line_cfg->monitor_dialing is true
   */
   AI_ST_OFF_DIALING,
   /*
    Phone is off hook.
    We can switch to this state from the state AI_ST_OFF_DIALING, if
    we have found a valid extension, or from the state AI_ST_ON_IDLE if the
    user hook off the phone and line_cfg->monitor_dialing is false
   */
   AI_ST_OFF_WAITING_ANSWER,
   /*
    Phone is off hook and we are sending and receiving audio.
    We can switch to this state from the state AI_ST_OFF_WAITING_ANSWER when
    Asterisk calls alsa_input_chan_answer(), or from the state AI_ST_ON_RINGING
    if the user hook off the phone
   */
   AI_ST_OFF_TALKING,
   /*
    Phone is off hook and we are waiting the user to hook on the phone.
    We can switch to this state when Asterisk calls alsa_input_chan_hangup()
    or if there's an internal error
   */
   AI_ST_OFF_NO_SERVICE,
} alsa_input_state_t;

typedef struct {
   bool enable;
   struct ast_jb_conf jb_conf;
   /* Name of sound capture device */
   char snd_capture_dev_name[50];
   /* Name of sound playback device */
   char snd_playback_dev_name[50];
   /*
    Name of device generating input events, used for reading digits.
    If null, a pipe will be created to allow sending of input events via
    Asterisk console
   */
   char ev_in_dev_name[50];
   /*
    Name of device accepting output events, used for ringing. Can be the same
    as for ev_in_dev_name
   */
   char ev_out_dev_name[50];
   bool monitor_dialing;
   /*
    Character that when dialed, triggers the search for a valid extension
    immediately, if monitor_dialing is true
   */
   char search_extension_trigger;
   /*
    Delay to wait before searching for default extension is no digit is
    dialed, if monitor_dialing is true
   */
   int dialing_timeout_1st_digit;
   /*
    Delay to wait after dialing a digit before searching for an extension,
    if monitor_dialing is true
   */
   int dialing_timeout;
   /* Context where to search extension, if monitor_dialing is true */
   char context[AST_MAX_CONTEXT];
   /* Caller id name and number */
   char cid_name[AST_MAX_EXTENSION];
   char cid_num[AST_MAX_EXTENSION];
   /* For music to play on hold */
   char moh_interpret[MAX_MUSICCLASS];
} alsa_input_line_config_t;

#define MAX_LINES 1

typedef struct {
   char language[MAX_LANGUAGE];
   size_t line_count;
   alsa_input_line_config_t line_cfgs[MAX_LINES];
} alsa_input_chan_config_t;

#define SAMPLE_SIZE 2
#if __BYTE_ORDER == __LITTLE_ENDIAN
#define SND_PCM_SAMPLE_FORMAT SND_PCM_FORMAT_S16_LE
#else
#define SND_PCM_SAMPLE_FORMAT SND_PCM_FORMAT_S16_BE
#endif

/* Let's use a 30 ms period */
#define PERIOD_SIZE_IN_FRAMES (30 * DEFAULT_SAMPLES_PER_MS)
/* Buffer size must contain at least PERIOD_SIZE_IN_FRAMES * 2 bytes */
#define BUFFER_SIZE (PERIOD_SIZE_IN_FRAMES * SAMPLE_SIZE)

typedef struct alsa_input_pvt
{
   AST_LIST_ENTRY(alsa_input_pvt) list;
   struct alsa_input_chan *channel;
   /* Index of the line in array channel->config.line_cfgs */
   size_t index_line;
   /* Configuration of the line */
   const alsa_input_line_config_t *line_cfg;
   /*
    Asterisk channel linked to this line :
    any change of this field is protected by the monitor's lock and the channel
    lock at the same time, so read of this field is safe whenever one of the
    lock is acquired.
   */
   struct ast_channel *owner;
#ifdef DEBUG
   int owner_lock_count;
#endif /* DEBUG */

   /*
    The following fields are used by the monitor.
    If the monitor is running they must be accessed under the protection of
    the monitor's lock
   */
   struct {
      /*
       Copy of ast_channel.state that can be accessed from the
       monitor thread when the ast_channel is not locked
      */
      alsa_input_state_t last_known_state;
      /*
       Copy of ast_channel.snd_capture_muted that can be accessed from the
       monitor thread when the ast_channel is not locked
      */
      bool last_known_snd_capture_muted;
      /* File descriptor of sound capture device */
      int fd_snd_capture;
      /* File descriptor of input event device */
      int fd_input;
      /* File descriptor of output event device */
      int fd_output;
      /* If fd_input is the read end of a pipe, fd_pipe is the write end */
      int fd_pipe;
      /* Buffer of input_events */
      struct input_event events[64];
      /* Number of significant bytes in array events */
      size_t events_len_in_bytes;
   } monitor;

   /*
    The following fields must be accessed under the protection of the
    ast_channel's lock (if an ast_channel is associated with this line)
   */
   struct {
      /* Logical state */
      alsa_input_state_t state;
      /* Handle of sound capture device */
      snd_pcm_t *snd_capture;
      /* Handle of sound playback device */
      snd_pcm_t *snd_playback;

      /*
       Used to accumulate digits dialed
       - to recognize an extension when dialing,
       - to send dtmfs when in conversation (because we can't send
       DTMFs too quickly so we must use a buffer to store them)
      */
      char digits[AST_MAX_EXTENSION + 1];
      /* Number of significants digits in array digits */
      size_t digits_len;
      /*
       When dialing flag set to true when a new digit is dialed, meaning
       we can search for an extension
       (after waiting the delay line_cfg->dialing_timeout).
       Flag reset to false when the search has been done to not repeat
       the search until no new digit has been dialed
      */
      bool search_extension;
      /*
       When AI_STATE_RINGING == ast_channel.state, boolean to know if buzzer
       is on or off
      */
      bool buzzer_is_on;
      /*
       When in conversation flag set to NONE, ON or OFF to handle
       sending of DTMF.
      */
      enum {
         NONE, ON, OFF
      } dtmf_sent;
      /*
       Used to wait a minimum delay :
       - when ringing to toggle the buzzer on and off
       - when dialing between last digit dialed and the search for
       an extension
       - when in conversation, between the queuing of two DTMFs
      */
      struct timeval tv_wait;
      /* Status of the line */
      alsa_input_status_t status;
      /* When in conversation to know is sound capture is on or off */
      bool snd_capture_muted;
      /* Current tone playing */
      alsa_input_tone_t tone;
      /*
       If tone must be played for a limited time (like for DTMF tones),
       number of bytes to generate.
       If null, tone will be played until stopped explicitly.
      */
      size_t tone_duration_in_bytes;
      /* State variable for tone generation */
      size_t tone_bytes_generated;
      alsa_input_tone_state_t tone_state;
      /* Frame used when calling ast_queue_frame() */
      struct ast_frame frame_to_queue;
      __u8 buf_fr_to_queue[BUFFER_SIZE];
      size_t offset_buf_fr_to_queue;
      /* Frame used in alsa_input_chan_read() */
      struct ast_frame frame;
      __u8 buf_fr[AST_FRIENDLY_OFFSET + BUFFER_SIZE];
      /*
       Buffer used to hold an incomplete sample in order to only write
       to the driver, a number of bytes that is a factor of SAMPLE_SIZE.
       Buffer is also used to hold tone samples when playing tone.
      */
      __u8 bytes_not_written[BUFFER_SIZE];
      /*
       Number of significant bytes in array bytes_not_written and position
       of the first byte
      */
      size_t bytes_not_written_len;
      size_t offset_bytes_not_written;
   } ast_channel;
} alsa_input_pvt_t;

typedef struct {
   bool channel_is_locked;
   int timeout;
} alsa_input_monitor_prms_t;

typedef struct alsa_input_chan
{
   /* Driver config */
   alsa_input_chan_config_t config;
   struct ast_channel_tech chan_tech;
   bool channel_registered;
   AST_LIST_HEAD_NOLOCK(pvt_list, alsa_input_pvt) pvt_list;
   struct
   {
      /* Flag set to false to stop the monitor */
      volatile bool run;
      /* This is the thread for the monitor which checks for input on the lines
         which are not currently in use.  */
      pthread_t thread;
      ast_mutex_t lock;
#ifdef DEBUG
      int lock_count;
#endif /* DEBUG */
      /* Used to poll files of the lines */
      struct pollfd pfds[MAX_LINES * 2];
   } monitor;
} alsa_input_chan_t;

/*! Global jitterbuffer configuration - by default, jb is disabled
 *  \note Values shown here match the defaults shown in alsa.conf.sample */
static struct ast_jb_conf default_jb_conf = {
   .flags = 0,
   .max_size = 200,
   .resync_threshold = 1000,
   .impl = "fixed",
   .target_extra = 40,
};

/* dial = 350+440 */
alsa_input_tone_part_t alsa_input_tone_dial_parts[1] = {
   {
      .freq1 = 350,
      .freq2 = 440,
      .time = 0,
      .modulate = 0,
      .midinote = 0,
   }
};

/* busy = 480+620/500,0/500 */
static alsa_input_tone_part_t alsa_input_tone_busy_parts[2] = {
   {
      .freq1 = 480,
      .freq2 = 620,
      .time = 500,
      .modulate = 0,
      .midinote = 0,
   },
   {
      .freq1 = 0,
      .freq2 = 0,
      .time = 500,
      .modulate = 0,
      .midinote = 0,
   }
};

/* congestion = 480+620/250,0/250 */
static alsa_input_tone_part_t alsa_input_tone_invalid_parts[2] = {
   {
      .freq1 = 480,
      .freq2 = 620,
      .time = 250,
      .modulate = 0,
      .midinote = 0,
   },
   {
      .freq1 = 0,
      .freq2 = 0,
      .time = 200,
      .modulate = 0,
      .midinote = 0,
   }
};

alsa_input_tone_part_t alsa_input_tone_dtmf_0_parts[1] = {
   {
      .freq1 = 1336,
      .freq2 = 941,
      .time = 0,
      .modulate = 0,
      .midinote = 0,
   }
};

alsa_input_tone_part_t alsa_input_tone_dtmf_1_parts[1] = {
   {
      .freq1 = 1209,
      .freq2 = 697,
      .time = 0,
      .modulate = 0,
      .midinote = 0,
   }
};

alsa_input_tone_part_t alsa_input_tone_dtmf_2_parts[1] = {
   {
      .freq1 = 1336,
      .freq2 = 697,
      .time = 0,
      .modulate = 0,
      .midinote = 0,
   }
};

alsa_input_tone_part_t alsa_input_tone_dtmf_3_parts[1] = {
   {
      .freq1 = 1477,
      .freq2 = 697,
      .time = 0,
      .modulate = 0,
      .midinote = 0,
   }
};

alsa_input_tone_part_t alsa_input_tone_dtmf_4_parts[1] = {
   {
      .freq1 = 1209,
      .freq2 = 770,
      .time = 0,
      .modulate = 0,
      .midinote = 0,
   }
};

alsa_input_tone_part_t alsa_input_tone_dtmf_5_parts[1] = {
   {
      .freq1 = 1336,
      .freq2 = 770,
      .time = 0,
      .modulate = 0,
      .midinote = 0,
   }
};

alsa_input_tone_part_t alsa_input_tone_dtmf_6_parts[1] = {
   {
      .freq1 = 1477,
      .freq2 = 770,
      .time = 0,
      .modulate = 0,
      .midinote = 0,
   }
};

alsa_input_tone_part_t alsa_input_tone_dtmf_7_parts[1] = {
   {
      .freq1 = 1209,
      .freq2 = 852,
      .time = 0,
      .modulate = 0,
      .midinote = 0,
   }
};

alsa_input_tone_part_t alsa_input_tone_dtmf_8_parts[1] = {
   {
      .freq1 = 1336,
      .freq2 = 852,
      .time = 0,
      .modulate = 0,
      .midinote = 0,
   }
};

alsa_input_tone_part_t alsa_input_tone_dtmf_9_parts[1] = {
   {
      .freq1 = 1477,
      .freq2 = 852,
      .time = 0,
      .modulate = 0,
      .midinote = 0,
   }
};

alsa_input_tone_part_t alsa_input_tone_dtmf_aster_parts[1] = {
   {
      .freq1 = 1209,
      .freq2 = 941,
      .time = 0,
      .modulate = 0,
      .midinote = 0,
   }
};

alsa_input_tone_part_t alsa_input_tone_dtmf_pound_parts[1] = {
   {
      .freq1 = 1477,
      .freq2 = 941,
      .time = 0,
      .modulate = 0,
      .midinote = 0,
   }
};

alsa_input_tone_part_t alsa_input_tone_dtmf_A_parts[1] = {
   {
      .freq1 = 1633,
      .freq2 = 697,
      .time = 0,
      .modulate = 0,
      .midinote = 0,
   }
};

alsa_input_tone_part_t alsa_input_tone_dtmf_B_parts[1] = {
   {
      .freq1 = 1633,
      .freq2 = 770,
      .time = 0,
      .modulate = 0,
      .midinote = 0,
   }
};

alsa_input_tone_part_t alsa_input_tone_dtmf_C_parts[1] = {
   {
      .freq1 = 1633,
      .freq2 = 852,
      .time = 0,
      .modulate = 0,
      .midinote = 0,
   }
};

alsa_input_tone_part_t alsa_input_tone_dtmf_D_parts[1] = {
   {
      .freq1 = 1633,
      .freq2 = 941,
      .time = 0,
      .modulate = 0,
      .midinote = 0,
   }
};

static alsa_input_tone_def_t alsa_input_tone_dial;
static alsa_input_tone_def_t alsa_input_tone_busy;
static alsa_input_tone_def_t alsa_input_tone_invalid;
static alsa_input_tone_def_t alsa_input_tone_dtmf_0;
static alsa_input_tone_def_t alsa_input_tone_dtmf_1;
static alsa_input_tone_def_t alsa_input_tone_dtmf_2;
static alsa_input_tone_def_t alsa_input_tone_dtmf_3;
static alsa_input_tone_def_t alsa_input_tone_dtmf_4;
static alsa_input_tone_def_t alsa_input_tone_dtmf_5;
static alsa_input_tone_def_t alsa_input_tone_dtmf_6;
static alsa_input_tone_def_t alsa_input_tone_dtmf_7;
static alsa_input_tone_def_t alsa_input_tone_dtmf_8;
static alsa_input_tone_def_t alsa_input_tone_dtmf_9;
static alsa_input_tone_def_t alsa_input_tone_dtmf_aster;
static alsa_input_tone_def_t alsa_input_tone_dtmf_pound;
static alsa_input_tone_def_t alsa_input_tone_dtmf_A;
static alsa_input_tone_def_t alsa_input_tone_dtmf_B;
static alsa_input_tone_def_t alsa_input_tone_dtmf_C;
static alsa_input_tone_def_t alsa_input_tone_dtmf_D;

static const char alsa_input_chan_type[] = "AlsaInput";
static const char alsa_input_chan_desc[] = "ALSA / Input Channel Driver";
static const char alsa_input_cfg_file[] = "alsa_input.conf";
static const char alsa_input_default_extension[] = "s";
/*
 Constant short_timeout is used for example when we fail to lock a
 mutex : we set a short timeout for poll() to retry quicly
*/
static const int alsa_input_monitor_short_timeout = 10 /* ms */;
/*
 When no line are in conversation mode, we set a timeout on poll()
 of several seconds (but remember that this timeout will delay the unloading of
 the module, more specifically, when we stop the monitor)
*/
static const int alsa_input_monitor_idle_timeout = 5000; /* ms */

/*
 When at least one line is in conversation mode, as monitor thread is
 in charge of queuing data received, we set a period equal to the time
 needed to fill a frame
*/
static const int alsa_input_monitor_busy_period = (BUFFER_SIZE / (SAMPLE_SIZE * DEFAULT_SAMPLES_PER_MS));

static void alsa_input_convert_tone_part_to_item(
   const alsa_input_tone_part_t *pp, alsa_input_tone_item_t *pi, int vol)
{
   static const int midi_tohz[128] = {
      8,     8,     9,     9,     10,    10,    11,    12,    12,    13,
      14,    15,    16,    17,    18,    19,    20,    21,    23,    24,
      25,    27,    29,    30,    32,    34,    36,    38,    41,    43,
      46,    48,    51,    55,    58,    61,    65,    69,    73,    77,
      82,    87,    92,    97,    103,   110,   116,   123,   130,   138,
      146,   155,   164,   174,   184,   195,   207,   220,   233,   246,
      261,   277,   293,   311,   329,   349,   369,   391,   415,   440,
      466,   493,   523,   554,   587,   622,   659,   698,   739,   783,
      830,   880,   932,   987,   1046,  1108,  1174,  1244,  1318,  1396,
      1479,  1567,  1661,  1760,  1864,  1975,  2093,  2217,  2349,  2489,
      2637,  2793,  2959,  3135,  3322,  3520,  3729,  3951,  4186,  4434,
      4698,  4978,  5274,  5587,  5919,  6271,  6644,  7040,  7458,  7902,
      8372,  8869,  9397,  9956,  10548, 11175, 11839, 12543
   };
   static const double max_sample_val = 32768.0;

   double freq1;
   double freq2;

   if (pp->midinote) {
      /* midi notes must be between 0 and 127 */
      if (/* (pp->freq1 >= 0) && */ (pp->freq1 <= 127)) {
         freq1 = midi_tohz[pp->freq1];
      }
      else {
         freq1 = 0;
      }
      if (/* (pp->freq2 >= 0) && */ (pp->freq2 <= 127)) {
         freq2 = midi_tohz[pp->freq2];
      }
      else {
         freq2 = 0;
      }
   }
   else {
      freq1 = pp->freq1;
      freq2 = pp->freq2;
   }

   pi->fac1 = 2.0 * cos(2.0 * M_PI * (freq1 / DEFAULT_SAMPLE_RATE)) * max_sample_val;
   pi->init_v2_1 = sin(-4.0 * M_PI * (freq1 / DEFAULT_SAMPLE_RATE)) * vol;
   pi->init_v3_1 = sin(-2.0 * M_PI * (freq1 / DEFAULT_SAMPLE_RATE)) * vol;

   pi->fac2 = 2.0 * cos(2.0 * M_PI * (freq2 / DEFAULT_SAMPLE_RATE)) * max_sample_val;
   pi->init_v2_2 = sin(-4.0 * M_PI * (freq2 / DEFAULT_SAMPLE_RATE)) * vol;
   pi->init_v3_2 = sin(-2.0 * M_PI * (freq2 / DEFAULT_SAMPLE_RATE)) * vol;

   pi->duration = pp->time;
   pi->modulate = pp->modulate;
}

static void alsa_input_tone_def_init(alsa_input_tone_def_t *pd,
   int vol, const alsa_input_tone_part_t *parts, size_t part_count)
{
   size_t i;

   alsa_input_assert((NULL != parts) && (part_count > 0) && (part_count <= ARRAY_LEN(pd->items)));
   pd->nitems = part_count;
   pd->reppos = 0;
   for (i = 0; (i < part_count); i += 1) {
      alsa_input_convert_tone_part_to_item(&(parts[i]), &(pd->items[i]), vol);
   }
}

static void alsa_input_tone_state_init(alsa_input_tone_state_t *ps,
   const alsa_input_tone_def_t *pd)
{
   memset(ps, 0, sizeof(*ps));
   ps->reppos = pd->reppos;
   ps->nitems = pd->nitems;
   ps->items = pd->items;
   ps->pos = 0;
   ps->npos = 0;
   ps->oldnpos = ARRAY_LEN(pd->items) + 1;
}

#if (2 != SAMPLE_SIZE)
#error "SAMPLE_SIZE must be equal to 2"
#endif

static size_t alsa_input_generate_tone_data(alsa_input_tone_state_t *ps, __u8 *data, size_t len)
{
   size_t ret = 0;

   /* alsa_input_pr_debug("alsa_input_generate_tone_data(len=%lu)\n",
      (unsigned long)(len)); */

   alsa_input_assert((NULL != data) && (len > 0));

   if (ps->npos < ps->nitems) {

      /* we need to prepare a frame with 16 * timelen samples as we're
       * generating SLIN audio */
      memset(data, 0, len);

      for (;;) {
         const alsa_input_tone_item_t *pi = &(ps->items[ps->npos]);
         size_t sample_count;
         size_t x;

         if (ps->oldnpos != ps->npos) {
            /* Load new parameters */
            ps->v1_1 = 0;
            ps->v2_1 = pi->init_v2_1;
            ps->v3_1 = pi->init_v3_1;
            ps->v1_2 = 0;
            ps->v2_2 = pi->init_v2_2;
            ps->v3_2 = pi->init_v3_2;
            ps->pos = 0;
            ps->oldnpos = ps->npos;
         }

         sample_count = len / SAMPLE_SIZE;
         if ((pi->duration > 0)
             && ((ps->pos + sample_count) > (pi->duration * DEFAULT_SAMPLES_PER_MS))) {
            sample_count = (pi->duration * DEFAULT_SAMPLES_PER_MS) - ps->pos;
         }

         for (x = 0; (x < sample_count); x += 1) {
            __s16 sample;
            ps->v1_1 = ps->v2_1;
            ps->v2_1 = ps->v3_1;
            ps->v3_1 = (pi->fac1 * ps->v2_1 >> 15) - ps->v1_1;

            ps->v1_2 = ps->v2_2;
            ps->v2_2 = ps->v3_2;
            ps->v3_2 = (pi->fac2 * ps->v2_2 >> 15) - ps->v1_2;
            if (pi->modulate) {
               int p;
               p = ps->v3_2 - 32768;
               if (p < 0) {
                  p = -p;
               }
               p = ((p * 9) / 10) + 1;
               sample = (ps->v3_1 * p) >> 15;
            } else {
               sample = ps->v3_1 + ps->v3_2;
            }
#if __BYTE_ORDER == __LITTLE_ENDIAN
            *data = (__u8)(sample & 0xFF);
            data += 1;
            *data = (__u8)(sample >> 8);
#else
            *data = (__u8)(sample >> 8);
            data += 1;
            *data = (__u8)(sample & 0xFF);
#endif
            data += 1;
         }

         ps->pos += x;
         ret += x;
         len -= (x * SAMPLE_SIZE);

         if ((pi->duration > 0)
             && (ps->pos >= (pi->duration * DEFAULT_SAMPLES_PER_MS))) { /* item finished? */
            ps->npos += 1;
            if (ps->npos >= ps->nitems) { /* last item? */
               if (ps->reppos >= 0) {     /* repeat set? */
                  ps->npos = ps->reppos;  /* redo from top */
               }
            }
         }

         if (len < SAMPLE_SIZE) {
            break;
         }
      }
   }

   return (ret * SAMPLE_SIZE);
}

static void alsa_input_init_tones(void)
{
   static const int vol = 7219; /* Default to -8db */

   alsa_input_tone_def_init(&(alsa_input_tone_dial), vol, alsa_input_tone_dial_parts, ARRAY_LEN(alsa_input_tone_dial_parts));
   alsa_input_tone_def_init(&(alsa_input_tone_busy), vol, alsa_input_tone_busy_parts, ARRAY_LEN(alsa_input_tone_busy_parts));
   alsa_input_tone_def_init(&(alsa_input_tone_invalid), vol, alsa_input_tone_invalid_parts, ARRAY_LEN(alsa_input_tone_invalid_parts));
   alsa_input_tone_def_init(&(alsa_input_tone_dtmf_0), vol, alsa_input_tone_dtmf_0_parts, ARRAY_LEN(alsa_input_tone_dtmf_0_parts));
   alsa_input_tone_def_init(&(alsa_input_tone_dtmf_1), vol, alsa_input_tone_dtmf_1_parts, ARRAY_LEN(alsa_input_tone_dtmf_1_parts));
   alsa_input_tone_def_init(&(alsa_input_tone_dtmf_2), vol, alsa_input_tone_dtmf_2_parts, ARRAY_LEN(alsa_input_tone_dtmf_2_parts));
   alsa_input_tone_def_init(&(alsa_input_tone_dtmf_3), vol, alsa_input_tone_dtmf_3_parts, ARRAY_LEN(alsa_input_tone_dtmf_3_parts));
   alsa_input_tone_def_init(&(alsa_input_tone_dtmf_4), vol, alsa_input_tone_dtmf_4_parts, ARRAY_LEN(alsa_input_tone_dtmf_4_parts));
   alsa_input_tone_def_init(&(alsa_input_tone_dtmf_5), vol, alsa_input_tone_dtmf_5_parts, ARRAY_LEN(alsa_input_tone_dtmf_5_parts));
   alsa_input_tone_def_init(&(alsa_input_tone_dtmf_6), vol, alsa_input_tone_dtmf_6_parts, ARRAY_LEN(alsa_input_tone_dtmf_6_parts));
   alsa_input_tone_def_init(&(alsa_input_tone_dtmf_7), vol, alsa_input_tone_dtmf_7_parts, ARRAY_LEN(alsa_input_tone_dtmf_7_parts));
   alsa_input_tone_def_init(&(alsa_input_tone_dtmf_8), vol, alsa_input_tone_dtmf_8_parts, ARRAY_LEN(alsa_input_tone_dtmf_8_parts));
   alsa_input_tone_def_init(&(alsa_input_tone_dtmf_9), vol, alsa_input_tone_dtmf_9_parts, ARRAY_LEN(alsa_input_tone_dtmf_9_parts));
   alsa_input_tone_def_init(&(alsa_input_tone_dtmf_aster), vol, alsa_input_tone_dtmf_aster_parts, ARRAY_LEN(alsa_input_tone_dtmf_aster_parts));
   alsa_input_tone_def_init(&(alsa_input_tone_dtmf_pound), vol, alsa_input_tone_dtmf_pound_parts, ARRAY_LEN(alsa_input_tone_dtmf_pound_parts));
   alsa_input_tone_def_init(&(alsa_input_tone_dtmf_A), vol, alsa_input_tone_dtmf_A_parts, ARRAY_LEN(alsa_input_tone_dtmf_A_parts));
   alsa_input_tone_def_init(&(alsa_input_tone_dtmf_B), vol, alsa_input_tone_dtmf_B_parts, ARRAY_LEN(alsa_input_tone_dtmf_B_parts));
   alsa_input_tone_def_init(&(alsa_input_tone_dtmf_C), vol, alsa_input_tone_dtmf_C_parts, ARRAY_LEN(alsa_input_tone_dtmf_C_parts));
   alsa_input_tone_def_init(&(alsa_input_tone_dtmf_D), vol, alsa_input_tone_dtmf_D_parts, ARRAY_LEN(alsa_input_tone_dtmf_D_parts));
}

static int alsa_input_card_init(const char *dev,
   snd_pcm_stream_t stream, snd_pcm_t **card, int *fd)
{
   int ret = -1;
   snd_pcm_t *handle = NULL;
   snd_pcm_hw_params_t *hw_params = NULL;
   snd_pcm_sw_params_t *sw_params = NULL;

   do { /* Empty loop */
      int err;
      int direction;
      snd_pcm_uframes_t period_size;
      snd_pcm_uframes_t buffer_size = 0;
      unsigned int rate;
      snd_pcm_uframes_t start_threshold;
      snd_pcm_uframes_t stop_threshold;

      err = snd_pcm_open(&(handle), dev, stream, SND_PCM_NONBLOCK);
      if (err) {
         ast_log(AST_LOG_ERROR, "snd_pcm_open() failed for device '%s': '%s'\n", dev, snd_strerror(err));
         ret = err;
         break;
      }
      alsa_input_pr_debug("Opening device '%s' in %s mode\n", dev, (stream == SND_PCM_STREAM_CAPTURE) ? "read" : "write");

      hw_params = NULL;
      err = snd_pcm_hw_params_malloc(&(hw_params));
      if ((err < 0) || (NULL == hw_params)) {
         ast_log(AST_LOG_ERROR, "Failed to allocate hw_params structure for device '%s'\n", dev);
         break;
      }

      err = snd_pcm_hw_params_any(handle, hw_params);
      if (err < 0) {
         ret = err;
         ast_log(AST_LOG_ERROR, "snd_pcm_hw_params_any() failed for device '%s': '%s'\n", dev, snd_strerror(err));
         break;
      }

      err = snd_pcm_hw_params_set_access(handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
      if (err < 0) {
         ret = err;
         ast_log(AST_LOG_ERROR, "snd_pcm_hw_params_set_access() failed for device '%s': '%s'\n", dev, snd_strerror(err));
         break;
      }

      err = snd_pcm_hw_params_set_format(handle, hw_params, SND_PCM_SAMPLE_FORMAT);
      if (err < 0) {
         ret = err;
         ast_log(AST_LOG_ERROR, "snd_pcm_hw_params_set_format() failed for device '%s': '%s'\n", dev, snd_strerror(err));
         break;
      }

      err = snd_pcm_hw_params_set_channels(handle, hw_params, 1);
      if (err < 0) {
         ret = err;
         ast_log(AST_LOG_ERROR, "snd_pcm_hw_params_set_channels() failed for device '%s': '%s'\n", dev, snd_strerror(err));
         break;
      }

      direction = 0;
      rate = DEFAULT_SAMPLES_PER_MS * 1000;
      err = snd_pcm_hw_params_set_rate_near(handle, hw_params, &(rate), &(direction));
      if (err < 0) {
         ret = err;
         ast_log(AST_LOG_ERROR, "snd_pcm_hw_params_set_rate_near() failed for device '%s': '%s'\n", dev, snd_strerror(err));
         break;
      }
      if (rate != (DEFAULT_SAMPLES_PER_MS * 1000)) {
         ast_log(AST_LOG_WARNING, "Can't set rate for device '%s', requested %u, got %u\n",
            dev, (unsigned int)(DEFAULT_SAMPLES_PER_MS * 1000), (unsigned int)(rate));
         break;
      }

      direction = 0;
      period_size = PERIOD_SIZE_IN_FRAMES;
      err = snd_pcm_hw_params_set_period_size_near(handle, hw_params, &(period_size), &(direction));
      if (err < 0) {
         ast_log(AST_LOG_ERROR, "snd_pcm_hw_params_set_period_size_near() failed for device '%s': '%s'\n", dev, snd_strerror(err));
         break;
      }
      alsa_input_pr_debug("Period size is %lu\n", (unsigned long)(period_size));

      buffer_size = period_size * 16;
      err = snd_pcm_hw_params_set_buffer_size_near(handle, hw_params, &(buffer_size));
      if (err < 0) {
         ast_log(AST_LOG_WARNING, "snd_pcm_hw_params_set_buffer_size_near() failed for device '%s': '%s'\n", dev, snd_strerror(err));
         break;
      }
      alsa_input_pr_debug("Buffer size is set to %lu frames\n", (unsigned long)(buffer_size));

      err = snd_pcm_hw_params(handle, hw_params);
      if (err < 0) {
         ret = err;
         ast_log(AST_LOG_ERROR, "Couldn't set the new hw params for device '%s': '%s'\n", dev, snd_strerror(err));
         break;
      }

      /* Do not free hw_params on exit */
      hw_params = NULL;

      sw_params = NULL;
      err = snd_pcm_sw_params_malloc(&(sw_params));
      if ((err < 0) || (NULL == sw_params)) {
         ast_log(AST_LOG_ERROR, "Failed to allocate sw_params structure for device '%s'\n", dev);
         break;
      }

      err = snd_pcm_sw_params_current(handle, sw_params);
      if (err < 0) {
         ret = err;
         ast_log(AST_LOG_ERROR, "snd_pcm_sw_params_current() failed for device '%s': '%s'\n", dev, snd_strerror(err));
         break;
      }

      if (stream == SND_PCM_STREAM_PLAYBACK) {
         start_threshold = period_size;
      }
      else {
         start_threshold = 1;
      }
      err = snd_pcm_sw_params_set_start_threshold(handle, sw_params, start_threshold);
      if (err < 0) {
         ast_log(AST_LOG_ERROR, "snd_pcm_sw_params_set_start_threshold() failed for device '%s': '%s'\n", dev, snd_strerror(err));
         break;
      }

      if (stream == SND_PCM_STREAM_PLAYBACK) {
         stop_threshold = buffer_size;
      }
      else {
         stop_threshold = buffer_size;
      }
      err = snd_pcm_sw_params_set_stop_threshold(handle, sw_params, stop_threshold);
      if (err < 0) {
         ast_log(AST_LOG_ERROR, "snd_pcm_sw_params_set_stop_threshold() failed for device '%s': '%s'\n", dev, snd_strerror(err));
         break;
      }

      err = snd_pcm_sw_params(handle, sw_params);
      if (err < 0) {
         ast_log(AST_LOG_ERROR, "Couldn't set the new sw params for device '%s': '%s'\n", dev, snd_strerror(err));
         break;
      }

      /* Do not free sw_params on exit */
      sw_params = NULL;

      if (NULL != fd) {
         struct pollfd pfd;
         err = snd_pcm_poll_descriptors_count(handle);
         if (err <= 0) {
            ast_log(AST_LOG_ERROR, "Unable to get a poll descriptors count for device '%s': '%s'\n", dev, snd_strerror(err));
            break;
         }
         if (err != 1) {
            alsa_input_pr_debug("Can't handle more than one poll descritor\n");
            break;
         }

         snd_pcm_poll_descriptors(handle, &(pfd), err);
         *fd = pfd.fd;
      }
      *card = handle;
      handle = NULL;

      ret = 0;
   }
   while (false);

   if (NULL != sw_params) {
      snd_pcm_sw_params_free(sw_params);
      sw_params = NULL;
   }

   if (NULL != hw_params) {
      snd_pcm_hw_params_free(hw_params);
      hw_params = NULL;
   }

   if (NULL != handle) {
      snd_pcm_close(handle);
      handle = NULL;
   }

   return (ret);
}

static bool alsa_input_handle_card_error(snd_pcm_t *card, snd_pcm_sframes_t error, const char *function)
{
   bool ret = false;

   alsa_input_assert(error < 0);

   do { /* Empty loop */
      if (-EAGAIN == error) {
          /*  Nothing todo */
         break;
      }
      alsa_input_pr_debug("%s() failed with error %ld : '%s'\n",
         function, (long)(error), snd_strerror(error));
      error = snd_pcm_recover(card, error, 0);
      if (error >= 0) {
         break;
      }
      if (-EAGAIN == error) {
          /*  Nothing todo */
         break;
      }
      if (-EPIPE == error) {
         break;
      }
      if (-ESTRPIPE == error) {
         /* Device is in suspend state */
         int err = 0;
         for (;;) {
            int err = snd_pcm_resume(card);
            if (-EAGAIN != err) {
               break;
            }
         }
         if (err) {
            err = snd_pcm_prepare(card);
            if (err) {
               ast_log(AST_LOG_ERROR, "snd_pcm_prepare() failed: '%s'\n", snd_strerror(error));
            }
         }
         break;
      }
      ast_log(AST_LOG_ERROR, "%s() failed: '%s'\n", function, snd_strerror(error));
      if ((-ENODEV == error) || (-ENOTTY == error)) {
         ret = true;
         break;
      }
   } while (false);

   return (ret);
}

static void alsa_input_start_card(snd_pcm_t *card)
{
   int err = snd_pcm_prepare(card);
   if (err) {
      alsa_input_pr_debug("snd_pcm_prepare() failed: '%s'\n", snd_strerror(err));
   }
   err = snd_pcm_start(card);
   if (err) {
      alsa_input_pr_debug("snd_pcm_start() failed: '%s'\n", snd_strerror(err));
   }
}

static void alsa_input_stop_card(snd_pcm_t *card)
{
   int err = snd_pcm_drop(card);
   if (err) {
      alsa_input_pr_debug("snd_pcm_drop() failed: '%s'\n", snd_strerror(err));
   }
}

/* Must be called with pvt->owner locked */
static inline void alsa_input_reset_pvt_monitor_state(alsa_input_pvt_t *pvt)
{
   alsa_input_pr_debug("alsa_input_reset_pvt_monitor_state()\n");

   alsa_input_assert((NULL == pvt->owner) || (pvt->owner_lock_count > 0));

   pvt->ast_channel.digits[0] = '\0';
   pvt->ast_channel.digits_len = 0;
   pvt->ast_channel.search_extension = false;
   pvt->ast_channel.dtmf_sent = NONE;
}

/*
 Must be called with pvt->owner locked.
*/
static void alsa_input_reset_buf_fr_to_queue(alsa_input_pvt_t *pvt)
{
   alsa_input_assert((NULL == pvt->owner) || (pvt->owner_lock_count > 0));
   pvt->ast_channel.offset_buf_fr_to_queue = 0;
}

/*
 Must be called with pvt->owner locked.
*/
static void alsa_input_reset_buf_bytes_not_written(alsa_input_pvt_t *pvt)
{
   alsa_input_assert((NULL == pvt->owner) || (pvt->owner_lock_count > 0));
   pvt->ast_channel.bytes_not_written_len = 0;
   pvt->ast_channel.offset_bytes_not_written = 0;
}

/*
 Must be called with pvt->owner locked
*/
static void alsa_input_turn_buzzer_on(alsa_input_pvt_t *pvt)
{
   alsa_input_pr_debug("alsa_input_turn_buzzer_on()\n");

   alsa_input_assert((NULL == pvt->owner) || (pvt->owner_lock_count > 0));

   if (pvt->monitor.fd_output >= 0) {
      struct input_event event;
      event.type = EV_SND;
      event.code = SND_BELL;
      event.value = 1;
      write(pvt->monitor.fd_output, &(event), sizeof(event));
   }
   pvt->ast_channel.buzzer_is_on = true;
   pvt->ast_channel.tv_wait = ast_tvnow();
   alsa_input_pr_debug("Line %lu should be ringing\n", (unsigned long)(pvt->index_line + 1));
}

/*
 Must be called with pvt->owner locked
*/
static void alsa_input_turn_buzzer_off(alsa_input_pvt_t *pvt)
{
   alsa_input_pr_debug("alsa_input_turn_buzzer_off()\n");

   alsa_input_assert((NULL == pvt->owner) || (pvt->owner_lock_count > 0));

   if (pvt->monitor.fd_output >= 0) {
      struct input_event event;
      event.type = EV_SND;
      event.code = SND_BELL;
      event.value = 0;
      write(pvt->monitor.fd_output, &(event), sizeof(event));
   }
   pvt->ast_channel.buzzer_is_on = false;
   pvt->ast_channel.tv_wait = ast_tvnow();
   alsa_input_pr_debug("Line %lu should not ring anymore\n", (unsigned long)(pvt->index_line + 1));
}

static void alsa_input_set_line_tone(alsa_input_pvt_t *pvt,
   alsa_input_tone_t tone, size_t tone_duration);

/* Must be called with pvt->owner locked */
static void alsa_input_set_new_state(alsa_input_pvt_t *pvt,
   alsa_input_state_t new_state, alsa_input_event_t cause)
{
   alsa_input_pr_debug("alsa_input_set_new_state(new_state=%d, cause=%d)\n",
      (int)(new_state), (int)(cause));

   alsa_input_assert((NULL == pvt->owner) || (pvt->owner_lock_count > 0));

   if (new_state != pvt->ast_channel.state) {
      if (((AI_ST_OFF_TALKING == new_state)
           || (AI_ST_OFF_WAITING_ANSWER == new_state))
          && (AI_ST_OFF_TALKING != pvt->ast_channel.state)
          && (AI_ST_OFF_WAITING_ANSWER != pvt->ast_channel.state)) {
         /*
          Start capture if not muted, playback will be started the
          next call of alsa_input_chan_write() */
         pvt->ast_channel.snd_capture_muted = false;
         alsa_input_start_card(pvt->ast_channel.snd_capture);
         alsa_input_reset_buf_fr_to_queue(pvt);
      }
      else if (AI_ST_ON_RINGING == new_state) {
         alsa_input_turn_buzzer_on(pvt);
      }
      if (((AI_ST_OFF_TALKING == pvt->ast_channel.state)
           || (AI_ST_OFF_WAITING_ANSWER == pvt->ast_channel.state))
          && (AI_ST_OFF_TALKING != new_state)
          && (AI_ST_OFF_WAITING_ANSWER != new_state)) {
         /* Stop capture and playback */
         pvt->ast_channel.snd_capture_muted = true;
         alsa_input_stop_card(pvt->ast_channel.snd_capture);
         if (AI_TONE_NONE == pvt->ast_channel.tone) {
            alsa_input_stop_card(pvt->ast_channel.snd_playback);
            alsa_input_reset_buf_bytes_not_written(pvt);
         }
      }
      else if (AI_ST_ON_RINGING == pvt->ast_channel.state) {
         alsa_input_turn_buzzer_off(pvt);
      }
   }

   switch (new_state) {
      case AI_ST_DISCONNECTED: {
         alsa_input_assert((AI_STATUS_DISCONNECTED == pvt->ast_channel.status)
            && (NULL == pvt->owner));
         alsa_input_assert(AI_EV_DISCONNECTED == cause);
         alsa_input_set_line_tone(pvt, AI_TONE_NONE, 0);
         alsa_input_reset_pvt_monitor_state(pvt);
         pvt->ast_channel.snd_capture_muted = true;
         alsa_input_reset_buf_fr_to_queue(pvt);
         alsa_input_reset_buf_bytes_not_written(pvt);
         break;
      }
      case AI_ST_ON_IDLE: {
         alsa_input_assert((AI_STATUS_ON_HOOK == pvt->ast_channel.status)
            && (NULL == pvt->owner));
         alsa_input_assert((AI_EV_AST_HANGUP == cause)
            || (AI_EV_INTERNAL_ERROR == cause)
            || (AI_EV_ON_HOOK == cause));
         alsa_input_set_line_tone(pvt, AI_TONE_NONE, 0);
         alsa_input_reset_pvt_monitor_state(pvt);
         pvt->ast_channel.snd_capture_muted = true;
         alsa_input_reset_buf_fr_to_queue(pvt);
         break;
      }
      case AI_ST_ON_PRE_RINGING: {
         alsa_input_assert((AI_STATUS_ON_HOOK == pvt->ast_channel.status)
            && (NULL != pvt->owner));
         alsa_input_assert((AI_ST_ON_IDLE == pvt->ast_channel.state)
            && (AI_EV_AST_REQUEST == cause));
         break;
      }
      case AI_ST_ON_RINGING: {
         alsa_input_assert((AI_STATUS_ON_HOOK == pvt->ast_channel.status)
            && (NULL != pvt->owner));
         alsa_input_assert((AI_ST_ON_PRE_RINGING == pvt->ast_channel.state)
            && (AI_EV_AST_CALL == cause));
         break;
      }
      case AI_ST_OFF_DIALING: {
         alsa_input_assert((AI_STATUS_OFF_HOOK == pvt->ast_channel.status)
            && (NULL == pvt->owner) && (pvt->line_cfg->monitor_dialing));
         alsa_input_assert((AI_ST_ON_IDLE == pvt->ast_channel.state)
            && (AI_EV_OFF_HOOK == cause));
         alsa_input_set_line_tone(pvt, AI_TONE_WAITING_DIAL, 0);
         alsa_input_reset_pvt_monitor_state(pvt);
         /*
          We start searching alsa_input_default_extension ('s') after
          pvt->line_cfg->dialing_timeout_1st_digit milliseconds
         */
         pvt->ast_channel.search_extension = true;
         pvt->ast_channel.tv_wait = ast_tvnow();
         break;
      }
      case AI_ST_OFF_WAITING_ANSWER: {
         alsa_input_assert((AI_STATUS_OFF_HOOK == pvt->ast_channel.status)
            && (NULL != pvt->owner));
         alsa_input_assert(((AI_ST_ON_IDLE == pvt->ast_channel.state)
            && (AI_EV_OFF_HOOK == cause) && (!pvt->line_cfg->monitor_dialing))
                    || ((AI_ST_OFF_DIALING == pvt->ast_channel.state)
            && (AI_EV_EXT_FOUND == cause)));
         alsa_input_set_line_tone(pvt, AI_TONE_NONE, 0);
         /*
          We forget all digits dialed
         */
         alsa_input_reset_pvt_monitor_state(pvt);
         break;
      }
      case AI_ST_OFF_TALKING: {
         alsa_input_assert((AI_STATUS_OFF_HOOK == pvt->ast_channel.status)
            && (NULL != pvt->owner));
         alsa_input_assert(((AI_ST_ON_RINGING == pvt->ast_channel.state)
            && (AI_EV_OFF_HOOK == cause))
                    || ((AI_ST_OFF_WAITING_ANSWER == pvt->ast_channel.state)
            && (AI_EV_AST_ANSWER == cause)));
         alsa_input_set_line_tone(pvt, AI_TONE_NONE, 0);
         break;
      }
      case AI_ST_OFF_NO_SERVICE: {
         alsa_input_assert((AI_STATUS_OFF_HOOK == pvt->ast_channel.status)
            && (NULL == pvt->owner));
         alsa_input_assert(((AI_ST_OFF_DIALING == pvt->ast_channel.state) && (AI_EV_NO_EXT_CAN_BE_FOUND == cause))
            || (AI_EV_AST_HANGUP == cause)
            || (AI_EV_INTERNAL_ERROR == cause));
         if (AI_EV_AST_HANGUP == cause) {
            alsa_input_set_line_tone(pvt, AI_TONE_BUSY, 0);
         }
         else {
            alsa_input_set_line_tone(pvt, AI_TONE_INVALID, 0);
         }
         alsa_input_reset_pvt_monitor_state(pvt);
         pvt->ast_channel.snd_capture_muted = true;
         alsa_input_reset_buf_fr_to_queue(pvt);
         alsa_input_reset_buf_bytes_not_written(pvt);
         /*
          Store time this state was entered to hook on the phone after
          DELAY_AUTO_HOOK_ON
         */
         pvt->ast_channel.tv_wait = ast_tvnow();
         break;
      }
   }
   pvt->ast_channel.state = new_state;
}

/* Must be called with pvt->owner and monitor.lock locked */
static void alsa_input_unlink_from_ast_channel(alsa_input_pvt_t *pvt,
   alsa_input_event_t cause, bool unlock_ast_channel)
{
   struct ast_channel *ast = pvt->owner;

   alsa_input_pr_debug("alsa_input_unlink_from_ast_channel(cause=%d)\n",
      (int)(cause));

   alsa_input_assert((pvt->channel->monitor.lock_count > 0)
      && (NULL != pvt->owner) && (pvt->owner_lock_count > 0));
   alsa_input_ast_channel_tech_pvt_set(ast, NULL);
   pvt->owner = NULL;
   ast_setstate(ast, AST_STATE_DOWN);
   /* Set state before unlocking the channel */
   if (AI_STATUS_OFF_HOOK == pvt->ast_channel.status) {
      alsa_input_set_new_state(pvt, AI_ST_OFF_NO_SERVICE, cause);
   }
   else if (AI_STATUS_ON_HOOK == pvt->ast_channel.status) {
      alsa_input_set_new_state(pvt, AI_ST_ON_IDLE, cause);
   }
   else {
      alsa_input_assert(AI_STATUS_DISCONNECTED == pvt->ast_channel.status);
      alsa_input_set_new_state(pvt, AI_ST_DISCONNECTED, AI_EV_DISCONNECTED);
   }
   alsa_input_pr_debug("Line %lu unlink from channel '%s'.\n",
      (unsigned long)(pvt->index_line + 1), alsa_input_ast_channel_name(ast));
   ast_module_unref(ast_module_info->self);
   if (unlock_ast_channel) {
#ifdef DEBUG
      alsa_input_assert(pvt->owner_lock_count > 0);
      pvt->owner_lock_count -= 1;
#endif /* DEBUG */
      ast_channel_unlock(ast);
   }
}

/*
 Must be called with pvt->owner and monitor.lock locked.
 When exiting pvt->owner has been unlocked, pvt->owner is set to NULL.
*/
static void alsa_input_queue_hangup(alsa_input_pvt_t *pvt,
   alsa_input_event_t cause)
{
   struct ast_channel *ast = pvt->owner;

   alsa_input_pr_debug("alsa_input_queue_hangup(cause=%d)\n", (int)(cause));

   alsa_input_assert((pvt->channel->monitor.lock_count > 0)
      && (NULL != pvt->owner) && (pvt->owner_lock_count > 0));

   alsa_input_unlink_from_ast_channel(pvt, cause, true);
   alsa_input_assert(NULL == pvt->owner);
   if (ast_queue_hangup(ast)) {
      ast_log(AST_LOG_WARNING, "Unable to queue hangup on line '%s'\n", alsa_input_ast_channel_name(ast));
   }
}

/*
 Must be called with pvt->owner and monitor.lock locked
*/
static void alsa_input_disconnect_line(alsa_input_pvt_t *pvt)
{
   alsa_input_pr_debug("alsa_input_disconnect_line()\n");

   alsa_input_assert((pvt->channel->monitor.lock_count > 0)
      && ((NULL == pvt->owner) || (pvt->owner_lock_count > 0))
      && (AI_STATUS_DISCONNECTED == pvt->ast_channel.status));

   if (NULL != pvt->owner) {
      alsa_input_queue_hangup(pvt, AI_EV_DISCONNECTED);
      alsa_input_assert(NULL == pvt->owner);
   }
   else {
      alsa_input_set_new_state(pvt, AI_ST_DISCONNECTED, AI_EV_DISCONNECTED);
   }
   pvt->monitor.last_known_state = pvt->ast_channel.state;
   /* Closes sound devices */
   if (NULL != pvt->ast_channel.snd_capture) {
      snd_pcm_close(pvt->ast_channel.snd_capture);
      pvt->ast_channel.snd_capture = NULL;
      pvt->monitor.fd_snd_capture = -1;
   }
   if (NULL != pvt->ast_channel.snd_playback) {
      snd_pcm_close(pvt->ast_channel.snd_playback);
      pvt->ast_channel.snd_playback = NULL;
   }
   if (pvt->monitor.fd_pipe >= 0) {
      close(pvt->monitor.fd_pipe);
      pvt->monitor.fd_pipe = -1;
   }
   if (pvt->monitor.fd_input >= 0) {
      close(pvt->monitor.fd_input);
      pvt->monitor.fd_input = -1;
   }
   if (pvt->monitor.fd_output >= 0) {
      close(pvt->monitor.fd_output);
      pvt->monitor.fd_output = -1;
   }
}

/*
 Must be called with pvt->owner locked
*/
static void alsa_input_critical_error(alsa_input_pvt_t *pvt,
   bool monitor_is_locked)
{
   alsa_input_pr_debug("alsa_input_critical_error()\n");
   alsa_input_assert((NULL == pvt->owner) || (pvt->owner_lock_count > 0));
   ast_log(AST_LOG_ERROR, "Line %lu : critical error dectected, so line is disconnected\n", (unsigned long)(pvt->index_line + 1));
   pvt->ast_channel.status = AI_STATUS_DISCONNECTED;
   if (monitor_is_locked) {
      alsa_input_disconnect_line(pvt);
   }
   else {
      if (NULL != pvt->owner) {
         ast_hangup(pvt->owner);
      }
   }
}

/* Must be called with pvt->owner locked */
static void alsa_input_write_tone_data(alsa_input_pvt_t *pvt)
{
   /* alsa_input_pr_debug("alsa_input_write_tone_data()\n"); */

   alsa_input_assert(((NULL == pvt->owner) || (pvt->owner_lock_count > 0))
      && (AI_TONE_NONE != pvt->ast_channel.tone));

   for (;;) {
      /* We test if there is samples to write */
      if (pvt->ast_channel.bytes_not_written_len < SAMPLE_SIZE) {
         /*
          * Not enough data, we generate some samples, but we must be careful about
          * tone duration asked
          */
         size_t len_needed;

         if (pvt->ast_channel.bytes_not_written_len > 0) {
            memmove(pvt->ast_channel.bytes_not_written,
               &(pvt->ast_channel.bytes_not_written[pvt->ast_channel.offset_bytes_not_written]),
               pvt->ast_channel.bytes_not_written_len);
         }
         pvt->ast_channel.offset_bytes_not_written = 0;

         len_needed = ARRAY_LEN(pvt->ast_channel.bytes_not_written) - pvt->ast_channel.bytes_not_written_len;
         if (pvt->ast_channel.tone_duration_in_bytes > 0) {
            if ((pvt->ast_channel.tone_bytes_generated + len_needed) > pvt->ast_channel.tone_duration_in_bytes) {
               len_needed = pvt->ast_channel.tone_duration_in_bytes - pvt->ast_channel.tone_bytes_generated;
            }
         }
         if (len_needed > 0) {
            size_t tmp = alsa_input_generate_tone_data(&(pvt->ast_channel.tone_state),
                  &(pvt->ast_channel.bytes_not_written[pvt->ast_channel.bytes_not_written_len]),
                  len_needed);
            pvt->ast_channel.bytes_not_written_len += tmp;
            pvt->ast_channel.tone_bytes_generated += tmp;
         }
      }

      if (pvt->ast_channel.bytes_not_written_len > 0) {
         snd_pcm_state_t state;
         snd_pcm_sframes_t written;
         size_t tmp;

         /* We test if playback card is started */
         state = snd_pcm_state(pvt->ast_channel.snd_playback);
         if ((state != SND_PCM_STATE_PREPARED) && (state != SND_PCM_STATE_RUNNING)) {
            int err = snd_pcm_prepare(pvt->ast_channel.snd_playback);
            if (err) {
               ast_log(AST_LOG_ERROR, "snd_pcm_prepare() failed: '%s'\n", snd_strerror(err));
            }
         }
         written = snd_pcm_writei(pvt->ast_channel.snd_playback,
            &(pvt->ast_channel.bytes_not_written[pvt->ast_channel.offset_bytes_not_written]),
            pvt->ast_channel.bytes_not_written_len / SAMPLE_SIZE);
         if (written < 0) {
            if (alsa_input_handle_card_error(pvt->ast_channel.snd_playback, written, "snd_pcm_writei")) {
               /* Critical error */
               alsa_input_critical_error(pvt, false);
            }
            break;
         }
         tmp = (written * SAMPLE_SIZE);
         pvt->ast_channel.offset_bytes_not_written += tmp;
         pvt->ast_channel.bytes_not_written_len -= tmp;
         if (pvt->ast_channel.bytes_not_written_len >= SAMPLE_SIZE) {
            break;
         }
      }
      else {
         alsa_input_set_line_tone(pvt, AI_TONE_NONE, 0);
         break;
      }
   }
}

/* Must be called with pvt->owner locked */
static void alsa_input_set_line_tone(alsa_input_pvt_t *pvt,
   alsa_input_tone_t tone, size_t tone_duration)
{
   alsa_input_pr_debug("alsa_input_set_line_tone(tone=%d, tone_duration=%lu)\n",
      (int)(tone), (long)(tone_duration));

   alsa_input_assert((NULL == pvt->owner) || (pvt->owner_lock_count > 0));

   switch (tone) {
      case AI_TONE_NONE: {
         tone_duration = 0;
         break;
      }
      case AI_TONE_WAITING_DIAL: {
         alsa_input_tone_state_init(&(pvt->ast_channel.tone_state), &(alsa_input_tone_dial));
         break;
      }
      case AI_TONE_INVALID: {
         alsa_input_tone_state_init(&(pvt->ast_channel.tone_state), &(alsa_input_tone_invalid));
         break;
      }
      case AI_TONE_BUSY: {
         alsa_input_tone_state_init(&(pvt->ast_channel.tone_state), &(alsa_input_tone_busy));
         break;
      }
      case AI_TONE_DTMF_0: {
         alsa_input_tone_state_init(&(pvt->ast_channel.tone_state), &(alsa_input_tone_dtmf_0));
         break;
      }
      case AI_TONE_DTMF_1: {
         alsa_input_tone_state_init(&(pvt->ast_channel.tone_state), &(alsa_input_tone_dtmf_1));
         break;
      }
      case AI_TONE_DTMF_2: {
         alsa_input_tone_state_init(&(pvt->ast_channel.tone_state), &(alsa_input_tone_dtmf_2));
         break;
      }
      case AI_TONE_DTMF_3: {
         alsa_input_tone_state_init(&(pvt->ast_channel.tone_state), &(alsa_input_tone_dtmf_3));
         break;
      }
      case AI_TONE_DTMF_4: {
         alsa_input_tone_state_init(&(pvt->ast_channel.tone_state), &(alsa_input_tone_dtmf_4));
         break;
      }
      case AI_TONE_DTMF_5: {
         alsa_input_tone_state_init(&(pvt->ast_channel.tone_state), &(alsa_input_tone_dtmf_5));
         break;
      }
      case AI_TONE_DTMF_6: {
         alsa_input_tone_state_init(&(pvt->ast_channel.tone_state), &(alsa_input_tone_dtmf_6));
         break;
      }
      case AI_TONE_DTMF_7: {
         alsa_input_tone_state_init(&(pvt->ast_channel.tone_state), &(alsa_input_tone_dtmf_7));
         break;
      }
      case AI_TONE_DTMF_8: {
         alsa_input_tone_state_init(&(pvt->ast_channel.tone_state), &(alsa_input_tone_dtmf_8));
         break;
      }
      case AI_TONE_DTMF_9: {
         alsa_input_tone_state_init(&(pvt->ast_channel.tone_state), &(alsa_input_tone_dtmf_9));
         break;
      }
      case AI_TONE_DTMF_ASTER: {
         alsa_input_tone_state_init(&(pvt->ast_channel.tone_state), &(alsa_input_tone_dtmf_aster));
         break;
      }
      case AI_TONE_DTMF_POUND: {
         alsa_input_tone_state_init(&(pvt->ast_channel.tone_state), &(alsa_input_tone_dtmf_pound));
         break;
      }
      case AI_TONE_DTMF_A: {
         alsa_input_tone_state_init(&(pvt->ast_channel.tone_state), &(alsa_input_tone_dtmf_A));
         break;
      }
      case AI_TONE_DTMF_B: {
         alsa_input_tone_state_init(&(pvt->ast_channel.tone_state), &(alsa_input_tone_dtmf_B));
         break;
      }
      case AI_TONE_DTMF_C: {
         alsa_input_tone_state_init(&(pvt->ast_channel.tone_state), &(alsa_input_tone_dtmf_C));
         break;
      }
      case AI_TONE_DTMF_D: {
         alsa_input_tone_state_init(&(pvt->ast_channel.tone_state), &(alsa_input_tone_dtmf_D));
         break;
      }
      default: {
         alsa_input_assert(false);
         tone = AI_TONE_NONE;
         break;
      }
   }
   pvt->ast_channel.tone = tone;
   pvt->ast_channel.tone_duration_in_bytes = tone_duration * DEFAULT_SAMPLES_PER_MS * SAMPLE_SIZE;
   pvt->ast_channel.tone_bytes_generated = 0;
   if (AI_TONE_NONE == tone) {
      if ((AI_ST_OFF_TALKING != pvt->ast_channel.state)
          && (AI_ST_OFF_WAITING_ANSWER != pvt->ast_channel.state)) {
         alsa_input_stop_card(pvt->ast_channel.snd_playback);
         alsa_input_reset_buf_bytes_not_written(pvt);
      }
      else {
         /*
          * Remove remaining data but take care that we wrote
          * a number of bytes that is a factor of SAMPLE_SIZE
          */
         size_t tmp = pvt->ast_channel.bytes_not_written_len % SAMPLE_SIZE;
         if (tmp > 0) {
            memmove(pvt->ast_channel.bytes_not_written,
               &(pvt->ast_channel.bytes_not_written[pvt->ast_channel.offset_bytes_not_written + pvt->ast_channel.bytes_not_written_len - tmp]), tmp);
         }
         pvt->ast_channel.bytes_not_written_len = tmp;
         pvt->ast_channel.offset_bytes_not_written = 0;
      }
   }
   else {
      alsa_input_write_tone_data(pvt);
   }
}

/* Must be called with monitor.lock locked */
static void alsa_input_new(alsa_input_pvt_t *pvt,
   alsa_input_state_t state, alsa_input_event_t cause,
#if (AST_VERSION <= 110)
   const char *linkedid,
#else /* (AST_VERSION > 110) */
   const struct ast_assigned_ids *assigned_ids,
   const struct ast_channel *requestor,
#endif /* (AST_VERSION > 110) */
   bool *try_to_lock_ast_channel)
{
   enum ast_channel_state ast_state;
   struct ast_channel *tmp = NULL;

   alsa_input_pr_debug("alsa_input_new(state=%d, cause=%d)\n",
      (int)(state), (int)(cause));

   alsa_input_assert((pvt->channel->monitor.lock_count > 0)
                     && (NULL == pvt->owner));

   switch (state) {
      case AI_ST_OFF_WAITING_ANSWER: {
         /*
          We don't set AST_STATE_UP right now, because it makes Asterisk
          believe that the call is already answered : in function
          ast_channel_alloc(), Asterisk call ast_cdr_init() and test if state
          is AST_STATE_UP to pass AST_CDR_ANSWERED or AST_CDR_NOANSWER
         */
         ast_state = AST_STATE_RING;
         alsa_input_assert((cause == AI_EV_OFF_HOOK) || (cause == AI_EV_EXT_FOUND));
         break;
      }
      case AI_ST_ON_PRE_RINGING: {
         ast_state = AST_STATE_DOWN;
         alsa_input_assert(cause == AI_EV_AST_REQUEST);
         break;
      }
      default: {
         alsa_input_assert(false);
         ast_state = AST_STATE_DOWN;
         break;
      }
   }
   alsa_input_assert(pvt->ast_channel.digits_len < ARRAY_LEN(pvt->ast_channel.digits));
   if (pvt->ast_channel.digits_len <= 0) {
      alsa_input_assert(ARRAY_LEN(alsa_input_default_extension) <= ARRAY_LEN(pvt->ast_channel.digits));
      strcpy(pvt->ast_channel.digits, alsa_input_default_extension);
   }
   else {
      pvt->ast_channel.digits[pvt->ast_channel.digits_len] = '\0';
   }
   /*
    Because we provide no file descriptor to poll (for ast_waitfor()), it's
    important to set parameter needqueue to true.
    That way if Asterisk can't create a timer (eg because there's no timing
    interfaces) it will at least create an "alert" pipe whose read end is polled
    in ast_waitfor() and whose write end is written each time we queue a frame
    (in ast_queue_frame()). read end of the pipe is read in ast_read().
    If Asterisk can create a timer and if its name is "timerfd", it will not
    create the "alert" pipe.
    Asterisk uses the timer by setting it in continuous mode in
    ast_queue_frame() and unsetting from continuous mode in ast_read()
    The file descriptor provided by the timer is used in ast_waitfor().
   */
   tmp = ast_channel_alloc(1, ast_state,
      ('\0' != pvt->line_cfg->cid_num[0]) ? pvt->line_cfg->cid_num : NULL,
      ('\0' != pvt->line_cfg->cid_name[0]) ? pvt->line_cfg->cid_name : NULL,
      "" /* acctcode */, pvt->ast_channel.digits, pvt->line_cfg->context,
#if (AST_VERSION <= 110)
      linkedid, 0,
#else /* (AST_VERSION > 110) */
      assigned_ids, requestor, AST_AMA_NONE,
#endif /* (AST_VERSION > 110) */
      "%s/%lu", pvt->channel->chan_tech.type, (unsigned long)(pvt->index_line + 1));
   if (NULL != tmp) {
      bool hangup = false;

      /* From version 12.0, ast_channel_alloc() returned an ast_channel locked */
#if (AST_VERSION <= 110)
      /*
       We can safely lock channel (without fear of a deadlock because
       monitor.lock is locked), for 2 reasons :
       - channel_tech is not set
       - pvt->owner is still NULL
      */
      ast_channel_lock(tmp);
#endif /* (AST_VERSION <= 110) */
#ifdef DEBUG
      pvt->owner_lock_count += 1;
      alsa_input_assert(pvt->owner_lock_count > 0);
#endif /* DEBUG */

      alsa_input_ast_channel_tech_set(tmp, &(pvt->channel->chan_tech));

      alsa_input_ast_channel_nativeformats_set(tmp, alsa_input_get_chan_tech_cap(&(pvt->channel->chan_tech)));
      alsa_input_ast_channel_set_rawreadformat(tmp, ast_format_slin);
      alsa_input_ast_channel_set_rawwriteformat(tmp, ast_format_slin);
      alsa_input_ast_channel_set_readformat(tmp, ast_format_slin);
      alsa_input_ast_channel_set_writeformat(tmp, ast_format_slin);
      /* no need to call ast_setstate: the channel_alloc already did its job */
      alsa_input_ast_channel_exten_set(tmp, pvt->ast_channel.digits);
      if (!ast_strlen_zero(pvt->channel->config.language)) {
         alsa_input_ast_channel_language_set(tmp, pvt->channel->config.language);
      }
      if (AST_STATE_RING == ast_state) {
         alsa_input_ast_channel_rings_set(tmp, 1);
      }
      /* Don't use ast_set_callerid() here because it will
       * generate a NewCallerID event before the NewChannel event */
      if (!ast_strlen_zero(pvt->line_cfg->cid_num)) {
         alsa_input_ast_channel_caller(tmp)->ani.number.valid = 1;
         ast_free(alsa_input_ast_channel_caller(tmp)->ani.number.str);
         alsa_input_ast_channel_caller(tmp)->ani.number.str = ast_strdup(pvt->line_cfg->cid_num);
      }
      ast_jb_configure(tmp, &(pvt->line_cfg->jb_conf));

      alsa_input_ast_channel_tech_pvt_set(tmp, pvt);
      ast_module_ref(ast_module_info->self);
      pvt->owner = tmp;

      /*
       Set internal state before starting channel's thread
      */
      alsa_input_set_new_state(pvt, state, cause);

      /* Unset pvt->owner before unlocking channel (see below) */
      pvt->owner = NULL;
#ifdef DEBUG
      alsa_input_assert(pvt->owner_lock_count > 0);
      pvt->owner_lock_count -= 1;
#endif /* DEBUG */
      ast_channel_unlock(tmp);

      if ((!hangup) && (AST_STATE_DOWN != ast_state) && (ast_pbx_start(tmp))) {
         ast_log(AST_LOG_ERROR, "Unable to start PBX on '%s'\n", alsa_input_ast_channel_name(tmp));
         hangup = true;
      }
      /*
       Here we can still safely lock channel (without fear of a deadlock
       because monitor.lock is locked), as pvt->owner is still NULL
      */
      alsa_input_assert(NULL == pvt->owner);
      ast_channel_lock(tmp);
#ifdef DEBUG
      pvt->owner_lock_count += 1;
      alsa_input_assert(pvt->owner_lock_count > 0);
#endif /* DEBUG */
      pvt->owner = tmp;
      if (hangup) {
         alsa_input_unlink_from_ast_channel(pvt, AI_EV_INTERNAL_ERROR, true);
         alsa_input_assert(NULL == pvt->owner);
         /* ast_channel_unlock(tmp) is done in alsa_input_unlink_from_ast_channel() */
         /*
          Calling ast_hangup() means that we call indirectly
          alsa_input_chan_hangup().
          As the association between the ast_channel and the tech_pvt
          is already removed alsa_input_chan_hangup() will do nothing.
          Of course in alsa_input_chan_hangup(), we must check that the
          ast_channel has a valid tech_pvt
         */
         ast_hangup(tmp);
         tmp = NULL;
      }
      else {
         if ((NULL == try_to_lock_ast_channel) || (!(*try_to_lock_ast_channel))) {
#ifdef DEBUG
            alsa_input_assert(pvt->owner_lock_count > 0);
            pvt->owner_lock_count -= 1;
#endif /* DEBUG */
            ast_channel_unlock(pvt->owner);
         }
      }
   }
   else {
      ast_log(AST_LOG_ERROR, "Unable to allocate ast_channel structure\n");
      if (AST_STATE_DOWN != ast_state) {
         alsa_input_assert(AI_ST_OFF_WAITING_ANSWER == state);
         /* We signals the user that there's a problem */
         alsa_input_set_new_state(pvt, AI_ST_OFF_NO_SERVICE, AI_EV_INTERNAL_ERROR);
      }
      else {
         alsa_input_assert(AI_ST_ON_PRE_RINGING == state);
         alsa_input_set_new_state(pvt, AI_ST_ON_IDLE, AI_EV_INTERNAL_ERROR);
      }
   }
}

/* Must be call with pvt->owner locked. */
static int alsa_input_setup(alsa_input_pvt_t *pvt,
   const alsa_input_ast_format *format, enum ast_channel_state ast_state,
   alsa_input_event_t cause)
{
   int ret = 0;

   alsa_input_pr_debug("alsa_input_setup(ast_state=%d)\n",
      (int)(ast_state));

   alsa_input_assert((NULL != pvt->owner) && (pvt->owner_lock_count > 0));
   do { /* Empty loop */
      if (NULL == format) {
         ast_log(AST_LOG_WARNING, "No format specified\n");
         ret = -1;
         break;
      }
      if (!alsa_input_ast_formats_are_equal(ast_format_slin, format)) {
         ast_log(AST_LOG_WARNING, "Can't do format '%s'\n", alsa_input_ast_format_get_name(format));
         ret = -1;
         break;
      }
      alsa_input_set_new_state(pvt, AI_ST_OFF_TALKING, cause);
      ast_setstate(pvt->owner, ast_state);
   } while (0);

   return (ret);
}

/*
 Must be called with pvt->owner locked.
 If queue_frame is true, return true if a frame has been queued
 If queue_frame is false, return true if a frame is ready in
 pvt->ast_channel.frame
*/
static bool alsa_input_read_data(alsa_input_pvt_t *pvt,
   bool queue_frame, bool monitor_is_locked)
{
   bool ret = false;
   snd_pcm_state_t state;
   snd_pcm_sframes_t read;
   snd_pcm_uframes_t to_read;

   /* alsa_input_pr_debug("alsa_input_read_data()\n"); */
   alsa_input_assert((NULL != pvt->owner) && (pvt->owner_lock_count > 0));
   if (!pvt->ast_channel.snd_capture_muted) {
      to_read = (ARRAY_LEN(pvt->ast_channel.buf_fr_to_queue) - pvt->ast_channel.offset_buf_fr_to_queue);
      for (;;) {
         if (to_read >= SAMPLE_SIZE) {
            state = snd_pcm_state(pvt->ast_channel.snd_capture);
            if ((state != SND_PCM_STATE_PREPARED) && (state != SND_PCM_STATE_RUNNING)) {
               int err = snd_pcm_prepare(pvt->ast_channel.snd_capture);
               if (err) {
                  ast_log(AST_LOG_ERROR, "snd_pcm_prepare() failed: '%s'\n", snd_strerror(err));
               }
            }

            read = snd_pcm_readi(pvt->ast_channel.snd_capture, pvt->ast_channel.buf_fr_to_queue + pvt->ast_channel.offset_buf_fr_to_queue,
               to_read / SAMPLE_SIZE);
            if (read < 0) {
               if (alsa_input_handle_card_error(pvt->ast_channel.snd_capture, read, "snd_pcm_readi")) {
                  /* Critical error */
                  alsa_input_critical_error(pvt, monitor_is_locked);
               }
               break;
            }

            /* Update the number of bytes in the frame */
            pvt->ast_channel.offset_buf_fr_to_queue += (read * SAMPLE_SIZE);
            to_read = (ARRAY_LEN(pvt->ast_channel.buf_fr_to_queue) - pvt->ast_channel.offset_buf_fr_to_queue);
         }
         if (to_read < SAMPLE_SIZE) {
            /* Buffer is full */
            if (queue_frame) {
               pvt->ast_channel.frame_to_queue.data.ptr = pvt->ast_channel.buf_fr_to_queue;
               pvt->ast_channel.frame_to_queue.datalen = pvt->ast_channel.offset_buf_fr_to_queue;
               pvt->ast_channel.frame_to_queue.samples = pvt->ast_channel.frame_to_queue.datalen / SAMPLE_SIZE;
               pvt->ast_channel.frame_to_queue.frametype = AST_FRAME_VOICE;
               alsa_input_ast_set_frame_format(&(pvt->ast_channel.frame_to_queue), ast_format_slin);
               pvt->ast_channel.frame_to_queue.src = alsa_input_chan_type;
               pvt->ast_channel.frame_to_queue.offset = 0;
               pvt->ast_channel.frame_to_queue.mallocd = 0;
               pvt->ast_channel.frame_to_queue.delivery = ast_tv(0,0);
               if (ast_queue_frame(pvt->owner, &(pvt->ast_channel.frame_to_queue))) {
                  ast_frfree(&(pvt->ast_channel.frame_to_queue));
                  ast_log(AST_LOG_WARNING, "Can't queue voice frame for line %lu\n", (unsigned long)(pvt->index_line + 1));
                  break;
               }
               ast_frfree(&(pvt->ast_channel.frame_to_queue));
               ret = true;
               alsa_input_reset_buf_fr_to_queue(pvt);
               to_read = (ARRAY_LEN(pvt->ast_channel.buf_fr_to_queue) - pvt->ast_channel.offset_buf_fr_to_queue);
            }
            else {
               memcpy(&(pvt->ast_channel.buf_fr[AST_FRIENDLY_OFFSET]), pvt->ast_channel.buf_fr_to_queue, pvt->ast_channel.offset_buf_fr_to_queue);
               alsa_input_reset_buf_fr_to_queue(pvt);
               pvt->ast_channel.frame.data.ptr = &(pvt->ast_channel.buf_fr[AST_FRIENDLY_OFFSET]);
               pvt->ast_channel.frame.datalen = pvt->ast_channel.offset_buf_fr_to_queue;
               pvt->ast_channel.frame.samples = pvt->ast_channel.frame.datalen / SAMPLE_SIZE;
               pvt->ast_channel.frame.frametype = AST_FRAME_VOICE;
               alsa_input_ast_set_frame_format(&(pvt->ast_channel.frame), ast_format_slin);
               pvt->ast_channel.frame.src = alsa_input_chan_type;
               pvt->ast_channel.frame.offset = AST_FRIENDLY_OFFSET;
               pvt->ast_channel.frame.mallocd = 0;
               pvt->ast_channel.frame.delivery = ast_tv(0,0);
               ret = true;
               break;
            }
         }
         else {
            break;
         }
      }
   }

   return (ret);
}

static inline void alsa_input_change_monitor_timeout(
   alsa_input_monitor_prms_t *monitor_prms, int timeout)
{
   /* alsa_input_pr_debug("alsa_input_change_monitor_timeout(timeout=%d)\n",
      (int)(timeout)); */

   alsa_input_assert((timeout > 0) && (NULL != monitor_prms));
   if (timeout < monitor_prms->timeout) {
      monitor_prms->timeout = timeout;
   }
}

/*
 Must be called with pvt->owner and monitor.lock locked.
*/
static void alsa_input_handle_status_change(alsa_input_pvt_t *pvt,
   alsa_input_monitor_prms_t *monitor_prms, alsa_input_status_t new_status)
{
   alsa_input_pr_debug("alsa_input_handle_status_change(new_status=%d)\n",
      (int)(new_status));

   alsa_input_assert((pvt->channel->monitor.lock_count > 0)
      && ((NULL == pvt->owner) || (pvt->owner_lock_count > 0))
      && (NULL != monitor_prms) && (monitor_prms->channel_is_locked));

   alsa_input_assert(((AI_STATUS_OFF_HOOK == new_status) || (AI_STATUS_ON_HOOK == new_status))
      && (new_status != pvt->ast_channel.status));

   pvt->ast_channel.status = new_status;

   /* Handle status change */
   if (AI_STATUS_OFF_HOOK == new_status) {
      if ((AI_ST_ON_RINGING == pvt->ast_channel.state)
          || (AI_ST_ON_PRE_RINGING == pvt->ast_channel.state)) {
         alsa_input_assert(NULL != pvt->owner);
         /*
          alsa_input_chan_request() has created the channel.
          Either alsa_input_chan_call() has been called so the phone
          is ringing and the user picked up the phone to answer
          the call or alsa_input_chan_call() has not been called and
          the user picked up the phone to make a call.
          In this latter case there's a conflict, so we hang
          up the channel created by alsa_input_chan_request()
          and emit the tone busy to the user.
         */
         bool hangup = true;
         if (AI_ST_ON_RINGING == pvt->ast_channel.state) {
            alsa_input_assert(AST_STATE_RINGING == alsa_input_ast_channel_state(pvt->owner));
            /* The user wants to answer the call */
            if (alsa_input_setup(pvt, alsa_input_ast_channel_rawreadformat(pvt->owner), AST_STATE_UP, AI_EV_OFF_HOOK)) {
               ast_log(AST_LOG_ERROR, "Unable to answer the call on '%s'\n", alsa_input_ast_channel_name(pvt->owner));
            }
            else {
               if (ast_queue_control(pvt->owner, AST_CONTROL_ANSWER)) {
                  ast_log(AST_LOG_ERROR, "Unable to answer the call on '%s'\n", alsa_input_ast_channel_name(pvt->owner));
               }
               else {
                  alsa_input_pr_debug("Call answered on '%s'\n", alsa_input_ast_channel_name(pvt->owner));
                  hangup = false;
                  alsa_input_change_monitor_timeout(monitor_prms, alsa_input_monitor_busy_period);
               }
            }
         }
         else {
            alsa_input_assert(AI_ST_ON_PRE_RINGING == pvt->ast_channel.state);
            /*
             The user wants to make a call but that's not possible as
             there's an incoming call pending
            */
         }
         if (hangup) {
            alsa_input_queue_hangup(pvt, AI_EV_INTERNAL_ERROR);
            /* ast_channel_unlock(pvt->owner) is done in alsa_input_queue_hangup() */
            alsa_input_assert((NULL == pvt->owner) && (AI_ST_OFF_NO_SERVICE == pvt->ast_channel.state));
         }
      }
      else {
         alsa_input_assert((AI_ST_ON_IDLE == pvt->ast_channel.state)
            && (NULL == pvt->owner));
         /* The user has picked up the phone to make a call */
         if (pvt->line_cfg->monitor_dialing) {
            /*
             We do not create an ast_channel now
             We just change the tone to AI_TONE_WAITING_DIAL, and
             wait for digits dialed by the user.
             When the digits will form a valid extension then we will
             create the ast_channel dedicated to handle the call
            */
            alsa_input_set_new_state(pvt, AI_ST_OFF_DIALING, AI_EV_OFF_HOOK);
         }
         else { /* (!pvt->line_cfg->monitor_dialing) */
            /*
             We create an ast_channel now.
            */
            pvt->ast_channel.digits_len = 0;
            alsa_input_new(pvt, AI_ST_OFF_WAITING_ANSWER, AI_EV_OFF_HOOK,
#if (AST_VERSION > 110)
               NULL,
#endif /* (AST_VERSION > 110) */
               NULL, &(monitor_prms->channel_is_locked));
         }
      }
   }
   else { /* (AI_STATUS_OFF_HOOK != new_status) */
      /* Gone on hook, so notify hangup */
      if (NULL != pvt->owner) {
         alsa_input_queue_hangup(pvt, AI_EV_ON_HOOK);
         /* ast_channel_unlock(pvt->owner) is done in alsa_input_queue_hangup() */
         alsa_input_assert(NULL == pvt->owner);
      }
      else {
         alsa_input_set_new_state(pvt, AI_ST_ON_IDLE, AI_EV_ON_HOOK);
      }
   }
}

/*
 Must be called with pvt->owner and monitor.lock locked.
*/
static void alsa_input_handle_mute_change(alsa_input_pvt_t *pvt,
   alsa_input_monitor_prms_t *monitor_prms)
{
   alsa_input_pr_debug("alsa_input_handle_mute_change()\n");

   alsa_input_assert((pvt->channel->monitor.lock_count > 0)
      && ((NULL == pvt->owner) || (pvt->owner_lock_count > 0))
      && (NULL != monitor_prms) && (monitor_prms->channel_is_locked));

   if ((AI_ST_OFF_TALKING == pvt->ast_channel.state)
       || (AI_ST_OFF_WAITING_ANSWER == pvt->ast_channel.state)) {
      /* Toggle mute */
      if (pvt->ast_channel.snd_capture_muted) {
         alsa_input_pr_debug("Line %lu is unmuted\n",
            (unsigned long)(pvt->index_line + 1));
         pvt->ast_channel.snd_capture_muted = false;
         alsa_input_start_card(pvt->ast_channel.snd_capture);
      }
      else {
         alsa_input_pr_debug("Line %lu is muted\n",
            (unsigned long)(pvt->index_line + 1));
         pvt->ast_channel.snd_capture_muted = true;
         alsa_input_stop_card(pvt->ast_channel.snd_capture);
      }
      pvt->monitor.last_known_snd_capture_muted = pvt->ast_channel.snd_capture_muted;
      alsa_input_reset_buf_fr_to_queue(pvt);
   }
}

/*
 Must be called with monitor.lock locked and pvt->owner set to NULL.
*/
static void alsa_input_search_extension(alsa_input_pvt_t *pvt,
   alsa_input_monitor_prms_t *monitor_prms, bool ignore_timeout)
{
   /* alsa_input_pr_debug("alsa_input_search_extension(ignore_timeout=%d)\n",
      (int)(ignore_timeout)); */

   alsa_input_assert((pvt->channel->monitor.lock_count > 0)
      && (NULL == pvt->owner) && (pvt->line_cfg->monitor_dialing)
      && (NULL != monitor_prms) && (monitor_prms->channel_is_locked)
      && (AI_ST_OFF_DIALING == pvt->ast_channel.state));

   do { /* Empty loop */
      if (!pvt->ast_channel.search_extension) {
         break;
      }
      alsa_input_assert(pvt->ast_channel.digits_len < ARRAY_LEN(pvt->ast_channel.digits));
      if (pvt->ast_channel.digits_len <= 0) {
         if ((!ignore_timeout) && (pvt->line_cfg->dialing_timeout_1st_digit > 0)) {
            struct timeval now = ast_tvnow();
            int64_t tvdiff = ast_tvdiff_ms(now, pvt->ast_channel.tv_wait);
            if (tvdiff < pvt->line_cfg->dialing_timeout_1st_digit) {
               alsa_input_change_monitor_timeout(monitor_prms, (int)(pvt->line_cfg->dialing_timeout_1st_digit - tvdiff));
               break;
            }
         }
         alsa_input_assert(ARRAY_LEN(alsa_input_default_extension) <= ARRAY_LEN(pvt->ast_channel.digits));
         strcpy(pvt->ast_channel.digits, alsa_input_default_extension);
      }
      else {
         if ((!ignore_timeout) && (pvt->line_cfg->dialing_timeout > 0)) {
            struct timeval now = ast_tvnow();
            int64_t tvdiff = ast_tvdiff_ms(now, pvt->ast_channel.tv_wait);
            if (tvdiff < pvt->line_cfg->dialing_timeout) {
               alsa_input_change_monitor_timeout(monitor_prms, (int)(pvt->line_cfg->dialing_timeout - tvdiff));
               break;
            }
         }
         pvt->ast_channel.digits[pvt->ast_channel.digits_len] = '\0';
      }
      pvt->ast_channel.search_extension = false;
      /* We test if, with these numbers, the extension is valid */
      alsa_input_pr_debug("Searching for extension '%s' in context '%s'\n",
         pvt->ast_channel.digits, pvt->line_cfg->context);
      /*
       Reset flag search_extension to false to stop
       searching for an extension while no new digit is
       dialed
      */
      if (ast_exists_extension(NULL, pvt->line_cfg->context, pvt->ast_channel.digits, 1, pvt->line_cfg->cid_num)) {
         /* It's a valid extension in its context, get moving! */
         alsa_input_pr_debug("Extension '%s' found in context '%s'\n", pvt->ast_channel.digits, pvt->line_cfg->context);
         alsa_input_new(pvt, AI_ST_OFF_WAITING_ANSWER, AI_EV_EXT_FOUND,
#if (AST_VERSION > 110)
            NULL,
#endif /* (AST_VERSION > 110) */
            NULL, &(monitor_prms->channel_is_locked));
      }
      else {
         if (!ast_canmatch_extension(NULL, pvt->line_cfg->context, pvt->ast_channel.digits, 1, pvt->line_cfg->cid_num)) {
            /* It's not a valid extension */
            alsa_input_pr_debug("Extension '%s' can't match anything in '%s'\n",
               pvt->ast_channel.digits, pvt->line_cfg->context);
            /* We signals the user that there's a problem */
            alsa_input_set_new_state(pvt, AI_ST_OFF_NO_SERVICE, AI_EV_NO_EXT_CAN_BE_FOUND);
         }
         else if (pvt->ast_channel.digits_len >= (ARRAY_LEN(pvt->ast_channel.digits) - 1)) {
            /*
             We overrun maximum length of extension without
             matching anything
            */
            alsa_input_pr_debug("Extension '%s' can't match anything in '%s' and reaches maximum length\n",
               pvt->ast_channel.digits, pvt->line_cfg->context);
            /* We signals the user that there's a problem */
            alsa_input_set_new_state(pvt, AI_ST_OFF_NO_SERVICE, AI_EV_NO_EXT_CAN_BE_FOUND);
         }
      }
   } while (false);
}

/*
 Must be called with pvt->owner and monitor.lock locked.
*/
static void alsa_input_try_to_send_dtmf(alsa_input_pvt_t *pvt,
   alsa_input_monitor_prms_t *monitor_prms)
{
   bool send_a_null_frame = false;

   alsa_input_assert((pvt->channel->monitor.lock_count > 0)
      && (NULL != pvt->owner) && (pvt->owner_lock_count > 0)
      && (NULL != monitor_prms) && (monitor_prms->channel_is_locked)
      && ((AI_ST_OFF_TALKING == pvt->ast_channel.state)
          || (AI_ST_OFF_WAITING_ANSWER == pvt->ast_channel.state)));

   if (NONE != pvt->ast_channel.dtmf_sent) {
      struct timeval now = ast_tvnow();
      int64_t tvdiff = ast_tvdiff_ms(now, pvt->ast_channel.tv_wait);
      if (ON == pvt->ast_channel.dtmf_sent) {
         if (tvdiff < MIN_DTMF_DURATION) {
            alsa_input_change_monitor_timeout(monitor_prms, (int)(MIN_DTMF_DURATION - tvdiff));
         }
         else {
            pvt->ast_channel.dtmf_sent = OFF;
            pvt->ast_channel.tv_wait = ast_tvnow();
            alsa_input_change_monitor_timeout(monitor_prms, MIN_TIME_BETWEEN_DTMF);
            send_a_null_frame = true;
         }
      }
      else {
         alsa_input_assert(OFF == pvt->ast_channel.dtmf_sent);
         if (tvdiff < MIN_TIME_BETWEEN_DTMF) {
            alsa_input_change_monitor_timeout(monitor_prms, (int)(MIN_TIME_BETWEEN_DTMF - tvdiff));
         }
         else {
            pvt->ast_channel.dtmf_sent = NONE;
            send_a_null_frame = true;
         }
      }
   }
   if (send_a_null_frame) {
      /*
       As we only send AST_FRAME_DTMF, we send a null frame
       for Asterisk to properly generate AST_FRAME_DTMF_BEGIN
       and AST_FRAME_DTMF_END (it does it only when processing
       a frame)
      */
      if (ast_queue_frame(pvt->owner, &(ast_null_frame))) {
         ast_log(AST_LOG_WARNING, "Can't queue null frame for line %lu\n", (unsigned long)(pvt->index_line + 1));
      }
   }
   if ((NONE == pvt->ast_channel.dtmf_sent) && (pvt->ast_channel.digits_len > 0)) {
      int ret;
      pvt->ast_channel.frame_to_queue.datalen = 0;
      pvt->ast_channel.frame_to_queue.samples = 0;
      pvt->ast_channel.frame_to_queue.data.ptr =  NULL;
      pvt->ast_channel.frame_to_queue.src = alsa_input_chan_type;
      pvt->ast_channel.frame_to_queue.offset = 0;
      pvt->ast_channel.frame_to_queue.mallocd = 0;
      pvt->ast_channel.frame_to_queue.delivery = ast_tv(0,0);
      pvt->ast_channel.frame_to_queue.frametype = AST_FRAME_DTMF;
      pvt->ast_channel.frame_to_queue.subclass.integer = pvt->ast_channel.digits[0];
      ret = ast_queue_frame(pvt->owner, &(pvt->ast_channel.frame_to_queue));
      ast_frfree(&(pvt->ast_channel.frame_to_queue));
      if (ret) {
         ast_log(AST_LOG_WARNING, "Unable to queue a digit on line %lu\n", (unsigned long)(pvt->index_line + 1));
      }
      else {
         alsa_input_pr_debug("DTMF '%c' sent\n", (int)(pvt->ast_channel.frame_to_queue.subclass.integer));
         pvt->ast_channel.dtmf_sent = ON;
         pvt->ast_channel.tv_wait = ast_tvnow();
         alsa_input_change_monitor_timeout(monitor_prms, MIN_DTMF_DURATION);
         pvt->ast_channel.digits_len -= 1;
         if (pvt->ast_channel.digits_len > 0) {
            memmove(pvt->ast_channel.digits, &(pvt->ast_channel.digits[1]), pvt->ast_channel.digits_len);
         }
      }
   }
}

/*
 Must be called with pvt->owner and monitor.lock locked.
*/
static void alsa_input_handle_digit(alsa_input_pvt_t *pvt,
   alsa_input_monitor_prms_t *monitor_prms, char digit)
{
   alsa_input_pr_debug("alsa_input_handle_digits(digit='%c')\n",
      (char)(digit));

   alsa_input_assert((pvt->channel->monitor.lock_count > 0)
      && ((NULL == pvt->owner) || (pvt->owner_lock_count > 0))
      && (NULL != monitor_prms) && (monitor_prms->channel_is_locked));

   if ((AI_ST_OFF_TALKING == pvt->ast_channel.state)
       || (AI_ST_OFF_WAITING_ANSWER == pvt->ast_channel.state)) {
      alsa_input_assert(NULL != pvt->owner);
      if (pvt->ast_channel.digits_len < ARRAY_LEN(pvt->ast_channel.digits)) {
         pvt->ast_channel.digits[pvt->ast_channel.digits_len] = digit;
         pvt->ast_channel.digits_len += 1;
      }
   }
   else if (AI_ST_OFF_DIALING == pvt->ast_channel.state) {
      alsa_input_assert((NULL == pvt->owner) && (pvt->line_cfg->monitor_dialing));
      do { /* Empty loop */
         /* We search for an extension before adding new digit */
         alsa_input_search_extension(pvt, monitor_prms, false);
         if ((AI_ST_OFF_DIALING != pvt->ast_channel.state) || (!monitor_prms->channel_is_locked)) {
            break;
         }
         if (pvt->ast_channel.digits_len < (ARRAY_LEN(pvt->ast_channel.digits) - 1)) {
            if (0 == pvt->ast_channel.digits_len) {
               /* We switch off the AI_TONE_WAITING_DIAL tone */
               alsa_input_set_line_tone(pvt, AI_TONE_NONE, 0);
            }
            /*
             If the digit is equal to pvt->line_cfg->search_extension_trigger,
             we search for an extension now, without adding the digit
            */
            alsa_input_assert('\0' != digit);
            if (/* ('\0' != pvt->line_cfg->search_extension_trigger)
                && */(digit == pvt->line_cfg->search_extension_trigger)) {
               pvt->ast_channel.search_extension = true;
               alsa_input_search_extension(pvt, monitor_prms, true);
               alsa_input_assert(!pvt->ast_channel.search_extension);
               if ((AI_ST_OFF_DIALING != pvt->ast_channel.state) || (!monitor_prms->channel_is_locked)) {
                  break;
               }
            }
            pvt->ast_channel.digits[pvt->ast_channel.digits_len] = digit;
            pvt->ast_channel.digits_len += 1;
            /* If there's no timeout, we search for an extension now */
            if (pvt->line_cfg->dialing_timeout <= 0) {
               pvt->ast_channel.search_extension = true;
               alsa_input_search_extension(pvt, monitor_prms, true);
               alsa_input_assert(!pvt->ast_channel.search_extension);
               if ((AI_ST_OFF_DIALING != pvt->ast_channel.state) || (!monitor_prms->channel_is_locked)) {
                  break;
               }
            }
            else {
               /*
                We set the flag saying we can search for an extension
                and we update the time last digit was dialed
               */
               pvt->ast_channel.search_extension = true;
               pvt->ast_channel.tv_wait = ast_tvnow();
               alsa_input_change_monitor_timeout(monitor_prms, pvt->line_cfg->dialing_timeout);
            }
         }
      } while (false);
   }
}

/*
 Must be called with pvt->owner and monitor.lock locked.
*/
static void alsa_input_monitor_pvt(alsa_input_pvt_t *pvt,
   alsa_input_monitor_prms_t *monitor_prms)
{
   /* alsa_input_pr_debug("alsa_input_monitor_pvt()\n"); */

   alsa_input_assert((pvt->channel->monitor.lock_count > 0)
      && ((NULL == pvt->owner) || (pvt->owner_lock_count > 0))
      && (NULL != monitor_prms) && (monitor_prms->channel_is_locked));

   if ((AI_ST_OFF_TALKING == pvt->ast_channel.state)
       || (AI_ST_OFF_WAITING_ANSWER == pvt->ast_channel.state)) {
      alsa_input_assert(NULL != pvt->owner);
      alsa_input_try_to_send_dtmf(pvt, monitor_prms);
      if (monitor_prms->channel_is_locked) {
         /* We read as much data as possible coming from the driver
          and queue ast_frame */
         alsa_input_read_data(pvt, true, true);
         alsa_input_change_monitor_timeout(monitor_prms, alsa_input_monitor_busy_period);
      }
   }
   else if (AI_ST_OFF_DIALING == pvt->ast_channel.state) {
      alsa_input_assert((NULL == pvt->owner) && (pvt->line_cfg->monitor_dialing));
      alsa_input_search_extension(pvt, monitor_prms, false);
   }
   else if (AI_ST_OFF_NO_SERVICE == pvt->ast_channel.state) {
      struct timeval now = ast_tvnow();
      int64_t tvdiff = ast_tvdiff_ms(now, pvt->ast_channel.tv_wait);
      if (tvdiff < DELAY_AUTO_HOOK_ON) {
         alsa_input_change_monitor_timeout(monitor_prms, (int)(DELAY_AUTO_HOOK_ON - tvdiff));
      }
      else {
         alsa_input_handle_status_change(pvt, monitor_prms, AI_STATUS_ON_HOOK);
      }
   }
   else if (AI_ST_ON_RINGING == pvt->ast_channel.state) {
      struct timeval now = ast_tvnow();
      int64_t tvdiff = ast_tvdiff_ms(now, pvt->ast_channel.tv_wait);
      if (pvt->ast_channel.buzzer_is_on) {
         if (tvdiff < RING_CADENCE_ON) {
            alsa_input_change_monitor_timeout(monitor_prms, (int)(RING_CADENCE_ON - tvdiff));
         }
         else {
            alsa_input_turn_buzzer_off(pvt);
            alsa_input_assert(!pvt->ast_channel.buzzer_is_on);
            alsa_input_change_monitor_timeout(monitor_prms, RING_CADENCE_OFF);
         }
      }
      else {
         if (tvdiff < RING_CADENCE_OFF) {
            alsa_input_change_monitor_timeout(monitor_prms, (int)(RING_CADENCE_OFF - tvdiff));
         }
         else {
            alsa_input_turn_buzzer_on(pvt);
            alsa_input_assert(pvt->ast_channel.buzzer_is_on);
            alsa_input_change_monitor_timeout(monitor_prms, RING_CADENCE_ON);
         }
      }
   }
   if (AI_TONE_NONE != pvt->ast_channel.tone) {
      alsa_input_write_tone_data(pvt);
      alsa_input_change_monitor_timeout(monitor_prms, alsa_input_monitor_busy_period);
   }
}

static inline void alsa_input_monitor_lock(alsa_input_chan_t *t)
{
   ast_mutex_lock(&(t->monitor.lock));
#ifdef DEBUG
   alsa_input_assert(0 == t->monitor.lock_count);
   t->monitor.lock_count += 1;
#endif /* DEBUG */
}

static inline void alsa_input_monitor_unlock(alsa_input_chan_t *t)
{
#ifdef DEBUG
   t->monitor.lock_count -= 1;
   alsa_input_assert(0 == t->monitor.lock_count);
#endif /* DEBUG */
   ast_mutex_unlock(&(t->monitor.lock));
}

static inline void alsa_input_prepare_start_monitor(alsa_input_chan_t *t)
{
   alsa_input_assert(!t->monitor.run);
   t->monitor.run = true;
}

static inline void alsa_input_prepare_stop_monitor(alsa_input_chan_t *t)
{
   t->monitor.run = false;
}

static void *alsa_input_do_monitor(void *data)
{
   alsa_input_chan_t *t = (alsa_input_chan_t *)(data);
   alsa_input_monitor_prms_t monitor_prms;
#ifdef DEBUG
   /* static int last_timeout; */
#endif /* DEBUG */

   monitor_prms.timeout = alsa_input_monitor_short_timeout;
#ifdef DEBUG
   /* last_timeout = monitor_prms.timeout; */
#endif /* DEBUG */

   alsa_input_pr_debug("Entering monitor's thread\n");

   while (t->monitor.run) {
      struct pollfd fds[2 * MAX_LINES];
      size_t fds_len;
      size_t pvt_count;
      alsa_input_pvt_t *pvt;

      alsa_input_monitor_lock(t);
      /* Initialize poll descriptors */
      memset(fds, 0, sizeof(fds));
      fds_len = 0;
      pvt_count = 0;
      AST_LIST_TRAVERSE(&(t->pvt_list), pvt, list) {
         int fd_input;
         int fd_icard;
         if (AI_ST_DISCONNECTED != pvt->monitor.last_known_state) {
            pvt_count += 1;
            fd_input = pvt->monitor.fd_input;
            /* Wait for data on sound input device only in conversation mode and if not muted */
            if (!pvt->monitor.last_known_snd_capture_muted) {
               alsa_input_assert((AI_ST_OFF_TALKING == pvt->monitor.last_known_state)
                  || (AI_ST_OFF_WAITING_ANSWER == pvt->monitor.last_known_state));
               fd_icard = pvt->monitor.fd_snd_capture;
            }
            else {
               fd_icard = -1;
            }
         }
         else {
            fd_input = -1;
            fd_icard = -1;
         }
         fds[fds_len].fd = fd_input;
         fds[fds_len].events = POLLIN;
         fds_len += 1;
         fds[fds_len].fd = fd_icard;
         fds[fds_len].events = POLLIN;
         fds_len += 1;
      }
      if (pvt_count <= 0) {
         /* No connected lines, so we exit the loop and stop the monitor */
         alsa_input_prepare_stop_monitor(t);
         alsa_input_monitor_unlock(t);
         break;
      }
      alsa_input_monitor_unlock(t);

      /* Wait for data on one of the file descriptors */
#ifdef DEBUG
      /*
      if (monitor_prms.timeout != last_timeout) {
         alsa_input_pr_debug("Monitor timeout changed from %d to %d\n",
            (int)(last_timeout), (int)(monitor_prms.timeout));
         last_timeout = monitor_prms.timeout;
      }
      */
#endif /* DEBUG */
      poll(fds, fds_len, monitor_prms.timeout);

      monitor_prms.timeout = alsa_input_monitor_idle_timeout;

      alsa_input_monitor_lock(t);
      pvt_count = 0;
      AST_LIST_TRAVERSE(&(t->pvt_list), pvt, list) {
         ssize_t rb;
         size_t events_count;
         size_t y;

         pvt_count += 1;

         /* If line is disconnected ignore it */
         if (AI_ST_DISCONNECTED == pvt->monitor.last_known_state) {
            continue;
         }

         if (NULL != pvt->owner) {
            if (ast_channel_trylock(pvt->owner)) {
               /*
                Because the channel is locked in another thread,
                we can't handle input events or read data.
                The only thing we can do is set a short timeout to retry
                quickly
               */
               alsa_input_change_monitor_timeout(&(monitor_prms), alsa_input_monitor_short_timeout);
               continue;
            }
#ifdef DEBUG
            alsa_input_assert(0 == pvt->owner_lock_count);
            pvt->owner_lock_count += 1;
#endif /* DEBUG */
         }
         monitor_prms.channel_is_locked = true;

         /* Update monitor status if changes occured in other threads */
         if (pvt->monitor.last_known_state != pvt->ast_channel.state) {
            pvt->monitor.last_known_state = pvt->ast_channel.state;
            if (AI_ST_DISCONNECTED == pvt->ast_channel.state) {
               alsa_input_disconnect_line(pvt);
               alsa_input_assert(NULL == pvt->owner);
            }
         }
         pvt->monitor.last_known_snd_capture_muted = pvt->ast_channel.snd_capture_muted;

         if (AI_ST_DISCONNECTED == pvt->monitor.last_known_state) {
            continue;
         }

         /*
          We call poll() with POLLIN only, but poll() can return
          (POLLERR | POLLHUP | POLLNVAL), so handle these events
         */
         {
            struct pollfd *pfds = &(fds[(pvt_count - 1) * 2]);
            if ((pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL))) {
               alsa_input_pr_debug("Line %lu : poll() returned an error for input event device (revents == %u)\n",
                  (unsigned long)(pvt->index_line + 1), (unsigned int)(pfds[0].revents));
               alsa_input_critical_error(pvt, true);
               alsa_input_assert((NULL == pvt->owner)
                  && (AI_ST_DISCONNECTED == pvt->ast_channel.state)
                  && (AI_ST_DISCONNECTED == pvt->monitor.last_known_state));
               continue;
            }
            if ((pfds[0].revents & POLLIN)) {
               if (pvt->monitor.fd_pipe >= 0) {
                  /*
                   If fd_input is connected to the pipe, input events
                   are already in array pvt->monitor.events
                   Just call read() to handle POLLIN event but ignore the data
                  */
                  __u8 dummy[64];
                  read(pvt->monitor.fd_input, dummy, sizeof(dummy));
               }
               else {
                  rb = read(pvt->monitor.fd_input, (__u8 *)(pvt->monitor.events) + pvt->monitor.events_len_in_bytes, sizeof(pvt->monitor.events) - pvt->monitor.events_len_in_bytes);
                  if (rb > 0) {
                     pvt->monitor.events_len_in_bytes += rb;
                  }
               }
            }
            /*
             Sound input device can return POLLERR after call of snd_pcm_drop()
             (when leaving state AI_ST_OFF_TALKING or AI_ST_OFF_WAITING_ANSWER, or muting capture)
             so handle (POLLERR | POLLHUP | POLLNVAL) only if condition
             to poll sound input device is still valid
            */
            if (!pvt->monitor.last_known_snd_capture_muted) {
               unsigned short revents;
               int err = snd_pcm_poll_descriptors_revents(pvt->ast_channel.snd_capture, &(pfds[1]), 1, &(revents));
               if (err) {
                  ast_log(AST_LOG_ERROR, "snd_pcm_poll_descriptors_revents() failed: '%s'\n", snd_strerror(err));
                  revents = POLLERR;
               }
               if ((revents & (POLLERR | POLLHUP | POLLNVAL))) {
                  alsa_input_pr_debug("Line %lu : poll() returned an error for sound input device (revents == %u)\n",
                     (unsigned long)(pvt->index_line + 1), (unsigned int)(revents));
                  alsa_input_critical_error(pvt, true);
                  alsa_input_assert((NULL == pvt->owner)
                     && (AI_ST_DISCONNECTED == pvt->ast_channel.state)
                     && (AI_ST_DISCONNECTED == pvt->monitor.last_known_state));
                  continue;
               }
            }
         }

         alsa_input_assert(monitor_prms.channel_is_locked);

         /* *** Handle input events received *** */
         events_count = pvt->monitor.events_len_in_bytes / sizeof(pvt->monitor.events[0]);
         y = 0;
         for (y = 0; ((y < events_count) && (monitor_prms.channel_is_locked)); y += 1) {
            char digit = '\0';

            /* alsa_input_pr_debug("Line %lu : event received (type=%u, code=%u, value=%ld)\n",
               (unsigned long)(pvt->index_line + 1), (unsigned int)(pvt->monitor.events[y].type),
               (unsigned int)(pvt->monitor.events[y].code), (long)(pvt->monitor.events[y].value)); */
            if ((EV_KEY != pvt->monitor.events[y].type) || (0 == pvt->monitor.events[y].value)) {
               /* alsa_input_pr_debug("Line %lu : event ignored (type or value not handled)\n",
                  (unsigned long)(pvt->index_line + 1)); */
               continue;
            }
            if (KEY_ENTER == pvt->monitor.events[y].code) {
               /* Off hook */
               alsa_input_pr_debug("Line %lu : key 'off hook' pressed\n",
                  (unsigned long)(pvt->index_line + 1));
               if (AI_STATUS_ON_HOOK == pvt->ast_channel.status) {
                  alsa_input_handle_status_change(pvt, &(monitor_prms), AI_STATUS_OFF_HOOK);
               }
               else {
                  alsa_input_pr_debug("Line %lu : event ignored because phone is already off hook\n",
                        (unsigned long)(pvt->index_line + 1));
               }
               continue;
            }
            if (AI_STATUS_OFF_HOOK != pvt->ast_channel.status) {
               /* When on hook no other key than KEY_ENTER can trigger an action */
               alsa_input_pr_debug("Line %lu : event ignored because phone is on hook\n",
                     (unsigned long)(pvt->index_line + 1));
               continue;
            }
            if (KEY_ESC == pvt->monitor.events[y].code) {
               alsa_input_pr_debug("Line %lu : key 'on hook' pressed\n",
                     (unsigned long)(pvt->index_line + 1));
               alsa_input_handle_status_change(pvt, &(monitor_prms), AI_STATUS_ON_HOOK);
               continue;
            }
            else if (KEY_MUTE == pvt->monitor.events[y].code) {
               alsa_input_pr_debug("Line %lu : key 'mute' pressed\n",
                     (unsigned long)(pvt->index_line + 1));
               alsa_input_handle_mute_change(pvt, &(monitor_prms));
               continue;
            }
            else if ((pvt->monitor.events[y].code >= KEY_NUMERIC_0) && (pvt->monitor.events[y].code <= KEY_NUMERIC_9)) {
               digit = (pvt->monitor.events[y].code - KEY_NUMERIC_0) + '0';
            }
            else if (KEY_NUMERIC_STAR == pvt->monitor.events[y].code) {
               /* Star key */
               digit = '*';
            }
            else if (KEY_NUMERIC_POUND == pvt->monitor.events[y].code) {
               /* Pound key */
               digit = '#';
            }
            else if (KEY_A == pvt->monitor.events[y].code) {
               digit = 'A';
            }
            else if (KEY_B == pvt->monitor.events[y].code) {
               digit = 'B';
            }
            else if (KEY_C == pvt->monitor.events[y].code) {
               digit = 'C';
            }
            else if (KEY_D == pvt->monitor.events[y].code) {
               digit = 'D';
            }
            if ('\0' == digit) {
               alsa_input_pr_debug("Line %lu : event ignored (code %u not handled)\n",
                  (unsigned long)(pvt->index_line + 1), (unsigned int)(pvt->monitor.events[y].code));
               continue;
            }
            alsa_input_pr_debug("Line %lu : key '%c' pressed\n",
                  (unsigned long)(pvt->index_line + 1), (char)(digit));
            alsa_input_handle_digit(pvt, &(monitor_prms), digit);
         }
         if (y > 0) {
            /* Remove handled events */
            pvt->monitor.events_len_in_bytes -= (y * sizeof(pvt->monitor.events[0]));
            if (pvt->monitor.events_len_in_bytes > 0) {
               memmove(pvt->monitor.events, &(pvt->monitor.events[y]), pvt->monitor.events_len_in_bytes);
            }
         }
         if (!monitor_prms.channel_is_locked) {
            continue;
         }

         /* Do periodic tasks */
         alsa_input_monitor_pvt(pvt, &(monitor_prms));
         if (!monitor_prms.channel_is_locked) {
            continue;
         }

         if (NULL != pvt->owner) {
#ifdef DEBUG
            pvt->owner_lock_count -= 1;
            alsa_input_assert(0 == pvt->owner_lock_count);
#endif /* DEBUG */
            ast_channel_unlock(pvt->owner);
         }
         monitor_prms.channel_is_locked = false;
      }
      alsa_input_monitor_unlock(t);
   }

   alsa_input_pr_debug("Exiting monitor's thread\n");

   return (NULL);
}

static int alsa_input_stop_monitor(alsa_input_chan_t *t)
{
   int ret = 0;

   alsa_input_pr_debug("Stopping the monitor\n");

   do { /* Empty loop */
      if (t->monitor.thread == pthread_self()) {
         ast_log(AST_LOG_ERROR, "Cannot kill myself\n");
         ret = -1;
         break;
      }
      if ((AST_PTHREADT_NULL != t->monitor.thread) && (AST_PTHREADT_STOP != t->monitor.thread)) {
         /* Stops the monitor thread */
         alsa_input_prepare_stop_monitor(t);
         /*
          Don't send signal SIGURG with pthread_kill() because
          poll() called by monitor thread,
          if interrupted by signal, could end returning -ERESTARTSYS.
          As the signal handler in Asterisk has the flags SA_RESTART,
          the syscall would be retried and the monitor will stay
          blocked on poll()
         */
         alsa_input_pr_debug("Calling pthread_join()\n");
         ret = pthread_join(t->monitor.thread, NULL);
         if (ret) {
            ast_log(AST_LOG_ERROR, "pthread_join() failed: %d, %d\n", ret, errno);
         }
         else {
            alsa_input_pr_debug("Monitor stopped\n");
         }
         t->monitor.thread = AST_PTHREADT_NULL;
         ret = 0;
      }
      else {
         alsa_input_pr_debug("Monitor not running\n");
      }
   } while (false);

   return (ret);
}

static int alsa_input_start_monitor(alsa_input_chan_t *t)
{
   int ret = 0;

   alsa_input_pr_debug("Starting the monitor\n");

   do { /* Empty loop */
      /* If we're supposed to be stopped -- stay stopped */
      if (AST_PTHREADT_STOP == t->monitor.thread) {
         ret = 0;
         break;
      }
      alsa_input_assert((AST_PTHREADT_NULL == t->monitor.thread) && (!t->monitor.run));
      alsa_input_prepare_start_monitor(t);
      /* Start a new monitor */
      if (ast_pthread_create_background(&(t->monitor.thread), NULL, alsa_input_do_monitor, t) < 0) {
         ast_log(AST_LOG_ERROR, "Unable to start monitor thread.\n");
         t->monitor.thread = AST_PTHREADT_NULL;
         ret = -1;
         break;
      }
      alsa_input_assert((AST_PTHREADT_STOP != t->monitor.thread) && (AST_PTHREADT_NULL != t->monitor.thread));
      ret = 0;
   } while (false);

   return (ret);
}

static inline alsa_input_pvt_t *alsa_input_get_pvt(struct ast_channel *ast)
{
   alsa_input_pvt_t *pvt = alsa_input_ast_channel_tech_pvt(ast);

   if ((NULL == pvt) || (pvt->owner != ast)) {
      ast_log(AST_LOG_WARNING, "Channel '%s' unlink or link to another line\n", alsa_input_ast_channel_name(ast));
      pvt = NULL;
   }
   return (pvt);
}

/*!
 * \brief Make a call
 * \note The channel is locked when this function gets called.
 * \param ast which channel to make the call on
 * \param addr destination of the call
 * \param timeout time to wait on for connect (Doesn't seem to be used.)
 * \retval 0 on success
 * \retval -1 on failure
 */
static int alsa_input_chan_call(struct ast_channel *ast,
#if (AST_VERSION < 110)
   char *addr,
#else /* (AST_VERSION >= 110) */
   const char *addr,
#endif /* (AST_VERSION >= 110)*/
   int timeout)
{
   int ret = -1;
   alsa_input_pvt_t *pvt = alsa_input_get_pvt(ast);

   alsa_input_pr_debug("alsa_input_chan_call(ast='%s', addr='%s', timeout=%d)\n", alsa_input_ast_channel_name(ast), addr, (int)(timeout));

   if (NULL != pvt) {
#ifdef DEBUG
      pvt->owner_lock_count += 1;
      alsa_input_assert(pvt->owner_lock_count > 0);
#endif /* DEBUG */
      do { /* Empty loop */
         /* If phone is off hook, call() is not allowed */
         if (AI_STATUS_ON_HOOK != pvt->ast_channel.status) {
            ast_log(AST_LOG_NOTICE, "'%s' is busy\n", alsa_input_ast_channel_name(ast));
            ast_setstate(ast, AST_STATE_BUSY);
            ast_queue_control(ast, AST_CONTROL_BUSY);
         }
         else {
            char number[20];
            char name[80];
            enum ast_channel_state ast_state = alsa_input_ast_channel_state(ast);

            if ((AST_STATE_DOWN != ast_state) && (AST_STATE_RESERVED != ast_state)) {
               ast_log(AST_LOG_WARNING, "alsa_input_chan_call() called on '%s', neither down nor reserved\n",
                  alsa_input_ast_channel_name(ast));
               break;
            }

            /* the standard format of ast->callerid is:  "name" <number>, but not always complete */
            if ((!alsa_input_ast_channel_connected(ast)->id.name.valid)
                || (ast_strlen_zero(alsa_input_ast_channel_connected(ast)->id.name.str))) {
               strcpy(name, "unknown");
            }
            else {
               ast_copy_string(name, alsa_input_ast_channel_connected(ast)->id.name.str, sizeof(name));
            }

            number[0] = '\0';
            if ((alsa_input_ast_channel_connected(ast)->id.number.valid) && (alsa_input_ast_channel_connected(ast)->id.number.str)) {
               ast_copy_string(number, alsa_input_ast_channel_connected(ast)->id.number.str, sizeof(number));
            }

            alsa_input_pr_debug("Ringing '%s' on '%s' (with CID '%s', '%s')\n",
               addr, alsa_input_ast_channel_name(ast), number, name);

            ast_setstate(ast, AST_STATE_RINGING);
            ast_queue_control(ast, AST_CONTROL_RINGING);
            alsa_input_set_new_state(pvt, AI_ST_ON_RINGING, AI_EV_AST_CALL);
            ret = 0;
         }
      } while (false);
#ifdef DEBUG
      alsa_input_assert(pvt->owner_lock_count > 0);
      pvt->owner_lock_count -= 1;
#endif /* DEBUG */
   }

   return (ret);
}

/*!
 * \brief Answer the channel
 *
 * \note The channel is locked when this function gets called.
 */
static int alsa_input_chan_answer(struct ast_channel *ast)
{
   int ret = -1;
   alsa_input_pvt_t *pvt = alsa_input_get_pvt(ast);

   /* Remote end has answered the call */
   alsa_input_pr_debug("alsa_input_chan_answer(ast='%s')\n", alsa_input_ast_channel_name(ast));

   if (NULL != pvt) {
#ifdef DEBUG
      pvt->owner_lock_count += 1;
      alsa_input_assert(pvt->owner_lock_count > 0);
#endif /* DEBUG */
      /* If phone not off hook, answer() is not allowed */
      if ((AI_ST_OFF_TALKING != pvt->ast_channel.state)
          && (AI_ST_OFF_NO_SERVICE != pvt->ast_channel.state)) {
         ast_log(AST_LOG_WARNING, "Channel '%s' can't answer now, because not off hook or not in the correct state\n", alsa_input_ast_channel_name(ast));
      }
      else {
         /* We accept that answer() can be called if AI_ST_OFF_TALKING == pvt->ast_channel.state */
         if (AI_ST_OFF_WAITING_ANSWER == pvt->ast_channel.state) {
            ret = alsa_input_setup(pvt, alsa_input_ast_channel_rawreadformat(pvt->owner), AST_STATE_UP, AI_EV_AST_ANSWER);
            if (!ret) {
               alsa_input_ast_channel_rings_set(pvt->owner, 0);
            }
         }
      }
#ifdef DEBUG
      alsa_input_assert(pvt->owner_lock_count > 0);
      pvt->owner_lock_count -= 1;
#endif /* DEBUG */
   }
   return (ret);
}

/*!
 * \brief Fix up a channel:  If a channel is consumed, this is called.  Basically update any ->owner links
 *
 * \note The channel is locked when this function gets called.
 */
static int alsa_input_chan_fixup(struct ast_channel *old_chan, struct ast_channel *new_chan)
{
   alsa_input_pvt_t *pvt = alsa_input_get_pvt(old_chan);

   alsa_input_pr_debug("alsa_input_chan_fixup(oldchan='%s', newchan='%s')\n",
      alsa_input_ast_channel_name(old_chan), alsa_input_ast_channel_name(new_chan));

   if (NULL != pvt) {
#ifdef DEBUG
      pvt->owner_lock_count += 1;
      alsa_input_assert(pvt->owner_lock_count > 0);
#endif /* DEBUG */
      /*
       Here the channel is locked and we get the monitor lock.
       In the monitor we get the monitor lock and then only TRY to lock the channel
       IMHO no deadlock can occur
      */
      alsa_input_monitor_lock(pvt->channel);
      pvt->owner = new_chan;
      alsa_input_monitor_unlock(pvt->channel);
#ifdef DEBUG
      alsa_input_assert(pvt->owner_lock_count > 0);
      pvt->owner_lock_count -= 1;
#endif /* DEBUG */
   }

   return (0);
}

/*!
 * \brief Start sending a literal DTMF digit
 *
 * \note The channel is not locked when this function gets called.
 */
static int alsa_input_chan_digit_begin(struct ast_channel *ast, char digit)
{
   alsa_input_pr_debug("alsa_input_chan_digit_begin(ast='%s', digit='%c')\n", alsa_input_ast_channel_name(ast), (char)(digit));

   /* Done in alsa_input_chan_digit_end() */

   return (0);
}

/*!
 * \brief Stop sending a literal DTMF digit
 *
 * \note The channel is not locked when this function gets called.
 */
static int alsa_input_chan_digit_end(struct ast_channel *ast, char digit, unsigned int duration)
{
   int ret = 0;
   alsa_input_pvt_t *pvt;
   alsa_input_tone_t tone;

   alsa_input_pr_debug("alsa_input_chan_digit_end(ast='%s', digit='%c', duration=%u)\n",
      alsa_input_ast_channel_name(ast), (char)(digit), (unsigned int)(duration));

   ast_channel_lock(ast);
   pvt = alsa_input_get_pvt(ast);
   if (NULL == pvt) {
      ret = -1;
   }
   else {
#ifdef DEBUG
      pvt->owner_lock_count += 1;
      alsa_input_assert(pvt->owner_lock_count > 0);
#endif /* DEBUG */
      if ((AI_ST_OFF_TALKING != pvt->ast_channel.state)
          && (AI_ST_OFF_WAITING_ANSWER != pvt->ast_channel.state)) {
         ast_log(AST_LOG_WARNING, "Can't send a DTMF while not off hook or not in the correct state\n");
         ret = -1;
      }
      else {
         switch (digit) {
            case '0': {
               tone = AI_TONE_DTMF_0;
               break;
            }
            case '1': {
               tone = AI_TONE_DTMF_1;
               break;
            }
            case '2': {
               tone = AI_TONE_DTMF_2;
               break;
            }
            case '3': {
               tone = AI_TONE_DTMF_3;
               break;
            }
            case '4': {
               tone = AI_TONE_DTMF_4;
               break;
            }
            case '5': {
               tone = AI_TONE_DTMF_5;
               break;
            }
            case '6': {
               tone = AI_TONE_DTMF_6;
               break;
            }
            case '7': {
               tone = AI_TONE_DTMF_7;
               break;
            }
            case '8': {
               tone = AI_TONE_DTMF_8;
               break;
            }
            case '9': {
               tone = AI_TONE_DTMF_9;
               break;
            }
            case '*': {
               tone = AI_TONE_DTMF_ASTER;
               break;
            }
            case '#': {
               tone = AI_TONE_DTMF_POUND;
               break;
            }
            case 'A': {
               tone = AI_TONE_DTMF_A;
               break;
            }
            case 'B': {
               tone = AI_TONE_DTMF_B;
               break;
            }
            case 'C': {
               tone = AI_TONE_DTMF_C;
               break;
            }
            case 'D': {
               tone = AI_TONE_DTMF_D;
               break;
            }
            default: {
               ast_log(AST_LOG_WARNING, "Unknown digit '%c'\n", (char)(digit));
               ret = -1;
               break;
            }
         }
         if (!ret) {
            alsa_input_pr_debug("Playing DTMF digit '%c'\n", (char)(digit));
            alsa_input_set_line_tone(pvt, tone, duration);
         }
      }
#ifdef DEBUG
      alsa_input_assert(pvt->owner_lock_count > 0);
      pvt->owner_lock_count -= 1;
#endif /* DEBUG */
   }

   ast_channel_unlock(ast);

   return (ret);
}

/*!
 * \brief Indicate a particular condition (e.g. AST_CONTROL_BUSY or AST_CONTROL_RINGING or AST_CONTROL_CONGESTION
 *
 * \note The channel is locked when this function gets called.
 */
static int alsa_input_chan_indicate(struct ast_channel *ast, int condition, const void *data, size_t datalen)
{
   int ret = 0;
   alsa_input_pvt_t *pvt = alsa_input_get_pvt(ast);

   if (NULL == pvt) {
      ret = -1;
   }
   else {
#ifdef DEBUG
      pvt->owner_lock_count += 1;
      alsa_input_assert(pvt->owner_lock_count > 0);
#endif /* DEBUG */
      if ((AI_ST_OFF_TALKING != pvt->ast_channel.state)
          && (AI_ST_OFF_WAITING_ANSWER != pvt->ast_channel.state)) {
         ast_log(AST_LOG_WARNING, "Can't indicate a condition while not off hook or not in the correct state\n");
      }
      else {
         switch (condition) {
            case AST_CONTROL_BUSY:
            case AST_CONTROL_CONGESTION:
            case AST_CONTROL_RINGING:
            case AST_CONTROL_INCOMPLETE:
#if (AST_VERSION >= 110)
            case AST_CONTROL_PVT_CAUSE_CODE:
#endif /* (AST_VERSION >= 110) */
            case -1: {
               ret = -1;  /* Ask for inband indications */
               break;
            }
            case AST_CONTROL_PROGRESS:
            case AST_CONTROL_PROCEEDING:
            case AST_CONTROL_VIDUPDATE:
            case AST_CONTROL_SRCUPDATE: {
               break;
            }
            case AST_CONTROL_HOLD: {
               ast_verbose(" << Channel Has Been Placed on Hold >> \n");
               ast_moh_start(ast, data, ('\0' != pvt->line_cfg->moh_interpret[0]) ? pvt->line_cfg->moh_interpret : NULL);
               break;
            }
            case AST_CONTROL_UNHOLD: {
               ast_verbose(" << Channel Has Been Retrieved from Hold >> \n");
               ast_moh_stop(ast);
               break;
            }
            default: {
               ast_log(AST_LOG_WARNING, "Don't know how to indicate condition %d on '%s'\n",
                  (int)(condition), alsa_input_ast_channel_name(ast));
               ret = -1;
               break;
            }
         }
      }
#ifdef DEBUG
      alsa_input_assert(pvt->owner_lock_count > 0);
      pvt->owner_lock_count -= 1;
#endif /* DEBUG */
   }

   return (ret);
}

/*!
 * \brief Read a frame, in standard format (see frame.h)
 *
 * \note The channel is locked when this function gets called.
 * As we don't associate a file descriptor to a channel, in theory this function
 * will be called only if there's a jitter buffer
 */
static struct ast_frame *alsa_input_chan_read(struct ast_channel *ast)
{
   struct ast_frame *ret = &(ast_null_frame);
   alsa_input_pvt_t *pvt = alsa_input_get_pvt(ast);

   /* alsa_input_pr_debug("alsa_input_chan_read(ast='%s')\n", alsa_input_ast_channel_name(ast)); */
   if (NULL != pvt) {
#ifdef DEBUG
      pvt->owner_lock_count += 1;
      alsa_input_assert(pvt->owner_lock_count > 0);
#endif /* DEBUG */
      do { /* Empty loop */
         if ((AI_ST_OFF_TALKING != pvt->ast_channel.state)
             && (AI_ST_OFF_WAITING_ANSWER != pvt->ast_channel.state)) {
            /* Don't try to receive audio on-hook */
            ast_log(AST_LOG_WARNING, "Trying to receive audio while not off hook or not in the correct state\n");
            break;
         }

         if (alsa_input_read_data(pvt, false, false)) {
            ret = &(pvt->ast_channel.frame);
         }
         else {
            ret = &(ast_null_frame);
         }
      } while (0);
#ifdef DEBUG
      alsa_input_assert(pvt->owner_lock_count > 0);
      pvt->owner_lock_count -= 1;
#endif /* DEBUG */
   }
   return (ret);
}

/*!
 * \brief Write a frame, in standard format (see frame.h)
 *
 * \note The channel is locked when this function gets called.
 */
static int alsa_input_chan_write(struct ast_channel *ast, struct ast_frame *frame)
{
   int ret = 0;
   alsa_input_pvt_t *pvt = alsa_input_get_pvt(ast);

   /* alsa_input_pr_debug("alsa_input_chan_write(ast='%s')\n", alsa_input_ast_channel_name(ast)); */
   if (NULL != pvt) {
#ifdef DEBUG
      pvt->owner_lock_count += 1;
      alsa_input_assert(pvt->owner_lock_count > 0);
#endif /* DEBUG */

      do { /* Empty loop */
         snd_pcm_state_t state;
         __u8 *pos;
         size_t tmp;
         size_t to_write;
         snd_pcm_sframes_t written;

         /* Write a frame of (presumably voice) data */
         if (AST_FRAME_VOICE != frame->frametype) {
            ast_log(AST_LOG_WARNING, "Don't know what to do with frame type %d\n", (int)(frame->frametype));
            break;
         }

         if (frame->datalen <= 0) {
            alsa_input_pr_debug("Void frame\n");
            break;
         }

         if (!alsa_input_ast_formats_are_equal(ast_format_slin, alsa_input_ast_get_frame_format(frame))) {
            ast_log(AST_LOG_WARNING, "Cannot handle frames in '%s' format\n",
               alsa_input_ast_format_get_name(alsa_input_ast_get_frame_format(frame)));
            ret = -1;
            break;
         }

         if ((AI_ST_OFF_TALKING != pvt->ast_channel.state)
             && (AI_ST_OFF_WAITING_ANSWER != pvt->ast_channel.state)) {
            /* Don't try to receive audio on-hook */
            ast_log(AST_LOG_WARNING, "Trying to receive audio while not off hook or not in the correct state\n");
            break;
         }

         if (AI_TONE_NONE != pvt->ast_channel.tone) {
            /* Don't try to send audio when emitting a tone */
            /* alsa_input_pr_debug("Trying to send audio while emitting a tone\n"); */
            alsa_input_write_tone_data(pvt);
            break;
         }

         state = snd_pcm_state(pvt->ast_channel.snd_playback);
         if ((state != SND_PCM_STATE_PREPARED) && (state != SND_PCM_STATE_RUNNING)) {
            int err = snd_pcm_prepare(pvt->ast_channel.snd_playback);
            if (err) {
               ast_log(AST_LOG_ERROR, "snd_pcm_prepare() failed: '%s'\n", snd_strerror(err));
            }
         }

         pos = frame->data.ptr;
         to_write = frame->datalen;
         alsa_input_assert((ARRAY_LEN(pvt->ast_channel.bytes_not_written) >= SAMPLE_SIZE)
            && (pvt->ast_channel.offset_bytes_not_written == 0));
         /* Are there some bytes not written ? */
         if (pvt->ast_channel.bytes_not_written_len > 0) {
            /* Yes */
            if ((pvt->ast_channel.bytes_not_written_len + to_write) < SAMPLE_SIZE) {
               /*
                Not enough byte to make a complete sample, we just
                add bytes to buffer pvt->ast_channel.bytes_not_written
               */
               memcpy(&(pvt->ast_channel.bytes_not_written[pvt->ast_channel.bytes_not_written_len]), pos, to_write);
               pvt->ast_channel.bytes_not_written_len += to_write;
               pos += to_write;
               to_write = 0;
               break;
            }
            alsa_input_assert(pvt->ast_channel.bytes_not_written_len <= SAMPLE_SIZE);
            tmp = SAMPLE_SIZE - pvt->ast_channel.bytes_not_written_len;
            alsa_input_assert(tmp <= to_write);
            if (tmp > 0) {
               memcpy(&(pvt->ast_channel.bytes_not_written[pvt->ast_channel.bytes_not_written_len]), pos, tmp);
               pvt->ast_channel.bytes_not_written_len += tmp;
               pos += tmp;
               to_write -= tmp;
            }
            written = snd_pcm_writei(pvt->ast_channel.snd_playback, pvt->ast_channel.bytes_not_written, 1);
            if (written < 0) {
               if (alsa_input_handle_card_error(pvt->ast_channel.snd_playback, written, "snd_pcm_writei")) {
                  /* Critical error */
                  alsa_input_critical_error(pvt, false);
               }
               break;
            }
            if (written > 0) {
               tmp = (written * SAMPLE_SIZE);
               pvt->ast_channel.bytes_not_written_len -= tmp;
               if (pvt->ast_channel.bytes_not_written_len > 0) {
                  memmove(pvt->ast_channel.bytes_not_written,
                     &(pvt->ast_channel.bytes_not_written[tmp]),
                     pvt->ast_channel.bytes_not_written_len);
               }
            }
         }
         if (pvt->ast_channel.bytes_not_written_len <= 0) {
            written = snd_pcm_writei(pvt->ast_channel.snd_playback, pos, to_write / SAMPLE_SIZE);
            if (written < 0) {
               if (alsa_input_handle_card_error(pvt->ast_channel.snd_playback, written, "snd_pcm_writei")) {
                  /* Critical error */
                  alsa_input_critical_error(pvt, false);
               }
               break;
            }
            tmp = (written * SAMPLE_SIZE);
            pos += tmp;
            to_write -= tmp;
         }
         if (to_write <= 0) {
            break;
         }
         tmp = (pvt->ast_channel.bytes_not_written_len + to_write) % SAMPLE_SIZE;
         if (tmp > 0) {
            if (tmp <= to_write) {
               pos += (to_write - tmp);
               memcpy(pvt->ast_channel.bytes_not_written, pos, tmp);
               pos += tmp;
               to_write -= tmp;
            }
            else {
               /* tmp > todo */
               memmove(pvt->ast_channel.bytes_not_written,
                  &(pvt->ast_channel.bytes_not_written[pvt->ast_channel.bytes_not_written_len + to_write - tmp]),
                  tmp - to_write);
               memcpy(&(pvt->ast_channel.bytes_not_written[tmp - to_write]), pos, to_write);
               pos += to_write;
               to_write = 0;
            }
         }
         pvt->ast_channel.bytes_not_written_len = tmp;
         if (to_write > 0) {
            ast_log(AST_LOG_WARNING, "Only wrote %lu of %lu bytes of audio data to line %lu\n",
               (unsigned long)(frame->datalen - to_write), (unsigned long)(frame->datalen),
               (unsigned long)(pvt->index_line + 1));
         }
      } while (false);

      alsa_input_read_data(pvt, true, false);
#ifdef DEBUG
      alsa_input_assert(pvt->owner_lock_count > 0);
      pvt->owner_lock_count -= 1;
#endif /* DEBUG */
   }
   return (ret);
}

/*!
 * \brief Hangup (and possibly destroy) the channel
 *
 * \note The channel is locked when this function gets called.
 */
static int alsa_input_chan_hangup(struct ast_channel *ast)
{
   alsa_input_pvt_t *pvt = alsa_input_ast_channel_tech_pvt(ast);

   alsa_input_pr_debug("alsa_input_chan_hangup(ast='%s')\n", alsa_input_ast_channel_name(ast));

   if ((NULL == pvt) || (pvt->owner != ast)) {
      alsa_input_pr_debug("Channel '%s' unlink or link to another line\n", alsa_input_ast_channel_name(ast));
      if ((NULL != pvt) && (NULL == pvt->owner)) {
         alsa_input_ast_channel_tech_pvt_set(ast, NULL);
      }
   }
   else {
#ifdef DEBUG
      pvt->owner_lock_count += 1;
      alsa_input_assert(pvt->owner_lock_count > 0);
#endif /* DEBUG */
      /*
       Here the channel is locked and we get the monitor lock.
       In the monitor we get the monitor lock and then only TRY to lock the channel
       IMHO no deadlock can occur
      */
      alsa_input_monitor_lock(pvt->channel);
      alsa_input_unlink_from_ast_channel(pvt, AI_EV_AST_HANGUP, false);
      alsa_input_assert(NULL == pvt->owner);
      alsa_input_monitor_unlock(pvt->channel);
#ifdef DEBUG
      alsa_input_assert(pvt->owner_lock_count > 0);
      pvt->owner_lock_count -= 1;
#endif /* DEBUG */
   }
   alsa_input_pr_debug("'%s' hung up\n", alsa_input_ast_channel_name(ast));
   ast_setstate(ast, AST_STATE_DOWN);

   return (0);
}

static void alsa_input_close_devices(alsa_input_chan_t *t)
{
   alsa_input_pvt_t *pvt;

   alsa_input_assert(!AST_LIST_EMPTY(&(t->pvt_list)));

   /* We hangup all lines if they have an owner */
   alsa_input_pr_debug("Freeing resources of the lines\n");
   AST_LIST_TRAVERSE(&(t->pvt_list), pvt, list) {
      if (NULL != pvt->ast_channel.snd_capture) {
         snd_pcm_close(pvt->ast_channel.snd_capture);
         pvt->ast_channel.snd_capture = NULL;
         pvt->monitor.fd_snd_capture = -1;
      }
      if (NULL != pvt->ast_channel.snd_playback) {
         snd_pcm_close(pvt->ast_channel.snd_playback);
         pvt->ast_channel.snd_playback = NULL;
      }
      if (pvt->monitor.fd_pipe >= 0) {
         close(pvt->monitor.fd_pipe);
         pvt->monitor.fd_pipe = -1;
      }
      if (pvt->monitor.fd_input >= 0) {
         close(pvt->monitor.fd_input);
         pvt->monitor.fd_input = -1;
      }
      if (pvt->monitor.fd_output >= 0) {
         close(pvt->monitor.fd_output);
         pvt->monitor.fd_output = -1;
      }
   }
}

static int alsa_input_open_devices(alsa_input_chan_t *t)
{
   int ret = AST_MODULE_LOAD_SUCCESS;

   alsa_input_assert(!AST_LIST_EMPTY(&(t->pvt_list)));

   do { /* Empty loop */
      alsa_input_pvt_t *pvt;

      /* Finally init the state of the lines */
      AST_LIST_TRAVERSE(&(t->pvt_list), pvt, list) {
         if (alsa_input_card_init(pvt->line_cfg->snd_capture_dev_name,
            SND_PCM_STREAM_CAPTURE, &(pvt->ast_channel.snd_capture), &(pvt->monitor.fd_snd_capture))) {
            ast_log(AST_LOG_ERROR, "Problem opening ALSA capture device '%s'\n", pvt->line_cfg->snd_capture_dev_name);
            ret = AST_MODULE_LOAD_FAILURE;
            break;
         }

         if (alsa_input_card_init(pvt->line_cfg->snd_playback_dev_name,
            SND_PCM_STREAM_PLAYBACK, &(pvt->ast_channel.snd_playback), NULL)) {
            ast_log(AST_LOG_ERROR, "Problem opening ALSA playback device '%s'\n", pvt->line_cfg->snd_playback_dev_name);
            ret = AST_MODULE_LOAD_FAILURE;
            break;
         }

         if ('\0' != pvt->line_cfg->ev_in_dev_name[0]) {
            pvt->monitor.fd_input = open(pvt->line_cfg->ev_in_dev_name, O_RDONLY | O_NONBLOCK);
            if (pvt->monitor.fd_input < 0) {
               ast_log(AST_LOG_ERROR, "Problem opening input event device '%s' ('%s')\n",
                  pvt->line_cfg->ev_in_dev_name, strerror(errno));
               ret = AST_MODULE_LOAD_FAILURE;
               break;
            }
         }
         else {
            int fds[2];
            if (pipe2(fds, O_CLOEXEC | O_DIRECT | O_NONBLOCK)) {
               ast_log(AST_LOG_ERROR, "Problem opening pipe to send and receive console events\n");
               ret = AST_MODULE_LOAD_FAILURE;
               break;
            }
            else {
               pvt->monitor.fd_input = fds[0];
               pvt->monitor.fd_pipe = fds[1];
            }
         }

         if ('\0' != pvt->line_cfg->ev_out_dev_name[0]) {
            pvt->monitor.fd_output = open(pvt->line_cfg->ev_out_dev_name, O_WRONLY | O_NONBLOCK);
            if (pvt->monitor.fd_output < 0) {
               ast_log(AST_LOG_ERROR, "Problem opening output event device '%s' ('%s')\n",
                  pvt->line_cfg->ev_out_dev_name, strerror(errno));
               ret = AST_MODULE_LOAD_FAILURE;
               break;
            }
         }
      }
   } while (0);

   if (AST_MODULE_LOAD_SUCCESS != ret) {
      alsa_input_close_devices(t);
   }

   return (ret);
}

static void alsa_input_hangup_all_lines(alsa_input_chan_t *t, bool monitor_is_really_stopped)
{
   alsa_input_pvt_t *pvt;

   /* We hangup all lines if they have an owner */
   alsa_input_pr_debug("Hanging up all the lines\n");
   alsa_input_assert(!t->monitor.run);
   alsa_input_monitor_lock(t);
   AST_LIST_TRAVERSE(&(t->pvt_list), pvt, list) {
      struct ast_channel *ast = pvt->owner;
      if (NULL != ast) {
         if (!monitor_is_really_stopped) {
            if (ast_channel_trylock(ast)) {
               continue;
            }
         }
         else {
            ast_channel_lock(ast);
         }
#ifdef DEBUG
         alsa_input_assert(0 == pvt->owner_lock_count);
         pvt->owner_lock_count += 1;
#endif /* DEBUG */
         alsa_input_queue_hangup(pvt, AI_EV_AST_HANGUP);
         /* ast_channel_unlock(ast) is done in alsa_input_queue_hangup() */
         alsa_input_assert((NULL == pvt->owner) && (0 == pvt->owner_lock_count));
      }
   }
   alsa_input_monitor_unlock(t);
}

static alsa_input_pvt_t *alsa_input_add_pvt(alsa_input_chan_t *t, size_t index_line)
{
   /* Make a alsa_input_pvt_t structure for this interface */
   alsa_input_pvt_t *tmp;

   alsa_input_assert((NULL != t) && (!t->monitor.run) && (AST_PTHREADT_NULL == t->monitor.thread));

   alsa_input_monitor_lock(t);
   do { /* Empty loop */
      tmp = ast_calloc(1, sizeof(*tmp));
      if (NULL == tmp) {
         ast_log(AST_LOG_ERROR, "Unable to allocate memory for line\n");
         break;
      }

      memset(tmp, 0, sizeof(*tmp));

      tmp->channel = t;
      tmp->index_line = index_line;
      tmp->line_cfg = &(t->config.line_cfgs[index_line]);
      tmp->owner = NULL;
#ifdef DEBUG
      tmp->owner_lock_count = 0;
#endif /* DEBUG */
      tmp->monitor.fd_snd_capture = -1;
      tmp->monitor.fd_input = -1;
      tmp->monitor.fd_output = -1;
      tmp->monitor.fd_pipe = -1;
      tmp->monitor.events_len_in_bytes = 0;
      alsa_input_reset_pvt_monitor_state(tmp);
      tmp->ast_channel.snd_capture = NULL;
      tmp->ast_channel.snd_playback = NULL;
      tmp->ast_channel.status = AI_STATUS_ON_HOOK;
      tmp->ast_channel.snd_capture_muted = true;
      tmp->ast_channel.tone = AI_TONE_NONE;
      tmp->ast_channel.tone_duration_in_bytes = 0;
      tmp->ast_channel.tone_bytes_generated = 0;
      tmp->ast_channel.state = AI_ST_ON_IDLE;
      alsa_input_reset_buf_fr_to_queue(tmp);
      alsa_input_reset_buf_bytes_not_written(tmp);
      tmp->monitor.last_known_state = tmp->ast_channel.state;
      tmp->monitor.last_known_snd_capture_muted = tmp->ast_channel.snd_capture_muted;
      AST_LIST_INSERT_TAIL(&(t->pvt_list), tmp, list);
   } while (false);
   alsa_input_monitor_unlock(t);

   return (tmp);
}

static struct ast_channel *alsa_input_chan_request(const char *type,
#if (AST_VERSION < 110)
   format_t cap,
#else /* (AST_VERSION >= 110) */
   alsa_input_ast_format_cap *cap,
#endif /* (AST_VERSION >= 110)*/
#if (AST_VERSION > 110)
   const struct ast_assigned_ids *assigned_ids,
#endif /* (AST_VERSION > 110) */
   const struct ast_channel *requestor,
#if (AST_VERSION < 110)
   void *addr,
#else /* (AST_VERSION >= 110) */
   const char *addr,
#endif /* (AST_VERSION >= 110)*/
   int *cause);

static alsa_input_chan_t alsa_input_chan = {
   .chan_tech = {
      .type = alsa_input_chan_type,
      .description = alsa_input_chan_desc,
      .requester = alsa_input_chan_request,
      .send_digit_begin = alsa_input_chan_digit_begin,
      .send_digit_end = alsa_input_chan_digit_end,
      .call = alsa_input_chan_call,
      .hangup = alsa_input_chan_hangup,
      .answer = alsa_input_chan_answer,
      .read = alsa_input_chan_read,
      .write = alsa_input_chan_write,
      .indicate = alsa_input_chan_indicate,
      .fixup = alsa_input_chan_fixup
   },
   .channel_registered = false,
};

/*!
 * \brief Requester - to set up call data structures (pvt's)
 *
 * \param type type of channel to request
 * \param cap Format capabilities for requested channel
 * \param requestor channel asking for data
 * \param addr destination of the call
 * \param cause Cause of failure
 *
 * \details
 * Request a channel of a given type, with addr as optional information used
 * by the low level module
 *
 * \retval NULL failure
 * \retval non-NULL channel on success
 */
static struct ast_channel *alsa_input_chan_request(const char *type,
#if (AST_VERSION < 110)
   format_t cap,
#else /* (AST_VERSION >= 110) */
   alsa_input_ast_format_cap *cap,
#endif /* (AST_VERSION >= 110)*/
#if (AST_VERSION > 110)
   const struct ast_assigned_ids *assigned_ids,
#endif /* (AST_VERSION > 110) */
   const struct ast_channel *requestor,
#if (AST_VERSION < 110)
   void *_addr,
#else /* (AST_VERSION >= 110) */
   const char *addr,
#endif /* (AST_VERSION >= 110)*/
   int *cause)
{
   struct ast_channel *ret = NULL;
   alsa_input_chan_t *t = &(alsa_input_chan);
   alsa_input_pvt_t *pvt;
#if (AST_VERSION < 110)
   const char *addr = _addr;
#endif /* (AST_VERSION < 110) */

   alsa_input_pr_debug("alsa_input_chan_request(type='%s', addr='%s')\n", type, addr);

   if (ast_strlen_zero(addr)) {
      ast_log(AST_LOG_WARNING, "Unable to create channel with empty destination.\n");
      *cause = AST_CAUSE_CHANNEL_UNACCEPTABLE;
   }
   else {
      /* Search for an unowned channel */
      alsa_input_monitor_lock(t);
      *cause = AST_CAUSE_CHANNEL_UNACCEPTABLE;
      AST_LIST_TRAVERSE(&(t->pvt_list), pvt, list) {
         char tmp[16];
         size_t length;
         sprintf(tmp, "%lu", (unsigned long)(pvt->index_line + 1));
         length = strlen(tmp);
         if ((0 == strncmp(addr, tmp, length)) && (!isalnum(addr[length]))) {
#if (AST_VERSION < 110)
            if (alsa_input_ast_format_cap_iscompatible_cap(&(cap), alsa_input_get_chan_tech_cap(&(pvt->channel->chan_tech)))) {
#else /* (AST_VERSION >= 110) */
            if (alsa_input_ast_format_cap_iscompatible_cap(cap, alsa_input_get_chan_tech_cap(&(pvt->channel->chan_tech)))) {
#endif /* (AST_VERSION >= 110)*/
               if ((NULL == pvt->owner) && (AI_STATUS_ON_HOOK == pvt->ast_channel.status)) {
                  alsa_input_assert(AI_ST_ON_IDLE == pvt->ast_channel.state);
                  alsa_input_new(pvt, AI_ST_ON_PRE_RINGING, AI_EV_AST_REQUEST,
#if (AST_VERSION <= 110)
                     ((NULL != requestor) ? alsa_input_ast_channel_linkedid(requestor) : NULL),
#else /* (AST_VERSION > 110) */
                     assigned_ids, requestor,
#endif /* (AST_VERSION > 110) */
                     NULL);
                  ret = pvt->owner;
               }
               else {
                  alsa_input_pr_debug("Channel is busy\n");
                  *cause = AST_CAUSE_BUSY;
               }
            }
            else {
#if (AST_VERSION <= 110)
               char buf[256];
               ast_log(AST_LOG_WARNING, "Asked to get a channel of unsupported format '%s'\n", ast_getformatname_multiple(buf, sizeof(buf), cap));
#else /* (AST_VERSION > 110) */
               struct ast_str *buf = ast_str_alloca(256);
               ast_log(AST_LOG_WARNING, "Asked to get a channel of unsupported format '%s'\n", ast_format_cap_get_names(cap, &(buf)));
#endif /* (AST_VERSION > 110) */
            }
            break;
         }
      }
      alsa_input_monitor_unlock(t);
   }
   return (ret);
}

/* Must be called with monitor.lock locked */
static int alsa_input_write_input_event(alsa_input_pvt_t *pvt, __u16 ev_type, __u16 ev_code)
{
   int ret = -1;
   __u8 c;
   struct input_event *ev;

   /*
    We write the event directly in the array pvt->monitor.events, and
    just write a single byte to generate POLLIN event for
    the file descriptor connected to the other end of the pipe
   */
   alsa_input_assert((pvt->channel->monitor.lock_count > 0) && (pvt->monitor.fd_pipe >= 0));
   if ((pvt->monitor.events_len_in_bytes + sizeof(*ev)) <= sizeof(pvt->monitor.events)) {
      alsa_input_assert((sizeof(*ev) == sizeof(pvt->monitor.events[0]))
         && (0 == (pvt->monitor.events_len_in_bytes % sizeof(pvt->monitor.events[0]))));
      ev = &(pvt->monitor.events[pvt->monitor.events_len_in_bytes / sizeof(pvt->monitor.events[0])]);
      memset(ev, 0, sizeof(*ev));
      ev->type = ev_type;
      ev->code = ev_code;
      ev->value = 1;
      pvt->monitor.events_len_in_bytes += sizeof(*ev);
      ret = 0;
   }
   else {
      ast_log(AST_LOG_ERROR, "Error writing input_event\n");
      ret = -1;
   }
   c = 0;
   write(pvt->monitor.fd_pipe, &(c), sizeof(c));

   return (ret);
}

static char *alsa_input_cli_press(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
   char *ret = CLI_SUCCESS;
   alsa_input_chan_t *t = &(alsa_input_chan);
   bool do_unlock = false;

   switch (cmd) {
      case CLI_INIT: {
         e->command = "ai press";
         e->usage =
            "Usage: ai press line [on|off|mute]\n"
            "       Press key for line 'line':\n"
            "       - 'on' hook key\n"
            "       - 'off' hook key\n"
            "       - 'mute' key\n";
         return (NULL);
      }
      case CLI_GENERATE: {
         return (NULL);
      }
   }

   do { /* Empty loop */
      alsa_input_pvt_t *pvt;
      int tmp;
      char str_status[64];
      char *f;
      __u16 code;

      if (4 != a->argc) {
         ret = CLI_SHOWUSAGE;
         break;
      }

      /* We parse the special key */
      ast_copy_string(str_status, a->argv[3], ARRAY_LEN(str_status));
      f = ast_strip(str_status);
      if (!strcasecmp(f, "on")) {
         code = KEY_ESC;
      }
      else if (!strcasecmp(f, "off")) {
         code = KEY_ENTER;
      }
      else if (!strcasecmp(f, "mute")) {
         code = KEY_MUTE;
      }
      else {
         ast_cli(a->fd, "Invalid key '%s'\n", f);
         ret = CLI_FAILURE;
         break;
      }

      /* We parse the line number */
      if ((1 != sscanf(a->argv[2], " %10d ", &(tmp))) || (tmp <= 0)) {
         ast_cli(a->fd, "Invalid line '%s'\n", a->argv[2]);
         ret = CLI_FAILURE;
         break;
      }
      tmp -= 1;

      alsa_input_monitor_lock(t);
      do_unlock = true;
      AST_LIST_TRAVERSE(&(t->pvt_list), pvt, list) {
         if (pvt->index_line == ((size_t)(tmp))) {
            break;
         }
      }
      if ((NULL == pvt) || (pvt->monitor.fd_pipe < 0)) {
         ast_cli(a->fd, "Invalid line '%d'\n", (int)(tmp + 1));
         ret = CLI_FAILURE;
         break;
      }

      /* We change the status of the line */
      if (alsa_input_write_input_event(pvt, EV_KEY, code)) {
         ret = CLI_FAILURE;
         break;
      }
   } while (false);

   if (do_unlock) {
      alsa_input_monitor_unlock(t);
      do_unlock = false;
   }

   return (ret);
}

static char *alsa_input_cli_dial(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
   char *ret = CLI_SUCCESS;
   alsa_input_chan_t *t = &(alsa_input_chan);
   bool do_unlock = false;

   switch (cmd) {
      case CLI_INIT: {
         e->command = "ai dial";
         e->usage =
            "Usage: ai dial line [digits]\n"
            "       Dials the digits\n";
         return (NULL);
      }
      case CLI_GENERATE: {
         return (NULL);
      }
   }

   do { /* Empty loop */
      alsa_input_pvt_t *pvt;
      int tmp;
      const char *f;
      __u16 code;

      if (4 != a->argc) {
         ret = CLI_SHOWUSAGE;
         break;
      }

      /* We parse the line number */
      if ((1 != sscanf(a->argv[2], " %10d ", &(tmp))) || (tmp <= 0)) {
         ast_cli(a->fd, "Invalid line '%s'\n", a->argv[2]);
         ret = CLI_FAILURE;
         break;
      }
      tmp -= 1;

      alsa_input_monitor_lock(t);
      do_unlock = true;
      AST_LIST_TRAVERSE(&(t->pvt_list), pvt, list) {
         if (pvt->index_line == ((size_t)(tmp))) {
            break;
         }
      }
      if ((NULL == pvt) || (pvt->monitor.fd_pipe < 0)) {
         ast_cli(a->fd, "Invalid line '%d'\n", (int)(tmp + 1));
         ret = CLI_FAILURE;
         break;
      }

      /* We parse the digits */
      f = a->argv[3];
      for (f = a->argv[3]; ('\0' != *f); f += 1) {
         if (isspace(*f)) {
            continue;
         }
         code = KEY_RESERVED;
         if ((*f >= '0') && (*f <= '9')) {
            code = KEY_NUMERIC_0 + (*f - '0');
         }
         else if ('*' == *f) {
            code = KEY_NUMERIC_STAR;
         }
         else if ('#' == *f) {
            code = KEY_NUMERIC_POUND;
         }
         else if (('A' == *f) || ('a' == *f)) {
            code = KEY_A;
         }
         else if (('B' == *f) || ('b' == *f)) {
            code = KEY_B;
         }
         else if (('C' == *f) || ('c' == *f)) {
            code = KEY_C;
         }
         else if (('D' == *f) || ('d' == *f)) {
            code = KEY_D;
         }
         else {
            break;
         }
         if (alsa_input_write_input_event(pvt, EV_KEY, code)) {
            ret = CLI_FAILURE;
            break;
         }
      }
      if ('\0' != *f) {
         if (KEY_RESERVED == code) {
            ast_cli(a->fd, "Invalid digit '%c'\n", (char)(*f));
         }
         ret = CLI_FAILURE;
         break;
      }
   } while (false);

   if (do_unlock) {
      alsa_input_monitor_unlock(t);
      do_unlock = false;
   }

   return (ret);
}

static struct ast_cli_entry cli_alsa_input[] = {
   AST_CLI_DEFINE(alsa_input_cli_press, "Press a special key"),
   AST_CLI_DEFINE(alsa_input_cli_dial, "Dial digits"),
};

static int __unload_module(void)
{
   int ret = 0;
   alsa_input_chan_t *t = &(alsa_input_chan);
   alsa_input_pvt_t *p;
   size_t i;
   bool unregister_cli = t->channel_registered;

   do { /* Empty loop */
      /* Ask the monitor thread to stop */
      alsa_input_prepare_stop_monitor(t);

      /* Take us out of the channel loop: no new ast_channel can be created for
      incoming calls (by alsa_input_chan_request()) */
      alsa_input_pr_debug("Unregistering channel\n");
      if (t->channel_registered) {
         ast_channel_unregister(&(t->chan_tech));
         t->channel_registered = false;
      }

      /* Hangup all lines */
      alsa_input_hangup_all_lines(t, false);

      /* We stop the monitor thread: no new ast_channel can be created
       for outgoing call because the user hooks off the phone */
      if (alsa_input_stop_monitor(t)) {
         ast_log(AST_LOG_ERROR, "Unable to stop the monitor\n");
         ret = -1;
         break;
      }

      /* Once again hangup all lines that could have been created by monitor
       before it stops */
      alsa_input_hangup_all_lines(t, true);

      if (unregister_cli) {
         ast_cli_unregister_multiple(cli_alsa_input, ARRAY_LEN(cli_alsa_input));
      }

      /* We close the device */
      alsa_input_close_devices(t);

      /* We destroy all the interfaces and free their memory */
      alsa_input_pr_debug("Destroying all the lines\n");
      p = AST_LIST_FIRST(&(t->pvt_list));
      while (NULL != p) {
         alsa_input_pvt_t *pl = p;
         p = AST_LIST_NEXT(p, list);
         ast_free(pl);
      }

      /* We free structures allocated for the configuration */
      for (i = 0; (i < ARRAY_LEN(t->config.line_cfgs)); i += 1) {
         /* Nothing to do */
      }

#if (AST_VERSION >= 110)
      if (NULL != t->chan_tech.capabilities) {
         alsa_input_ast_format_cap_destroy(t->chan_tech.capabilities);
         t->chan_tech.capabilities = NULL;
      }
#endif /* (AST_VERSION >= 110) */

      ast_mutex_destroy(&(t->monitor.lock));

      ret = 0;
   } while (false);

   return (ret);
}

static int unload_module(void)
{
   return (__unload_module());
}

static int load_module(void)
{
   int ret = AST_MODULE_LOAD_SUCCESS;
   alsa_input_chan_t *t = &(alsa_input_chan);
   struct ast_config *cfg = CONFIG_STATUS_FILEINVALID;
   size_t i;

   alsa_input_init_cache_ast_format();
   alsa_input_init_tones();

   memset(&(t->config), 0, sizeof(t->config));
   t->config.language[0] = '\0';
   t->config.line_count = 0;
   t->channel_registered = false;
   t->pvt_list.first = NULL;
   t->pvt_list.last = NULL;
   t->monitor.run = false;
   t->monitor.thread = AST_PTHREADT_NULL;
   ast_mutex_init(&(t->monitor.lock));
#ifdef DEBUG
   t->monitor.lock_count = 0;
#endif /* DEBUG */
#if (AST_VERSION < 110)
   t->chan_tech.capabilities = 0;
#else /* (AST_VERSION >= 110) */
   t->chan_tech.capabilities = NULL;
#endif /* (AST_VERSION >= 110)*/
   for (i = 0; (i < ARRAY_LEN(t->config.line_cfgs)); i += 1) {
      alsa_input_line_config_t *line_cfg = &(t->config.line_cfgs[i]);
      memcpy(&(line_cfg->jb_conf), &(default_jb_conf), sizeof(line_cfg->jb_conf));
      line_cfg->enable = false;
      line_cfg->snd_capture_dev_name[0] = '\0';
      line_cfg->snd_playback_dev_name[0] = '\0';
      line_cfg->ev_in_dev_name[0] = '\0';
      line_cfg->ev_out_dev_name[0] = '\0';
      line_cfg->monitor_dialing = false;
      line_cfg->search_extension_trigger = '\0';
      line_cfg->dialing_timeout_1st_digit = 5000;
      line_cfg->dialing_timeout = 3000;
      sprintf(line_cfg->context, "ai-line-%d", (int)(i + 1));
      sprintf(line_cfg->cid_name, "line%d", (int)(i + 1));
      sprintf(line_cfg->cid_num, "00-00-00-%02d", (int)(i + 1));
      ast_copy_string(line_cfg->moh_interpret, "default", ARRAY_LEN(line_cfg->moh_interpret));
   }

   do { /* Empty loop */
      struct ast_variable *v;
      struct ast_flags config_flags = { 0 };

#if (AST_VERSION >= 110)
      t->chan_tech.capabilities = alsa_input_ast_format_cap_alloc();
      if (NULL == t->chan_tech.capabilities) {
         ret = AST_MODULE_LOAD_DECLINE;
         break;
      }
#endif /* (AST_VERSION >= 110) */
      alsa_input_ast_format_cap_remove_by_type(alsa_input_get_chan_tech_cap(&(t->chan_tech)), AST_MEDIA_TYPE_UNKNOWN);
      alsa_input_ast_format_cap_append_format(alsa_input_get_chan_tech_cap(&(t->chan_tech)), ast_format_slin);

      alsa_input_pr_debug("Reading configuration file '%s'\n", alsa_input_cfg_file);

      cfg = ast_config_load2(alsa_input_cfg_file, alsa_input_chan_type, config_flags);
      if (CONFIG_STATUS_FILEINVALID == cfg) {
         ast_log(AST_LOG_ERROR, "Config file '%s' is in an invalid format. Aborting.\n", alsa_input_cfg_file);
         ret = AST_MODULE_LOAD_DECLINE;
         break;
      }

      /* We *must* have a config file otherwise stop immediately */
      if (CONFIG_STATUS_FILEMISSING == cfg) {
         cfg = CONFIG_STATUS_FILEINVALID;
         ast_log(AST_LOG_ERROR, "Unable to load config file '%s'\n", alsa_input_cfg_file);
         ret = AST_MODULE_LOAD_DECLINE;
         break;
      }

      for (v = ast_variable_browse(cfg, "general"); (NULL != v); v = v->next) {
         if (!strcasecmp(v->name, "lines")) {
            int tmp;
            if ((1 != sscanf(v->value, " %10d ", &(tmp))) || (tmp <= 0) || (((size_t)(tmp)) > ARRAY_LEN(t->config.line_cfgs))) {
               ast_log(AST_LOG_ERROR, "Invalid value for variable 'lines' in section 'interfaces' of config file '%s'\n",
                  alsa_input_cfg_file);
               ret = AST_MODULE_LOAD_DECLINE;
               break;
            }
            t->config.line_count = (size_t)(tmp);
         }
         else if (!strcasecmp(v->name, "language")) {
            ast_copy_string(t->config.language, v->value, sizeof(t->config.language));
         }
         else {
            ast_log(AST_LOG_WARNING, "Unknown variable '%s' in section 'interfaces' of config_file '%s'\n",
               v->name, alsa_input_cfg_file);
         }
      }

      if (AST_MODULE_LOAD_SUCCESS != ret) {
         break;
      }

      for (i = 0; (i < t->config.line_count); i += 1) {
         char section[64];
         alsa_input_line_config_t *line_cfg = &(t->config.line_cfgs[i]);

         snprintf(section, ARRAY_LEN(section), "line%d", (int)(i + 1));
         for (v = ast_variable_browse(cfg, section); (NULL != v); v = v->next) {
            if (!ast_jb_read_conf(&(line_cfg->jb_conf), v->name, v->value)) {
               continue;
            }
            if (!strcasecmp(v->name, "enable")) {
               if (ast_true(v->value)) {
                  line_cfg->enable = true;
               }
               else {
                  line_cfg->enable = false;
                  /* Stop parsing section */
                  break;
               }
            }
            else if (!strcasecmp(v->name, "context")) {
               ast_copy_string(line_cfg->context, v->value, ARRAY_LEN(line_cfg->context));
            }
            else if (!strcasecmp(v->name, "snd_capture_device")) {
               ast_copy_string(line_cfg->snd_capture_dev_name, v->value, sizeof(line_cfg->snd_capture_dev_name));
            }
            else if (!strcasecmp(v->name, "snd_playback_device")) {
               ast_copy_string(line_cfg->snd_playback_dev_name, v->value, sizeof(line_cfg->snd_playback_dev_name));
            }
            else if (!strcasecmp(v->name, "event_input_device")) {
               ast_copy_string(line_cfg->ev_in_dev_name, v->value, sizeof(line_cfg->ev_in_dev_name));
            }
            else if (!strcasecmp(v->name, "event_output_device")) {
               ast_copy_string(line_cfg->ev_out_dev_name, v->value, sizeof(line_cfg->ev_out_dev_name));
            }
            else if (!strcasecmp(v->name, "monitor_dialing")) {
               if (ast_true(v->value)) {
                  line_cfg->monitor_dialing = true;
               }
               else {
                  line_cfg->monitor_dialing = false;
               }
            }
            else if (!strcasecmp(v->name, "search_extension_trigger")) {
               char value[32];
               char *f;

               ast_copy_string(value, v->value, ARRAY_LEN(value));
               f = ast_strip(value);
               if (('\0' == f[0]) || ('\0' == f[1])) {
                  line_cfg->search_extension_trigger = f[0];
               }
               else {
                  ast_log(AST_LOG_ERROR, "Invalid value for variable 'search_extension_trigger' in section '%s' of config file '%s'\n",
                     section, alsa_input_cfg_file);
                  ret = AST_MODULE_LOAD_DECLINE;
                  break;
               }
            }
            else if (!strcasecmp(v->name, "dialing_timeout_1st_digit")) {
               int tmp;
               if ((1 != sscanf(v->value, " %10d ", &(tmp))) || (tmp < 0)) {
                  ast_log(AST_LOG_ERROR, "Invalid value for variable 'dialing_timeout_1st_digit' in section '%s' of config file '%s'\n",
                     section, alsa_input_cfg_file);
                  ret = AST_MODULE_LOAD_DECLINE;
                  break;
               }
               line_cfg->dialing_timeout_1st_digit = tmp;
            }
            else if (!strcasecmp(v->name, "dialing_timeout")) {
               int tmp;
               if ((1 != sscanf(v->value, " %10d ", &(tmp))) || (tmp < 0)) {
                  ast_log(AST_LOG_ERROR, "Invalid value for variable 'dialing_timeout' in section '%s' of config file '%s'\n",
                     section, alsa_input_cfg_file);
                  ret = AST_MODULE_LOAD_DECLINE;
                  break;
               }
               line_cfg->dialing_timeout = tmp;
            }
            else if (!strcasecmp(v->name, "caller_id")) {
               ast_callerid_split(v->value, line_cfg->cid_name,
                  ARRAY_LEN(line_cfg->cid_name),
                  line_cfg->cid_num, ARRAY_LEN(line_cfg->cid_num));
            }
            else if (!strcasecmp(v->name, "moh_interpret")) {
               ast_copy_string(line_cfg->moh_interpret, v->value, ARRAY_LEN(line_cfg->moh_interpret));
            }
            else {
               ast_log(AST_LOG_WARNING, "Unknown variable '%s' in section '%s' of config file '%s'\n",
                  v->name, section, alsa_input_cfg_file);
            }
         }
         if (AST_MODULE_LOAD_SUCCESS != ret) {
            break;
         }
         if (line_cfg->enable) {
            if ('\0' == line_cfg->context[0]) {
               ast_copy_string(line_cfg->context, "default", sizeof(line_cfg->context));
            }
            if ('\0' == line_cfg->snd_capture_dev_name[0]) {
               ast_copy_string(line_cfg->snd_capture_dev_name, "default", sizeof(line_cfg->snd_capture_dev_name));
            }
            if ('\0' == line_cfg->snd_playback_dev_name[0]) {
               ast_copy_string(line_cfg->snd_playback_dev_name, "default", sizeof(line_cfg->snd_playback_dev_name));
            }
            if (NULL == alsa_input_add_pvt(t, i)) {
               ret = AST_MODULE_LOAD_DECLINE;
               break;
            }
         }
      }
      if (AST_MODULE_LOAD_SUCCESS != ret) {
         break;
      }

      if (AST_LIST_EMPTY(&(t->pvt_list))) {
         ast_log(AST_LOG_ERROR, "No active line in config file '%s'\n", alsa_input_cfg_file);
         ret = AST_MODULE_LOAD_DECLINE;
         break;
      }

      ast_config_destroy(cfg);
      cfg = CONFIG_STATUS_FILEINVALID;

      ret = alsa_input_open_devices(t);
      if (AST_MODULE_LOAD_SUCCESS != ret) {
         break;
      }

      alsa_input_pr_debug("Registering channel\n");

      /*
       Now that all internal structures are initialized we can register
       the channel and start the monitor
      */
      if (ast_channel_register(&(t->chan_tech))) {
         ast_log(AST_LOG_ERROR, "Unable to register channel class '%s'\n",
            alsa_input_chan_desc);
         ret = AST_MODULE_LOAD_FAILURE;
         break;
      }
      ast_log(AST_LOG_VERBOSE, "Registering commands\n");

      if (ast_cli_register_multiple(cli_alsa_input, ARRAY_LEN(cli_alsa_input))) {
         ast_log(AST_LOG_ERROR, "Unable to register commands\n");
         ast_channel_unregister(&(t->chan_tech));
         ret = AST_MODULE_LOAD_FAILURE;
         break;
      }
      t->channel_registered = true;

      if (alsa_input_start_monitor(t)) {
         ret = AST_MODULE_LOAD_FAILURE;
         break;
      }

      ret = AST_MODULE_LOAD_SUCCESS;
   }
   while (false);

   if (CONFIG_STATUS_FILEINVALID != cfg) {
      ast_config_destroy(cfg);
   }

   if (AST_MODULE_LOAD_SUCCESS != ret) {
      __unload_module();
   }

   return (ret);
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER, "ALSA / Input Channel Driver",
      .load = load_module,
      .unload = unload_module,
      .load_pri = AST_MODPRI_CHANNEL_DRIVER,
   );
