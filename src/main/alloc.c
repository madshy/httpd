/* ====================================================================
 * Copyright (c) 1995-1998 The Apache Group.  All rights reserved.
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
 *    "This product includes software developed by the Apache Group
 *    for use in the Apache HTTP server project (http://www.apache.org/)."
 *
 * 4. The names "Apache Server" and "Apache Group" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For written permission, please contact
 *    apache@apache.org.
 *
 * 5. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by the Apache Group
 *    for use in the Apache HTTP server project (http://www.apache.org/)."
 *
 * THIS SOFTWARE IS PROVIDED BY THE APACHE GROUP ``AS IS'' AND ANY
 * EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE APACHE GROUP OR
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
 * This software consists of voluntary contributions made by many
 * individuals on behalf of the Apache Group and was originally based
 * on public domain software written at the National Center for
 * Supercomputing Applications, University of Illinois, Urbana-Champaign.
 * For more information on the Apache Group and the Apache HTTP server
 * project, please see <http://www.apache.org/>.
 *
 */


/*
 * Resource allocation code... the code here is responsible for making
 * sure that nothing leaks.
 *
 * rst --- 4/95 --- 6/95
 */

#include "httpd.h"
#include "multithread.h"
#include "http_log.h"

#include <stdarg.h>

/* debugging support, define this to enable code which helps detect re-use
 * of freed memory and other such nonsense.
 *
 * The theory is simple.  The FILL_BYTE (0xa5) is written over all malloc'd
 * memory as we receive it, and is written over everything that we free up
 * during a clear_pool.  We check that blocks on the free list always
 * have the FILL_BYTE in them, and we check during palloc() that the bytes
 * still have FILL_BYTE in them.  If you ever see garbage URLs or whatnot
 * containing lots of 0xa5s then you know something used data that's been
 * freed or uninitialized.
 */
/* #define ALLOC_DEBUG */

/* debugging support, if defined all allocations will be done with
 * malloc and free()d appropriately at the end.  This is intended to be
 * used with something like Electric Fence or Purify to help detect
 * memory problems.  Note that if you're using efence then you should also
 * add in ALLOC_DEBUG.  But don't add in ALLOC_DEBUG if you're using Purify
 * because ALLOC_DEBUG would hide all the uninitialized read errors that
 * Purify can diagnose.
 */
/* #define ALLOC_USE_MALLOC */

/* Pool debugging support.  This is intended to detect cases where the
 * wrong pool is used when assigning data to an object in another pool.
 * In particular, it causes the table_{set,add,merge}n routines to check
 * that their arguments are safe for the table they're being placed in.
 * It currently only works with the unix multiprocess model, but could
 * be extended to others.
 */
/* #define POOL_DEBUG */

/* Provide diagnostic information about make_table() calls which are
 * possibly too small.  This requires a recent gcc which supports
 * __builtin_return_address().  The error_log output will be a
 * message such as:
 *    table_push: table created by 0x804d874 hit limit of 10
 * Use "l *0x804d874" to find the source that corresponds to.  It
 * indicates that a table allocated by a call at that address has
 * possibly too small an initial table size guess.
 */
/* #define MAKE_TABLE_PROFILE */

#ifdef POOL_DEBUG
#ifdef ALLOC_USE_MALLOC
# error "sorry, no support for ALLOC_USE_MALLOC and POOL_DEBUG at the same time"
#endif
#ifdef MULTITHREAD
# error "sorry, no support for MULTITHREAD and POOL_DEBUG at the same time"
#endif
#endif

#ifdef ALLOC_USE_MALLOC
#undef BLOCK_MINFREE
#undef BLOCK_MINALLOC
#define BLOCK_MINFREE	0
#define BLOCK_MINALLOC	0
#endif

/*****************************************************************
 *
 * Managing free storage blocks...
 */

union align {
    /* Types which are likely to have the longest RELEVANT alignment
     * restrictions...
     */

    char *cp;
    void (*f) (void);
    long l;
    FILE *fp;
    double d;
};

#define CLICK_SZ (sizeof(union align))

union block_hdr {
    union align a;

    /* Actual header... */

    struct {
	char *endp;
	union block_hdr *next;
	char *first_avail;
#ifdef POOL_DEBUG
	union block_hdr *global_next;
	struct pool *owning_pool;
#endif
    } h;
};

static union block_hdr *block_freelist = NULL;
static mutex *alloc_mutex = NULL;
static mutex *spawn_mutex = NULL;
#ifdef POOL_DEBUG
static char *known_stack_point;
static int stack_direction;
static union block_hdr *global_block_list;
#define FREE_POOL	((struct pool *)(-1))
#endif

#ifdef ALLOC_DEBUG
#define FILL_BYTE	((char)(0xa5))

#define debug_fill(ptr,size)	((void)memset((ptr), FILL_BYTE, (size)))

static ap_inline void debug_verify_filled(const char *ptr,
    const char *endp, const char *error_msg)
{
    for (; ptr < endp; ++ptr) {
	if (*ptr != FILL_BYTE) {
	    fputs(error_msg, stderr);
	    abort();
	    exit(1);
	}
    }
}

#else
#define debug_fill(a,b)
#define debug_verify_filled(a,b,c)
#endif


/* Get a completely new block from the system pool. Note that we rely on
   malloc() to provide aligned memory. */

static union block_hdr *malloc_block(int size)
{
    union block_hdr *blok =
	(union block_hdr *) malloc(size + sizeof(union block_hdr));

    if (blok == NULL) {
	fprintf(stderr, "Ouch!  malloc failed in malloc_block()\n");
	exit(1);
    }
    debug_fill(blok, size + sizeof(union block_hdr));
    blok->h.next = NULL;
    blok->h.first_avail = (char *) (blok + 1);
    blok->h.endp = size + blok->h.first_avail;
#ifdef POOL_DEBUG
    blok->h.global_next = global_block_list;
    global_block_list = blok;
    blok->h.owning_pool = NULL;
#endif

    return blok;
}



#ifdef ALLOC_DEBUG
static void chk_on_blk_list(union block_hdr *blok, union block_hdr *free_blk)
{
    while (free_blk) {
	if (free_blk == blok) {
	    fprintf(stderr, "Ouch!  Freeing free block\n");
	    abort();
	    exit(1);
	}
	free_blk = free_blk->h.next;
    }
}
#else
#define chk_on_blk_list(_x, _y)
#endif

/* Free a chain of blocks --- must be called with alarms blocked. */

static void free_blocks(union block_hdr *blok)
{
#ifdef ALLOC_USE_MALLOC
    union block_hdr *next;

    for (; blok; blok = next) {
	next = blok->h.next;
	free(blok);
    }
#else
    /* First, put new blocks at the head of the free list ---
     * we'll eventually bash the 'next' pointer of the last block
     * in the chain to point to the free blocks we already had.
     */

    union block_hdr *old_free_list;

    if (blok == NULL)
	return;			/* Sanity check --- freeing empty pool? */

    (void) acquire_mutex(alloc_mutex);
    old_free_list = block_freelist;
    block_freelist = blok;

    /*
     * Next, adjust first_avail pointers of each block --- have to do it
     * sooner or later, and it simplifies the search in new_block to do it
     * now.
     */

    while (blok->h.next != NULL) {
	chk_on_blk_list(blok, old_free_list);
	blok->h.first_avail = (char *) (blok + 1);
	debug_fill(blok->h.first_avail, blok->h.endp - blok->h.first_avail);
#ifdef POOL_DEBUG
	blok->h.owning_pool = FREE_POOL;
#endif
	blok = blok->h.next;
    }

    chk_on_blk_list(blok, old_free_list);
    blok->h.first_avail = (char *) (blok + 1);
    debug_fill(blok->h.first_avail, blok->h.endp - blok->h.first_avail);
#ifdef POOL_DEBUG
    blok->h.owning_pool = FREE_POOL;
#endif

    /* Finally, reset next pointer to get the old free blocks back */

    blok->h.next = old_free_list;
    (void) release_mutex(alloc_mutex);
#endif
}


/* Get a new block, from our own free list if possible, from the system
 * if necessary.  Must be called with alarms blocked.
 */

static union block_hdr *new_block(int min_size)
{
    union block_hdr **lastptr = &block_freelist;
    union block_hdr *blok = block_freelist;

    /* First, see if we have anything of the required size
     * on the free list...
     */

    while (blok != NULL) {
	if (min_size + BLOCK_MINFREE <= blok->h.endp - blok->h.first_avail) {
	    *lastptr = blok->h.next;
	    blok->h.next = NULL;
	    debug_verify_filled(blok->h.first_avail, blok->h.endp,
		"Ouch!  Someone trounced a block on the free list!\n");
	    return blok;
	}
	else {
	    lastptr = &blok->h.next;
	    blok = blok->h.next;
	}
    }

    /* Nope. */

    min_size += BLOCK_MINFREE;
    blok = malloc_block((min_size > BLOCK_MINALLOC) ? min_size : BLOCK_MINALLOC);
    debug_verify_filled(blok->h.first_avail, blok->h.endp,
	"Ouch!  Someone trounced a block on the free list!\n");
    return blok;
}


/* Accounting */

static long bytes_in_block_list(union block_hdr *blok)
{
    long size = 0;

    while (blok) {
	size += blok->h.endp - (char *) (blok + 1);
	blok = blok->h.next;
    }

    return size;
}


/*****************************************************************
 *
 * Pool internals and management...
 * NB that subprocesses are not handled by the generic cleanup code,
 * basically because we don't want cleanups for multiple subprocesses
 * to result in multiple three-second pauses.
 */

struct process_chain;
struct cleanup;

static void run_cleanups(struct cleanup *);
static void free_proc_chain(struct process_chain *);

struct pool {
    union block_hdr *first;
    union block_hdr *last;
    struct cleanup *cleanups;
    struct process_chain *subprocesses;
    struct pool *sub_pools;
    struct pool *sub_next;
    struct pool *sub_prev;
    struct pool *parent;
    char *free_first_avail;
#ifdef ALLOC_USE_MALLOC
    void *allocation_list;
#endif
#ifdef POOL_DEBUG
    struct pool *joined;
#endif
};

static pool *permanent_pool;

/* Each pool structure is allocated in the start of its own first block,
 * so we need to know how many bytes that is (once properly aligned...).
 * This also means that when a pool's sub-pool is destroyed, the storage
 * associated with it is *completely* gone, so we have to make sure it
 * gets taken off the parent's sub-pool list...
 */

#define POOL_HDR_CLICKS (1 + ((sizeof(struct pool) - 1) / CLICK_SZ))
#define POOL_HDR_BYTES (POOL_HDR_CLICKS * CLICK_SZ)

API_EXPORT(struct pool *) make_sub_pool(struct pool *p)
{
    union block_hdr *blok;
    pool *new_pool;

    block_alarms();

    (void) acquire_mutex(alloc_mutex);

    blok = new_block(POOL_HDR_BYTES);
    new_pool = (pool *) blok->h.first_avail;
    blok->h.first_avail += POOL_HDR_BYTES;
#ifdef POOL_DEBUG
    blok->h.owning_pool = new_pool;
#endif

    memset((char *) new_pool, '\0', sizeof(struct pool));
    new_pool->free_first_avail = blok->h.first_avail;
    new_pool->first = new_pool->last = blok;

    if (p) {
	new_pool->parent = p;
	new_pool->sub_next = p->sub_pools;
	if (new_pool->sub_next)
	    new_pool->sub_next->sub_prev = new_pool;
	p->sub_pools = new_pool;
    }

    (void) release_mutex(alloc_mutex);
    unblock_alarms();

    return new_pool;
}

#ifdef POOL_DEBUG
static void stack_var_init(char *s)
{
    char t;

    if (s < &t) {
	stack_direction = 1; /* stack grows up */
    }
    else {
	stack_direction = -1; /* stack grows down */
    }
}
#endif

pool *init_alloc(void)
{
#ifdef POOL_DEBUG
    char s;

    known_stack_point = &s;
    stack_var_init(&s);
#endif
    alloc_mutex = create_mutex(NULL);
    spawn_mutex = create_mutex(NULL);
    permanent_pool = make_sub_pool(NULL);

    return permanent_pool;
}

API_EXPORT(void) clear_pool(struct pool *a)
{
    block_alarms();

    (void) acquire_mutex(alloc_mutex);
    while (a->sub_pools)
	destroy_pool(a->sub_pools);
    (void) release_mutex(alloc_mutex);
    /* Don't hold the mutex during cleanups. */
    run_cleanups(a->cleanups);
    a->cleanups = NULL;
    free_proc_chain(a->subprocesses);
    a->subprocesses = NULL;
    free_blocks(a->first->h.next);
    a->first->h.next = NULL;

    a->last = a->first;
    a->first->h.first_avail = a->free_first_avail;
    debug_fill(a->first->h.first_avail,
	a->first->h.endp - a->first->h.first_avail);

#ifdef ALLOC_USE_MALLOC
    {
	void *c, *n;

	for (c = a->allocation_list; c; c = n) {
	    n = *(void **)c;
	    free(c);
	}
	a->allocation_list = NULL;
    }
#endif

    unblock_alarms();
}

API_EXPORT(void) destroy_pool(pool *a)
{
    block_alarms();
    clear_pool(a);

    (void) acquire_mutex(alloc_mutex);
    if (a->parent) {
	if (a->parent->sub_pools == a)
	    a->parent->sub_pools = a->sub_next;
	if (a->sub_prev)
	    a->sub_prev->sub_next = a->sub_next;
	if (a->sub_next)
	    a->sub_next->sub_prev = a->sub_prev;
    }
    (void) release_mutex(alloc_mutex);

    free_blocks(a->first);
    unblock_alarms();
}

API_EXPORT(long) bytes_in_pool(pool *p)
{
    return bytes_in_block_list(p->first);
}
API_EXPORT(long) bytes_in_free_blocks(void)
{
    return bytes_in_block_list(block_freelist);
}

/*****************************************************************
 * POOL_DEBUG support
 */
#ifdef POOL_DEBUG

/* the unix linker defines this symbol as the last byte + 1 of
 * the executable... so it includes TEXT, BSS, and DATA
 */
extern char _end;

/* is ptr in the range [lo,hi) */
#define is_ptr_in_range(ptr, lo, hi)	\
    (((unsigned long)(ptr) - (unsigned long)(lo)) \
	< \
	(unsigned long)(hi) - (unsigned long)(lo))

/* Find the pool that ts belongs to, return NULL if it doesn't
 * belong to any pool.
 */
pool *find_pool(const void *ts)
{
    const char *s = ts;
    union block_hdr **pb;
    union block_hdr *b;

    /* short-circuit stuff which is in TEXT, BSS, or DATA */
    if (is_ptr_in_range(s, 0, &_end)) {
	return NULL;
    }
    /* consider stuff on the stack to also be in the NULL pool...
     * XXX: there's cases where we don't want to assume this
     */
    if ((stack_direction == -1 && is_ptr_in_range(s, &ts, known_stack_point))
	|| (stack_direction == 1 && is_ptr_in_range(s, known_stack_point, &ts))) {
	abort();
	return NULL;
    }
    block_alarms();
    /* search the global_block_list */
    for (pb = &global_block_list; *pb; pb = &b->h.global_next) {
	b = *pb;
	if (is_ptr_in_range(s, b, b->h.endp)) {
	    if (b->h.owning_pool == FREE_POOL) {
		fprintf(stderr,
		    "Ouch!  find_pool() called on pointer in a free block\n");
		abort();
		exit(1);
	    }
	    if (b != global_block_list) {
		/* promote b to front of list, this is a hack to speed
		 * up the lookup */
		*pb = b->h.global_next;
		b->h.global_next = global_block_list;
		global_block_list = b;
	    }
	    unblock_alarms();
	    return b->h.owning_pool;
	}
    }
    unblock_alarms();
    return NULL;
}

/* return TRUE iff a is an ancestor of b
 * NULL is considered an ancestor of all pools
 */
int pool_is_ancestor(pool *a, pool *b)
{
    if (a == NULL) {
	return 1;
    }
    while (a->joined) {
	a = a->joined;
    }
    while (b) {
	if (a == b) {
	    return 1;
	}
	b = b->parent;
    }
    return 0;
}

/* All blocks belonging to sub will be changed to point to p
 * instead.  This is a guarantee by the caller that sub will not
 * be destroyed before p is.
 */
API_EXPORT(void) pool_join(pool *p, pool *sub)
{
    union block_hdr *b;

    /* We could handle more general cases... but this is it for now. */
    if (sub->parent != p) {
	fprintf(stderr, "pool_join: p is not parent of sub\n");
	abort();
    }
    block_alarms();
    while (p->joined) {
	p = p->joined;
    }
    sub->joined = p;
    for (b = global_block_list; b; b = b->h.global_next) {
	if (b->h.owning_pool == sub) {
	    b->h.owning_pool = p;
	}
    }
    unblock_alarms();
}
#endif

/*****************************************************************
 *
 * Allocating stuff...
 */


API_EXPORT(void *) palloc(struct pool *a, int reqsize)
{
#ifdef ALLOC_USE_MALLOC
    int size = reqsize + CLICK_SZ;
    void *ptr;

    block_alarms();
    ptr = malloc(size);
    if (ptr == NULL) {
	fputs("Ouch!  Out of memory!\n", stderr);
	exit(1);
    }
    debug_fill(ptr, size); /* might as well get uninitialized protection */
    *(void **)ptr = a->allocation_list;
    a->allocation_list = ptr;
    unblock_alarms();
    return (char *)ptr + CLICK_SZ;
#else

    /* Round up requested size to an even number of alignment units (core clicks)
     */

    int nclicks = 1 + ((reqsize - 1) / CLICK_SZ);
    int size = nclicks * CLICK_SZ;

    /* First, see if we have space in the block most recently
     * allocated to this pool
     */

    union block_hdr *blok = a->last;
    char *first_avail = blok->h.first_avail;
    char *new_first_avail;

    if (reqsize <= 0)
	return NULL;

    new_first_avail = first_avail + size;

    if (new_first_avail <= blok->h.endp) {
	debug_verify_filled(first_avail, blok->h.endp,
	    "Ouch!  Someone trounced past the end of their allocation!\n");
	blok->h.first_avail = new_first_avail;
	return (void *) first_avail;
    }

    /* Nope --- get a new one that's guaranteed to be big enough */

    block_alarms();

    (void) acquire_mutex(alloc_mutex);

    blok = new_block(size);
    a->last->h.next = blok;
    a->last = blok;
#ifdef POOL_DEBUG
    blok->h.owning_pool = a;
#endif

    (void) release_mutex(alloc_mutex);

    unblock_alarms();

    first_avail = blok->h.first_avail;
    blok->h.first_avail += size;

    return (void *) first_avail;
#endif
}

API_EXPORT(void *) pcalloc(struct pool *a, int size)
{
    void *res = palloc(a, size);
    memset(res, '\0', size);
    return res;
}

API_EXPORT(char *) pstrdup(struct pool *a, const char *s)
{
    char *res;
    size_t len;

    if (s == NULL)
	return NULL;
    len = strlen(s) + 1;
    res = palloc(a, len);
    memcpy(res, s, len);
    return res;
}

API_EXPORT(char *) pstrndup(struct pool *a, const char *s, int n)
{
    char *res;

    if (s == NULL)
	return NULL;
    res = palloc(a, n + 1);
    memcpy(res, s, n);
    res[n] = '\0';
    return res;
}

char *pstrcat(pool *a,...)
{
    char *cp, *argp, *res;

    /* Pass one --- find length of required string */

    int len = 0;
    va_list adummy;

    va_start(adummy, a);

    while ((cp = va_arg(adummy, char *)) != NULL)
	     len += strlen(cp);

    va_end(adummy);

    /* Allocate the required string */

    res = (char *) palloc(a, len + 1);
    cp = res;
    *cp = '\0';

    /* Pass two --- copy the argument strings into the result space */

    va_start(adummy, a);

    while ((argp = va_arg(adummy, char *)) != NULL) {
	strcpy(cp, argp);
	cp += strlen(argp);
    }

    va_end(adummy);

    /* Return the result string */

    return res;
}

/* XXX */
#ifdef ALLOC_USE_MALLOC
#error "psprintf does not support ALLOC_USE_MALLOC yet..."
#endif

/* psprintf is implemented by writing directly into the current
 * block of the pool, starting right at first_avail.  If there's
 * insufficient room, then a new block is allocated and the earlier
 * output is copied over.  The new block isn't linked into the pool
 * until all the output is done.
 */

struct psprintf_data {
    pool *p;
    union block_hdr *blok;
    char *strp;
    int got_a_new_block;
};

static int psprintf_write(void *vdata, const char *inp, size_t len)
{
    struct psprintf_data *ps;
    union block_hdr *blok;
    union block_hdr *nblok;
    size_t cur_len;
    char *strp;

    ps = vdata;

    /* does it fit in the current block? */
    blok = ps->blok;
    strp = ps->strp;
    if (strp + len + 1 < blok->h.endp) {
	memcpy(strp, inp, len);
	ps->strp = strp + len;
	return 0;
    }

    cur_len = strp - blok->h.first_avail;

    /* must try another blok */
    block_alarms();
    (void) acquire_mutex(alloc_mutex);
    nblok = new_block((cur_len + len)*2);
    (void) release_mutex(alloc_mutex);
    unblock_alarms();
    strp = nblok->h.first_avail;
    memcpy(strp, blok->h.first_avail, cur_len);
    strp += cur_len;
    memcpy(strp, inp, len);
    strp += len;
    ps->strp = strp;

    /* did we allocate the current blok? if so free it up */
    if (ps->got_a_new_block) {
	debug_fill(blok->h.first_avail, blok->h.endp - blok->h.first_avail);
	block_alarms();
	(void) acquire_mutex(alloc_mutex);
	blok->h.next = block_freelist;
	block_freelist = blok;
	(void) release_mutex(alloc_mutex);
	unblock_alarms();
    }
    ps->blok = nblok;
    ps->got_a_new_block = 1;
    return 0;
}

API_EXPORT(char *) pvsprintf(pool *p, const char *fmt, va_list ap)
{
    struct psprintf_data ps;
    char *strp;
    int size;

    ps.p = p;
    ps.blok = p->last;
    ps.strp = ps.blok->h.first_avail;
    ps.got_a_new_block = 0;

    apapi_vformatter(psprintf_write, &ps, fmt, ap);

    strp = ps.strp;
    *strp++ = '\0';

    size = strp - ps.blok->h.first_avail;
    size = (1 + ((size - 1) / CLICK_SZ)) * CLICK_SZ;
    strp = ps.blok->h.first_avail;	/* save away result pointer */
    ps.blok->h.first_avail += size;

    /* have to link the block in if it's a new one */
    if (ps.got_a_new_block) {
	p->last->h.next = ps.blok;
	p->last = ps.blok;
#ifdef POOL_DEBUG
	ps.blok->h.owning_pool = p;
#endif
    }

    return strp;
}

API_EXPORT_NONSTD(char *) psprintf(pool *p, const char *fmt, ...)
{
    va_list ap;
    char *res;

    va_start(ap, fmt);
    res = pvsprintf(p, fmt, ap);
    va_end(ap);
    return res;
}

/*****************************************************************
 *
 * The 'array' functions...
 */

static void make_array_core(array_header *res, pool *p, int nelts, int elt_size)
{
    if (nelts < 1)
	nelts = 1;		/* Assure sanity if someone asks for
				 * array of zero elts.
				 */

    res->elts = pcalloc(p, nelts * elt_size);

    res->pool = p;
    res->elt_size = elt_size;
    res->nelts = 0;		/* No active elements yet... */
    res->nalloc = nelts;	/* ...but this many allocated */
}

API_EXPORT(array_header *) make_array(pool *p, int nelts, int elt_size)
{
    array_header *res = (array_header *) palloc(p, sizeof(array_header));

    make_array_core(res, p, nelts, elt_size);
    return res;
}

API_EXPORT(void *) push_array(array_header *arr)
{
    if (arr->nelts == arr->nalloc) {
	int new_size = (arr->nalloc <= 0) ? 1 : arr->nalloc * 2;
	char *new_data;

	new_data = pcalloc(arr->pool, arr->elt_size * new_size);

	memcpy(new_data, arr->elts, arr->nalloc * arr->elt_size);
	arr->elts = new_data;
	arr->nalloc = new_size;
    }

    ++arr->nelts;
    return arr->elts + (arr->elt_size * (arr->nelts - 1));
}

API_EXPORT(void) array_cat(array_header *dst, const array_header *src)
{
    int elt_size = dst->elt_size;

    if (dst->nelts + src->nelts > dst->nalloc) {
	int new_size = (dst->nalloc <= 0) ? 1 : dst->nalloc * 2;
	char *new_data;

	while (dst->nelts + src->nelts > new_size)
	    new_size *= 2;

	new_data = pcalloc(dst->pool, elt_size * new_size);
	memcpy(new_data, dst->elts, dst->nalloc * elt_size);

	dst->elts = new_data;
	dst->nalloc = new_size;
    }

    memcpy(dst->elts + dst->nelts * elt_size, src->elts, elt_size * src->nelts);
    dst->nelts += src->nelts;
}

API_EXPORT(array_header *) copy_array(pool *p, const array_header *arr)
{
    array_header *res = make_array(p, arr->nalloc, arr->elt_size);

    memcpy(res->elts, arr->elts, arr->elt_size * arr->nelts);
    res->nelts = arr->nelts;
    return res;
}

/* This cute function copies the array header *only*, but arranges
 * for the data section to be copied on the first push or arraycat.
 * It's useful when the elements of the array being copied are
 * read only, but new stuff *might* get added on the end; we have the
 * overhead of the full copy only where it is really needed.
 */

static ap_inline void copy_array_hdr_core(array_header *res,
    const array_header *arr)
{
    res->elts = arr->elts;
    res->elt_size = arr->elt_size;
    res->nelts = arr->nelts;
    res->nalloc = arr->nelts;	/* Force overflow on push */
}

API_EXPORT(array_header *) copy_array_hdr(pool *p, const array_header *arr)
{
    array_header *res = (array_header *) palloc(p, sizeof(array_header));

    res->pool = p;
    copy_array_hdr_core(res, arr);
    return res;
}

/* The above is used here to avoid consing multiple new array bodies... */

API_EXPORT(array_header *) append_arrays(pool *p,
					 const array_header *first,
					 const array_header *second)
{
    array_header *res = copy_array_hdr(p, first);

    array_cat(res, second);
    return res;
}


/*****************************************************************
 *
 * The "table" functions.
 */

/* XXX: if you tweak this you should look at is_empty_table() and table_elts()
 * in alloc.h */
struct table {
    /* This has to be first to promote backwards compatibility with
     * older modules which cast a table * to an array_header *...
     * they should use the table_elts() function for most of the
     * cases they do this for.
     */
    array_header a;
#ifdef MAKE_TABLE_PROFILE
    void *creator;
#endif
};

#ifdef MAKE_TABLE_PROFILE
static table_entry *table_push(table *t)
{
    if (t->a.nelts == t->a.nalloc) {
	fprintf(stderr,
	    "table_push: table created by %p hit limit of %u\n",
	    t->creator, t->a.nalloc);
    }
    return (table_entry *) push_array(&t->a);
}
#else
#define table_push(t)	((table_entry *) push_array(&(t)->a))
#endif


API_EXPORT(table *) make_table(pool *p, int nelts)
{
    table *t = palloc(p, sizeof(table));

    make_array_core(&t->a, p, nelts, sizeof(table_entry));
#ifdef MAKE_TABLE_PROFILE
    t->creator = __builtin_return_address(0);
#endif
    return t;
}

API_EXPORT(table *) copy_table(pool *p, const table *t)
{
    table *new = palloc(p, sizeof(table));

#ifdef POOL_DEBUG
    /* we don't copy keys and values, so it's necessary that t->a.pool
     * have a life span at least as long as p
     */
    if (!pool_is_ancestor(t->a.pool, p)) {
	fprintf(stderr, "copy_table: t's pool is not an ancestor of p\n");
	abort();
    }
#endif
    make_array_core(&new->a, p, t->a.nalloc, sizeof(table_entry));
    memcpy(new->a.elts, t->a.elts, t->a.nelts * sizeof(table_entry));
    new->a.nelts = t->a.nelts;
    return new;
}

API_EXPORT(void) clear_table(table *t)
{
    t->a.nelts = 0;
}

API_EXPORT(char *) table_get(const table *t, const char *key)
{
    table_entry *elts = (table_entry *) t->a.elts;
    int i;

    if (key == NULL)
	return NULL;

    for (i = 0; i < t->a.nelts; ++i)
	if (!strcasecmp(elts[i].key, key))
	    return elts[i].val;

    return NULL;
}

API_EXPORT(void) table_set(table *t, const char *key, const char *val)
{
    register int i, j, k;
    table_entry *elts = (table_entry *) t->a.elts;
    int done = 0;

    for (i = 0; i < t->a.nelts; ) {
	if (!strcasecmp(elts[i].key, key)) {
	    if (!done) {
		elts[i].val = pstrdup(t->a.pool, val);
		done = 1;
		++i;
	    }
	    else {		/* delete an extraneous element */
		for (j = i, k = i + 1; k < t->a.nelts; ++j, ++k) {
		    elts[j].key = elts[k].key;
		    elts[j].val = elts[k].val;
		}
		--t->a.nelts;
	    }
	}
	else {
	    ++i;
	}
    }

    if (!done) {
	elts = (table_entry *) table_push(t);
	elts->key = pstrdup(t->a.pool, key);
	elts->val = pstrdup(t->a.pool, val);
    }
}

API_EXPORT(void) table_setn(table *t, const char *key, const char *val)
{
    register int i, j, k;
    table_entry *elts = (table_entry *) t->a.elts;
    int done = 0;

#ifdef POOL_DEBUG
    {
	if (!pool_is_ancestor(find_pool(key), t->a.pool)) {
	    fprintf(stderr, "table_set: key not in ancestor pool of t\n");
	    abort();
	}
	if (!pool_is_ancestor(find_pool(val), t->a.pool)) {
	    fprintf(stderr, "table_set: key not in ancestor pool of t\n");
	    abort();
	}
    }
#endif

    for (i = 0; i < t->a.nelts; ) {
	if (!strcasecmp(elts[i].key, key)) {
	    if (!done) {
		elts[i].val = (char *)val;
		done = 1;
		++i;
	    }
	    else {		/* delete an extraneous element */
		for (j = i, k = i + 1; k < t->a.nelts; ++j, ++k) {
		    elts[j].key = elts[k].key;
		    elts[j].val = elts[k].val;
		}
		--t->a.nelts;
	    }
	}
	else {
	    ++i;
	}
    }

    if (!done) {
	elts = (table_entry *) table_push(t);
	elts->key = (char *)key;
	elts->val = (char *)val;
    }
}

API_EXPORT(void) table_unset(table *t, const char *key)
{
    register int i, j, k;
    table_entry *elts = (table_entry *) t->a.elts;

    for (i = 0; i < t->a.nelts;) {
	if (!strcasecmp(elts[i].key, key)) {

	    /* found an element to skip over
	     * there are any number of ways to remove an element from
	     * a contiguous block of memory.  I've chosen one that
	     * doesn't do a memcpy/bcopy/array_delete, *shrug*...
	     */
	    for (j = i, k = i + 1; k < t->a.nelts; ++j, ++k) {
		elts[j].key = elts[k].key;
		elts[j].val = elts[k].val;
	    }
	    --t->a.nelts;
	}
	else {
	    ++i;
	}
    }
}

API_EXPORT(void) table_merge(table *t, const char *key, const char *val)
{
    table_entry *elts = (table_entry *) t->a.elts;
    int i;

    for (i = 0; i < t->a.nelts; ++i)
	if (!strcasecmp(elts[i].key, key)) {
	    elts[i].val = pstrcat(t->a.pool, elts[i].val, ", ", val, NULL);
	    return;
	}

    elts = (table_entry *) table_push(t);
    elts->key = pstrdup(t->a.pool, key);
    elts->val = pstrdup(t->a.pool, val);
}

API_EXPORT(void) table_mergen(table *t, const char *key, const char *val)
{
    table_entry *elts = (table_entry *) t->a.elts;
    int i;

#ifdef POOL_DEBUG
    {
	if (!pool_is_ancestor(find_pool(key), t->a.pool)) {
	    fprintf(stderr, "table_set: key not in ancestor pool of t\n");
	    abort();
	}
	if (!pool_is_ancestor(find_pool(val), t->a.pool)) {
	    fprintf(stderr, "table_set: key not in ancestor pool of t\n");
	    abort();
	}
    }
#endif

    for (i = 0; i < t->a.nelts; ++i) {
	if (!strcasecmp(elts[i].key, key)) {
	    elts[i].val = pstrcat(t->a.pool, elts[i].val, ", ", val, NULL);
	    return;
	}
    }

    elts = (table_entry *) table_push(t);
    elts->key = (char *)key;
    elts->val = (char *)val;
}

API_EXPORT(void) table_add(table *t, const char *key, const char *val)
{
    table_entry *elts = (table_entry *) t->a.elts;

    elts = (table_entry *) table_push(t);
    elts->key = pstrdup(t->a.pool, key);
    elts->val = pstrdup(t->a.pool, val);
}

API_EXPORT(void) table_addn(table *t, const char *key, const char *val)
{
    table_entry *elts = (table_entry *) t->a.elts;

#ifdef POOL_DEBUG
    {
	if (!pool_is_ancestor(find_pool(key), t->a.pool)) {
	    fprintf(stderr, "table_set: key not in ancestor pool of t\n");
	    abort();
	}
	if (!pool_is_ancestor(find_pool(val), t->a.pool)) {
	    fprintf(stderr, "table_set: key not in ancestor pool of t\n");
	    abort();
	}
    }
#endif

    elts = (table_entry *) table_push(t);
    elts->key = (char *)key;
    elts->val = (char *)val;
}

API_EXPORT(table *) overlay_tables(pool *p, const table *overlay, const table *base)
{
    table *res;

#ifdef POOL_DEBUG
    /* we don't copy keys and values, so it's necessary that
     * overlay->a.pool and base->a.pool have a life span at least
     * as long as p
     */
    if (!pool_is_ancestor(overlay->a.pool, p)) {
	fprintf(stderr, "overlay_tables: overlay's pool is not an ancestor of p\n");
	abort();
    }
    if (!pool_is_ancestor(base->a.pool, p)) {
	fprintf(stderr, "overlay_tables: base's pool is not an ancestor of p\n");
	abort();
    }
#endif

    res = palloc(p, sizeof(table));
    /* behave like append_arrays */
    res->a.pool = p;
    copy_array_hdr_core(&res->a, &overlay->a);
    array_cat(&res->a, &base->a);

    return res;
}

/* And now for something completely abstract ...

 * For each key value given as a vararg:
 *   run the function pointed to as
 *     int comp(void *r, char *key, char *value);
 *   on each valid key-value pair in the table t that matches the vararg key,
 *   or once for every valid key-value pair if the vararg list is empty,
 *   until the function returns false (0) or we finish the table.
 *
 * Note that we restart the traversal for each vararg, which means that
 * duplicate varargs will result in multiple executions of the function
 * for each matching key.  Note also that if the vararg list is empty,
 * only one traversal will be made and will cut short if comp returns 0.
 *
 * Note that the table_get and table_merge functions assume that each key in
 * the table is unique (i.e., no multiple entries with the same key).  This
 * function does not make that assumption, since it (unfortunately) isn't
 * true for some of Apache's tables.
 *
 * Note that rec is simply passed-on to the comp function, so that the
 * caller can pass additional info for the task.
 */
void table_do(int (*comp) (void *, const char *, const char *), void *rec,
	      const table *t,...)
{
    va_list vp;
    char *argp;
    table_entry *elts = (table_entry *) t->a.elts;
    int rv, i;

    va_start(vp, t);

    argp = va_arg(vp, char *);

    do {
	for (rv = 1, i = 0; rv && (i < t->a.nelts); ++i) {
	    if (elts[i].key && (!argp || !strcasecmp(elts[i].key, argp))) {
		rv = (*comp) (rec, elts[i].key, elts[i].val);
	    }
	}
    } while (argp && ((argp = va_arg(vp, char *)) != NULL));

    va_end(vp);
}

/*****************************************************************
 *
 * Managing generic cleanups.  
 */

struct cleanup {
    void *data;
    void (*plain_cleanup) (void *);
    void (*child_cleanup) (void *);
    struct cleanup *next;
};

API_EXPORT(void) register_cleanup(pool *p, void *data, void (*plain_cleanup) (void *),
				  void (*child_cleanup) (void *))
{
    struct cleanup *c = (struct cleanup *) palloc(p, sizeof(struct cleanup));
    c->data = data;
    c->plain_cleanup = plain_cleanup;
    c->child_cleanup = child_cleanup;
    c->next = p->cleanups;
    p->cleanups = c;
}

API_EXPORT(void) kill_cleanup(pool *p, void *data, void (*cleanup) (void *))
{
    struct cleanup *c = p->cleanups;
    struct cleanup **lastp = &p->cleanups;

    while (c) {
	if (c->data == data && c->plain_cleanup == cleanup) {
	    *lastp = c->next;
	    break;
	}

	lastp = &c->next;
	c = c->next;
    }
}

API_EXPORT(void) run_cleanup(pool *p, void *data, void (*cleanup) (void *))
{
    block_alarms();		/* Run cleanup only once! */
    (*cleanup) (data);
    kill_cleanup(p, data, cleanup);
    unblock_alarms();
}

static void run_cleanups(struct cleanup *c)
{
    while (c) {
	(*c->plain_cleanup) (c->data);
	c = c->next;
    }
}

static void run_child_cleanups(struct cleanup *c)
{
    while (c) {
	(*c->child_cleanup) (c->data);
	c = c->next;
    }
}

static void cleanup_pool_for_exec(pool *p)
{
    run_child_cleanups(p->cleanups);
    p->cleanups = NULL;

    for (p = p->sub_pools; p; p = p->sub_next)
	cleanup_pool_for_exec(p);
}

API_EXPORT(void) cleanup_for_exec(void)
{
#ifndef WIN32
    /*
     * Don't need to do anything on NT, because I
     * am actually going to spawn the new process - not
     * exec it. All handles that are not inheritable, will
     * be automajically closed. The only problem is with
     * file handles that are open, but there isn't much
     * I can do about that (except if the child decides
     * to go out and close them
     */
    block_alarms();
    cleanup_pool_for_exec(permanent_pool);
    unblock_alarms();
#endif /* ndef WIN32 */
}

API_EXPORT_NONSTD(void) null_cleanup(void *data)
{
    /* do nothing cleanup routine */
}

/*****************************************************************
 *
 * Files and file descriptors; these are just an application of the
 * generic cleanup interface.
 */

static void fd_cleanup(void *fdv)
{
    close((int) (long) fdv);
}

API_EXPORT(void) note_cleanups_for_fd(pool *p, int fd)
{
    register_cleanup(p, (void *) (long) fd, fd_cleanup, fd_cleanup);
}

API_EXPORT(void) kill_cleanups_for_fd(pool *p, int fd)
{
    kill_cleanup(p, (void *) (long) fd, fd_cleanup);
}

API_EXPORT(int) popenf(pool *a, const char *name, int flg, int mode)
{
    int fd;
    int save_errno;

    block_alarms();
    fd = open(name, flg, mode);
    save_errno = errno;
    if (fd >= 0) {
	fd = ap_slack(fd, AP_SLACK_HIGH);
	note_cleanups_for_fd(a, fd);
    }
    unblock_alarms();
    errno = save_errno;
    return fd;
}

API_EXPORT(int) pclosef(pool *a, int fd)
{
    int res;
    int save_errno;

    block_alarms();
    res = close(fd);
    save_errno = errno;
    kill_cleanup(a, (void *) (long) fd, fd_cleanup);
    unblock_alarms();
    errno = save_errno;
    return res;
}

/* Note that we have separate plain_ and child_ cleanups for FILE *s,
 * since fclose() would flush I/O buffers, which is extremely undesirable;
 * we just close the descriptor.
 */

static void file_cleanup(void *fpv)
{
    fclose((FILE *) fpv);
}
static void file_child_cleanup(void *fpv)
{
    close(fileno((FILE *) fpv));
}

API_EXPORT(void) note_cleanups_for_file(pool *p, FILE *fp)
{
    register_cleanup(p, (void *) fp, file_cleanup, file_child_cleanup);
}

API_EXPORT(FILE *) pfopen(pool *a, const char *name, const char *mode)
{
    FILE *fd = NULL;
    int baseFlag, desc;
    int modeFlags = 0;

#ifdef WIN32
    modeFlags = _S_IREAD | _S_IWRITE;
#else
    modeFlags = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
#endif

    block_alarms();

    if (*mode == 'a') {
	/* Work around faulty implementations of fopen */
	baseFlag = (*(mode + 1) == '+') ? O_RDWR : O_WRONLY;
	desc = open(name, baseFlag | O_APPEND | O_CREAT,
		    modeFlags);
	if (desc >= 0) {
	    desc = ap_slack(desc, AP_SLACK_LOW);
	    fd = fdopen(desc, mode);
	}
    }
    else {
	fd = fopen(name, mode);
    }

    if (fd != NULL)
	note_cleanups_for_file(a, fd);
    unblock_alarms();
    return fd;
}

API_EXPORT(FILE *) pfdopen(pool *a, int fd, const char *mode)
{
    FILE *f;

    block_alarms();
    f = fdopen(fd, mode);
    if (f != NULL)
	note_cleanups_for_file(a, f);
    unblock_alarms();
    return f;
}


API_EXPORT(int) pfclose(pool *a, FILE *fd)
{
    int res;

    block_alarms();
    res = fclose(fd);
    kill_cleanup(a, (void *) fd, file_cleanup);
    unblock_alarms();
    return res;
}

/*
 * DIR * with cleanup
 */

static void dir_cleanup(void *dv)
{
    closedir((DIR *) dv);
}

API_EXPORT(DIR *) popendir(pool *p, const char *name)
{
    DIR *d;
    int save_errno;

    block_alarms();
    d = opendir(name);
    if (d == NULL) {
	save_errno = errno;
	unblock_alarms();
	errno = save_errno;
	return NULL;
    }
    register_cleanup(p, (void *) d, dir_cleanup, dir_cleanup);
    unblock_alarms();
    return d;
}

API_EXPORT(void) pclosedir(pool *p, DIR * d)
{
    block_alarms();
    kill_cleanup(p, (void *) d, dir_cleanup);
    closedir(d);
    unblock_alarms();
}

/*****************************************************************
 *
 * Files and file descriptors; these are just an application of the
 * generic cleanup interface.
 */

static void socket_cleanup(void *fdv)
{
    closesocket((int) (long) fdv);
}

API_EXPORT(void) note_cleanups_for_socket(pool *p, int fd)
{
    register_cleanup(p, (void *) (long) fd, socket_cleanup, socket_cleanup);
}

API_EXPORT(void) kill_cleanups_for_socket(pool *p, int sock)
{
    kill_cleanup(p, (void *) (long) sock, socket_cleanup);
}

API_EXPORT(int) psocket(pool *p, int domain, int type, int protocol)
{
    int fd;

    block_alarms();
    fd = socket(domain, type, protocol);
    if (fd == -1) {
	int save_errno = errno;
	unblock_alarms();
	errno = save_errno;
	return -1;
    }
    note_cleanups_for_socket(p, fd);
    unblock_alarms();
    return fd;
}

API_EXPORT(int) pclosesocket(pool *a, int sock)
{
    int res;
    int save_errno;

    block_alarms();
    res = closesocket(sock);
#ifdef WIN32
    errno = WSAGetLastError();
#endif /* WIN32 */
    save_errno = errno;
    kill_cleanup(a, (void *) (long) sock, socket_cleanup);
    unblock_alarms();
    errno = save_errno;
    return res;
}


/*
 * Here's a pool-based interface to POSIX regex's regcomp().
 * Note that we return regex_t instead of being passed one.
 * The reason is that if you use an already-used regex_t structure,
 * the memory that you've already allocated gets forgotten, and
 * regfree() doesn't clear it. So we don't allow it.
 */

static void regex_cleanup(void *preg)
{
    regfree((regex_t *) preg);
}

API_EXPORT(regex_t *) pregcomp(pool *p, const char *pattern, int cflags)
{
    regex_t *preg = palloc(p, sizeof(regex_t));

    if (regcomp(preg, pattern, cflags))
	return NULL;

    register_cleanup(p, (void *) preg, regex_cleanup, regex_cleanup);

    return preg;
}


API_EXPORT(void) pregfree(pool *p, regex_t * reg)
{
    block_alarms();
    regfree(reg);
    kill_cleanup(p, (void *) reg, regex_cleanup);
    unblock_alarms();
}

/*****************************************************************
 *
 * More grotty system stuff... subprocesses.  Frump.  These don't use
 * the generic cleanup interface because I don't want multiple
 * subprocesses to result in multiple three-second pauses; the
 * subprocesses have to be "freed" all at once.  If someone comes
 * along with another resource they want to allocate which has the
 * same property, we might want to fold support for that into the
 * generic interface, but for now, it's a special case
 */

struct process_chain {
    pid_t pid;
    enum kill_conditions kill_how;
    struct process_chain *next;
};

API_EXPORT(void) note_subprocess(pool *a, int pid, enum kill_conditions how)
{
    struct process_chain *new =
    (struct process_chain *) palloc(a, sizeof(struct process_chain));

    new->pid = pid;
    new->kill_how = how;
    new->next = a->subprocesses;
    a->subprocesses = new;
}

#ifdef WIN32
#define os_pipe(fds) _pipe(fds, 512, O_BINARY | O_NOINHERIT)
#else
#define os_pipe(fds) pipe(fds)
#endif /* WIN32 */

/* for fdopen, to get binary mode */
#if defined (__EMX__) || defined (WIN32)
#define BINMODE	"b"
#else
#define BINMODE
#endif

static int spawn_child_err_core(pool *p, int (*func) (void *), void *data,
				enum kill_conditions kill_how,
				int *pipe_in, int *pipe_out, int *pipe_err)
{
    int pid;
    int in_fds[2];
    int out_fds[2];
    int err_fds[2];
    int save_errno;

    if (pipe_in && os_pipe(in_fds) < 0) {
	return 0;
    }

    if (pipe_out && os_pipe(out_fds) < 0) {
	save_errno = errno;
	if (pipe_in) {
	    close(in_fds[0]);
	    close(in_fds[1]);
	}
	errno = save_errno;
	return 0;
    }

    if (pipe_err && os_pipe(err_fds) < 0) {
	save_errno = errno;
	if (pipe_in) {
	    close(in_fds[0]);
	    close(in_fds[1]);
	}
	if (pipe_out) {
	    close(out_fds[0]);
	    close(out_fds[1]);
	}
	errno = save_errno;
	return 0;
    }

#ifdef WIN32

    {
	HANDLE thread_handle;
	int hStdIn, hStdOut, hStdErr;
	int old_priority;

	(void) acquire_mutex(spawn_mutex);
	thread_handle = GetCurrentThread();	/* doesn't need to be closed */
	old_priority = GetThreadPriority(thread_handle);
	SetThreadPriority(thread_handle, THREAD_PRIORITY_HIGHEST);
	/* Now do the right thing with your pipes */
	if (pipe_in) {
	    hStdIn = dup(fileno(stdin));
	    if(dup2(in_fds[0], fileno(stdin)))
		aplog_error(APLOG_MARK, APLOG_ERR, NULL, "dup2(stdin) failed");
	    close(in_fds[0]);
	}
	if (pipe_out) {
	    hStdOut = dup(fileno(stdout));
	    close(fileno(stdout));
	    if(dup2(out_fds[1], fileno(stdout)))
		aplog_error(APLOG_MARK, APLOG_ERR, NULL, "dup2(stdout) failed");
	    close(out_fds[1]);
	}
	if (pipe_err) {
	    hStdErr = dup(fileno(stderr));
	    if(dup2(err_fds[1], fileno(stderr)))
		aplog_error(APLOG_MARK, APLOG_ERR, NULL, "dup2(stdin) failed");
	    close(err_fds[1]);
	}

	pid = (*func) (data);
        if (pid == -1) pid = 0;   /* map Win32 error code onto Unix default */

        if (!pid) {
	    save_errno = errno;
	    close(in_fds[1]);
	    close(out_fds[0]);
	    close(err_fds[0]);
	}

	/* restore the original stdin, stdout and stderr */
	if (pipe_in) {
	    dup2(hStdIn, fileno(stdin));
	    close(hStdIn);
        }
	if (pipe_out) {
	    dup2(hStdOut, fileno(stdout));
	    close(hStdOut);
	}
	if (pipe_err) {
	    dup2(hStdErr, fileno(stderr));
	    close(hStdErr);
	}

        if (pid) {
	    note_subprocess(p, pid, kill_how);
	    if (pipe_in) {
		*pipe_in = in_fds[1];
	    }
	    if (pipe_out) {
		*pipe_out = out_fds[0];
	    }
	    if (pipe_err) {
		*pipe_err = err_fds[0];
	    }
	}
	SetThreadPriority(thread_handle, old_priority);
	(void) release_mutex(spawn_mutex);
	/*
	 * go on to the end of the function, where you can
	 * unblock alarms and return the pid
	 */

    }
#else

    if ((pid = fork()) < 0) {
	save_errno = errno;
	if (pipe_in) {
	    close(in_fds[0]);
	    close(in_fds[1]);
	}
	if (pipe_out) {
	    close(out_fds[0]);
	    close(out_fds[1]);
	}
	if (pipe_err) {
	    close(err_fds[0]);
	    close(err_fds[1]);
	}
	errno = save_errno;
	return 0;
    }

    if (!pid) {
	/* Child process */
	RAISE_SIGSTOP(SPAWN_CHILD);

	if (pipe_out) {
	    close(out_fds[0]);
	    dup2(out_fds[1], STDOUT_FILENO);
	    close(out_fds[1]);
	}

	if (pipe_in) {
	    close(in_fds[1]);
	    dup2(in_fds[0], STDIN_FILENO);
	    close(in_fds[0]);
	}

	if (pipe_err) {
	    close(err_fds[0]);
	    dup2(err_fds[1], STDERR_FILENO);
	    close(err_fds[1]);
	}

	/* HP-UX SIGCHLD fix goes here, if someone will remind me what it is... */
	signal(SIGCHLD, SIG_DFL);	/* Was that it? */

	func(data);
	exit(1);		/* Should only get here if the exec in func() failed */
    }

    /* Parent process */

    note_subprocess(p, pid, kill_how);

    if (pipe_out) {
	close(out_fds[1]);
	*pipe_out = out_fds[0];
    }

    if (pipe_in) {
	close(in_fds[0]);
	*pipe_in = in_fds[1];
    }

    if (pipe_err) {
	close(err_fds[1]);
	*pipe_err = err_fds[0];
    }
#endif /* WIN32 */

    return pid;
}


API_EXPORT(int) spawn_child_err(pool *p, int (*func) (void *), void *data,
				enum kill_conditions kill_how,
			   FILE **pipe_in, FILE **pipe_out, FILE **pipe_err)
{
    int fd_in, fd_out, fd_err;
    int pid, save_errno;

    block_alarms();

    pid = spawn_child_err_core(p, func, data, kill_how,
			       pipe_in ? &fd_in : NULL,
			       pipe_out ? &fd_out : NULL,
			       pipe_err ? &fd_err : NULL);

    if (pid == 0) {
	save_errno = errno;
	unblock_alarms();
	errno = save_errno;
	return 0;
    }

    if (pipe_out) {
	*pipe_out = fdopen(fd_out, "r" BINMODE);
	if (*pipe_out)
	    note_cleanups_for_file(p, *pipe_out);
	else
	    close(fd_out);
    }

    if (pipe_in) {
	*pipe_in = fdopen(fd_in, "w" BINMODE);
	if (*pipe_in)
	    note_cleanups_for_file(p, *pipe_in);
	else
	    close(fd_in);
    }

    if (pipe_err) {
	*pipe_err = fdopen(fd_err, "r" BINMODE);
	if (*pipe_err)
	    note_cleanups_for_file(p, *pipe_err);
	else
	    close(fd_err);
    }

    unblock_alarms();
    return pid;
}


API_EXPORT(int) spawn_child_err_buff(pool *p, int (*func) (void *), void *data,
				     enum kill_conditions kill_how,
			   BUFF **pipe_in, BUFF **pipe_out, BUFF **pipe_err)
{
    int fd_in, fd_out, fd_err;
    int pid, save_errno;

    block_alarms();

    pid = spawn_child_err_core(p, func, data, kill_how,
			       pipe_in ? &fd_in : NULL,
			       pipe_out ? &fd_out : NULL,
			       pipe_err ? &fd_err : NULL);

    if (pid == 0) {
	save_errno = errno;
	unblock_alarms();
	errno = save_errno;
	return 0;
    }

    if (pipe_out) {
	*pipe_out = bcreate(p, B_RD);
	note_cleanups_for_fd(p, fd_out);
	bpushfd(*pipe_out, fd_out, fd_out);
    }

    if (pipe_in) {
	*pipe_in = bcreate(p, B_WR);
	note_cleanups_for_fd(p, fd_in);
	bpushfd(*pipe_in, fd_in, fd_in);
    }

    if (pipe_err) {
	*pipe_err = bcreate(p, B_RD);
	note_cleanups_for_fd(p, fd_err);
	bpushfd(*pipe_err, fd_err, fd_err);
    }

    unblock_alarms();
    return pid;
}

static void free_proc_chain(struct process_chain *procs)
{
    /* Dispose of the subprocesses we've spawned off in the course of
     * whatever it was we're cleaning up now.  This may involve killing
     * some of them off...
     */

    struct process_chain *p;
    int need_timeout = 0;
    int status;

    if (procs == NULL)
	return;			/* No work.  Whew! */

    /* First, check to see if we need to do the SIGTERM, sleep, SIGKILL
     * dance with any of the processes we're cleaning up.  If we've got
     * any kill-on-sight subprocesses, ditch them now as well, so they
     * don't waste any more cycles doing whatever it is that they shouldn't
     * be doing anymore.
     */
#ifdef WIN32
    /* Pick up all defunct processes */
    for (p = procs; p; p = p->next) {
	if (GetExitCodeProcess((HANDLE) p->pid, &status)) {
	    p->kill_how = kill_never;
	}
    }


    for (p = procs; p; p = p->next) {
	if (p->kill_how == kill_after_timeout) {
	    need_timeout = 1;
	}
	else if (p->kill_how == kill_always) {
	    TerminateProcess((HANDLE) p->pid, 1);
	}
    }
    /* Sleep only if we have to... */

    if (need_timeout)
	sleep(3);

    /* OK, the scripts we just timed out for have had a chance to clean up
     * --- now, just get rid of them, and also clean up the system accounting
     * goop...
     */

    for (p = procs; p; p = p->next) {
	if (p->kill_how == kill_after_timeout)
	    TerminateProcess((HANDLE) p->pid, 1);
    }

    for (p = procs; p; p = p->next) {
	CloseHandle((HANDLE) p->pid);
    }
#else
#ifndef NEED_WAITPID
    /* Pick up all defunct processes */
    for (p = procs; p; p = p->next) {
	if (waitpid(p->pid, (int *) 0, WNOHANG) > 0) {
	    p->kill_how = kill_never;
	}
    }
#endif

    for (p = procs; p; p = p->next) {
	if ((p->kill_how == kill_after_timeout)
	    || (p->kill_how == kill_only_once)) {
	    /* Subprocess may be dead already.  Only need the timeout if not. */
	    if (kill(p->pid, SIGTERM) != -1)
		need_timeout = 1;
	}
	else if (p->kill_how == kill_always) {
	    kill(p->pid, SIGKILL);
	}
    }

    /* Sleep only if we have to... */

    if (need_timeout)
	sleep(3);

    /* OK, the scripts we just timed out for have had a chance to clean up
     * --- now, just get rid of them, and also clean up the system accounting
     * goop...
     */

    for (p = procs; p; p = p->next) {

	if (p->kill_how == kill_after_timeout)
	    kill(p->pid, SIGKILL);

	if (p->kill_how != kill_never)
	    waitpid(p->pid, &status, 0);
    }
#endif /* WIN32 */
}
