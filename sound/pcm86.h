/**
 * @file	pcm86.h
 * @brief	Interface of the 86-PCM
 */

#pragma once

#include <sound/sound.h>
#include <nevent.h>

enum {
	PCM86_LOGICALBUF	= 0x8000,
	PCM86_BUFSIZE		= (1 << 16),
	PCM86_BUFMSK		= ((1 << 16) - 1),

	PCM86_DIVBIT		= 10,
	PCM86_DIVENV		= (1 << PCM86_DIVBIT),

	PCM86_RESCUE		= 20
};

#define	PCM86_EXTBUF		g_pcm86.rescue					/* 救済延滞… */
#define	PCM86_REALBUFSIZE	(PCM86_LOGICALBUF + PCM86_EXTBUF)

void RECALC_NOWCLKWAIT(UINT64 cnt);
//#define RECALC_NOWCLKWAIT(cnt)											¥
//	do																	¥
//	{																	¥
//		SINT64 decvalue =(SINT64)(cnt << g_pcm86.stepbit);				¥
//		if (g_pcm86.virbuf - decvalue < g_pcm86.virbuf) 				¥
//		{																¥
//			g_pcm86.virbuf -= decvalue;									¥
//		}																¥
//		if (g_pcm86.virbuf < 0)											¥
//		{																¥
//			g_pcm86.virbuf &= g_pcm86.stepmask;							¥
//		}																¥
//	} while (0 /*CONSTCOND*/)

typedef struct {
	SINT32	divremain;
	SINT32	div;
	SINT32	div2;
	SINT32	smp;
	SINT32	lastsmp;
	SINT32	smp_l;
	SINT32	lastsmp_l;
	SINT32	smp_r;
	SINT32	lastsmp_r;

	UINT32	readpos;			/* DSOUND再生位置 */
	UINT32	wrtpos;				/* 書込み位置 */
	SINT32	realbuf;			/* DSOUND用のデータ数 */
	SINT32	virbuf;				/* 86PCM(bufsize:0x8000)のデータ数 */
	SINT32	rescue;

	SINT32	fifosize;
	SINT32	volume;
	SINT32	vol5;

	union {
		struct {
			UINT32	lastclock_obsolate;
			UINT32	stepclock_obsolate;
		} obsolate;
		UINT64	lastclockforwait;
	};
	UINT	stepmask;

	UINT8	fifo;
	UINT8	soundflags;			/*!< サウンド フラグ (A460) */
	UINT8	dactrl;
	UINT8	_write;
	UINT8	stepbit;
	UINT8	irq;
	UINT8	reqirq;
	UINT8	irqflag;

	UINT8	buffer[PCM86_BUFSIZE];
	
	UINT	rateval;
	
	UINT64	lastclock;
	UINT64	stepclock;
} _PCM86, *PCM86;

typedef struct { // ステートセーブ互換性維持用（変更禁止）
	SINT32	divremain;
	SINT32	div;
	SINT32	div2;
	SINT32	smp;
	SINT32	lastsmp;
	SINT32	smp_l;
	SINT32	lastsmp_l;
	SINT32	smp_r;
	SINT32	lastsmp_r;

	UINT32	readpos;			/* DSOUND再生位置 */
	UINT32	wrtpos;				/* 書込み位置 */
	SINT32	realbuf;			/* DSOUND用のデータ数 */
	SINT32	virbuf;				/* 86PCM(bufsize:0x8000)のデータ数 */
	SINT32	rescue;

	SINT32	fifosize;
	SINT32	volume;
	SINT32	vol5;

	UINT32	lastclock;
	UINT32	stepclock;
	UINT	stepmask;

	UINT8	fifo;
	UINT8	soundflags;			/*!< サウンド フラグ (A460) */
	UINT8	dactrl;
	UINT8	_write;
	UINT8	stepbit;
	UINT8	irq;
	UINT8	reqirq;
	UINT8	irqflag;

	UINT8	buffer[PCM86_BUFSIZE];
} _PCM86_OLD, *PCM86_OLD;

typedef struct {
	UINT	rate;
	UINT	vol;
} PCM86CFG;

static inline BOOL pcm86_should_output_audio(UINT8 fifo, UINT div, SINT32 realbuf, SINT32 virbuf)
{
	(void)virbuf;
	/* Playback must stay silent until the driver sets the FIFO play-enable
	 * bit (0x80); buffered/priming data written before that must not be heard. */
	if (!(fifo & 0x80) || div == 0) {
		return FALSE;
	}
	if (realbuf <= 0) {
		return FALSE;
	}
	return TRUE;
}

static inline BOOL pcm86_has_valid_audio(SINT32 realbuf, SINT32 virbuf)
{
	(void)virbuf;
	return (realbuf > 0);
}

void pcm86_debug_ring_state(const char *where, PCM86 pcm86);
BOOL pcm86_is_stale_ring(PCM86 pcm86);
void pcm86_kill_stale_state(PCM86 pcm86);
void pcm86_clear_stale_ring(PCM86 pcm86);

#ifdef __cplusplus
extern "C"
{
#endif

extern const UINT pcm86rate8[];

void pcm86_cb(NEVENTITEM item);

void pcm86cs_initialize();
void pcm86cs_shutdown();
void pcm86cs_enter_criticalsection();
void pcm86cs_leave_criticalsection();

void pcm86_reset(void);
void pcm86gen_initialize(UINT rate);
void pcm86gen_setvol(UINT vol);
void pcm86gen_update(void);
void pcm86_setpcmrate(REG8 val);
void pcm86_setnextintr(void);
void pcm86_changeclock(UINT oldmultiple);

void SOUNDCALL pcm86gen_checkbuf(PCM86 pcm86, UINT nCount);
void SOUNDCALL pcm86gen_getpcm(PCM86 pcm86, SINT32 *lpBuffer, UINT nCount);

BOOL pcm86gen_intrq(int fromFMTimer);

#ifdef __cplusplus
}
#endif
