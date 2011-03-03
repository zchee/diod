/*
 * Copyright (C) 2005 by Latchesar Ionkov <lucho@ionkov.net>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * LATCHESAR IONKOV AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <assert.h>
#include <zlib.h>
#include "9p.h"
#include "npfs.h"
#include "npfsimpl.h"

struct cbuf {
	unsigned char *sp;
	unsigned char *p;
	unsigned char *ep;
};

static inline void
buf_init(struct cbuf *buf, void *data, int datalen)
{
	buf->sp = buf->p = data;
	buf->ep = data + datalen;
}

static inline int
buf_check_overflow(struct cbuf *buf)
{
	return buf->p > buf->ep;
}

static inline int
buf_check_size(struct cbuf *buf, int len)
{
	if (buf->p+len > buf->ep) {
		if (buf->p < buf->ep)
			buf->p = buf->ep + 1;

		return 0;
	}

	return 1;
}

static inline void *
buf_alloc(struct cbuf *buf, int len)
{
	void *ret = NULL;

	if (buf_check_size(buf, len)) {
		ret = buf->p;
		buf->p += len;
	}

	return ret;
}

static inline void
buf_put_int8(struct cbuf *buf, u8 val, u8* pval)
{
	if (buf_check_size(buf, 1)) {
		buf->p[0] = val;
		buf->p++;

		if (pval)
			*pval = val;
	}
}

static inline void
buf_put_int16(struct cbuf *buf, u16 val, u16 *pval)
{
	if (buf_check_size(buf, 2)) {
		buf->p[0] = val;
		buf->p[1] = val >> 8;
		buf->p += 2;

		if (pval)
			*pval = val;

	}
}

static inline void
buf_put_int32(struct cbuf *buf, u32 val, u32 *pval)
{
	if (buf_check_size(buf, 4)) {
		buf->p[0] = val;
		buf->p[1] = val >> 8;
		buf->p[2] = val >> 16;
		buf->p[3] = val >> 24;
		buf->p += 4;

		if (pval)
			*pval = val;
	}
}

static inline void
buf_put_int64(struct cbuf *buf, u64 val, u64 *pval)
{
	if (buf_check_size(buf, 8)) {
		buf->p[0] = val;
		buf->p[1] = val >> 8;
		buf->p[2] = val >> 16;
		buf->p[3] = val >> 24;
		buf->p[4] = val >> 32;
		buf->p[5] = val >> 40;
		buf->p[6] = val >> 48;
		buf->p[7] = val >> 56;
		buf->p += 8;

		if (pval)
			*pval = val;
	}
}

static inline void
buf_put_str(struct cbuf *buf, char *s, Npstr *ps)
{
	int slen = 0;

	if (s)
		slen = strlen(s);

	if (buf_check_size(buf, 2+slen)) {
		ps->len = slen;
		buf_put_int16(buf, slen, NULL);
		ps->str = buf_alloc(buf, slen);
		memmove(ps->str, s, slen);
	}
}

static inline void
buf_put_qid(struct cbuf *buf, Npqid *qid, Npqid *pqid)
{
	buf_put_int8(buf, qid->type, &pqid->type);
	buf_put_int32(buf, qid->version, &pqid->version);
	buf_put_int64(buf, qid->path, &pqid->path);
}

static inline u8
buf_get_int8(struct cbuf *buf)
{
	u8 ret = 0;

	if (buf_check_size(buf, 1)) {
		ret = buf->p[0];
		buf->p++;
	}

	return ret;
}

static inline u16
buf_get_int16(struct cbuf *buf)
{
	u16 ret = 0;

	if (buf_check_size(buf, 2)) {
		ret = buf->p[0] | (buf->p[1] << 8);
		buf->p += 2;
	}

	return ret;
}

static inline u32
buf_get_int32(struct cbuf *buf)
{
	u32 ret = 0;

	if (buf_check_size(buf, 4)) {
		ret = buf->p[0] | (buf->p[1] << 8) | (buf->p[2] << 16) | 
			(buf->p[3] << 24);
		buf->p += 4;
	}

	return ret;
}

static inline u64
buf_get_int64(struct cbuf *buf)
{
	u64 ret = 0;

	if (buf_check_size(buf, 8)) {
		ret = (u64) buf->p[0] | 
			((u64) buf->p[1] << 8) |
			((u64) buf->p[2] << 16) | 
			((u64) buf->p[3] << 24) |
			((u64) buf->p[4] << 32) | 
			((u64) buf->p[5] << 40) |
			((u64) buf->p[6] << 48) | 
			((u64) buf->p[7] << 56);
		buf->p += 8;
	}

	return ret;
}

static inline void
buf_get_str(struct cbuf *buf, Npstr *str)
{
	str->len = buf_get_int16(buf);
	str->str = buf_alloc(buf, str->len);
}

static inline void
buf_get_qid(struct cbuf *buf, Npqid *qid)
{
	qid->type = buf_get_int8(buf);
	qid->version = buf_get_int32(buf);
	qid->path = buf_get_int64(buf);
}

void
np_strzero(Npstr *str)
{
	str->str = NULL;
	str->len = 0;
}

char *
np_strdup(Npstr *str)
{
	char *ret;

	ret = malloc(str->len + 1);
	if (ret) {
		memmove(ret, str->str, str->len);
		ret[str->len] = '\0';
	}

	return ret;
}

int
np_strcmp(Npstr *str, char *cs)
{
	int ret;

	ret = strncmp(str->str, cs, str->len);
	if (!ret && cs[str->len])
		ret = 1;

	return ret;
}

int
np_strncmp(Npstr *str, char *cs, int len)
{
	int ret;

	if (str->len >= len)
		ret = strncmp(str->str, cs, len);
	else
		ret = np_strcmp(str, cs);

	return ret;
}

void
np_set_tag(Npfcall *fc, u16 tag)
{
	fc->tag = tag;
	fc->pkt[5] = tag;
	fc->pkt[6] = tag >> 8;
}

static Npfcall *
np_create_common(struct cbuf *bufp, u32 size, u8 id)
{
	Npfcall *fc;

	size += 4 + 1 + 2; /* size[4] id[1] tag[2] */
	fc = malloc(sizeof(Npfcall) + size);
	if (!fc)
		return NULL;

	fc->pkt = (u8 *) fc + sizeof(*fc);
	buf_init(bufp, (char *) fc->pkt, size);
	buf_put_int32(bufp, size, &fc->size);
	buf_put_int8(bufp, id, &fc->type);
	buf_put_int16(bufp, P9_NOTAG, &fc->tag);

	return fc;
}

static Npfcall *
np_post_check(Npfcall *fc, struct cbuf *bufp)
{
	if (buf_check_overflow(bufp)) {
		//fprintf(stderr, "buffer overflow\n");
		free (fc);
		return NULL;
	}

//	fprintf(stderr, "serialize dump: ");
//	dumpdata(fc->pkt, fc->size);
	return fc;
}

Npfcall *
np_create_tversion(u32 msize, char *version)
{
        int size;
        Npfcall *fc;
        struct cbuf buffer;
        struct cbuf *bufp;

        bufp = &buffer;
        size = 4 + 2 + strlen(version); /* msize[4] version[s] */
        fc = np_create_common(bufp, size, P9_TVERSION);
        if (!fc)
                return NULL;

        buf_put_int32(bufp, msize, &fc->u.tversion.msize);
        buf_put_str(bufp, version, &fc->u.tversion.version);

        return np_post_check(fc, bufp);
}

Npfcall *
np_create_rversion(u32 msize, char *version)
{
	int size;
	Npfcall *fc;
	struct cbuf buffer;
	struct cbuf *bufp;

	bufp = &buffer;
	size = 4 + 2 + strlen(version); /* msize[4] version[s] */
	fc = np_create_common(bufp, size, P9_RVERSION);
	if (!fc)
		return NULL;

	buf_put_int32(bufp, msize, &fc->u.rversion.msize);
	buf_put_str(bufp, version, &fc->u.rversion.version);

	return np_post_check(fc, bufp);
}

Npfcall *
np_create_tauth(u32 fid, char *uname, char *aname, u32 n_uname)
{
        int size;
        Npfcall *fc;
        struct cbuf buffer;
        struct cbuf *bufp;

        bufp = &buffer;
        size = 4 + 2 + 2; /* fid[4] uname[s] aname[s] */
        if (uname)
                size += strlen(uname);

        if (aname)
                size += strlen(aname);

        fc = np_create_common(bufp, size, P9_TAUTH);
        if (!fc)
                return NULL;

        buf_put_int32(bufp, fid, &fc->u.tauth.afid);
        buf_put_str(bufp, uname, &fc->u.tauth.uname);
        buf_put_str(bufp, aname, &fc->u.tauth.aname);
        buf_put_int32(bufp, n_uname, &fc->u.tauth.n_uname);

        return np_post_check(fc, bufp);
}

Npfcall *
np_create_rauth(Npqid *aqid)
{
	int size;
	Npfcall *fc;
	struct cbuf buffer;
	struct cbuf *bufp;

	bufp = &buffer;
	size = 13; /* aqid[13] */
	fc = np_create_common(bufp, size, P9_RAUTH);
	if (!fc)
		return NULL;

	buf_put_qid(bufp, aqid, &fc->u.rauth.qid);
	return np_post_check(fc, bufp);
}

Npfcall *
np_create_tflush(u16 oldtag)
{
        int size;
        Npfcall *fc;
        struct cbuf buffer;
        struct cbuf *bufp;

        bufp = &buffer;
        size = 2;
        fc = np_create_common(bufp, size, P9_TFLUSH);
        if (!fc)
                return NULL;

        buf_put_int16(bufp, oldtag, &fc->u.tflush.oldtag);
        return np_post_check(fc, bufp);
}

Npfcall *
np_create_rflush(void)
{
	int size;
	Npfcall *fc;
	struct cbuf buffer;
	struct cbuf *bufp;

	bufp = &buffer;
	size = 0;
	fc = np_create_common(bufp, size, P9_RFLUSH);
	if (!fc)
		return NULL;

	return np_post_check(fc, bufp);
}

Npfcall *
np_create_tattach(u32 fid, u32 afid, char *uname, char *aname, u32 n_uname)
{
        int size;
        Npfcall *fc;
        struct cbuf buffer;
        struct cbuf *bufp;

        bufp = &buffer;
        size = 4 + 4 + 2 + 2; /* fid[4] afid[4] uname[s] aname[s] */
        if (uname)
                size += strlen(uname);

        if (aname)
                size += strlen(aname);

        size += 4; /* n_uname[4] */

        fc = np_create_common(bufp, size, P9_TATTACH);
        if (!fc)
                return NULL;

        buf_put_int32(bufp, fid, &fc->u.tattach.fid);
        buf_put_int32(bufp, afid, &fc->u.tattach.afid);
        buf_put_str(bufp, uname, &fc->u.tattach.uname);
        buf_put_str(bufp, aname, &fc->u.tattach.aname);
        buf_put_int32(bufp, n_uname, &fc->u.tattach.n_uname);
        return np_post_check(fc, bufp);
}

Npfcall *
np_create_rattach(Npqid *qid)
{
	int size;
	Npfcall *fc;
	struct cbuf buffer;
	struct cbuf *bufp;

	bufp = &buffer;
	size = 13; /* qid[13] */
	fc = np_create_common(bufp, size, P9_RATTACH);
	if (!fc)
		return NULL;

	buf_put_qid(bufp, qid, &fc->u.rattach.qid);
	return np_post_check(fc, bufp);
}

Npfcall *
np_create_twalk(u32 fid, u32 newfid, u16 nwname, char **wnames)
{
        int i, size;
        Npfcall *fc;
        struct cbuf buffer;
        struct cbuf *bufp;

        if (nwname > P9_MAXWELEM) {
                fprintf(stderr, "nwqid > P9_MAXWELEM\n");
                return NULL;
        }

        bufp = &buffer;
        size = 4 + 4 + 2 + nwname * 2; /* fid[4] newfid[4] nwname[2] nwname*wname[s] */
        for(i = 0; i < nwname; i++)
                size += strlen(wnames[i]);

        fc = np_create_common(bufp, size, P9_TWALK);
        if (!fc)
                return NULL;

        buf_put_int32(bufp, fid, &fc->u.twalk.fid);
        buf_put_int32(bufp, newfid, &fc->u.twalk.newfid);
        buf_put_int16(bufp, nwname, &fc->u.twalk.nwname);
        for(i = 0; i < nwname; i++)
                buf_put_str(bufp, wnames[i], &fc->u.twalk.wnames[i]);

        return np_post_check(fc, bufp);
}

Npfcall *
np_create_rwalk(int nwqid, Npqid *wqids)
{
	int i, size;
	Npfcall *fc;
	struct cbuf buffer;
	struct cbuf *bufp;

	if (nwqid > P9_MAXWELEM) {
		fprintf(stderr, "nwqid > P9_MAXWELEM\n");
		return NULL;
	}

	bufp = &buffer;
	size = 2 + nwqid*13; /* nwqid[2] nwqid*wqid[13] */
	fc = np_create_common(bufp, size, P9_RWALK);
	if (!fc)
		return NULL;

	buf_put_int16(bufp, nwqid, &fc->u.rwalk.nwqid);
	for(i = 0; i < nwqid; i++) {
		buf_put_qid(bufp, &wqids[i], &fc->u.rwalk.wqids[i]);
	}

	return np_post_check(fc, bufp);
}

Npfcall *
np_create_tread(u32 fid, u64 offset, u32 count)
{
        int size;
        Npfcall *fc;
        struct cbuf buffer;
        struct cbuf *bufp;

        bufp = &buffer;
        size = 4 + 8 + 4; /* fid[4] offset[8] count[4] */
        fc = np_create_common(bufp, size, P9_TREAD);
        if (!fc)
                return NULL;

        buf_put_int32(bufp, fid, &fc->u.tread.fid);
        buf_put_int64(bufp, offset, &fc->u.tread.offset);
        buf_put_int32(bufp, count, &fc->u.tread.count);
        return np_post_check(fc, bufp);
}

Npfcall *
np_alloc_rread(u32 count)
{
	int size;
	Npfcall *fc;
	struct cbuf buffer;
	struct cbuf *bufp;
	void *p;

	bufp = &buffer;
	size = 4 + count; /* count[4] data[count] */
	fc = np_create_common(bufp, size, P9_RREAD);
	if (!fc)
		return NULL;

	buf_put_int32(bufp, count, &fc->u.rread.count);
	p = buf_alloc(bufp, count);
	fc->u.rread.data = p;

	return np_post_check(fc, bufp);
}

Npfcall *
np_create_rread(u32 count, u8* data)
{
	Npfcall *fc;

	fc = np_alloc_rread(count);
	if (fc->u.rread.data)
		memmove(fc->u.rread.data, data, count);

	return fc;
}

void
np_set_rread_count(Npfcall *fc, u32 count)
{
	int size;
	struct cbuf buffer;
	struct cbuf *bufp;

	assert(count <= fc->u.rread.count);
	bufp = &buffer;
	size = 4 + 1 + 2 + 4 + count; /* size[4] id[1] tag[2] count[4] data[count] */

	buf_init(bufp, (char *) fc->pkt, size);
	buf_put_int32(bufp, size, &fc->size);
	buf_init(bufp, (char *) fc->pkt + 7, size - 7);
	buf_put_int32(bufp, count, &fc->u.rread.count);
}

Npfcall *
np_create_twrite(u32 fid, u64 offset, u32 count, u8 *data)
{
        int size;
        Npfcall *fc;
        struct cbuf buffer;
        struct cbuf *bufp;
        void *p;

        bufp = &buffer;
        size = 4 + 8 + 4 + count; /* fid[4] offset[8] count[4] data[count] */
        fc = np_create_common(bufp, size, P9_TWRITE);
        if (!fc)
                return NULL;

        buf_put_int32(bufp, fid, &fc->u.twrite.fid);
        buf_put_int64(bufp, offset, &fc->u.twrite.offset);
        buf_put_int32(bufp, count, &fc->u.twrite.count);
        p = buf_alloc(bufp, count);
        fc->u.twrite.data = p;
        if (fc->u.twrite.data)
                memmove(fc->u.twrite.data, data, count);

        return np_post_check(fc, bufp);
}

Npfcall *
np_create_rwrite(u32 count)
{
	int size;
	Npfcall *fc;
	struct cbuf buffer;
	struct cbuf *bufp;

	bufp = &buffer;
	size = 4; /* count[4] */
	fc = np_create_common(bufp, size, P9_RWRITE);
	if (!fc)
		return NULL;

	buf_put_int32(bufp, count, &fc->u.rwrite.count);

	return np_post_check(fc, bufp);
}

Npfcall *
np_create_tclunk(u32 fid)
{
        int size;
        Npfcall *fc;
        struct cbuf buffer;
        struct cbuf *bufp;

        bufp = &buffer;
        size = 4;       /* fid[4] */
        fc = np_create_common(bufp, size, P9_TCLUNK);
        if (!fc)
                return NULL;

        buf_put_int32(bufp, fid, &fc->u.tclunk.fid);
        return np_post_check(fc, bufp);
}

Npfcall *
np_create_rclunk(void)
{
	int size;
	Npfcall *fc;
	struct cbuf buffer;
	struct cbuf *bufp;

	bufp = &buffer;
	size = 0;
	fc = np_create_common(bufp, size, P9_RCLUNK);
	if (!fc)
		return NULL;

	return np_post_check(fc, bufp);
}

Npfcall *
np_create_tremove(u32 fid)
{
        int size;
        Npfcall *fc;
        struct cbuf buffer;
        struct cbuf *bufp;

        bufp = &buffer;
        size = 4;       /* fid[4] */
        fc = np_create_common(bufp, size, P9_TREMOVE);
        if (!fc)
                return NULL;

        buf_put_int32(bufp, fid, &fc->u.tremove.fid);
        return np_post_check(fc, bufp);
}

Npfcall *
np_create_rremove(void)
{
	int size;
	Npfcall *fc;
	struct cbuf buffer;
	struct cbuf *bufp;

	bufp = &buffer;
	size = 0;
	fc = np_create_common(bufp, size, P9_RREMOVE);

	return np_post_check(fc, bufp);
}

#if HAVE_LARGEIO
/* N.B. srv->aread handler should
 * 1. call np_create_raread()
 * 2. fill in fc->data
 * 3. call np_finalize_raread() 
*/

Npfcall *
np_create_taread(u32 fid, u8 datacheck, u64 offset, u32 count, u32 rsize)
{
        Npfcall *fc;
        struct cbuf buffer;
	struct cbuf *bufp = &buffer;
	int size = sizeof(u32) + sizeof(u8) + sizeof(u64) + sizeof(u32)
			+ sizeof(u32);

        if (!(fc = np_create_common(bufp, size, P9_TAREAD)))
                return NULL;

        buf_put_int32(bufp, fid, &fc->u.taread.fid);
        buf_put_int8(bufp, datacheck, &fc->u.taread.datacheck);
        buf_put_int64(bufp, offset, &fc->u.taread.offset);
        buf_put_int32(bufp, count, &fc->u.taread.count);
        buf_put_int32(bufp, rsize, &fc->u.taread.rsize);

        return np_post_check(fc, bufp);
}

Npfcall *
np_create_raread(u32 count)
{
	Npfcall *fc;
	struct cbuf buffer;
	struct cbuf *bufp = &buffer;
	int size = sizeof(u32) + count + sizeof(u32);
	void *p;

	fc = np_create_common(bufp, size, P9_RAREAD);
	if (!fc)
		return NULL;

	buf_put_int32(bufp, count, &fc->u.raread.count);
	p = buf_alloc(bufp, count);
	fc->u.raread.data = p;

	return np_post_check(fc, bufp);
}

void
np_finalize_raread(Npfcall *fc, u32 count, u8 datacheck)
{
	int size = sizeof(u32) + sizeof(u8) + sizeof(u16)
			+ sizeof(u32) + count + sizeof(u32);
	struct cbuf buffer;
	struct cbuf *bufp = &buffer;
	u32 check = 0;
	void *p;

	assert(count <= fc->u.raread.count);
	buf_init(bufp, (char *) fc->pkt, size);
	buf_put_int32(bufp, size, &fc->size);
	buf_init(bufp, (char *) fc->pkt + 7, size - 7);
	buf_put_int32(bufp, count, &fc->u.raread.count);
	p = buf_alloc(bufp, count);
	if (datacheck == P9_CHECK_ADLER32) {
		check = adler32(0L, Z_NULL, 0);
		check = adler32(check, p, count);
	}
	buf_put_int32(bufp, check, &fc->u.raread.check);
}

Npfcall *
np_create_tawrite(u32 fid, u8 datacheck, u64 offset, u32 count, u32 rsize,
                  u8 *data)
{
        Npfcall *fc;
        struct cbuf buffer;
        struct cbuf *bufp = &buffer;
        int size = sizeof(u32) + sizeof(u8) + sizeof(u64) + sizeof(u32)
			+ sizeof(u32) + rsize;
        void *p;

        if (!(fc = np_create_common(bufp, size, P9_TAWRITE)))
                return NULL;

        buf_put_int32(bufp, fid, &fc->u.tawrite.fid);
        buf_put_int8(bufp, datacheck, &fc->u.tawrite.datacheck);
        buf_put_int64(bufp, offset, &fc->u.tawrite.offset);
        buf_put_int32(bufp, count, &fc->u.tawrite.count);
        buf_put_int32(bufp, rsize, &fc->u.tawrite.rsize);
        p = buf_alloc(bufp, rsize);
        fc->u.tawrite.data = p;
        if (fc->u.tawrite.data)
                memmove(fc->u.tawrite.data, data, rsize);

        return np_post_check(fc, bufp);
}

Npfcall *
np_create_rawrite(u32 count)
{
	int size = sizeof(u32);
	Npfcall *fc;
	struct cbuf buffer;
	struct cbuf *bufp = &buffer;

	fc = np_create_common(bufp, size, P9_RAWRITE);
	if (!fc)
		return NULL;

	buf_put_int32(bufp, count, &fc->u.rawrite.count);

	return np_post_check(fc, bufp);
}
#endif

Npfcall *
np_create_rlerror(u32 ecode)
{
	int size = sizeof(u32);
	struct cbuf buffer;
	struct cbuf *bufp = &buffer;
	Npfcall *fc;

	if (!(fc = np_create_common(bufp, size, P9_RLERROR)))
		return NULL;

	buf_put_int32(bufp, ecode, &fc->u.rlerror.ecode);

	return np_post_check(fc, bufp);
}

Npfcall *
np_create_rstatfs(u32 type, u32 bsize, u64 blocks, u64 bfree, u64 bavail, u64 files, u64 ffree, u64 fsid, u32 namelen)
{
	struct cbuf buffer;
	struct cbuf *bufp = &buffer;
	int size = 2*sizeof(u32) + 6*sizeof(u64) + sizeof(u32);
	Npfcall *fc; 

	if (!(fc = np_create_common(bufp, size, P9_RSTATFS)))
		return NULL;

	buf_put_int32(bufp, type,    &fc->u.rstatfs.type);	
	buf_put_int32(bufp, bsize,   &fc->u.rstatfs.bsize);
	buf_put_int64(bufp, blocks,  &fc->u.rstatfs.blocks);
	buf_put_int64(bufp, bfree,   &fc->u.rstatfs.bfree);
	buf_put_int64(bufp, bavail,  &fc->u.rstatfs.bavail);
	buf_put_int64(bufp, files,   &fc->u.rstatfs.files);
	buf_put_int64(bufp, ffree,   &fc->u.rstatfs.ffree);
	buf_put_int64(bufp, fsid,    &fc->u.rstatfs.fsid);
	buf_put_int32(bufp, namelen, &fc->u.rstatfs.namelen);

	return np_post_check(fc, bufp);
}

Npfcall *
np_create_tlopen(u32 fid, u32 mode)
{
        struct cbuf buffer;
        struct cbuf *bufp = &buffer;
        int size = sizeof(u32) + sizeof(u32);
        Npfcall *fc;

        if (!(fc = np_create_common(bufp, size, P9_TLOPEN)))
                return NULL;

        buf_put_int32(bufp, fid, &fc->u.tlopen.fid);
        buf_put_int32(bufp, mode, &fc->u.tlopen.mode);

        return np_post_check(fc, bufp);
}

Npfcall *
np_create_rlopen(Npqid *qid, u32 iounit)
{
	int size = sizeof(*qid) + sizeof(u32);
	struct cbuf buffer;
	struct cbuf *bufp = &buffer;
	Npfcall *fc;

	if (!(fc = np_create_common(bufp, size, P9_RLOPEN)))
		return NULL;

	buf_put_qid(bufp, qid, &fc->u.rlopen.qid);
	buf_put_int32(bufp, iounit, &fc->u.rlopen.iounit);

	return np_post_check(fc, bufp);
}

Npfcall *
np_create_tlcreate(u32 fid, char *name, u32 flags, u32 mode, u32 gid)
{
        struct cbuf buffer;
        struct cbuf *bufp = &buffer;
        int size = sizeof(u32) + sizeof(u32) + strlen(name) + sizeof(u32)
		 + sizeof(u32) + sizeof(u32);
        Npfcall *fc;

        if (!(fc = np_create_common(bufp, size, P9_TLCREATE)))
                return NULL;

        buf_put_int32(bufp, fid, &fc->u.tlcreate.fid);
        buf_put_str(bufp, name, &fc->u.tlcreate.name);
        buf_put_int32(bufp, fid, &fc->u.tlcreate.flags);
        buf_put_int32(bufp, fid, &fc->u.tlcreate.mode);
        buf_put_int32(bufp, fid, &fc->u.tlcreate.gid);

        return np_post_check(fc, bufp);
}

Npfcall *
np_create_rlcreate(struct p9_qid *qid, u32 iounit)
{
	int size = sizeof(*qid) + sizeof(u32);
	Npfcall *fc;
	struct cbuf buffer;
	struct cbuf *bufp = &buffer;

	if (!(fc = np_create_common(bufp, size, P9_RLCREATE)))
		return NULL;
	buf_put_qid(bufp, qid, &fc->u.rlcreate.qid);
	buf_put_int32(bufp, iounit, &fc->u.rlcreate.iounit);

	return np_post_check(fc, bufp);
}

Npfcall *
np_create_rsymlink(struct p9_qid *qid)
{
	int size = sizeof(*qid);
	Npfcall *fc;
	struct cbuf buffer;
	struct cbuf *bufp = &buffer;

	if (!(fc = np_create_common(bufp, size, P9_RSYMLINK)))
		return NULL;
	buf_put_qid(bufp, qid, &fc->u.rsymlink.qid);

	return np_post_check(fc, bufp);
}

Npfcall *
np_create_rmknod (struct p9_qid *qid)
{
	int size = sizeof(*qid);
	Npfcall *fc;
	struct cbuf buffer;
	struct cbuf *bufp = &buffer;

	if (!(fc = np_create_common(bufp, size, P9_RMKNOD)))
		return NULL;
	buf_put_qid(bufp, qid, &fc->u.rmknod.qid);

	return np_post_check(fc, bufp);
}

Npfcall *
np_create_rrename(void)
{
	Npfcall *fc;
	struct cbuf buffer;
	struct cbuf *bufp = &buffer;

	if (!(fc = np_create_common(bufp, 0, P9_RRENAME)))
		return NULL;

	return np_post_check(fc, bufp);
}

Npfcall *
np_create_rreadlink(char *target)
{
	int size = strlen(target) + 2;
	Npfcall *fc;
	struct cbuf buffer;
	struct cbuf *bufp = &buffer;

	if (!(fc = np_create_common(bufp, size, P9_RREADLINK)))
		return NULL;
	buf_put_str(bufp, target, &fc->u.rreadlink.target);

	return np_post_check(fc, bufp);
}

Npfcall *
np_create_rgetattr(u64 valid, struct p9_qid *qid, u32 mode,
  		u32 uid, u32 gid, u64 nlink, u64 rdev,
		u64 size, u64 blksize, u64 blocks,
		u64 atime_sec, u64 atime_nsec,
		u64 mtime_sec, u64 mtime_nsec,
		u64 ctime_sec, u64 ctime_nsec,
		u64 btime_sec, u64 btime_nsec,
		u64 gen, u64 data_version)
{
	int bufsize = sizeof(u64) + sizeof(*qid) + 3*sizeof(u32) + 15*sizeof(u64);
	Npfcall *fc;
	struct cbuf buffer;
	struct cbuf *bufp = &buffer;

	if (!(fc = np_create_common(bufp, bufsize, P9_RGETATTR)))
		return NULL;

	buf_put_int64(bufp, valid, &fc->u.rgetattr.valid);
	buf_put_qid(bufp, qid, &fc->u.rgetattr.qid);
	buf_put_int32(bufp, mode, &fc->u.rgetattr.mode);
	buf_put_int32(bufp, uid, &fc->u.rgetattr.uid);
	buf_put_int32(bufp, gid, &fc->u.rgetattr.gid);
	buf_put_int64(bufp, nlink, &fc->u.rgetattr.nlink);
	buf_put_int64(bufp, rdev, &fc->u.rgetattr.rdev);
	buf_put_int64(bufp, size, &fc->u.rgetattr.size);
	buf_put_int64(bufp, blksize, &fc->u.rgetattr.blksize);
	buf_put_int64(bufp, blocks, &fc->u.rgetattr.blocks);
	buf_put_int64(bufp, atime_sec, &fc->u.rgetattr.atime_sec);
	buf_put_int64(bufp, atime_nsec, &fc->u.rgetattr.atime_nsec);
	buf_put_int64(bufp, mtime_sec, &fc->u.rgetattr.mtime_sec);
	buf_put_int64(bufp, mtime_nsec, &fc->u.rgetattr.mtime_nsec);
	buf_put_int64(bufp, ctime_sec, &fc->u.rgetattr.ctime_sec);
	buf_put_int64(bufp, ctime_nsec, &fc->u.rgetattr.ctime_nsec);
	buf_put_int64(bufp, btime_sec, &fc->u.rgetattr.btime_sec);
	buf_put_int64(bufp, btime_nsec, &fc->u.rgetattr.btime_nsec);
	buf_put_int64(bufp, gen, &fc->u.rgetattr.gen);
	buf_put_int64(bufp, data_version, &fc->u.rgetattr.data_version);

	return np_post_check(fc, bufp);
}

Npfcall *
np_create_rsetattr(void)
{
	Npfcall *fc;
	struct cbuf buffer;
	struct cbuf *bufp = &buffer;

	if (!(fc = np_create_common(bufp, 0, P9_RSETATTR)))
		return NULL;

	return np_post_check(fc, bufp);
}

/* FIXME: Npfcall * np_create_rxattrwalk() */
/* FIXME: Npfcall * np_create_rxattrcreate() */

/* srv->readdir () should:
 * 1) call np_alloc_rreaddir ()
 * 2) copy up to count bytes of dirent data in u.readdir.data
 * 3) call np_set_rreaddir_count () to set actual byte count
 */
Npfcall *
np_create_rreaddir(u32 count)
{
	Npfcall *fc;
	struct cbuf buffer;
	struct cbuf *bufp = &buffer;
	int size = sizeof(u32) + count;

	if (!(fc = np_create_common(bufp, size, P9_RREADDIR)))
		return NULL;
	buf_put_int32(bufp, count, &fc->u.rreaddir.count);
	fc->u.rreaddir.data = buf_alloc(bufp, count);

	return np_post_check(fc, bufp);
}

void
np_finalize_rreaddir(Npfcall *fc, u32 count)
{
	struct cbuf buffer;
	struct cbuf *bufp = &buffer;
	int size = sizeof(u32) + sizeof(u8) + sizeof(u16) + sizeof(u32) + count;

	assert(count <= fc->u.rreaddir.count);

	buf_init(bufp, (char *) fc->pkt, size);
	buf_put_int32(bufp, size, &fc->size);
	buf_init(bufp, (char *) fc->pkt + 7, size - 7);
	buf_put_int32(bufp, count, &fc->u.rreaddir.count);
}

Npfcall *
np_create_rfsync(void)
{
	Npfcall *fc;
	struct cbuf buffer;
	struct cbuf *bufp = &buffer;

	if (!(fc = np_create_common(bufp, 0, P9_RFSYNC)))
		return NULL;

	return np_post_check(fc, bufp);
}

Npfcall *
np_create_rlock(u8 status)
{
	int size = sizeof(u8);
	Npfcall *fc;
	struct cbuf buffer;
	struct cbuf *bufp = &buffer;

	if (!(fc = np_create_common(bufp, size, P9_RLOCK)))
		return NULL;
	buf_put_int8(bufp, status, &fc->u.rlock.status);

	return np_post_check(fc, bufp);
}

Npfcall *
np_create_rgetlock(u8 type, u64 start, u64 length, u32 proc_id, char *client_id)
{
	int size = sizeof(u8) + sizeof(u64) + sizeof(u64) + sizeof(u32)
			+ strlen(client_id) + 2;
	Npfcall *fc;
	struct cbuf buffer;
	struct cbuf *bufp = &buffer;

	if (!(fc = np_create_common(bufp, size, P9_RGETLOCK)))
		return NULL;
	buf_put_int8(bufp, type, &fc->u.rgetlock.type);
	buf_put_int64(bufp, start, &fc->u.rgetlock.start);
	buf_put_int64(bufp, length, &fc->u.rgetlock.length);
	buf_put_int32(bufp, proc_id, &fc->u.rgetlock.proc_id);
	buf_put_str(bufp, client_id, &fc->u.rgetlock.client_id);

	return np_post_check(fc, bufp);
}

Npfcall *
np_create_rlink(void)
{
	Npfcall *fc;
	struct cbuf buffer;
	struct cbuf *bufp = &buffer;

	if (!(fc = np_create_common(bufp, 0, P9_RLINK)))
		return NULL;

	return np_post_check(fc, bufp);
}

Npfcall *
np_create_rmkdir(struct p9_qid *qid)
{
	int size = sizeof(*qid);
	Npfcall *fc;
	struct cbuf buffer;
	struct cbuf *bufp = &buffer;

	if (!(fc = np_create_common(bufp, size, P9_RMKDIR)))
		return NULL;
	buf_put_qid(bufp, qid, &fc->u.rmkdir.qid);

	return np_post_check(fc, bufp);
}

int
np_deserialize(Npfcall *fc, u8 *data)
{
	int i;
	struct cbuf buffer;
	struct cbuf *bufp;

	bufp = &buffer;
	buf_init(bufp, data, 4);
	fc->size = buf_get_int32(bufp);

//	fprintf(stderr, "deserialize dump: ");
//	dumpdata(data, fc->size);

	buf_init(bufp, data + 4, fc->size - 4);
	fc->type = buf_get_int8(bufp);
	fc->tag = buf_get_int16(bufp);

	switch (fc->type) {
	default:
		fprintf(stderr, "unhandled op: %d\n", fc->type);
		fflush(stderr);
		goto error;
	case P9_RLERROR:
		fc->u.rlerror.ecode = buf_get_int32(bufp);
		break;
	case P9_TVERSION:
		fc->u.tversion.msize = buf_get_int32(bufp);
		buf_get_str(bufp, &fc->u.tversion.version);
		break;
	case P9_RVERSION:
                fc->u.rversion.msize = buf_get_int32(bufp);
                buf_get_str(bufp, &fc->u.rversion.version);
                break;
	case P9_TAUTH:
		fc->u.tauth.afid = buf_get_int32(bufp);
		buf_get_str(bufp, &fc->u.tauth.uname);
		buf_get_str(bufp, &fc->u.tauth.aname);
		fc->u.tauth.n_uname = buf_get_int32(bufp); /* .u extension */
		break;
	case P9_RAUTH:
                buf_get_qid(bufp, &fc->u.rauth.qid);
                break;
	case P9_TFLUSH:
		fc->u.tflush.oldtag = buf_get_int16(bufp);
		break;
	case P9_RFLUSH:
		break;
	case P9_TATTACH:
		fc->u.tattach.fid = buf_get_int32(bufp);
		fc->u.tattach.afid = buf_get_int32(bufp);
		buf_get_str(bufp, &fc->u.tattach.uname);
		buf_get_str(bufp, &fc->u.tattach.aname);
		fc->u.tattach.n_uname = buf_get_int32(bufp); /* .u extension */
		break;
	case P9_RATTACH:
		buf_get_qid(bufp, &fc->u.rattach.qid);
                break;
	case P9_TWALK:
		fc->u.twalk.fid = buf_get_int32(bufp);
		fc->u.twalk.newfid = buf_get_int32(bufp);
		fc->u.twalk.nwname = buf_get_int16(bufp);
		if (fc->u.twalk.nwname > P9_MAXWELEM)
			goto error;

		for(i = 0; i < fc->u.twalk.nwname; i++) {
			buf_get_str(bufp, &fc->u.twalk.wnames[i]);
		}
		break;
	case P9_RWALK:
		fc->u.rwalk.nwqid = buf_get_int16(bufp);
                if (fc->u.rwalk.nwqid > P9_MAXWELEM)
                        goto error;
                for(i = 0; i < fc->u.rwalk.nwqid; i++)
                        buf_get_qid(bufp, &fc->u.rwalk.wqids[i]);
                break;
	case P9_TREAD:
		fc->u.tread.fid = buf_get_int32(bufp);
		fc->u.tread.offset = buf_get_int64(bufp);
		fc->u.tread.count = buf_get_int32(bufp);
		break;
	case P9_RREAD:
		fc->u.rread.count = buf_get_int32(bufp);
                fc->u.rread.data = buf_alloc(bufp, fc->u.rread.count);
                break;
	case P9_TWRITE:
		fc->u.twrite.fid = buf_get_int32(bufp);
		fc->u.twrite.offset = buf_get_int64(bufp);
		fc->u.twrite.count = buf_get_int32(bufp);
		fc->u.twrite.data = buf_alloc(bufp, fc->u.twrite.count);
		break;
	case P9_RWRITE:
		fc->u.rwrite.count = buf_get_int32(bufp);
                break;
	case P9_TCLUNK:
		fc->u.tclunk.fid = buf_get_int32(bufp);
		break;
	case P9_RCLUNK:
		break;
	case P9_TREMOVE:
		fc->u.tremove.fid = buf_get_int32(bufp);
		break;
	case P9_RREMOVE:
		break;
#if HAVE_LARGEIO
	case P9_TAREAD:
		fc->u.taread.fid = buf_get_int32(bufp);
		fc->u.taread.datacheck = buf_get_int8(bufp);
		fc->u.taread.offset = buf_get_int64(bufp);
		fc->u.taread.count = buf_get_int32(bufp);
		fc->u.taread.rsize = buf_get_int32(bufp);
		break;
	case P9_RAREAD:
		fc->u.raread.count = buf_get_int32(bufp);
                fc->u.raread.data = buf_alloc(bufp, fc->u.raread.count);
                fc->u.raread.check = buf_get_int32(bufp);
                break;
	case P9_TAWRITE:
		fc->u.tawrite.fid = buf_get_int32(bufp);
		fc->u.tawrite.datacheck = buf_get_int8(bufp);
		fc->u.tawrite.offset = buf_get_int64(bufp);
		fc->u.tawrite.count = buf_get_int32(bufp);
		fc->u.tawrite.rsize = buf_get_int32(bufp);
		fc->u.tawrite.data = buf_alloc(bufp, fc->u.tawrite.rsize);
		fc->u.tawrite.check = buf_get_int32(bufp);
		break;
	case P9_RAWRITE:
                fc->u.rawrite.count = buf_get_int32(bufp);
                break;
#endif
	case P9_TSTATFS:
		fc->u.tstatfs.fid = buf_get_int32(bufp);
		break;
	case P9_RSTATFS:
       		fc->u.rstatfs.type = buf_get_int32(bufp);
                fc->u.rstatfs.bsize = buf_get_int32(bufp);
                fc->u.rstatfs.blocks = buf_get_int64(bufp);
                fc->u.rstatfs.bfree = buf_get_int64(bufp);
                fc->u.rstatfs.bavail = buf_get_int64(bufp);
                fc->u.rstatfs.files = buf_get_int64(bufp);
                fc->u.rstatfs.ffree = buf_get_int64(bufp);
                fc->u.rstatfs.fsid = buf_get_int64(bufp);
                fc->u.rstatfs.namelen = buf_get_int32(bufp);
                break;
	case P9_TLOPEN:
		fc->u.tlopen.fid = buf_get_int32(bufp);
		fc->u.tlopen.mode = buf_get_int32(bufp);
		break;
	case P9_RLOPEN:
		buf_get_qid(bufp, &fc->u.rlopen.qid);
                fc->u.rlopen.iounit = buf_get_int32(bufp);
		break;
	case P9_TLCREATE:
		fc->u.tlcreate.fid = buf_get_int32(bufp);
		buf_get_str(bufp, &fc->u.tlcreate.name);
		fc->u.tlcreate.flags = buf_get_int32(bufp);
		fc->u.tlcreate.mode = buf_get_int32(bufp);
		fc->u.tlcreate.gid = buf_get_int32(bufp);
		break;
	case P9_RLCREATE:
		buf_get_qid(bufp, &fc->u.rlcreate.qid);
                fc->u.rlcreate.iounit = buf_get_int32(bufp);
		break;
	case P9_TSYMLINK:
		fc->u.tsymlink.fid = buf_get_int32(bufp);
		buf_get_str(bufp, &fc->u.tsymlink.name);
		buf_get_str(bufp, &fc->u.tsymlink.symtgt);
		fc->u.tsymlink.gid = buf_get_int32(bufp);
		break;
	case P9_RSYMLINK:
		buf_get_qid(bufp, &fc->u.rsymlink.qid);
		break;
	case P9_TMKNOD:
		fc->u.tmknod.fid = buf_get_int32(bufp);
		buf_get_str(bufp, &fc->u.tmknod.name);
		fc->u.tmknod.mode = buf_get_int32(bufp);
		fc->u.tmknod.major = buf_get_int32(bufp);
		fc->u.tmknod.minor = buf_get_int32(bufp);
		fc->u.tmknod.gid = buf_get_int32(bufp);
		break;
	case P9_RMKNOD:
		buf_get_qid(bufp, &fc->u.rmknod.qid);
		break;
	case P9_TRENAME:
		fc->u.trename.fid = buf_get_int32(bufp);
		fc->u.trename.dfid = buf_get_int32(bufp);
		buf_get_str(bufp, &fc->u.trename.name);
		break;
	case P9_RRENAME:
		break;
	case P9_TREADLINK:
		fc->u.treadlink.fid = buf_get_int32(bufp);
		break;
	case P9_RREADLINK:
		buf_get_str(bufp, &fc->u.rreadlink.target);
		break;
	case P9_TGETATTR:
		fc->u.tgetattr.fid = buf_get_int32(bufp);
		fc->u.tgetattr.request_mask = buf_get_int64(bufp);
		break;
	case P9_RGETATTR:
		fc->u.rgetattr.valid = buf_get_int64(bufp);
		buf_get_qid(bufp, &fc->u.rgetattr.qid);
		fc->u.rgetattr.mode = buf_get_int32(bufp);
		fc->u.rgetattr.uid = buf_get_int32(bufp);
		fc->u.rgetattr.gid = buf_get_int32(bufp);
		fc->u.rgetattr.nlink = buf_get_int64(bufp);
		fc->u.rgetattr.rdev = buf_get_int64(bufp);
		fc->u.rgetattr.size = buf_get_int64(bufp);
		fc->u.rgetattr.blksize = buf_get_int64(bufp);
		fc->u.rgetattr.blocks = buf_get_int64(bufp);
		fc->u.rgetattr.atime_sec = buf_get_int64(bufp);
		fc->u.rgetattr.atime_nsec = buf_get_int64(bufp);
		fc->u.rgetattr.mtime_sec = buf_get_int64(bufp);
		fc->u.rgetattr.mtime_nsec = buf_get_int64(bufp);
		fc->u.rgetattr.ctime_sec = buf_get_int64(bufp);
		fc->u.rgetattr.ctime_nsec = buf_get_int64(bufp);
		fc->u.rgetattr.btime_sec = buf_get_int64(bufp);
		fc->u.rgetattr.btime_nsec = buf_get_int64(bufp);
		fc->u.rgetattr.gen = buf_get_int64(bufp);
		fc->u.rgetattr.data_version = buf_get_int64(bufp);
		break;
	case P9_TSETATTR:
		fc->u.tsetattr.fid = buf_get_int32(bufp);
		fc->u.tsetattr.valid = buf_get_int32(bufp);
		fc->u.tsetattr.mode = buf_get_int32(bufp);
		fc->u.tsetattr.uid = buf_get_int32(bufp);
		fc->u.tsetattr.gid = buf_get_int32(bufp);
		fc->u.tsetattr.size = buf_get_int64(bufp);
		fc->u.tsetattr.atime_sec = buf_get_int64(bufp);
		fc->u.tsetattr.atime_nsec = buf_get_int64(bufp);
		fc->u.tsetattr.mtime_sec = buf_get_int64(bufp);
		fc->u.tsetattr.mtime_nsec = buf_get_int64(bufp);
		break;
	case P9_RSETATTR:
		break;
	case P9_TXATTRWALK:
	case P9_RXATTRWALK:
	case P9_TXATTRCREATE:
	case P9_RXATTRCREATE:
		assert(0); /* FIXME */
		break;
	case P9_TREADDIR:
		fc->u.treaddir.fid = buf_get_int32(bufp);
		fc->u.treaddir.offset = buf_get_int64(bufp);
		fc->u.treaddir.count = buf_get_int32(bufp);
		break;
	case P9_RREADDIR:
		fc->u.rreaddir.count = buf_get_int32(bufp);
                fc->u.rreaddir.data = buf_alloc(bufp, fc->u.rreaddir.count);
		break;
	case P9_TFSYNC:
		fc->u.tfsync.fid = buf_get_int32(bufp);
		break;
	case P9_RFSYNC:
		break;
	case P9_TLOCK:
		fc->u.tlock.fid = buf_get_int32(bufp);
		fc->u.tlock.type = buf_get_int8(bufp);	
		fc->u.tlock.flags = buf_get_int32(bufp);	
		fc->u.tlock.start = buf_get_int64(bufp);	
		fc->u.tlock.length = buf_get_int64(bufp);	
		fc->u.tlock.proc_id = buf_get_int32(bufp);	
		buf_get_str(bufp, &fc->u.tlock.client_id);	
		break;
	case P9_RLOCK:
		fc->u.rlock.status = buf_get_int8(bufp);
		break;
	case P9_TGETLOCK:
		fc->u.tgetlock.fid = buf_get_int32(bufp);
		fc->u.tgetlock.type = buf_get_int8(bufp);	
		fc->u.tgetlock.start = buf_get_int64(bufp);	
		fc->u.tgetlock.length = buf_get_int64(bufp);	
		fc->u.tgetlock.proc_id = buf_get_int32(bufp);	
		buf_get_str(bufp, &fc->u.tgetlock.client_id);	
		break;
	case P9_RGETLOCK:
		fc->u.rgetlock.type = buf_get_int8(bufp);	
		fc->u.rgetlock.start = buf_get_int64(bufp);	
		fc->u.rgetlock.length = buf_get_int64(bufp);	
		fc->u.rgetlock.proc_id = buf_get_int32(bufp);	
		buf_get_str(bufp, &fc->u.rgetlock.client_id);	
		break;
	case P9_TLINK:
		fc->u.tlink.dfid = buf_get_int32(bufp);
		fc->u.tlink.fid = buf_get_int32(bufp);
		buf_get_str(bufp, &fc->u.tlink.name);
		break;
	case P9_RLINK:
		break;
	case P9_TMKDIR:
		fc->u.tmkdir.fid = buf_get_int32(bufp);
		buf_get_str(bufp, &fc->u.tmkdir.name);
		fc->u.tmkdir.mode = buf_get_int32(bufp);
		fc->u.tmkdir.gid = buf_get_int32(bufp);
		break;
	case P9_RMKDIR:
		buf_get_qid(bufp, &fc->u.rmkdir.qid);
		break;	
	}

	if (buf_check_overflow(bufp))
		goto error;

	return fc->size;

error:
	return 0;
}

int
np_serialize_p9dirent(Npqid *qid, u64 offset, u8 type, char *name,
		      u8 *buf, int buflen)
{
	struct cbuf buffer;
	struct cbuf *bufp = &buffer;
	int size = sizeof(*qid) + sizeof(u64) + sizeof(u8) + strlen(name) + 2;
	Npstr nstr;
	Npqid nqid;

	if (size > buflen)
		return 0;
	buf_init(bufp, buf, buflen);
	buf_put_qid(bufp, qid, &nqid);
	buf_put_int64(bufp, offset, NULL);
	buf_put_int8(bufp, type, NULL);
	buf_put_str(bufp, name, &nstr);
	
	if (buf_check_overflow(bufp))
		return 0;

	return bufp->p - bufp->sp;
}
