/* crypto/mem_dbg.c */
/* Written by Richard Levitte (richard@levitte.org) for the OpenSSL
 * project 1999.
 */
/* ====================================================================
 * Copyright (c) 1999 The OpenSSL Project.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. All advertising materials mentioning features or use of this
 *    software must display the following acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit. (http://www.OpenSSL.org/)"
 *
 * 4. The names "OpenSSL Toolkit" and "OpenSSL Project" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For written permission, please contact
 *    licensing@OpenSSL.org.
 *
 * 5. Products derived from this software may not be called "OpenSSL"
 *    nor may "OpenSSL" appear in their names without prior written
 *    permission of the OpenSSL Project.
 *
 * 6. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit (http://www.OpenSSL.org/)"
 *
 * THIS SOFTWARE IS PROVIDED BY THE OpenSSL PROJECT ``AS IS'' AND ANY
 * EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE OpenSSL PROJECT OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 * ====================================================================
 *
 * This product includes cryptographic software written by Eric Young
 * (eay@cryptsoft.com).  This product includes software written by Tim
 * Hudson (tjh@cryptsoft.com).
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>	
#include <openssl/crypto.h>
#include <openssl/buffer.h>
#include <openssl/bio.h>
#include <openssl/lhash.h>
#include "cryptlib.h"

/* State CRYPTO_MEM_CHECK_ON exists only temporarily when the library
 * thinks that certain allocations should not be checked (e.g. the data
 * structures used for memory checking).  It is not suitable as an initial
 * state: the library will unexpectedly enable memory checking when it
 * executes one of those sections that want to disable checking
 * temporarily.
 *
 * State CRYPTO_MEM_CHECK_ENABLE without ..._ON makes no sense whatsoever.
 */
static int mh_mode=CRYPTO_MEM_CHECK_OFF;
static unsigned long disabling_thread = 0;


static unsigned long order=0;

static LHASH *amih=NULL;

typedef struct app_mem_info_st
	{	
	unsigned long thread;
	const char *file;
	int line;
	const char *info;
	struct app_mem_info_st *next;
	int references;
	} APP_INFO;

static LHASH *mh=NULL;

typedef struct mem_st
	{
	char *addr;
	int num;
	const char *file;
	int line;
	unsigned long thread;
	unsigned long order;
	time_t time;
	APP_INFO *app_info;
	} MEM;

static int options = V_CRYPTO_MDEBUG_TIME | V_CRYPTO_MDEBUG_THREAD;


int CRYPTO_mem_ctrl(int mode)
	{
	int ret=mh_mode;

	CRYPTO_w_lock(CRYPTO_LOCK_MALLOC);
	switch (mode)
		{
	/* for applications: */
	case CRYPTO_MEM_CHECK_ON: /* aka MemCheck_start() */
		mh_mode = CRYPTO_MEM_CHECK_ON|CRYPTO_MEM_CHECK_ENABLE;
		disabling_thread = 0;
		break;
	case CRYPTO_MEM_CHECK_OFF: /* aka MemCheck_stop() */
		mh_mode = 0;
		disabling_thread = 0;
		break;

	/* switch off temporarily (for library-internal use): */
	case CRYPTO_MEM_CHECK_DISABLE: /* aka MemCheck_off() */
		if (mh_mode & CRYPTO_MEM_CHECK_ON)
			{
			mh_mode&= ~CRYPTO_MEM_CHECK_ENABLE;
			if (disabling_thread != CRYPTO_thread_id()) /* otherwise we already have the MALLOC2 lock */
				{
				/* Long-time lock CRYPTO_LOCK_MALLOC2 must not be claimed while
				 * we're holding CRYPTO_LOCK_MALLOC, or we'll deadlock if
				 * somebody else holds CRYPTO_LOCK_MALLOC2 (and cannot release it
				 * because we block entry to this function).
				 * Give them a chance, first, and then claim the locks in
				 * appropriate order (long-time lock first).
				 */
				CRYPTO_w_unlock(CRYPTO_LOCK_MALLOC);
				/* Note that after we have waited for CRYPTO_LOCK_MALLOC2
				 * and CRYPTO_LOCK_MALLOC, we'll still be in the right
				 * "case" and "if" branch because MemCheck_start and
				 * MemCheck_stop may never be used while there are multiple
				 * OpenSSL threads. */
				CRYPTO_w_lock(CRYPTO_LOCK_MALLOC2);
				CRYPTO_w_lock(CRYPTO_LOCK_MALLOC);
				disabling_thread=CRYPTO_thread_id();
				}
			}
		break;
	case CRYPTO_MEM_CHECK_ENABLE: /* aka MemCheck_on() */
		if (mh_mode & CRYPTO_MEM_CHECK_ON)
			{
			mh_mode|=CRYPTO_MEM_CHECK_ENABLE;
			if (disabling_thread != 0)
				{
				disabling_thread=0;
				CRYPTO_w_unlock(CRYPTO_LOCK_MALLOC2);
				}
			}
		break;

	default:
		break;
		}
	CRYPTO_w_unlock(CRYPTO_LOCK_MALLOC);
	return(ret);
	}

int CRYPTO_mem_check_on(void)
	{
	int ret = 0;

	if (mh_mode & CRYPTO_MEM_CHECK_ON)
		{
		CRYPTO_w_lock(CRYPTO_LOCK_MALLOC);

		ret = (mh_mode & CRYPTO_MEM_CHECK_ENABLE)
			&& disabling_thread != CRYPTO_thread_id();

		CRYPTO_w_unlock(CRYPTO_LOCK_MALLOC);
		}
	return(ret);
	}	


void CRYPTO_dbg_set_options(int bits)
	{
	options = bits;
	}

int CRYPTO_dbg_get_options()
	{
	return options;
	}

static int mem_cmp(MEM *a, MEM *b)
	{
	return(a->addr - b->addr);
	}

static unsigned long mem_hash(MEM *a)
	{
	unsigned long ret;

	ret=(unsigned long)a->addr;

	ret=ret*17851+(ret>>14)*7+(ret>>4)*251;
	return(ret);
	}

static int app_info_cmp(APP_INFO *a, APP_INFO *b)
	{
	return(a->thread - b->thread);
	}

static unsigned long app_info_hash(APP_INFO *a)
	{
	unsigned long ret;

	ret=(unsigned long)a->thread;

	ret=ret*17851+(ret>>14)*7+(ret>>4)*251;
	return(ret);
	}

static APP_INFO *remove_info()
	{
	APP_INFO tmp;
	APP_INFO *ret = NULL;

	if (amih != NULL)
		{
		tmp.thread=CRYPTO_thread_id();
		if ((ret=(APP_INFO *)lh_delete(amih,(char *)&tmp)) != NULL)
			{
			APP_INFO *next=ret->next;

			if (next != NULL)
				{
				next->references++;
				lh_insert(amih,(char *)next);
				}
#ifdef LEVITTE_DEBUG
			if (ret->thread != tmp.thread)
				{
				fprintf(stderr, "remove_info(): deleted info has other thread ID (%lu) than the current thread (%lu)!!!!\n",
					ret->thread, tmp.thread);
				abort();
				}
#endif
			if (--(ret->references) <= 0)
				{
				ret->next = NULL;
				if (next != NULL)
					next->references--;
				Free(ret);
				}
			}
		}
	return(ret);
	}

int CRYPTO_add_info(const char *file, int line, const char *info)
	{
	APP_INFO *ami, *amim;
	int ret=0;

	if (is_MemCheck_on())
		{
		MemCheck_off();

		if ((ami = (APP_INFO *)Malloc(sizeof(APP_INFO))) == NULL)
			{
			ret=0;
			goto err;
			}
		if (amih == NULL)
			{
			if ((amih=lh_new(app_info_hash,app_info_cmp)) == NULL)
				{
				Free(ami);
				ret=0;
				goto err;
				}
			}

		ami->thread=CRYPTO_thread_id();
		ami->file=file;
		ami->line=line;
		ami->info=info;
		ami->references=1;
		ami->next=NULL;

		if ((amim=(APP_INFO *)lh_insert(amih,(char *)ami)) != NULL)
			{
#ifdef LEVITTE_DEBUG
			if (ami->thread != amim->thread)
				{
				fprintf(stderr, "CRYPTO_add_info(): previous info has other thread ID (%lu) than the current thread (%lu)!!!!\n",
					amim->thread, ami->thread);
				abort();
				}
#endif
			ami->next=amim;
			}
 err:
		MemCheck_on();
		}

	return(ret);
	}

int CRYPTO_remove_info(void)
	{
	int ret=0;

	if (is_MemCheck_on())
		{
		MemCheck_off();

		ret=(remove_info() != NULL);

		MemCheck_on();
		}
	return(ret);
	}

int CRYPTO_remove_all_info(void)
	{
	int ret=0;

	if (is_MemCheck_on())
		{
		MemCheck_off();

		while(remove_info() != NULL)
			ret++;

		MemCheck_on();
		}
	return(ret);
	}


static unsigned long break_order_num=0;
void CRYPTO_dbg_malloc(void *addr, int num, const char *file, int line,
	int before_p)
	{
	MEM *m,*mm;
	APP_INFO tmp,*amim;

	switch(before_p & 127)
		{
	case 0:
		break;
	case 1:
		if (addr == NULL)
			break;

		if (is_MemCheck_on())
			{
			MemCheck_off();
			if ((m=(MEM *)Malloc(sizeof(MEM))) == NULL)
				{
				Free(addr);
				MemCheck_on();
				return;
				}
			if (mh == NULL)
				{
				if ((mh=lh_new(mem_hash,mem_cmp)) == NULL)
					{
					Free(addr);
					Free(m);
					addr=NULL;
					goto err;
					}
				}

			m->addr=addr;
			m->file=file;
			m->line=line;
			m->num=num;
			if (options & V_CRYPTO_MDEBUG_THREAD)
				m->thread=CRYPTO_thread_id();
			else
				m->thread=0;

			if (order == break_order_num)
				{
				/* BREAK HERE */
				m->order=order;
				}
			m->order=order++;
#ifdef LEVITTE_DEBUG
			fprintf(stderr, "LEVITTE_DEBUG: [%5d] %c 0x%p (%d)\n",
				m->order,
				(before_p & 128) ? '*' : '+',
				m->addr, m->num);
#endif
			if (options & V_CRYPTO_MDEBUG_TIME)
				m->time=time(NULL);
			else
				m->time=0;

			tmp.thread=CRYPTO_thread_id();
			m->app_info=NULL;
			if (amih != NULL
				&& (amim=(APP_INFO *)lh_retrieve(amih,(char *)&tmp)) != NULL)
				{
				m->app_info = amim;
				amim->references++;
				}

			if ((mm=(MEM *)lh_insert(mh,(char *)m)) != NULL)
				{
				/* Not good, but don't sweat it */
				if (mm->app_info != NULL)
					{
					mm->app_info->references--;
					}
				Free(mm);
				}
		err:
			MemCheck_on();
			}
		break;
		}
	return;
	}

void CRYPTO_dbg_free(void *addr, int before_p)
	{
	MEM m,*mp;

	switch(before_p)
		{
	case 0:
		if (addr == NULL)
			break;

		if (is_MemCheck_on() && (mh != NULL))
			{
			MemCheck_off();

			m.addr=addr;
			mp=(MEM *)lh_delete(mh,(char *)&m);
			if (mp != NULL)
				{
#ifdef LEVITTE_DEBUG
			fprintf(stderr, "LEVITTE_DEBUG: [%5d] - 0x%p (%d)\n",
				mp->order, mp->addr, mp->num);
#endif
				if (mp->app_info != NULL)
					{
					mp->app_info->references--;
					}
				Free(mp);
				}

			MemCheck_on();
			}
		break;
	case 1:
		break;
		}
	}

void CRYPTO_dbg_realloc(void *addr1, void *addr2, int num,
	const char *file, int line, int before_p)
	{
	MEM m,*mp;

#ifdef LEVITTE_DEBUG
	fprintf(stderr, "LEVITTE_DEBUG: --> CRYPTO_dbg_malloc(addr1 = %p, addr2 = %p, num = %d, file = \"%s\", line = %d, before_p = %d)\n",
		addr1, addr2, num, file, line, before_p);
#endif

	switch(before_p)
		{
	case 0:
		break;
	case 1:
		if (addr2 == NULL)
			break;

		if (addr1 == NULL)
			{
			CRYPTO_dbg_malloc(addr2, num, file, line, 128 | before_p);
			break;
			}

		if (is_MemCheck_on())
			{
			MemCheck_off();

			m.addr=addr1;
			mp=(MEM *)lh_delete(mh,(char *)&m);
			if (mp != NULL)
				{
#ifdef LEVITTE_DEBUG
				fprintf(stderr, "LEVITTE_DEBUG: [%5d] * 0x%p (%d) -> 0x%p (%d)\n",
					mp->order,
					mp->addr, mp->num,
					addr2, num);
#endif
				mp->addr=addr2;
				mp->num=num;
				lh_insert(mh,(char *)mp);
				}

			MemCheck_on();
			}
		break;
		}
	return;
	}


typedef struct mem_leak_st
	{
	BIO *bio;
	int chunks;
	long bytes;
	} MEM_LEAK;

static void print_leak(MEM *m, MEM_LEAK *l)
	{
	char buf[1024];
	char *bufp = buf;
	APP_INFO *amip;
	int ami_cnt;
	struct tm *lcl = NULL;
	unsigned long ti;

	if(m->addr == (char *)l->bio)
	    return;

	if (options & V_CRYPTO_MDEBUG_TIME)
		{
		lcl = localtime(&m->time);
	
		sprintf(bufp, "[%02d:%02d:%02d] ",
			lcl->tm_hour,lcl->tm_min,lcl->tm_sec);
		bufp += strlen(bufp);
		}

	sprintf(bufp, "%5lu file=%s, line=%d, ",
		m->order,m->file,m->line);
	bufp += strlen(bufp);

	if (options & V_CRYPTO_MDEBUG_THREAD)
		{
		sprintf(bufp, "thread=%lu, ", m->thread);
		bufp += strlen(bufp);
		}

	sprintf(bufp, "number=%d, address=%08lX\n",
		m->num,(unsigned long)m->addr);
	bufp += strlen(bufp);

	BIO_puts(l->bio,buf);
	
	l->chunks++;
	l->bytes+=m->num;

	amip=m->app_info;
	ami_cnt=0;
	if (amip)
		ti=amip->thread;
	while(amip && amip->thread == ti)
		{
		int buf_len;
		int info_len;

		ami_cnt++;
		memset(buf,'>',ami_cnt);
		sprintf(buf + ami_cnt,
			"thread=%lu, file=%s, line=%d, info=\"",
			amip->thread, amip->file, amip->line);
		buf_len=strlen(buf);
		info_len=strlen(amip->info);
		if (128 - buf_len - 3 < info_len)
			{
			memcpy(buf + buf_len, amip->info, 128 - buf_len - 3);
			buf_len = 128 - 3;
			}
		else
			{
			strcpy(buf + buf_len, amip->info);
			buf_len = strlen(buf);
			}
		sprintf(buf + buf_len, "\"\n");
		
		BIO_puts(l->bio,buf);

		amip = amip->next;
		}
#ifdef LEVITTE_DEBUG
	if (amip)
		{
		fprintf(stderr, "Thread switch detected i backtrace!!!!\n");
		abort();
		}
#endif
	}

void CRYPTO_mem_leaks(BIO *b)
	{
	MEM_LEAK ml;
	char buf[80];

	if (mh == NULL) return;
	ml.bio=b;
	ml.bytes=0;
	ml.chunks=0;
	CRYPTO_w_lock(CRYPTO_LOCK_MALLOC2);
	lh_doall_arg(mh,(void (*)())print_leak,(char *)&ml);
	CRYPTO_w_unlock(CRYPTO_LOCK_MALLOC2);
	if (ml.chunks != 0)
		{
		sprintf(buf,"%ld bytes leaked in %d chunks\n",
			ml.bytes,ml.chunks);
		BIO_puts(b,buf);
		}

#if 0
	lh_stats_bio(mh,b);
	lh_node_stats_bio(mh,b);
	lh_node_usage_stats_bio(mh,b);
#endif
	}

static void (*mem_cb)()=NULL;

static void cb_leak(MEM *m, char *cb)
	{
	void (*mem_callback)()=(void (*)())cb;
	mem_callback(m->order,m->file,m->line,m->num,m->addr);
	}

void CRYPTO_mem_leaks_cb(void (*cb)())
	{
	if (mh == NULL) return;
	CRYPTO_w_lock(CRYPTO_LOCK_MALLOC2);
	mem_cb=cb;
	lh_doall_arg(mh,(void (*)())cb_leak,(char *)mem_cb);
	mem_cb=NULL;
	CRYPTO_w_unlock(CRYPTO_LOCK_MALLOC2);
	}

#ifndef NO_FP_API
void CRYPTO_mem_leaks_fp(FILE *fp)
	{
	BIO *b;

	if (mh == NULL) return;
	if ((b=BIO_new(BIO_s_file())) == NULL)
		return;
	BIO_set_fp(b,fp,BIO_NOCLOSE);
	CRYPTO_mem_leaks(b);
	BIO_free(b);
	}
#endif

