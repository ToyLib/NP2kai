/**
 * @file	pcm86g.c
 * @brief	Implementation of the 86-PCM
 */

#include <compiler.h>
#include <cpucore.h>
#include <stdio.h>
#include <sound/pcm86.h>

#if 0
#undef	TRACEOUT
#define	TRACEOUT(s)	(void)(s)
static void trace_fmt_ex(const char* fmt, ...)
{
	char stmp[2048];
	va_list ap;
	va_start(ap, fmt);
	vsprintf(stmp, fmt, ap);
	strcat(stmp, "¥n");
	va_end(ap);
	OutputDebugStringA(stmp);
}
#define	TRACEOUT(s)	trace_fmt_ex s
#endif	/* 1 */

#define PCM86GET8(p, a)													\
	do																	\
	{																	\
		(a) = (SINT8)((p)->buffer[(p)->readpos & PCM86_BUFMSK]) << 8;	\
		(p)->readpos++;													\
	} while (0 /*CONSTCOND*/)

#define PCM86GET16(p, a)												\
	do																	\
	{																	\
		(a) = (SINT8)((p)->buffer[(p)->readpos & PCM86_BUFMSK]) << 8;	\
		(a) |= (p)->buffer[((p)->readpos + 1) & PCM86_BUFMSK];			\
		(p)->readpos += 2;												\
	} while (0 /*CONSTCOND*/)

#define	BYVOLUME(p, s)	((((s) >> 6) * (p)->volume) >> (PCM86_DIVBIT + 4))

void pcm86_debug_ring_state(const char *where, PCM86 pcm86)
{
	UINT64 clk = CPU_CLOCK + CPU_BASECLOCK - CPU_REMCLOCK;
	fprintf(stderr,
		"[PCM86] clk=%llu %s read=%u wrt=%u real=%d vir=%d fifo=%d div=%d div2=%d dactrl=%.2x stepbit=%d stepmask=%d vol=%d reqirq=%d irqflag=%d\n",
		(unsigned long long)clk,
		where,
		pcm86->readpos,
		pcm86->wrtpos,
		pcm86->realbuf,
		pcm86->virbuf,
		pcm86->fifosize,
		pcm86->div,
		pcm86->div2,
		pcm86->dactrl,
		pcm86->stepbit,
		pcm86->stepmask,
		pcm86->volume,
		pcm86->reqirq,
		pcm86->irqflag);
}

BOOL pcm86_is_stale_ring(PCM86 pcm86)
{
	/* virbuf reaching 0 while realbuf still has backlog is normal (the wall-clock
	 * "virtual" watermark just outran actual playback); it is NOT stale on its own.
	 * A ring is only genuinely desynced when the physical read position has caught
	 * up to the write position yet the byte counter still claims data remains. */
	if (pcm86->realbuf <= 0) {
		return FALSE;
	}
	return (pcm86->readpos == pcm86->wrtpos);
}

void pcm86_kill_stale_state(PCM86 pcm86)
{
	pcm86_debug_ring_state("kill-stale", pcm86);
	pcm86_clear_stale_ring(pcm86);
	pcm86->reqirq = 0;
	pcm86->irqflag = 0;
	nevent_reset(NEVENT_86PCM);
	pcm86->lastclockforwait = CPU_CLOCK + CPU_BASECLOCK - CPU_REMCLOCK;
}

void pcm86_clear_stale_ring(PCM86 pcm86)
{
	memset(pcm86->buffer, 0, sizeof(pcm86->buffer));
	pcm86->readpos = 0;
	pcm86->wrtpos = 0;
	pcm86->realbuf = 0;
	pcm86->virbuf = 0;
	pcm86->divremain = 0;
	pcm86->smp = 0;
	pcm86->lastsmp = 0;
	pcm86->smp_l = 0;
	pcm86->lastsmp_l = 0;
	pcm86->smp_r = 0;
	pcm86->lastsmp_r = 0;
	pcm86->reqirq = 0;
	pcm86->irqflag = 0;
}

static void pcm86mono16(PCM86 pcm86, SINT32 *lpBuffer, UINT nCount)
{
	if (pcm86->div < PCM86_DIVENV)					/* アップさんぷる */
	{
		do
		{
			SINT32 smp;
			if (pcm86->divremain < 0)
			{
				SINT32 dat;
				pcm86->divremain += PCM86_DIVENV;
				pcm86->realbuf -= 2;
				if (pcm86->realbuf < 0)
				{
					goto pm16_bufempty;
				}
				PCM86GET16(pcm86, dat);
				pcm86->lastsmp = pcm86->smp;
				pcm86->smp = dat;
			}
			smp = (pcm86->lastsmp * pcm86->divremain) - (pcm86->smp * (pcm86->divremain - PCM86_DIVENV));
			lpBuffer[0] += BYVOLUME(pcm86, smp);
			lpBuffer += 2;
			pcm86->divremain -= pcm86->div;
		} while (--nCount);
	}
	else
	{
		do
		{
			SINT32 smp;
			smp = pcm86->smp * (pcm86->divremain * -1);
			pcm86->divremain += PCM86_DIVENV;
			while (1)
			{
				SINT32 dat;
				pcm86->realbuf -= 2;
				if (pcm86->realbuf < 0)
				{
					goto pm16_bufempty;
				}
				PCM86GET16(pcm86, dat);
				pcm86->lastsmp = pcm86->smp;
				pcm86->smp = dat;
				if (pcm86->divremain > pcm86->div2)
				{
					pcm86->divremain -= pcm86->div2;
					smp += pcm86->smp * pcm86->div2;
				}
				else
				{
					break;
				}
			}
			smp += pcm86->smp * pcm86->divremain;
			lpBuffer[0] += BYVOLUME(pcm86, smp);
			lpBuffer += 2;
			pcm86->divremain -= pcm86->div2;
		} while (--nCount);
	}
	return;

pm16_bufempty:
	// Ordinary underrun (not corruption): back off the decrement and let the
	// ring keep its position so pending writes are not discarded.
	pcm86->realbuf += 2;
	pcm86->divremain = 0;
	pcm86->smp = 0;
	pcm86->lastsmp = 0;
}

static void pcm86stereo16(PCM86 pcm86, SINT32 *lpBuffer, UINT nCount)
{
	if (pcm86->div < PCM86_DIVENV)					/* アップさんぷる */
	{
		do
		{
			SINT32 smp;
			if (pcm86->divremain < 0)
			{
				SINT32 dat;
				pcm86->divremain += PCM86_DIVENV;
				pcm86->realbuf -= 4;
				if (pcm86->realbuf < 0)
				{
					goto ps16_bufempty;
				}
				PCM86GET16(pcm86, dat);
				pcm86->lastsmp_l = pcm86->smp_l;
				pcm86->smp_l = dat;
				PCM86GET16(pcm86, dat);
				pcm86->lastsmp_r = pcm86->smp_r;
				pcm86->smp_r = dat;
			}
			smp = (pcm86->lastsmp_l * pcm86->divremain) - (pcm86->smp_l * (pcm86->divremain - PCM86_DIVENV));
			lpBuffer[0] += BYVOLUME(pcm86, smp);
			smp = (pcm86->lastsmp_r * pcm86->divremain) - (pcm86->smp_r * (pcm86->divremain - PCM86_DIVENV));
			lpBuffer[1] += BYVOLUME(pcm86, smp);
			lpBuffer += 2;
			pcm86->divremain -= pcm86->div;
		} while (--nCount);
	}
	else
	{
		do
		{
			SINT32 smp_l;
			SINT32 smp_r;
			smp_l = pcm86->smp_l * (pcm86->divremain * -1);
			smp_r = pcm86->smp_r * (pcm86->divremain * -1);
			pcm86->divremain += PCM86_DIVENV;
			while (1 /*CONSTCOND*/)
			{
				SINT32 dat;
				pcm86->realbuf -= 4;
				if (pcm86->realbuf < 0)
				{
					goto ps16_bufempty;
				}
				PCM86GET16(pcm86, dat);
				pcm86->lastsmp_l = pcm86->smp_l;
				pcm86->smp_l = dat;
				PCM86GET16(pcm86, dat);
				pcm86->lastsmp_r = pcm86->smp_r;
				pcm86->smp_r = dat;
				if (pcm86->divremain > pcm86->div2)
				{
					pcm86->divremain -= pcm86->div2;
					smp_l += pcm86->smp_l * pcm86->div2;
					smp_r += pcm86->smp_r * pcm86->div2;
				}
				else
				{
					break;
				}
			}
			smp_l += pcm86->smp_l * pcm86->divremain;
			smp_r += pcm86->smp_r * pcm86->divremain;
			lpBuffer[0] += BYVOLUME(pcm86, smp_l);
			lpBuffer[1] += BYVOLUME(pcm86, smp_r);
			lpBuffer += 2;
			pcm86->divremain -= pcm86->div2;
		} while (--nCount);
	}
	return;

ps16_bufempty:
	pcm86->realbuf += 4;
	pcm86->divremain = 0;
	pcm86->smp_l = 0;
	pcm86->smp_r = 0;
	pcm86->lastsmp_l = 0;
	pcm86->lastsmp_r = 0;
}

static void pcm86mono8(PCM86 pcm86, SINT32 *lpBuffer, UINT nCount)
{
	if (pcm86->div < PCM86_DIVENV)					/* アップさんぷる */
	{
		do
		{
			SINT32 smp;
			if (pcm86->divremain < 0)
			{
				SINT32 dat;
				pcm86->divremain += PCM86_DIVENV;
				pcm86->realbuf--;
				if (pcm86->realbuf < 0)
				{
					goto pm8_bufempty;
				}
				PCM86GET8(pcm86, dat);
				pcm86->lastsmp = pcm86->smp;
				pcm86->smp = dat;
			}
			smp = (pcm86->lastsmp * pcm86->divremain) - (pcm86->smp * (pcm86->divremain - PCM86_DIVENV));
			lpBuffer[0] += BYVOLUME(pcm86, smp);
			lpBuffer += 2;
			pcm86->divremain -= pcm86->div;
		} while (--nCount);
	}
	else
	{
		do
		{
			SINT32 smp;
			smp = pcm86->smp * (pcm86->divremain * -1);
			pcm86->divremain += PCM86_DIVENV;
			while (1 /*CONSTCOND*/)
			{
				SINT32 dat;
				pcm86->realbuf--;
				if (pcm86->realbuf < 0)
				{
					goto pm8_bufempty;
				}
				PCM86GET8(pcm86, dat);
				pcm86->lastsmp = pcm86->smp;
				pcm86->smp = dat;
				if (pcm86->divremain > pcm86->div2)
				{
					pcm86->divremain -= pcm86->div2;
					smp += pcm86->smp * pcm86->div2;
				}
				else
				{
					break;
				}
			}
			smp += pcm86->smp * pcm86->divremain;
			lpBuffer[0] += BYVOLUME(pcm86, smp);
			lpBuffer += 2;
			pcm86->divremain -= pcm86->div2;
		} while (--nCount);
	}
	return;

pm8_bufempty:
	pcm86->realbuf += 1;
	pcm86->divremain = 0;
	pcm86->smp = 0;
	pcm86->lastsmp = 0;
}

static void pcm86stereo8(PCM86 pcm86, SINT32 *lpBuffer, UINT nCount)
{
	if (pcm86->div < PCM86_DIVENV)					/* アップさんぷる */
	{
		do
		{
			SINT32 smp;
			if (pcm86->divremain < 0)
			{
				SINT32 dat;
				pcm86->divremain += PCM86_DIVENV;
				pcm86->realbuf -= 2;
				if (pcm86->realbuf < 0)
				{
					goto pm8_bufempty;
				}
				PCM86GET8(pcm86, dat);
				pcm86->lastsmp_l = pcm86->smp_l;
				pcm86->smp_l = dat;
				PCM86GET8(pcm86, dat);
				pcm86->lastsmp_r = pcm86->smp_r;
				pcm86->smp_r = dat;
			}
			smp = (pcm86->lastsmp_l * pcm86->divremain) - (pcm86->smp_l * (pcm86->divremain - PCM86_DIVENV));
			lpBuffer[0] += BYVOLUME(pcm86, smp);
			smp = (pcm86->lastsmp_r * pcm86->divremain) - (pcm86->smp_r * (pcm86->divremain - PCM86_DIVENV));
			lpBuffer[1] += BYVOLUME(pcm86, smp);
			lpBuffer += 2;
			pcm86->divremain -= pcm86->div;
		} while (--nCount);
	}
	else
	{
		do
		{
			SINT32 smp_l;
			SINT32 smp_r;
			smp_l = pcm86->smp_l * (pcm86->divremain * -1);
			smp_r = pcm86->smp_r * (pcm86->divremain * -1);
			pcm86->divremain += PCM86_DIVENV;
			while (1 /*CONSTCOND*/)
			{
				SINT32 dat;
				pcm86->realbuf -= 2;
				if (pcm86->realbuf < 0)
				{
					goto pm8_bufempty;
				}
				PCM86GET8(pcm86, dat);
				pcm86->lastsmp_l = pcm86->smp_l;
				pcm86->smp_l = dat;
				PCM86GET8(pcm86, dat);
				pcm86->lastsmp_r = pcm86->smp_r;
				pcm86->smp_r = dat;
				if (pcm86->divremain > pcm86->div2)
				{
					pcm86->divremain -= pcm86->div2;
					smp_l += pcm86->smp_l * pcm86->div2;
					smp_r += pcm86->smp_r * pcm86->div2;
				}
				else
				{
					break;
				}
			}
			smp_l += pcm86->smp_l * pcm86->divremain;
			smp_r += pcm86->smp_r * pcm86->divremain;
			lpBuffer[0] += BYVOLUME(pcm86, smp_l);
			lpBuffer[1] += BYVOLUME(pcm86, smp_r);
			lpBuffer += 2;
			pcm86->divremain -= pcm86->div2;
		} while (--nCount);
	}
	return;

pm8_bufempty:
	pcm86->realbuf += 2;
	pcm86->divremain = 0;
	pcm86->smp_l = 0;
	pcm86->smp_r = 0;
	pcm86->lastsmp_l = 0;
	pcm86->lastsmp_r = 0;
}

void SOUNDCALL pcm86gen_getpcm(PCM86 pcm86, SINT32 *lpBuffer, UINT nCount)
{
	if (!nCount || lpBuffer == NULL) {
		return;
	}
	if (pcm86_is_stale_ring(pcm86))
	{
		pcm86_kill_stale_state(pcm86);
		while (nCount--) {
			lpBuffer[0] = 0;
			lpBuffer[1] = 0;
			lpBuffer += 2;
		}
		return;
	}
	if (pcm86->realbuf == 0 && pcm86->virbuf == 0)
	{
		pcm86->reqirq = 0;
		pcm86->irqflag = 0;
		pcm86->divremain = 0;
		pcm86->smp = 0;
		pcm86->lastsmp = 0;
		pcm86->smp_l = 0;
		pcm86->lastsmp_l = 0;
		pcm86->smp_r = 0;
		pcm86->lastsmp_r = 0;
	}
	/* realbuf>0 with virbuf<=0 is normal backlog (the wall-clock watermark just
	 * depleted first); keep draining realbuf instead of wiping pending audio. */
	if (!pcm86_should_output_audio(pcm86->fifo, pcm86->div, pcm86->realbuf, pcm86->virbuf)) {
		while (nCount--) {
			lpBuffer[0] = 0;
			lpBuffer[1] = 0;
			lpBuffer += 2;
		}
		return;
	}

#if defined(SUPPORT_MULTITHREAD)
	pcm86cs_enter_criticalsection();
#endif
	switch (pcm86->dactrl & 0x70)
	{
	case 0x00:						/* 16bit-none */
		break;

	case 0x10:						/* 16bit-right */
		pcm86mono16(pcm86, lpBuffer + 1, nCount);
		break;

	case 0x20:						/* 16bit-left */
		pcm86mono16(pcm86, lpBuffer, nCount);
		break;

	case 0x30:						/* 16bit-stereo */
		pcm86stereo16(pcm86, lpBuffer, nCount);
		break;

	case 0x40:						/* 8bit-none */
		break;

	case 0x50:						/* 8bit-right */
		pcm86mono8(pcm86, lpBuffer + 1, nCount);
		break;

	case 0x60:						/* 8bit-left */
		pcm86mono8(pcm86, lpBuffer, nCount);
		break;

	case 0x70:						/* 8bit-stereo */
		pcm86stereo8(pcm86, lpBuffer, nCount);
		break;
	}
#if defined(SUPPORT_MULTITHREAD)
	pcm86cs_leave_criticalsection();
#endif
	pcm86gen_checkbuf(pcm86, nCount);
}