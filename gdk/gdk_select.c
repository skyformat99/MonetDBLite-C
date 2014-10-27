/*
 * The contents of this file are subject to the MonetDB Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.monetdb.org/Legal/MonetDBLicense
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See the
 * License for the specific language governing rights and limitations
 * under the License.
 *
 * The Original Code is the MonetDB Database System.
 *
 * The Initial Developer of the Original Code is CWI.
 * Portions created by CWI are Copyright (C) 1997-July 2008 CWI.
 * Copyright August 2008-2014 MonetDB B.V.
 * All Rights Reserved.
 */

#include "monetdb_config.h"
#include "gdk.h"
#include "gdk_private.h"
#include <math.h>

#ifdef _MSC_VER
#define nextafter	_nextafter
float nextafterf(float x, float y);
#endif

/* auxiliary functions and structs for imprints */
#include "gdk_imprints.h"

#define buninsfix(B,A,I,V,G,M,R)				\
	do {							\
		if ((I) == BATcapacity(B)) {			\
			BATsetcount((B), (I));			\
			if (BATextend((B),			\
				      MIN(BATcapacity(B) + (G),	\
					  (M))) == NULL) {	\
				BBPreclaim(B);			\
				return (R);			\
			}					\
			A = (oid *) Tloc((B), BUNfirst(B));	\
		}						\
		A[(I)] = (V);					\
	} while (0)

BAT *
virtualize(BAT *bn)
{
	/* input must be a valid candidate list or NULL */
	assert(bn == NULL ||
	       (((bn->ttype == TYPE_void && bn->tseqbase != oid_nil) ||
		 bn->ttype == TYPE_oid) &&
		bn->tkey && bn->tsorted));
	/* since bn has unique and strictly ascending tail values, we
	 * can easily check whether the tail is dense */
	if (bn && bn->ttype == TYPE_oid &&
	    (BATcount(bn) == 0 ||
	     * (const oid *) Tloc(bn, BUNfirst(bn)) + BATcount(bn) - 1 ==
	     * (const oid *) Tloc(bn, BUNlast(bn) - 1))) {
		/* tail is dense, replace by virtual oid */
		ALGODEBUG fprintf(stderr, "#virtualize(bn=%s#"BUNFMT",seq="OIDFMT")\n",
				  BATgetId(bn), BATcount(bn),
				  * (const oid *) Tloc(bn, BUNfirst(bn)));
		if (BATcount(bn) == 0)
			bn->tseqbase = 0;
		else
			bn->tseqbase = * (const oid *) Tloc(bn, BUNfirst(bn));
		bn->tdense = 1;
		HEAPfree(&bn->T->heap, 1);
		bn->ttype = TYPE_void;
		bn->tvarsized = 1;
		bn->T->width = 0;
		bn->T->shift = 0;
	}

	return bn;
}

static BAT *
newempty(const char *func)
{
	BAT *bn = BATnew(TYPE_void, TYPE_void, 0, TRANSIENT);
	if (bn == NULL) {
		GDKerror("%s: memory allocation error", func);
		return NULL;
	}
	BATseqbase(bn, 0);
	BATseqbase(BATmirror(bn), 0);
	return bn;
}

static BAT *
doublerange(oid l1, oid h1, oid l2, oid h2)
{
	BAT *bn;
	oid *p;

	assert(l1 <= h1);
	assert(l2 <= h2);
	assert(h1 <= l2);
	if (l1 == h1 || l2 == h2) {
		bn = BATnew(TYPE_void, TYPE_void, h1 - l1 + h2 - l2, TRANSIENT);
		if (bn == NULL)
			return NULL;
		BATsetcount(bn, h1 - l1 + h2 - l2);
		BATseqbase(bn, 0);
		BATseqbase(BATmirror(bn), l1 == h1 ? l2 : l1);
		return bn;
	}
	bn = BATnew(TYPE_void, TYPE_oid, h1 - l1 + h2 - l2, TRANSIENT);
	if (bn == NULL)
		return NULL;
	BATsetcount(bn, h1 - l1 + h2 - l2);
	BATseqbase(bn, 0);
	p = (oid *) Tloc(bn, BUNfirst(bn));
	while (l1 < h1)
		*p++ = l1++;
	while (l2 < h2)
		*p++ = l2++;
	bn->tkey = 1;
	bn->tsorted = 1;
	bn->trevsorted = BATcount(bn) <= 1;
	bn->T->nil = 0;
	bn->T->nonil = 1;
	return bn;
}

static BAT *
doubleslice(BAT *b, BUN l1, BUN h1, BUN l2, BUN h2)
{
	BAT *bn;
	oid *p;
	const oid *o;

	assert(l1 <= h1);
	assert(l2 <= h2);
	assert(h1 <= l2);
	assert(b->tsorted);
	assert(b->tkey);
	if (b->ttype == TYPE_void)
		return doublerange(l1 + b->tseqbase, h1 + b->tseqbase,
				   l2 + b->tseqbase, h2 + b->tseqbase);
	bn = BATnew(TYPE_void, TYPE_oid, h1 - l1 + h2 - l2, TRANSIENT);
	if (bn == NULL)
		return NULL;
	BATsetcount(bn, h1 - l1 + h2 - l2);
	BATseqbase(bn, 0);
	p = (oid *) Tloc(bn, BUNfirst(bn));
	o = (const oid *) Tloc(b, BUNfirst(b) + l1);
	while (l1++ < h1)
		*p++ = *o++;
	o = (const oid *) Tloc(b, BUNfirst(b) + l2);
	while (l2++ < h2)
		*p++ = *o++;
	bn->tkey = 1;
	bn->tsorted = 1;
	bn->trevsorted = BATcount(bn) <= 1;
	bn->T->nil = 0;
	bn->T->nonil = 1;
	return virtualize(bn);
}

static BAT *
BAT_hashselect(BAT *b, BAT *s, BAT *bn, const void *tl, BUN maximum)
{
	BATiter bi;
	BUN i, cnt;
	oid o, *dst;
	/* off must be signed as it can be negative,
	 * e.g., if b->hseqbase == 0 and b->batFirst > 0;
	 * instead of wrd, we could also use ssize_t or int/lng with
	 * 32/64-bit OIDs */
	wrd off;

	assert(bn->htype == TYPE_void);
	assert(bn->ttype == TYPE_oid);
	assert(BAThdense(b));
	off = b->hseqbase - b->batFirst;
	b = BATmirror(b);	/* BATprepareHash works on HEAD column */
	if (BATprepareHash(b)) {
		BBPreclaim(bn);
		return NULL;
	}
	bi = bat_iterator(b);
	dst = (oid *) Tloc(bn, BUNfirst(bn));
	cnt = 0;
	if (s) {
		assert(s->tsorted);
		HASHloop(bi, b->H->hash, i, tl) {
			o = (oid) (i + off);
			if (SORTfnd(s, &o) != BUN_NONE) {
				buninsfix(bn, dst, cnt, o,
					  maximum - BATcapacity(bn),
					  maximum, NULL);
				cnt++;
			}
		}
	} else {
		HASHloop(bi, b->H->hash, i, tl) {
			o = (oid) (i + off);
			buninsfix(bn, dst, cnt, o,
				  maximum - BATcapacity(bn),
				  maximum, NULL);
			cnt++;
		}
	}
	BATsetcount(bn, cnt);
	bn->tkey = 1;
	bn->tdense = bn->tsorted = bn->trevsorted = bn->batCount <= 1;
	if (bn->batCount == 1)
		bn->tseqbase = *(oid *) Tloc(bn, BUNfirst(bn));
	/* temporarily set head to nil so that BATorder doesn't materialize */
	BATseqbase(bn, oid_nil);
	bn = BATmirror(BATorder(BATmirror(bn)));
	BATseqbase(bn, 0);
	return bn;
}

/* Imprints select code */

/* inner check */
#define impscheck(CAND,TEST,ADD)			\
do {							\
	e = (BUN) (i+limit-pr_off+off);			\
	if (im[icnt] & mask) {				\
		if ((im[icnt] & ~innermask) == 0) {	\
			while (p < q && o < e) {	\
				v = src[o-off];		\
				ADD;			\
				cnt++;			\
				p++;			\
				if (p < q)		\
					CAND;		\
			}				\
		} else {				\
			while (p < q && o < e) {	\
				v = src[o-off];		\
				ADD;			\
				cnt += (TEST);		\
				p++;			\
				if (p < q)		\
					CAND;		\
			}				\
		}					\
	} else {					\
		while (p <= q && o < e) {		\
			p++;				\
			CAND;				\
		}					\
	}						\
} while (0)

/* main loop for imprints */
/*
 * icnt is the iterator for imprints
 * dcnt is the iterator for dictionary entries
 * i    is the iterator for the values in imprints
 */
#define impsloop(CAND,TEST,ADD)						\
do {									\
	BUN dcnt, icnt, limit, i, l, e;					\
	cchdc_t *d = (cchdc_t *) imprints->dict;			\
	bte rpp    = ATOMelmshift(IMPS_PAGE >> b->T->shift);		\
	CAND;								\
	for (i = 0, dcnt = 0, icnt = 0;					\
	     dcnt < imprints->dictcnt && i + off < w + pr_off && p < q; \
	     dcnt++) {							\
		limit = ((BUN) d[dcnt].cnt) << rpp;			\
		while (i + limit + off <= o + pr_off) {			\
			i += limit;					\
			icnt += d[dcnt].repeat ? 1 : d[dcnt].cnt;	\
			dcnt++;						\
			limit = ((BUN) d[dcnt].cnt) << rpp;		\
		}							\
		if (!d[dcnt].repeat) {					\
			limit = (BUN) 1 << rpp;				\
			l = icnt + d[dcnt].cnt;				\
			while (i + limit + off <= o + pr_off) {		\
				icnt++;					\
				i += limit;				\
			}						\
			for (;						\
			     icnt < l && i + off < w + pr_off;		\
			     icnt++) {					\
				impscheck(CAND,TEST,ADD);		\
				i += limit;				\
			}						\
		}							\
		else {							\
			impscheck(CAND,TEST,ADD);			\
			i += limit;					\
			icnt++;						\
		}							\
	}								\
} while (0)

#define quickins(dst, cnt, o, bn)			\
	do {						\
		assert((cnt) < BATcapacity(bn));	\
		dst[cnt] = (o);				\
	} while (0)

/* construct the mask */
#define impsmask(CAND,TEST,B)						\
do {									\
	uint##B##_t *im = (uint##B##_t *) imprints->imps;		\
	uint##B##_t mask = 0, innermask;				\
	int lbin, hbin;							\
	lbin = IMPSgetbin(ATOMstorage(b->ttype), imprints->bits,	\
			  imprints->bins, tl);				\
	hbin = IMPSgetbin(ATOMstorage(b->ttype), imprints->bits,	\
			  imprints->bins, th);				\
	/* note: (1<<n)-1 gives a sequence of n one bits */		\
	/* to set bits hbin..lbin inclusive, we would do: */		\
	/* mask = ((1 << (hbin + 1)) - 1) - ((1 << lbin) - 1); */	\
	/* except ((1 << (hbin + 1)) - 1) is not defined if */		\
	/* hbin == sizeof(uint##B##_t)*8 - 1 */				\
	/* the following does work, however */				\
	mask = (((((uint##B##_t) 1 << hbin) - 1) << 1) | 1) - (((uint##B##_t) 1 << lbin) - 1); \
	innermask = mask;						\
	if (!b->T->nonil || vl != minval)				\
		innermask = IMPSunsetBit(B, innermask, lbin);		\
	if (vh != maxval)						\
		innermask = IMPSunsetBit(B, innermask, hbin);		\
	if (anti) {							\
		uint##B##_t tmp = mask;					\
		mask = ~innermask;					\
		innermask = ~tmp;					\
	}								\
									\
	if (BATcapacity(bn) < maximum) {				\
		impsloop(CAND, TEST,					\
			 buninsfix(bn, dst, cnt, o,			\
				   (BUN) ((dbl) cnt / (dbl) (p-r)	\
					  * (dbl) (q-p) * 1.1 + 1024),	\
				   BATcapacity(bn) + q - p, BUN_NONE));	\
	} else {							\
		impsloop(CAND, TEST, quickins(dst, cnt, o, bn));	\
	}								\
} while (0)

#define checkMINMAX(B)							\
do {									\
	int ii;								\
	BUN *imp_cnt = imprints->stats + 128;				\
	imp_min = imp_max = nil;					\
	for (ii = 0; ii < B; ii++) {					\
		if ((imp_min == nil) && (imp_cnt[ii])) {		\
			imp_min = basesrc[imprints->stats[ii]];		\
		}							\
		if ((imp_max == nil) && (imp_cnt[B-1-ii])) {		\
			imp_max = basesrc[imprints->stats[64+B-1-ii]];	\
		}							\
	}								\
	assert((imp_min != nil) && (imp_max != nil));			\
	if (!s && !VIEWtparent(b)) { /* no candidate list and no views*/ \
		if ((vl > imp_max || vh < imp_min) ||			\
		    (anti && (vl < imp_min && vh > imp_max))) {		\
			return 0;					\
		}							\
	}								\
} while (0)

/* choose number of bits */
#define bitswitch(CAND,TEST)						\
do {									\
	assert(imprints);						\
	ALGODEBUG fprintf(stderr,					\
			  "#BATsubselect(b=%s#"BUNFMT",s=%s%s,anti=%d): " \
			  "imprints select %s\n", BATgetId(b), BATcount(b), \
			  s ? BATgetId(s) : "NULL",			\
			  s && BATtdense(s) ? "(dense)" : "",		\
			  anti, #TEST);					\
	switch (imprints->bits) {					\
	case 8:  checkMINMAX(8); impsmask(CAND,TEST,8); break;		\
	case 16: checkMINMAX(16); impsmask(CAND,TEST,16); break;	\
	case 32: checkMINMAX(32); impsmask(CAND,TEST,32); break;	\
	case 64: checkMINMAX(64); impsmask(CAND,TEST,64); break;	\
	default: assert(0); break;					\
	}								\
} while (0)

/* scan select without imprints */

/* core scan select loop with & without candidates */
#define scanloop(NAME,CAND,TEST)					\
do {									\
	ALGODEBUG fprintf(stderr,					\
			  "#BATsubselect(b=%s#"BUNFMT",s=%s%s,anti=%d): " \
			  "%s %s\n", BATgetId(b), BATcount(b),		\
			  s ? BATgetId(s) : "NULL",			\
			  s && BATtdense(s) ? "(dense)" : "",		\
			  anti, #NAME, #TEST);				\
	if (BATcapacity(bn) < maximum) {				\
		while (p < q) {						\
			CAND;						\
			v = src[o-off];					\
			buninsfix(bn, dst, cnt, o,			\
				  (BUN) ((dbl) cnt / (dbl) (p-r)	\
					 * (dbl) (q-p) * 1.1 + 1024),	\
				  BATcapacity(bn) + q - p, BUN_NONE);	\
			cnt += (TEST);					\
			p++;						\
		}							\
	} else {							\
		while (p < q) {						\
			CAND;						\
			v = src[o-off];					\
			assert(cnt < BATcapacity(bn));			\
			dst[cnt] = o;					\
			cnt += (TEST);					\
			p++;						\
		}							\
	}								\
} while (0)

/* argument list for type-specific core scan select function call */
#define scanargs							\
	b, s, bn, tl, th, li, hi, equi, anti, lval, hval, p, q, cnt, off, \
	dst, candlist, maximum, use_imprints

#define PREVVALUEbte(x)	((x) - 1)
#define PREVVALUEsht(x)	((x) - 1)
#define PREVVALUEint(x)	((x) - 1)
#define PREVVALUElng(x)	((x) - 1)
#ifdef HAVE_HGE
#define PREVVALUEhge(x)	((x) - 1)
#endif
#define PREVVALUEoid(x)	((x) - 1)
#define PREVVALUEflt(x)	nextafterf((x), -GDK_flt_max)
#define PREVVALUEdbl(x)	nextafter((x), -GDK_dbl_max)

#define NEXTVALUEbte(x)	((x) + 1)
#define NEXTVALUEsht(x)	((x) + 1)
#define NEXTVALUEint(x)	((x) + 1)
#define NEXTVALUElng(x)	((x) + 1)
#ifdef HAVE_HGE
#define NEXTVALUEhge(x)	((x) + 1)
#endif
#define NEXTVALUEoid(x)	((x) + 1)
#define NEXTVALUEflt(x)	nextafterf((x), GDK_flt_max)
#define NEXTVALUEdbl(x)	nextafter((x), GDK_dbl_max)

#define MINVALUEbte	NEXTVALUEbte(GDK_bte_min)
#define MINVALUEsht	NEXTVALUEsht(GDK_sht_min)
#define MINVALUEint	NEXTVALUEint(GDK_int_min)
#define MINVALUElng	NEXTVALUElng(GDK_lng_min)
#ifdef HAVE_HGE
#define MINVALUEhge	NEXTVALUEhge(GDK_hge_min)
#endif
#define MINVALUEoid	GDK_oid_min
#define MINVALUEflt	NEXTVALUEflt(GDK_flt_min)
#define MINVALUEdbl	NEXTVALUEdbl(GDK_dbl_min)

#define MAXVALUEbte	GDK_bte_max
#define MAXVALUEsht	GDK_sht_max
#define MAXVALUEint	GDK_int_max
#define MAXVALUElng	GDK_lng_max
#ifdef HAVE_HGE
#define MAXVALUEhge	GDK_hge_max
#endif
#define MAXVALUEoid	GDK_oid_max
#define MAXVALUEflt	GDK_flt_max
#define MAXVALUEdbl	GDK_dbl_max

#define choose(NAME, CAND, TEST)			\
	do {						\
		if (use_imprints) {			\
			bitswitch(CAND, TEST);		\
		} else {				\
			scanloop(NAME, CAND, TEST);	\
		}					\
	} while (0)

/* definition of type-specific core scan select function */
#define scanfunc(NAME, TYPE, CAND, END)					\
static BUN								\
NAME##_##TYPE (BAT *b, BAT *s, BAT *bn, const void *tl, const void *th,	\
	       int li, int hi, int equi, int anti, int lval, int hval,	\
	       BUN r, BUN q, BUN cnt, wrd off, oid *dst,		\
	       const oid *candlist, BUN maximum, int use_imprints)	\
{									\
	TYPE vl = *(const TYPE *) tl;					\
	TYPE vh = *(const TYPE *) th;					\
	TYPE imp_min;							\
	TYPE imp_max;							\
	TYPE v;								\
	TYPE nil = TYPE##_nil;						\
	TYPE minval = MINVALUE##TYPE;					\
	TYPE maxval = MAXVALUE##TYPE;					\
	const TYPE *src = (const TYPE *) Tloc(b, 0);			\
	const TYPE *basesrc;						\
	oid o;								\
	BUN w, p = r;							\
	BUN pr_off = 0;							\
	Imprints *imprints;						\
	(void) candlist;						\
	(void) li;							\
	(void) hi;							\
	(void) lval;							\
	(void) hval;							\
	if (use_imprints && VIEWtparent(b)) {				\
		BAT *parent = BATmirror(BATdescriptor(VIEWtparent(b)));	\
		basesrc = (const TYPE *) Tloc(parent, BUNfirst(parent)); \
		imprints = parent->T->imprints;				\
		pr_off = (BUN) ((TYPE *)Tloc(b,0) -			\
				(TYPE *)Tloc(parent,0)+BUNfirst(parent)); \
		BBPunfix(parent->batCacheid);				\
	} else {							\
		imprints= b->T->imprints;				\
		basesrc = (const TYPE *) Tloc(b, BUNfirst(b));		\
	}								\
	END;								\
	if (equi) {							\
		assert(!use_imprints);					\
		scanloop(NAME, CAND, v == vl);				\
	} else if (anti) {						\
		if (b->T->nonil) {					\
			choose(NAME,CAND,(v <= vl || v >= vh));		\
		} else {						\
			choose(NAME,CAND,(v <= vl || v >= vh) && v != nil); \
		}							\
	} else if (b->T->nonil && vl == minval) {			\
		choose(NAME,CAND,v <= vh);				\
	} else if (vh == maxval) {					\
		choose(NAME,CAND,v >= vl);				\
	} else {							\
		choose(NAME,CAND,v >= vl && v <= vh);			\
	}								\
	return cnt;							\
}

static BUN
candscan_any (BAT *b, BAT *s, BAT *bn, const void *tl, const void *th,
	      int li, int hi, int equi, int anti, int lval, int hval,
	      BUN r, BUN q, BUN cnt, wrd off, oid *dst,
	      const oid *candlist, BUN maximum, int use_imprints)
{
	const void *v;
	const void *nil = ATOMnilptr(b->ttype);
	int (*cmp)(const void *, const void *) = BATatoms[b->ttype].atomCmp;
	BATiter bi = bat_iterator(b);
	oid o;
	BUN p = r;
	int c;

	(void) maximum;
	(void) use_imprints;
	if (equi) {
		ALGODEBUG fprintf(stderr,
				  "#BATsubselect(b=%s#"BUNFMT",s=%s%s,anti=%d): "
				  "candscan equi\n", BATgetId(b), BATcount(b),
				  BATgetId(s), BATtdense(s) ? "(dense)" : "",
				  anti);
		while (p < q) {
			o = *candlist++;
			v = BUNtail(bi,(BUN)(o-off));
			buninsfix(bn, dst, cnt, o,
				  (BUN) ((dbl) cnt / (dbl) (p-r)
					 * (dbl) (q-p) * 1.1 + 1024),
				  BATcapacity(bn) + q - p, BUN_NONE);
			cnt += ((*cmp)(tl, v) == 0);
			p++;
		}
	} else if (anti) {
		ALGODEBUG fprintf(stderr,
				  "#BATsubselect(b=%s#"BUNFMT",s=%s%s,anti=%d): "
				  "candscan anti\n", BATgetId(b), BATcount(b),
				  BATgetId(s), BATtdense(s) ? "(dense)" : "",
				  anti);
		while (p < q) {
			o = *candlist++;
			v = BUNtail(bi,(BUN)(o-off));
			buninsfix(bn, dst, cnt, o,
				  (BUN) ((dbl) cnt / (dbl) (p-r)
					 * (dbl) (q-p) * 1.1 + 1024),
				  BATcapacity(bn) + q - p, BUN_NONE);
			cnt += ((nil == NULL || (*cmp)(v, nil) != 0) &&
			     ((lval &&
			       ((c = (*cmp)(tl, v)) > 0 ||
				(!li && c == 0))) ||
			      (hval &&
			       ((c = (*cmp)(th, v)) < 0 ||
				(!hi && c == 0)))));
			p++;
		}
	} else {
		ALGODEBUG fprintf(stderr,
				  "#BATsubselect(b=%s#"BUNFMT",s=%s%s,anti=%d): "
				  "candscan range\n", BATgetId(b), BATcount(b),
				  BATgetId(s), BATtdense(s) ? "(dense)" : "",
				  anti);
		while (p < q) {
			o = *candlist++;
			v = BUNtail(bi,(BUN)(o-off));
			buninsfix(bn, dst, cnt, o,
				  (BUN) ((dbl) cnt / (dbl) (p-r)
					 * (dbl) (q-p) * 1.1 + 1024),
				  BATcapacity(bn) + q - p, BUN_NONE);
			cnt += ((nil == NULL || (*cmp)(v, nil) != 0) &&
			     ((!lval ||
			       (c = cmp(tl, v)) < 0 ||
			       (li && c == 0)) &&
			      (!hval ||
			       (c = cmp(th, v)) > 0 ||
			       (hi && c == 0))));
			p++;
		}
	}
	return cnt;
}

static BUN
fullscan_any(BAT *b, BAT *s, BAT *bn, const void *tl, const void *th,
	     int li, int hi, int equi, int anti, int lval, int hval,
	     BUN r, BUN q, BUN cnt, wrd off, oid *dst,
	     const oid *candlist, BUN maximum, int use_imprints)
{
	const void *v;
	const void *nil = ATOMnilptr(b->ttype);
	int (*cmp)(const void *, const void *) = BATatoms[b->ttype].atomCmp;
	BATiter bi = bat_iterator(b);
	oid o;
	BUN p = r;
	int c;

	(void) candlist;
	(void) maximum;
	(void) use_imprints;

	if (equi) {
		ALGODEBUG fprintf(stderr,
				  "#BATsubselect(b=%s#"BUNFMT",s=%s%s,anti=%d): "
				  "fullscan equi\n", BATgetId(b), BATcount(b),
				  s ? BATgetId(s) : "NULL",
				  s && BATtdense(s) ? "(dense)" : "", anti);
		while (p < q) {
			o = (oid)(p + off);
			v = BUNtail(bi,(BUN)(o-off));
			buninsfix(bn, dst, cnt, o,
				  (BUN) ((dbl) cnt / (dbl) (p-r)
					 * (dbl) (q-p) * 1.1 + 1024),
				  BATcapacity(bn) + q - p, BUN_NONE);
			cnt += ((*cmp)(tl, v) == 0);
			p++;
		}
	} else if (anti) {
		ALGODEBUG fprintf(stderr,
				  "#BATsubselect(b=%s#"BUNFMT",s=%s%s,anti=%d): "
				  "fullscan anti\n", BATgetId(b), BATcount(b),
				  s ? BATgetId(s) : "NULL",
				  s && BATtdense(s) ? "(dense)" : "", anti);
		while (p < q) {
			o = (oid)(p + off);
			v = BUNtail(bi,(BUN)(o-off));
			buninsfix(bn, dst, cnt, o,
				  (BUN) ((dbl) cnt / (dbl) (p-r)
					 * (dbl) (q-p) * 1.1 + 1024),
				  BATcapacity(bn) + q - p, BUN_NONE);
			cnt += ((nil == NULL || (*cmp)(v, nil) != 0) &&
			     ((lval &&
			       ((c = (*cmp)(tl, v)) > 0 ||
				(!li && c == 0))) ||
			      (hval &&
			       ((c = (*cmp)(th, v)) < 0 ||
				(!hi && c == 0)))));
			p++;
		}
	} else {
		ALGODEBUG fprintf(stderr,
				  "#BATsubselect(b=%s#"BUNFMT",s=%s%s,anti=%d): "
				  "fullscan range\n", BATgetId(b), BATcount(b),
				  s ? BATgetId(s) : "NULL",
				  s && BATtdense(s) ? "(dense)" : "", anti);
		while (p < q) {
			o = (oid)(p + off);
			v = BUNtail(bi,(BUN)(o-off));
			buninsfix(bn, dst, cnt, o,
				  (BUN) ((dbl) cnt / (dbl) (p-r)
					 * (dbl) (q-p) * 1.1 + 1024),
				  BATcapacity(bn) + q - p, BUN_NONE);
			cnt += ((nil == NULL || (*cmp)(v, nil) != 0) &&
			     ((!lval ||
			       (c = cmp(tl, v)) < 0 ||
			       (li && c == 0)) &&
			      (!hval ||
			       (c = cmp(th, v)) > 0 ||
			       (hi && c == 0))));
			p++;
		}
	}
	return cnt;
}

/* scan select type switch */
#ifdef HAVE_HGE
#define scanfunc_hge(NAME, CAND, END)	\
	scanfunc(NAME, hge, CAND, END)
#else
#define scanfunc_hge(NAME, CAND, END)
#endif
#define scan_sel(NAME, CAND, END)		\
	scanfunc(NAME, bte, CAND, END)		\
	scanfunc(NAME, sht, CAND, END)		\
	scanfunc(NAME, int, CAND, END)		\
	scanfunc(NAME, flt, CAND, END)		\
	scanfunc(NAME, dbl, CAND, END)		\
	scanfunc(NAME, lng, CAND, END)		\
	scanfunc_hge(NAME, CAND, END)

/* scan/imprints select with candidates */
scan_sel(candscan, o = *candlist++, w = (BUN) ((*(oid *) Tloc(s,q?(q - 1):0)) + 1))
/* scan/imprints select without candidates */
scan_sel(fullscan, o = (oid) (p+off), w = (BUN) (q+off))


static BAT *
BAT_scanselect(BAT *b, BAT *s, BAT *bn, const void *tl, const void *th,
	       int li, int hi, int equi, int anti, int lval, int hval,
	       BUN maximum, int use_imprints)
{
#ifndef NDEBUG
	int (*cmp)(const void *, const void *);
#endif
	int t;
	BUN p, q, cnt;
	oid o, *dst;
	/* off must be signed as it can be negative,
	 * e.g., if b->hseqbase == 0 and b->batFirst > 0;
	 * instead of wrd, we could also use ssize_t or int/lng with
	 * 32/64-bit OIDs */
	wrd off;
	const oid *candlist;

	assert(b != NULL);
	assert(bn != NULL);
	assert(bn->htype == TYPE_void);
	assert(bn->ttype == TYPE_oid);
	assert(anti == 0 || anti == 1);
	assert(!lval || tl != NULL);
	assert(!hval || th != NULL);
	assert(!equi || (li && hi && !anti));
	assert(!anti || lval || hval);
	assert( anti || lval || hval || !b->T->nonil);
	assert(b->ttype != TYPE_void || equi || b->T->nonil);

#ifndef NDEBUG
	cmp = BATatoms[b->ttype].atomCmp;
#endif

	assert(!lval || !hval || (*cmp)(tl, th) <= 0);

	/* build imprints if they do not exist */
	if (use_imprints && (BATimprints(b) == NULL)) {
		use_imprints = 0;
	}

	off = b->hseqbase - BUNfirst(b);
	dst = (oid *) Tloc(bn, BUNfirst(bn));
	cnt = 0;

	t = b->ttype;
	if (t != ATOMstorage(t) &&
	    ATOMnilptr(ATOMstorage(t)) == ATOMnilptr(t) &&
	    BATatoms[ATOMstorage(t)].atomCmp == BATatoms[t].atomCmp)
		t = ATOMstorage(t);

	if (s && !BATtdense(s)) {

		assert(s->tsorted);
		assert(s->tkey);
		/* setup candscanloop loop vars to only iterate over
		 * part of s that has values that are in range of b */
		o = b->hseqbase + BATcount(b);
		q = SORTfndfirst(s, &o);
		p = SORTfndfirst(s, &b->hseqbase);
		/* should we return an error if p > BUNfirst(s) || q <
		 * BUNlast(s) (i.e. s not fully used)? */
		candlist = (const oid *) Tloc(s, p);
		/* call type-specific core scan select function */
		assert(b->batCapacity >= BATcount(b));
		assert(s->batCapacity >= BATcount(s));
		switch (t) {
		case TYPE_bte:
			cnt = candscan_bte(scanargs);
			break;
		case TYPE_sht:
			cnt = candscan_sht(scanargs);
			break;
		case TYPE_int:
			cnt = candscan_int(scanargs);
			break;
		case TYPE_flt:
			cnt = candscan_flt(scanargs);
			break;
		case TYPE_dbl:
			cnt = candscan_dbl(scanargs);
			break;
		case TYPE_lng:
			cnt = candscan_lng(scanargs);
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			cnt = candscan_hge(scanargs);
			break;
#endif
		default:
			cnt = candscan_any(scanargs);
		}
		if (cnt == BUN_NONE)
			return NULL;
		assert(bn->batCapacity >= cnt);
	} else {
		if (s) {
			assert(BATtdense(s));
			p = (BUN) s->tseqbase;
			q = p + BATcount(s);
			if ((oid) p < b->hseqbase)
				p = (BUN) b->hseqbase;
			if ((oid) q > b->hseqbase + BATcount(b))
				q = (BUN) b->hseqbase + BATcount(b);
			p = (BUN) (p - off);
			q = (BUN) (q - off);
		} else {
			p = BUNfirst(b);
			q = BUNlast(b);
		}
		candlist = NULL;
		/* call type-specific core scan select function */
		switch (t) {
		case TYPE_bte:
			cnt = fullscan_bte(scanargs);
			break;
		case TYPE_sht:
			cnt = fullscan_sht(scanargs);
			break;
		case TYPE_int:
			cnt = fullscan_int(scanargs);
			break;
		case TYPE_flt:
			cnt = fullscan_flt(scanargs);
			break;
		case TYPE_dbl:
			cnt = fullscan_dbl(scanargs);
			break;
		case TYPE_lng:
			cnt = fullscan_lng(scanargs);
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			cnt = fullscan_hge(scanargs);
			break;
#endif
		default:
			cnt = fullscan_any(scanargs);
		}
		if (cnt == BUN_NONE)
			return NULL;
		assert(bn->batCapacity >= cnt);
	}
	BATsetcount(bn, cnt);
	bn->tsorted = 1;
	bn->trevsorted = bn->batCount <= 1;
	bn->tkey = 1;
	bn->tdense = bn->batCount <= 1;
	if (bn->batCount == 1)
		bn->tseqbase = *(oid *) Tloc(bn, BUNfirst(bn));
	bn->hsorted = 1;
	bn->hdense = 1;
	bn->hseqbase = 0;
	bn->hkey = 1;
	bn->hrevsorted = bn->batCount <= 1;

	return bn;
}

/* generic range select
 *
 * Return a dense-headed BAT with the OID values of b in the tail for
 * qualifying tuples.  The return BAT is sorted on the tail value
 * (i.e. in the same order as the input BAT).
 *
 * If s[dense,OID] is specified, its tail column is a list of
 * candidates.  s should be sorted on the tail value.
 *
 * tl may not be NULL, li, hi, and anti must be either 0 or 1.
 *
 * If th is NULL, hi is ignored.
 *
 * If anti is 0, qualifying tuples are those whose tail value is
 * between tl and th.  If li or hi is 1, the respective boundary is
 * inclusive, otherwise exclusive.  If th is NULL it is taken to be
 * equal to tl, turning this into an equi- or point-select.  Note that
 * for a point select to return anything, li (and hi if th was not
 * NULL) must be 1.  There is a special case if tl is nil and th is
 * NULL.  This is the only way to select for nil values.
 *
 * If anti is 1, the result is the complement of what the result would
 * be if anti were 0, except that nils are filtered out.
 *
 * In brief:
 * - if tl==nil and th==NULL and anti==0, return all nils (only way to
 *   get nils);
 * - it tl==nil and th==nil, return all but nils;
 * - if tl==nil and th!=NULL, no lower bound;
 * - if th==NULL or tl==th, point (equi) select;
 * - if th==nil, no upper bound
 *
 * A complete breakdown of the various arguments follows.  Here, v, v1
 * and v2 are values from the appropriate domain, and
 * v != nil, v1 != nil, v2 != nil, v1 < v2.
 *	tl	th	li	hi	anti	result list of OIDs for values
 *	-----------------------------------------------------------------
 *	nil	NULL	ignored	ignored	false	x = nil (only way to get nil)
 *	nil	NULL	ignored	ignored	true	x != nil
 *	nil	nil	ignored	ignored	false	x != nil
 *	nil	v	ignored	false	false	x < v
 *	nil	v	ignored	true	false	x <= v
 *	nil	v	ignored	false	true	x >= v
 *	nil	v	ignored	true	true	x > v
 *	v	nil	false	ignored	false	x > v
 *	v	nil	true	ignored	false	x >= v
 *	v	nil	false	ignored	true	x <= v
 *	v	nil	true	ignored	true	x < v
 *	v	NULL	false	ignored	false	NOTHING
 *	v	NULL	true	ignored	false	x == v
 *	v	NULL	false	ignored	true	x != nil
 *	v	NULL	true	ignored	true	x != v
 *	v	v	false	false	false	NOTHING
 *	v	v	true	false	false	NOTHING
 *	v	v	false	true	false	NOTHING
 *	v	v	true	true	false	x == v
 *	v	v	false	false	true	x != nil
 *	v	v	true	false	true	x != nil
 *	v	v	false	true	true	x != nil
 *	v	v	true	true	true	x != v
 *	v1	v2	false	false	false	v1 < x < v2
 *	v1	v2	true	false	false	v1 <= x < v2
 *	v1	v2	false	1	false	v1 < x <= v2
 *	v1	v2	true	true	false	v1 <= x <= v2
 *	v1	v2	false	false	true	x <= v1 or x >= v2
 *	v1	v2	true	false	true	x < v1 or x >= v2
 *	v1	v2	false	true	true	x <= v1 or x > v2
 *	v1	v2	true	true	true	x < v1 or x > v2
 *	v2	v1	ignored	ignored	ignored	NOTHING
 */

/* Normalize the variables li, hi, lval, hval, possibly changing anti
 * in the process.  This works for all (and only) numeric types.
 *
 * Note that the expression x < v is equivalent to x <= v' where v' is
 * the next smaller value in the domain of v (similarly for x > v).
 * Also note that for floating point numbers there actually is such a
 * value.  In fact, there is a function in standard C that calculates
 * that value.
 *
 * The result of this macro is:
 * li == !anti, hi == !anti, lval == 1, hval == 1
 * This means that all ranges that we check for are closed ranges.  If
 * a range is one-sided, we fill in the minimum resp. maximum value in
 * the domain so that we create a closed ranges. */
#define NORMALIZE(TYPE)							\
	do {								\
		if (anti && li) {					\
			/* -inf < x < vl === -inf < x <= vl-1 */	\
			if (*(TYPE*)tl == MINVALUE##TYPE) {		\
				/* -inf < x < MIN || *th <[=] x < +inf */ \
				/* degenerates into half range */	\
				/* *th <[=] x < +inf */			\
				anti = 0;				\
				tl = th;				\
				li = !hi;				\
				hval = 0;				\
				/* further dealt with below */		\
			} else {					\
				vl.v_##TYPE = PREVVALUE##TYPE(*(TYPE*)tl); \
				tl = &vl.v_##TYPE;			\
				li = 0;					\
			}						\
		}							\
		if (anti && hi) {					\
			/* vl < x < +inf === vl+1 <= x < +inf */	\
			if (*(TYPE*)th == MAXVALUE##TYPE) {		\
				/* -inf < x <[=] *tl || MAX > x > +inf */ \
				/* degenerates into half range */	\
				/* -inf < x <[=] *tl */			\
				anti = 0;				\
				th = tl;				\
				hi = !li;				\
				lval = 0;				\
				/* further dealt with below */		\
			} else {					\
				vh.v_##TYPE = NEXTVALUE##TYPE(*(TYPE*)th); \
				th = &vh.v_##TYPE;			\
				hi = 0;					\
			}						\
		}							\
		if (!anti) {						\
			if (lval) {					\
				/* range bounded on left */		\
				if (!li) {				\
					/* open range on left */	\
					if (*(TYPE*)tl == MAXVALUE##TYPE) \
						return newempty("BATsubselect"); \
					/* vl < x === vl+1 <= x */	\
					vl.v_##TYPE = NEXTVALUE##TYPE(*(TYPE*)tl); \
					li = 1;				\
					tl = &vl.v_##TYPE;		\
				}					\
			} else {					\
				/* -inf, i.e. smallest value */		\
				vl.v_##TYPE = MINVALUE##TYPE;		\
				li = 1;					\
				tl = &vl.v_##TYPE;			\
				lval = 1;				\
			}						\
			if (hval) {					\
				/* range bounded on right */		\
				if (!hi) {				\
					/* open range on right */	\
					if (*(TYPE*)th == MINVALUE##TYPE) \
						return newempty("BATsubselect"); \
					/* x < vh === x <= vh-1 */	\
					vh.v_##TYPE = PREVVALUE##TYPE(*(TYPE*)th); \
					hi = 1;				\
					th = &vh.v_##TYPE;		\
				}					\
			} else {					\
				/* +inf, i.e. largest value */		\
				vh.v_##TYPE = MAXVALUE##TYPE;		\
				hi = 1;					\
				th = &vh.v_##TYPE;			\
				hval = 1;				\
			}						\
		}							\
		assert(lval);						\
		assert(hval);						\
		assert(li != anti);					\
		assert(hi != anti);					\
		/* if anti is set, we can now check */			\
		/* (x <= *tl || x >= *th) && x != nil */		\
		/* if equi==1, the check is x != *tl && x != nil */	\
		/* if anti is not set, we can check just */		\
		/* *tl <= x && x <= *th */				\
		/* if equi==1, the check is x == *tl */			\
		/* note that this includes the check for != nil */	\
		/* in the case where equi==1, the check is x == *tl */	\
	} while (0)

BAT *
BATsubselect(BAT *b, BAT *s, const void *tl, const void *th,
	     int li, int hi, int anti)
{
	int hval, lval, equi, t, lnil, hash;
	bat parent;
	const void *nil;
	BAT *bn;
	BUN estimate = BUN_NONE, maximum = BUN_NONE;
	union {
		bte v_bte;
		sht v_sht;
		int v_int;
		lng v_lng;
#ifdef HAVE_HGE
		hge v_hge;
#endif
		flt v_flt;
		dbl v_dbl;
		oid v_oid;
	} vl, vh;

	BATcheck(b, "BATsubselect");
	BATcheck(tl, "BATsubselect: tl value required");

	assert(BAThdense(b));
	assert(s == NULL || BAThdense(s));
	assert(s == NULL || s->ttype == TYPE_oid || s->ttype == TYPE_void);
	assert(hi == 0 || hi == 1);
	assert(li == 0 || li == 1);
	assert(anti == 0 || anti == 1);

	if ((li != 0 && li != 1) ||
	    (hi != 0 && hi != 1) ||
	    (anti != 0 && anti != 1)) {
		GDKerror("BATsubselect: invalid arguments: "
			 "li, hi, anti must be 0 or 1\n");
		return NULL;
	}
	if (!BAThdense(b)) {
		GDKerror("BATsubselect: invalid argument: "
			 "b must have a dense head.\n");
		return NULL;
	}
	if (s && !BATtordered(s)) {
		GDKerror("BATsubselect: invalid argument: "
			 "s must be sorted.\n");
		return NULL;
	}

	if (b->batCount == 0 ||
	    (s && (s->batCount == 0 ||
		   (BATtdense(s) &&
		    (s->tseqbase >= b->hseqbase + BATcount(b) ||
		     s->tseqbase + BATcount(s) <= b->hseqbase))))) {
		/* trivially empty result */
		ALGODEBUG fprintf(stderr, "#BATsubselect(b=%s#" BUNFMT
				  ",s=%s%s,anti=%d): trivially empty\n",
				  BATgetId(b), BATcount(b),
				  s ? BATgetId(s) : "NULL",
				  s && BATtdense(s) ? "(dense)" : "", anti);
		return newempty("BATsubselect");
	}

	t = b->ttype;
	nil = ATOMnilptr(t);
	lnil = ATOMcmp(t, tl, nil) == 0; /* low value = nil? */
	lval = !lnil || th == NULL;	 /* low value used for comparison */
	equi = th == NULL || (lval && ATOMcmp(t, tl, th) == 0); /* point select? */
	if (equi) {
		assert(lval);
		if (th == NULL)
			hi = li;
		th = tl;
		hval = 1;
	} else {
		hval = ATOMcmp(t, th, nil) != 0;
	}
	if (anti) {
		if (lval != hval) {
			/* one of the end points is nil and the other
			 * isn't: swap sub-ranges */
			const void *tv;
			int ti;
			assert(!equi);
			ti = li;
			li = !hi;
			hi = !ti;
			tv = tl;
			tl = th;
			th = tv;
			ti = lval;
			lval = hval;
			hval = ti;
			lnil = ATOMcmp(t, tl, nil) == 0;
			anti = 0;
			ALGODEBUG fprintf(stderr, "#BATsubselect(b=%s#" BUNFMT
					  ",s=%s%s,anti=%d): anti: "
					  "switch ranges\n",
					  BATgetId(b), BATcount(b),
					  s ? BATgetId(s) : "NULL",
					  s && BATtdense(s) ? "(dense)" : "",
					  anti);
		} else if (!lval && !hval) {
			/* antiselect for nil-nil range: all non-nil
			 * values are in range; we must return all
			 * other non-nil values, i.e. nothing */
			ALGODEBUG fprintf(stderr, "#BATsubselect(b=%s#" BUNFMT
					  ",s=%s%s,anti=%d): anti: "
					  "nil-nil range, nonil\n",
					  BATgetId(b), BATcount(b),
					  s ? BATgetId(s) : "NULL",
					  s && BATtdense(s) ? "(dense)" : "",
					  anti);
			return newempty("BATsubselect");
		} else if (equi && lnil) {
			/* antiselect for nil value: turn into range
			 * select for nil-nil range (i.e. everything
			 * but nil) */
			equi = 0;
			anti = 0;
			lval = 0;
			hval = 0;
			ALGODEBUG fprintf(stderr, "#BATsubselect(b=%s#" BUNFMT
					  ",s=%s%s,anti=0): anti-nil\n",
					  BATgetId(b), BATcount(b),
					  s ? BATgetId(s) : "NULL",
					  s && BATtdense(s) ? "(dense)" : "");
		} else if (equi) {
			equi = 0;
			if (!(li && hi)) {
				/* antiselect for nothing: turn into
				 * range select for nil-nil range
				 * (i.e. everything but nil) */
				anti = 0;
				lval = 0;
				hval = 0;
				ALGODEBUG fprintf(stderr, "#BATsubselect(b=%s#"
						  BUNFMT ",s=%s%s,anti=0): "
						  "anti-nothing\n",
						  BATgetId(b), BATcount(b),
						  s ? BATgetId(s) : "NULL",
						  s && BATtdense(s) ? "(dense)" : "");
			}
		}
	}

	/* if equi set, then so are both lval and hval */
	assert(!equi || (lval && hval));

	if (hval && ((equi && !(li && hi)) || ATOMcmp(t, tl, th) > 0)) {
		/* empty range */
		ALGODEBUG fprintf(stderr, "#BATsubselect(b=%s#" BUNFMT
				  ",s=%s%s,anti=%d): empty range\n",
				  BATgetId(b), BATcount(b),
				  s ? BATgetId(s) : "NULL",
				  s && BATtdense(s) ? "(dense)" : "", anti);
		return newempty("BATsubselect");
	}
	if (equi && lnil && b->T->nonil) {
		/* return all nils, but there aren't any */
		ALGODEBUG fprintf(stderr, "#BATsubselect(b=%s#" BUNFMT
				  ",s=%s%s,anti=%d): equi-nil, nonil\n",
				  BATgetId(b), BATcount(b),
				  s ? BATgetId(s) : "NULL",
				  s && BATtdense(s) ? "(dense)" : "", anti);
		return newempty("BATsubselect");
	}

	if (!equi && !lval && !hval && lnil && b->T->nonil) {
		/* return all non-nils from a BAT that doesn't have
		 * any: i.e. return everything */
		ALGODEBUG fprintf(stderr, "#BATsubselect(b=%s#" BUNFMT
				  ",s=%s%s,anti=%d): everything, nonil\n",
				  BATgetId(b), BATcount(b),
				  s ? BATgetId(s) : "NULL",
				  s && BATtdense(s) ? "(dense)" : "", anti);
		if (s) {
			return BATcopy(s, TYPE_void, s->ttype, 0, TRANSIENT);
		} else {
			return BATmirror(BATmark(b, 0));
		}
	}

	if (b->ttype == TYPE_oid || b->ttype == TYPE_void) {
		NORMALIZE(oid);
	} else {
		switch (ATOMstorage(b->ttype)) {
		case TYPE_bte:
			NORMALIZE(bte);
			break;
		case TYPE_sht:
			NORMALIZE(sht);
			break;
		case TYPE_int:
			NORMALIZE(int);
			break;
		case TYPE_lng:
			NORMALIZE(lng);
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			NORMALIZE(hge);
			break;
#endif
		case TYPE_flt:
			NORMALIZE(flt);
			break;
		case TYPE_dbl:
			NORMALIZE(dbl);
			break;
		}
	}

	if (b->tsorted || b->trevsorted) {
		BUN low = 0;
		BUN high = b->batCount;

		if (BATtdense(b)) {
			/* positional */
			/* we expect nonil to be set, in which case we
			 * already know that we're not dealing with a
			 * nil equiselect (dealt with above) */
			oid h, l;
			assert(b->T->nonil);
			assert(b->tsorted);
			ALGODEBUG fprintf(stderr, "#BATsubselect(b=%s#" BUNFMT
					  ",s=%s%s,anti=%d): dense\n",
					  BATgetId(b), BATcount(b),
					  s ? BATgetId(s) : "NULL",
					  s && BATtdense(s) ? "(dense)" : "",
					  anti);
			h = * (oid *) th + hi;
			if (h > b->tseqbase)
				h -= b->tseqbase;
			else
				h = 0;
			if ((BUN) h < high)
				high = (BUN) h;

			l = *(oid *) tl + !li;
			if (l > b->tseqbase)
				l -= b->tseqbase;
			else
				l = 0;
			if ((BUN) l > low)
				low = (BUN) l;
			if (low > high)
				low = high;
		} else if (b->tsorted) {
			ALGODEBUG fprintf(stderr, "#BATsubselect(b=%s#" BUNFMT
					  ",s=%s%s,anti=%d): sorted\n",
					  BATgetId(b), BATcount(b),
					  s ? BATgetId(s) : "NULL",
					  s && BATtdense(s) ? "(dense)" : "",
					  anti);
			if (lval) {
				if (li)
					low = SORTfndfirst(b, tl);
				else
					low = SORTfndlast(b, tl);
			} else {
				/* skip over nils at start of column */
				low = SORTfndlast(b, nil);
			}
			low -= BUNfirst(b);
			if (hval) {
				if (hi)
					high = SORTfndlast(b, th);
				else
					high = SORTfndfirst(b, th);
				high -= BUNfirst(b);
			}
		} else {
			assert(b->trevsorted);
			ALGODEBUG fprintf(stderr, "#BATsubselect(b=%s#" BUNFMT
					  ",s=%s%s,anti=%d): reverse sorted\n",
					  BATgetId(b), BATcount(b),
					  s ? BATgetId(s) : "NULL",
					  s && BATtdense(s) ? "(dense)" : "",
					  anti);
			if (lval) {
				if (li)
					high = SORTfndlast(b, tl);
				else
					high = SORTfndfirst(b, tl);
			} else {
				/* skip over nils at end of column */
				high = SORTfndfirst(b, nil);
			}
			high -= BUNfirst(b);
			if (hval) {
				if (hi)
					low = SORTfndfirst(b, th);
				else
					low = SORTfndlast(b, th);
				low -= BUNfirst(b);
			}
		}
		if (anti) {
			if (b->tsorted) {
				BUN first = SORTfndlast(b, nil) - BUNfirst(b);
				/* match: [first..low) + [high..count) */
				if (s) {
					oid o = (oid) first + b->H->seq;
					first = SORTfndfirst(s, &o) - BUNfirst(s);
					o = (oid) low + b->H->seq;
					low = SORTfndfirst(s, &o) - BUNfirst(s);
					o = (oid) high + b->H->seq;
					high = SORTfndfirst(s, &o) - BUNfirst(s);
					bn = doubleslice(s, first, low, high, BATcount(s));
				} else {
					bn = doublerange(first + b->hseqbase,
							 low + b->hseqbase,
							 high + b->hseqbase,
							 BATcount(b) + b->hseqbase);
				}
			} else {
				BUN last = SORTfndlast(b, nil) - BUNfirst(b);
				/* match: [0..low) + [high..last) */
				if (s) {
					oid o = (oid) last + b->H->seq;
					last = SORTfndfirst(s, &o) - BUNfirst(s);
					o = (oid) low + b->H->seq;
					low = SORTfndfirst(s, &o) - BUNfirst(s);
					o = (oid) high + b->H->seq;
					high = SORTfndfirst(s, &o) - BUNfirst(s);
					bn = doubleslice(s, 0, low, high, last);
				} else {
					bn = doublerange(0 + b->hseqbase,
							 low + b->hseqbase,
							 high + b->hseqbase,
							 last + b->hseqbase);
				}
			}
		} else {
			/* match: [low..high) */
			if (s) {
				oid o = (oid) low + b->H->seq;
				low = SORTfndfirst(s, &o) - BUNfirst(s);
				o = (oid) high + b->H->seq;
				high = SORTfndfirst(s, &o) - BUNfirst(s);
				bn = doubleslice(s, 0, 0, low, high);
			} else {
				bn = doublerange(0, 0,
						 low + b->hseqbase,
						 high + b->hseqbase);
			}
		}
		bn->hseqbase = 0;
		bn->hkey = 1;
		bn->hsorted = 1;
		bn->hrevsorted = bn->batCount <= 1;
		bn->H->nonil = 1;
		bn->H->nil = 0;
		return virtualize(bn);
	}

	/* upper limit for result size */
	maximum = BATcount(b);
	if (s) {
		/* refine upper limit of result size by candidate list */
		oid ol = b->hseqbase;
		oid oh = ol + BATcount(b);
		assert(s->tsorted);
		assert(s->tkey);
		if (BATtdense(s)) {
			maximum = MIN(maximum ,
				      MIN(oh, s->tseqbase + BATcount(s))
				      - MAX(ol, s->tseqbase));
		} else {
			maximum = MIN(maximum,
				      SORTfndfirst(s, &oh)
				      - SORTfndfirst(s, &ol));
		}
	}
	if (b->tkey) {
		/* exact result size in special cases */
		if (equi) {
			estimate = 1;
		} else if (!anti && lval && hval) {
			switch (ATOMstorage(b->ttype)) {
			case TYPE_bte:
				estimate = (BUN) (*(bte *) th - *(bte *) tl);
				break;
			case TYPE_sht:
				estimate = (BUN) (*(sht *) th - *(sht *) tl);
				break;
			case TYPE_int:
				estimate = (BUN) (*(int *) th - *(int *) tl);
				break;
			case TYPE_lng:
				estimate = (BUN) (*(lng *) th - *(lng *) tl);
				break;
#ifdef HAVE_HGE
			case TYPE_hge:
				estimate = (BUN) (*(hge *) th - *(hge *) tl);
				break;
#endif
			}
			if (estimate != BUN_NONE)
				estimate += li + hi - 1;
		}
	}
	/* refine upper limit by exact size (if known) */
	maximum = MIN(maximum, estimate);
	hash = equi &&
	       b->batPersistence == PERSISTENT &&
	       (size_t) ATOMsize(b->ttype) > sizeof(BUN) / 4 &&
	       BATcount(b) * (ATOMsize(b->ttype) + 2 * sizeof(BUN)) < GDK_mem_maxsize / 2;
	if (hash && estimate == BUN_NONE && !b->T->hash) {
		/* no exact result size, but we need estimate to choose
		 * between hash- & scan-select */
		if (BATcount(b) <= 10000) {
			/* "small" input: don't bother about more accurate
			 * estimate */
			estimate = maximum;
		} else {
			/* layman's quick "pseudo-sample" of 1000 tuples,
			 * i.e., 333 from begin, middle & end of BAT */
			BUN smpl_cnt = 0, slct_cnt = 0, pos, skip, delta;
			BAT *smpl, *slct;

			delta = 1000 / 3 / 2;
			skip = (BATcount(b) - (2 * delta)) / 2;
			for (pos = delta; pos < BATcount(b); pos += skip) {
				smpl = BATslice(b, pos - delta, pos + delta);
				if (smpl) {
					slct = BATsubselect(smpl, NULL, tl,
							    th, li, hi, anti);
					if (slct) {
						smpl_cnt += BATcount(smpl);
						slct_cnt += BATcount(slct);
						BBPreclaim(slct);
					}
					BBPreclaim(smpl);
				}
			}
			if (smpl_cnt > 0 && slct_cnt > 0) {
				/* linear extrapolation plus 10% margin */
				estimate = (BUN) ((dbl) slct_cnt / (dbl) smpl_cnt 
						  * (dbl) BATcount(b) * 1.1);
			} else if (smpl_cnt > 0 && slct_cnt == 0) {
				/* estimate low enough to trigger hash select */
				estimate = (BATcount(b) / 100) - 1;
			}
		}
		hash = estimate < BATcount(b) / 100;
	}
	if (estimate == BUN_NONE) {
		/* no better estimate possible/required:
		 * (pre-)allocate 1M tuples, i.e., avoid/delay extend
		 * without too much overallocation */
		estimate = 1000000;
	}
	/* limit estimation by upper limit */
	estimate = MIN(estimate, maximum);

	bn = BATnew(TYPE_void, TYPE_oid, estimate, TRANSIENT);
	if (bn == NULL)
		return NULL;

	if (equi && (b->T->hash || hash)) {
		ALGODEBUG fprintf(stderr, "#BATsubselect(b=%s#" BUNFMT
				  ",s=%s%s,anti=%d): hash select\n",
				  BATgetId(b), BATcount(b),
				  s ? BATgetId(s) : "NULL",
				  s && BATtdense(s) ? "(dense)" : "", anti);
		bn = BAT_hashselect(b, s, bn, tl, maximum);
	} else {
		int use_imprints = 0;
		if (!equi &&
		    !b->tvarsized &&
		    (b->batPersistence == PERSISTENT ||
		     ((parent = VIEWtparent(b)) != 0 &&
		      BBPquickdesc(abs(parent),0)->batPersistence == PERSISTENT))) {
			/* use imprints if
			 *   i) bat is persistent, or parent is persistent
			 *  ii) it is not an equi-select, and
			 * iii) is not var-sized.
			 */
			use_imprints = 1;
		}
		bn = BAT_scanselect(b, s, bn, tl, th, li, hi, equi, anti,
				    lval, hval, maximum, use_imprints);
	}

	return virtualize(bn);
}

/* theta select
 *
 * Returns a dense-headed BAT with the OID values of b in the tail for
 * qualifying tuples.  The return BAT is sorted on the tail value
 * (i.e. in the same order as the input BAT).
 *
 * If s[dense,OID] is specified, its tail column is a list of
 * candidates.  s should be sorted on the tail value.
 *
 * Theta select returns all values from b which are less/greater than
 * or (not) equal to the provided value depending on the value of op.
 * Op is a string with one of the values: "=", "==", "<", "<=", ">",
 * ">=", "<>", "!=" (the first two are equivalent and the last two are
 * equivalent).  Theta select never returns nils.
 *
 * If value is nil, the result is empty.
 */
BAT *
BATthetasubselect(BAT *b, BAT *s, const void *val, const char *op)
{
	const void *nil;

	BATcheck(b, "BATthetasubselect");
	BATcheck(val, "BATthetasubselect");
	BATcheck(op, "BATthetasubselect");

	nil = ATOMnilptr(b->ttype);
	if (ATOMcmp(b->ttype, val, nil) == 0)
		return newempty("BATthetasubselect");
	if (op[0] == '=' && ((op[1] == '=' && op[2] == 0) || op[2] == 0)) {
		/* "=" or "==" */
		return BATsubselect(b, s, val, NULL, 1, 1, 0);
	}
	if (op[0] == '!' && op[1] == '=' && op[2] == 0) {
		/* "!=" (equivalent to "<>") */
		return BATsubselect(b, s, val, NULL, 1, 1, 1);
	}
	if (op[0] == '<') {
		if (op[1] == 0) {
			/* "<" */
			return BATsubselect(b, s, nil, val, 0, 0, 0);
		}
		if (op[1] == '=' && op[2] == 0) {
			/* "<=" */
			return BATsubselect(b, s, nil, val, 0, 1, 0);
		}
		if (op[1] == '>' && op[2] == 0) {
			/* "<>" (equivalent to "!=") */
			return BATsubselect(b, s, val, NULL, 1, 1, 1);
		}
	}
	if (op[0] == '>') {
		if (op[1] == 0) {
			/* ">" */
			return BATsubselect(b, s, val, nil, 0, 0, 0);
		}
		if (op[1] == '=' && op[2] == 0) {
			/* ">=" */
			return BATsubselect(b, s, val, nil, 1, 0, 0);
		}
	}
	GDKerror("BATthetasubselect: unknown operator.\n");
	return NULL;
}
