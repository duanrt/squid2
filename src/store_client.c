
/*
 * $Id$
 *
 * DEBUG: section 20    Storage Manager Client-Side Interface
 * AUTHOR: Duane Wessels
 *
 * SQUID Web Proxy Cache          http://www.squid-cache.org/
 * ----------------------------------------------------------
 *
 *  Squid is the result of efforts by numerous individuals from
 *  the Internet community; see the CONTRIBUTORS file for full
 *  details.   Many organizations have provided support for Squid's
 *  development; see the SPONSORS file for full details.  Squid is
 *  Copyrighted (C) 2001 by the Regents of the University of
 *  California; see the COPYRIGHT file for full details.  Squid
 *  incorporates software developed and/or copyrighted by other
 *  sources; see the CREDITS file for full details.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *  
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *  
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111, USA.
 *
 */

#include "squid.h"

/*
 * NOTE: 'Header' refers to the swapfile metadata header.
 *       'Body' refers to the swapfile body, which is the full
 *        HTTP reply (including HTTP headers and body).
 */
static STRCB storeClientReadBody;
static STRCB storeClientReadHeader;
static void storeClientCopy2(StoreEntry * e, store_client * sc);
static void storeClientCopy3(StoreEntry * e, store_client * sc);
static void storeClientFileRead(store_client * sc);
static EVH storeClientCopyEvent;
static store_client_t storeClientType(StoreEntry *);
static int CheckQuickAbort2(StoreEntry * entry);
static void CheckQuickAbort(StoreEntry * entry);

#if STORE_CLIENT_LIST_DEBUG
static store_client *
storeClientListSearch(const MemObject * mem, void *data)
{
    dlink_node *node;
    store_client *sc = NULL;
    for (node = mem->clients.head; node; node = node->next) {
	sc = node->data;
	if (sc->owner == data)
	    return sc;
    }
    return NULL;
}
#endif

static store_client_t
storeClientType(StoreEntry * e)
{
    MemObject *mem = e->mem_obj;
    if (mem->inmem_lo)
	return STORE_DISK_CLIENT;
    if (EBIT_TEST(e->flags, ENTRY_ABORTED)) {
	/* I don't think we should be adding clients to aborted entries */
	debug(20, 1) ("storeClientType: adding to ENTRY_ABORTED entry\n");
	return STORE_MEM_CLIENT;
    }
    if (e->store_status == STORE_OK) {
	if (mem->inmem_lo == 0 && mem->inmem_hi > 0)
	    return STORE_MEM_CLIENT;
	else
	    return STORE_DISK_CLIENT;
    }
    /* here and past, entry is STORE_PENDING */
    /*
     * If this is the first client, let it be the mem client
     */
    else if (mem->nclients == 1)
	return STORE_MEM_CLIENT;
    /*
     * If there is no disk file to open yet, we must make this a
     * mem client.  If we can't open the swapin file before writing
     * to the client, there is no guarantee that we will be able
     * to open it later when we really need it.
     */
    else if (e->swap_status == SWAPOUT_NONE)
	return STORE_MEM_CLIENT;
    /*
     * otherwise, make subsequent clients read from disk so they
     * can not delay the first, and vice-versa.
     */
    else
	return STORE_DISK_CLIENT;
}

/* add client with fd to client list */
store_client *
storeClientRegister(StoreEntry * e, void *owner)
{
    MemObject *mem = e->mem_obj;
    store_client *sc;
    assert(mem);
    e->refcount++;
    mem->nclients++;
    sc = cbdataAlloc(store_client);
    sc->callback_data = NULL;
    sc->seen_offset = 0;
    sc->copy_offset = 0;
    sc->flags.disk_io_pending = 0;
    sc->entry = e;
    storeLockObject(sc->entry);
    sc->type = storeClientType(e);
#if STORE_CLIENT_LIST_DEBUG
    assert(!storeClientListSearch(mem, owner));
    sc->owner = owner;
#endif
    dlinkAdd(sc, &sc->node, &mem->clients);
#if DELAY_POOLS
    sc->delay_id = 0;
#endif
    return sc;
}

static void
storeClientCallback(store_client * sc, ssize_t sz)
{
    STNCB *new_callback = sc->new_callback;
    void *cbdata = sc->callback_data;
    mem_node_ref nr;

    assert(sc->new_callback);
    sc->new_callback = NULL;
    sc->callback_data = NULL;
    nr = sc->node_ref;		/* XXX this should be a reference; and we should dereference our copy! */
    /* This code "transfers" its ownership (and reference) of the node_ref to the caller. Ugly, but works. */
    sc->node_ref.node = NULL;
    sc->node_ref.offset = -1;
    /* Can't use XMIN here - sz is signed; copy_size isn't; things get messy */
    if (sz < 0)
	new_callback(cbdata, nr, -1);
    else
	new_callback(cbdata, nr, XMIN(sz, sc->copy_size));
    cbdataUnlock(cbdata);
}

static void
storeClientCopyEvent(void *data)
{
    store_client *sc = data;
    debug(20, 3) ("storeClientCopyEvent: Running\n");
    sc->flags.copy_event_pending = 0;
    if (!sc->new_callback)
	return;
    storeClientCopy2(sc->entry, sc);
}

/* copy bytes requested by the client */

void
storeClientRef(store_client * sc,
    StoreEntry * e,
    squid_off_t seen_offset,
    squid_off_t copy_offset,
    size_t size,
    STNCB * callback,
    void *data)
{
    debug(20, 3) ("storeClientRef: %s, seen %" PRINTF_OFF_T ", want %" PRINTF_OFF_T ", size %d, cb %p, cbdata %p\n",
	storeKeyText(e->hash.key),
	seen_offset,
	copy_offset,
	(int) size,
	callback,
	data);
    assert(sc != NULL);
#if STORE_CLIENT_LIST_DEBUG
    assert(sc == storeClientListSearch(e->mem_obj, data));
#endif
    assert(sc->new_callback == NULL);
    assert(sc->entry == e);
    sc->seen_offset = seen_offset;
    sc->new_callback = callback;
    sc->callback_data = data;
    cbdataLock(sc->callback_data);
    sc->copy_size = size;
    sc->copy_offset = copy_offset;
    /* If the read is being deferred, run swapout in case this client has the 
     * lowest seen_offset. storeSwapOut() frees the memory and clears the 
     * ENTRY_DEFER_READ bit if necessary */
    if (EBIT_TEST(e->flags, ENTRY_DEFER_READ)) {
	storeSwapOut(e);
    }
    storeClientCopy2(e, sc);
}

/*
 * This function is used below to decide if we have any more data to
 * send to the client.  If the store_status is STORE_PENDING, then we
 * do have more data to send.  If its STORE_OK, then
 * we continue checking.  If the object length is negative, then we
 * don't know the real length and must open the swap file to find out.
 * If the length is >= 0, then we compare it to the requested copy
 * offset.
 */
static int
storeClientNoMoreToSend(StoreEntry * e, store_client * sc)
{
    squid_off_t len;
    if (e->store_status == STORE_PENDING)
	return 0;
    if ((len = objectLen(e)) < 0)
	return 0;
    if (sc->copy_offset < len)
	return 0;
    return 1;
}

static void
storeClientCopy2(StoreEntry * e, store_client * sc)
{
    if (sc->flags.copy_event_pending)
	return;
    if (EBIT_TEST(e->flags, ENTRY_FWD_HDR_WAIT)) {
	debug(20, 5) ("storeClientCopy2: returning because ENTRY_FWD_HDR_WAIT set\n");
	return;
    }
    if (sc->flags.store_copying) {
	sc->flags.copy_event_pending = 1;
	debug(20, 3) ("storeClientCopy2: Queueing storeClientCopyEvent()\n");
	eventAdd("storeClientCopyEvent", storeClientCopyEvent, sc, 0.0, 0);
	return;
    }
    cbdataLock(sc);		/* ick, prevent sc from getting freed */
    sc->flags.store_copying = 1;
    debug(20, 3) ("storeClientCopy2: %s\n", storeKeyText(e->hash.key));
    assert(sc->new_callback);
    /*
     * We used to check for ENTRY_ABORTED here.  But there were some
     * problems.  For example, we might have a slow client (or two) and
     * the server-side is reading far ahead and swapping to disk.  Even
     * if the server-side aborts, we want to give the client(s)
     * everything we got before the abort condition occurred.
     */
    storeClientCopy3(e, sc);
    sc->flags.store_copying = 0;
    cbdataUnlock(sc);		/* ick, allow sc to be freed */
}

static void
storeClientCopy3(StoreEntry * e, store_client * sc)
{
    MemObject *mem = e->mem_obj;
    ssize_t sz = -1;

    if (storeClientNoMoreToSend(e, sc)) {
	/* There is no more to send! */
	storeClientCallback(sc, 0);
	return;
    }
    if (e->store_status == STORE_PENDING && sc->seen_offset >= mem->inmem_hi) {
	/* client has already seen this, wait for more */
	debug(20, 3) ("storeClientCopy3: Waiting for more\n");

	/* If the read is backed off and all clients have seen all the data in
	 * memory, re-poll the fd */
	if ((EBIT_TEST(e->flags, ENTRY_DEFER_READ)) &&
	    (storeLowestMemReaderOffset(e) == mem->inmem_hi)) {
	    debug(20, 3) ("storeClientCopy3: %s - clearing ENTRY_DEFER_READ\n", e->mem_obj->url);
	    /* Clear the flag and re-poll the fd */
	    storeResumeRead(e);
	}
	return;
    }
    /*
     * Slight weirdness here.  We open a swapin file for any
     * STORE_DISK_CLIENT, even if we can copy the requested chunk
     * from memory in the next block.  We must try to open the
     * swapin file before sending any data to the client side.  If
     * we postpone the open, and then can not open the file later
     * on, the client loses big time.  Its transfer just gets cut
     * off.  Better to open it early (while the client side handler
     * is clientCacheHit) so that we can fall back to a cache miss
     * if needed.
     */
    if (STORE_DISK_CLIENT == sc->type && NULL == sc->swapin_sio) {
	debug(20, 3) ("storeClientCopy3: Need to open swap in file\n");
	/* gotta open the swapin file */
	if (storeTooManyDiskFilesOpen()) {
	    /* yuck -- this causes a TCP_SWAPFAIL_MISS on the client side */
	    storeClientCallback(sc, -1);
	    return;
	} else if (!sc->flags.disk_io_pending) {
	    /* Don't set store_io_pending here */
	    storeSwapInStart(sc);
	    if (NULL == sc->swapin_sio) {
		storeClientCallback(sc, -1);
		return;
	    }
	    /*
	     * If the open succeeds we either copy from memory, or
	     * schedule a disk read in the next block.
	     */
	} else {
	    debug(20, 1) ("WARNING: Averted multiple fd operation (1)\n");
	    return;
	}
    }
    if (sc->copy_offset >= mem->inmem_lo && sc->copy_offset < mem->inmem_hi) {
	/* What the client wants is in memory */
	debug(20, 3) ("storeClientCopy3: Copying from memory\n");
	assert(sc->new_callback);
	assert(sc->node_ref.node == NULL);	/* We should never, ever have a node here; or we'd leak! */
	sz = stmemRef(&mem->data_hdr, sc->copy_offset, &sc->node_ref);
	if (EBIT_TEST(e->flags, RELEASE_REQUEST))
	    storeSwapOutMaintainMemObject(e);
	storeClientCallback(sc, sz);
	return;
    }
    /* What the client wants is not in memory. Schedule a disk read */
    assert(STORE_DISK_CLIENT == sc->type);
    assert(!sc->flags.disk_io_pending);
    debug(20, 3) ("storeClientCopy3: reading from STORE\n");
    /* Just in case there's a node here; free it */
    stmemNodeUnref(&sc->node_ref);
    storeClientFileRead(sc);
}

static void
storeClientFileRead(store_client * sc)
{
    MemObject *mem = sc->entry->mem_obj;
    assert(sc->new_callback);
    assert(!sc->flags.disk_io_pending);
    sc->flags.disk_io_pending = 1;
    assert(sc->node_ref.node == NULL);	/* We should never, ever have a node here; or we'd leak! */
    stmemNodeRefCreate(&sc->node_ref);	/* Creates an entry with reference count == 1 */
    if (mem->swap_hdr_sz == 0) {
	storeRead(sc->swapin_sio,
	    sc->node_ref.node->data,
	    XMIN(SM_PAGE_SIZE, sc->copy_size),
	    0,
	    storeClientReadHeader,
	    sc);
    } else {
	if (sc->entry->swap_status == SWAPOUT_WRITING)
	    assert(storeSwapOutObjectBytesOnDisk(mem) > sc->copy_offset);	/* XXX is this right? Shouldn't we incl. mem->swap_hdr_sz? */
	storeRead(sc->swapin_sio,
	    sc->node_ref.node->data,
	    XMIN(SM_PAGE_SIZE, sc->copy_size),
	    sc->copy_offset + mem->swap_hdr_sz,
	    storeClientReadBody,
	    sc);
    }
}

/*
 * Try to parse the header.
 * return -1 on error, 0 on more required, +1 on completed.
 */
static int
storeClientParseHeader(store_client * sc, const char *b, int l)
{
    if (sc->copy_offset == 0 && l > 0 && memHaveHeaders(sc->entry->mem_obj) == 0)
	return httpReplyParse(sc->entry->mem_obj->reply, b, headersEnd(b, l));
    else
	return 1;
}

static void
storeClientReadBody(void *data, const char *buf_unused, ssize_t len)
{
    char *cbuf = NULL;
    store_client *sc = data;
    assert(sc->flags.disk_io_pending);

    sc->flags.disk_io_pending = 0;
    assert(sc->new_callback);
    assert(sc->node_ref.node);
    cbuf = sc->node_ref.node->data;
    /* XXX update how much data in that mem page is active; argh this should be done in a storage layer */
    sc->node_ref.node->len = len;
    debug(20, 3) ("storeClientReadBody: len %d\n", (int) len);
    (void) storeClientParseHeader(sc, cbuf, len);
    storeClientCallback(sc, len);
}

static void
storeClientReadHeader(void *data, const char *buf_unused, ssize_t len)
{
    static int md5_mismatches = 0;
    store_client *sc = data;
    StoreEntry *e = sc->entry;
    MemObject *mem = e->mem_obj;
    int swap_hdr_sz = 0;
    size_t body_sz;
    size_t copy_sz;
    tlv *tlv_list;
    tlv *t;
    char *cbuf;
    int swap_object_ok = 1;
    char *new_url = NULL;
    char *new_store_url = NULL;
    assert(sc->flags.disk_io_pending);
    sc->flags.disk_io_pending = 0;
    assert(sc->new_callback);
    assert(sc->node_ref.node);
    cbuf = sc->node_ref.node->data;
    debug(20, 3) ("storeClientReadHeader: len %d\n", (int) len);
    /* XXX update how much data in that mem page is active; argh this should be done in a storage layer */
    sc->node_ref.node->len = len;
    if (len < 0) {
	debug(20, 3) ("storeClientReadHeader: %s\n", xstrerror());
	storeClientCallback(sc, len);
	return;
    }
    assert(len <= SM_PAGE_SIZE);
    tlv_list = storeSwapMetaUnpack(cbuf, &swap_hdr_sz);
    if (swap_hdr_sz > len) {
	/* oops, bad disk file? */
	debug(20, 1) ("WARNING: swapfile header too small\n");
	storeClientCallback(sc, -1);
	return;
    }
    if (tlv_list == NULL) {
	debug(20, 1) ("WARNING: failed to unpack meta data\n");
	storeClientCallback(sc, -1);
	return;
    }
    /*
     * Check the meta data and make sure we got the right object.
     */
    for (t = tlv_list; t && swap_object_ok; t = t->next) {
	switch (t->type) {
	case STORE_META_KEY:
	    assert(t->length == SQUID_MD5_DIGEST_LENGTH);
	    if (!EBIT_TEST(e->flags, KEY_PRIVATE) &&
		memcmp(t->value, e->hash.key, SQUID_MD5_DIGEST_LENGTH)) {
		debug(20, 2) ("storeClientReadHeader: swapin MD5 mismatch\n");
		debug(20, 2) ("\t%s\n", storeKeyText(t->value));
		debug(20, 2) ("\t%s\n", storeKeyText(e->hash.key));
		if (isPowTen(++md5_mismatches))
		    debug(20, 1) ("WARNING: %d swapin MD5 mismatches\n",
			md5_mismatches);
		swap_object_ok = 0;
	    }
	    break;
	case STORE_META_URL:
	    new_url = xstrdup(t->value);
	    break;
	case STORE_META_STOREURL:
	    new_store_url = xstrdup(t->value);
	    break;
	case STORE_META_OBJSIZE:
	    break;
	case STORE_META_STD:
	case STORE_META_STD_LFS:
	    break;
	case STORE_META_VARY_HEADERS:
	    if (mem->vary_headers) {
		if (strcmp(mem->vary_headers, t->value) != 0)
		    swap_object_ok = 0;
	    } else {
		/* Assume the object is OK.. remember the vary request headers */
		mem->vary_headers = xstrdup(t->value);
	    }
	    break;
	default:
	    debug(20, 2) ("WARNING: got unused STORE_META type %d\n", t->type);
	    break;
	}
    }

    /* Check url / store_url */
    do {
	if (new_url == NULL) {
	    debug(20, 1) ("storeClientReadHeader: no URL!\n");
	    swap_object_ok = 0;
	    break;
	}
	/*
	 * If we have a store URL then it must match the requested object URL.
	 * The theory is that objects with a store URL have been normalised
	 * and thus a direct access which didn't go via the rewrite framework
	 * are illegal!
	 */
	if (new_store_url) {
	    if (NULL == mem->store_url)
		mem->store_url = new_store_url;
	    else if (0 == strcasecmp(mem->store_url, new_store_url))
		(void) 0;	/* a match! */
	    else {
		debug(20, 1) ("storeClientReadHeader: store URL mismatch\n");
		debug(20, 1) ("\t{%s} != {%s}\n", (char *) new_store_url, mem->store_url);
		swap_object_ok = 0;
		break;
	    }
	}
	/* If we have no store URL then the request and the memory URL must match */
	if ((!new_store_url) && mem->url && strcasecmp(mem->url, new_url) != 0) {
	    debug(20, 1) ("storeClientReadHeader: URL mismatch\n");
	    debug(20, 1) ("\t{%s} != {%s}\n", (char *) new_url, mem->url);
	    swap_object_ok = 0;
	    break;
	}
    } while (0);

    storeSwapTLVFree(tlv_list);
    xfree(new_url);
    /* don't free new_store_url if its owned by the mem object now */
    if (mem->store_url != new_store_url)
	xfree(new_store_url);

    if (!swap_object_ok) {
	storeClientCallback(sc, -1);
	return;
    }
    mem->swap_hdr_sz = swap_hdr_sz;
    mem->object_sz = e->swap_file_sz - swap_hdr_sz;
    /*
     * If our last read got some data the client wants, then give
     * it to them, otherwise schedule another read.
     */
    body_sz = len - swap_hdr_sz;
    if (sc->copy_offset < body_sz) {
	/*
	 * we have (part of) what they want
	 */
	copy_sz = XMIN(sc->copy_size, body_sz);
	debug(20, 3) ("storeClientReadHeader: copying %d bytes of body\n",
	    (int) copy_sz);
	debug(20, 8) ("sc %p; node_ref->node %p; data %p; copy size %d; data size %d\n",
	    sc, sc->node_ref.node, sc->node_ref.node->data, (int) copy_sz, (int) len);
	xmemmove(cbuf, cbuf + swap_hdr_sz, copy_sz);
	if (sc->copy_offset == 0 && len > 0 && memHaveHeaders(mem) == 0)
	    (void) storeClientParseHeader(sc, cbuf, copy_sz);
	storeClientCallback(sc, copy_sz);
	return;
    }
    /*
     * we don't have what the client wants, but at least we now
     * know the swap header size.
     */
    /* Just in case there's a node here; free it */
    stmemNodeUnref(&sc->node_ref);
    storeClientFileRead(sc);
}

int
storeClientCopyPending(store_client * sc, StoreEntry * e, void *data)
{
#if STORE_CLIENT_LIST_DEBUG
    assert(sc == storeClientListSearch(e->mem_obj, data));
#endif
    assert(sc->entry == e);
    if (sc == NULL)
	return 0;
    if (sc->new_callback == NULL)
	return 0;
    return 1;
}

/*
 * This routine hasn't been optimised to take advantage of the
 * passed sc. Yet.
 */
int
storeClientUnregister(store_client * sc, StoreEntry * e, void *owner)
{
    MemObject *mem = e->mem_obj;
    if (sc == NULL)
	return 0;
    debug(20, 3) ("storeClientUnregister: called for '%s'\n", storeKeyText(e->hash.key));
#if STORE_CLIENT_LIST_DEBUG
    assert(sc == storeClientListSearch(e->mem_obj, owner));
#endif
    assert(sc->entry == e);
    if (mem->clients.head == NULL)
	return 0;
    dlinkDelete(&sc->node, &mem->clients);
    mem->nclients--;
    if (e->store_status == STORE_OK && e->swap_status != SWAPOUT_DONE)
	storeSwapOut(e);
    if (sc->swapin_sio) {
	storeClose(sc->swapin_sio);
	cbdataUnlock(sc->swapin_sio);
	sc->swapin_sio = NULL;
	statCounter.swap.ins++;
    }
    if (NULL != sc->new_callback) {
	/* callback with ssize = -1 to indicate unexpected termination */
	debug(20, 3) ("storeClientUnregister: store_client for %s has a callback\n",
	    mem->url);
	storeClientCallback(sc, -1);
    }
    stmemNodeUnref(&sc->node_ref);
#if DELAY_POOLS
    delayUnregisterDelayIdPtr(&sc->delay_id);
#endif
    storeSwapOutMaintainMemObject(e);
    if (mem->nclients == 0)
	CheckQuickAbort(e);
    storeUnlockObject(sc->entry);
    sc->entry = NULL;
    cbdataFree(sc);
    return 1;
}

squid_off_t
storeLowestMemReaderOffset(const StoreEntry * entry)
{
    const MemObject *mem = entry->mem_obj;
    squid_off_t lowest = mem->inmem_hi + 1;
    squid_off_t highest = -1;
    store_client *sc;
    dlink_node *nx = NULL;
    dlink_node *node;

    for (node = mem->clients.head; node; node = nx) {
	sc = node->data;
	nx = node->next;
	if (sc->copy_offset > highest)
	    highest = sc->copy_offset;
	if (mem->swapout.sio != NULL && sc->type != STORE_MEM_CLIENT)
	    continue;
	if (sc->copy_offset < lowest)
	    lowest = sc->copy_offset;
    }
    if (highest < lowest && highest >= 0)
	return highest;
    return lowest;
}

/* Call handlers waiting for  data to be appended to E. */
void
InvokeHandlers(StoreEntry * e)
{
    int i = 0;
    MemObject *mem = e->mem_obj;
    store_client *sc;
    dlink_node *nx = NULL;
    dlink_node *node;

    debug(20, 3) ("InvokeHandlers: %s\n", storeKeyText(e->hash.key));
    /* walk the entire list looking for valid callbacks */
    for (node = mem->clients.head; node; node = nx) {
	sc = node->data;
	nx = node->next;
	debug(20, 3) ("InvokeHandlers: checking client #%d\n", i++);
	if (sc->new_callback == NULL)
	    continue;
	if (sc->flags.disk_io_pending)
	    continue;
	storeClientCopy2(e, sc);
    }
}

int
storePendingNClients(const StoreEntry * e)
{
    MemObject *mem = e->mem_obj;
    int npend = NULL == mem ? 0 : mem->nclients;
    debug(20, 3) ("storePendingNClients: returning %d\n", npend);
    return npend;
}

/* return 1 if the request should be aborted */
static int
CheckQuickAbort2(StoreEntry * entry)
{
    squid_off_t curlen;
    squid_off_t minlen;
    squid_off_t expectlen;
    MemObject *mem = entry->mem_obj;
    assert(mem);
    debug(20, 3) ("CheckQuickAbort2: entry=%p, mem=%p\n", entry, mem);
    if (mem->request && !mem->request->flags.cachable) {
	debug(20, 3) ("CheckQuickAbort2: YES !mem->request->flags.cachable\n");
	return 1;
    }
    expectlen = httpReplyBodySize(mem->method, mem->reply) + mem->reply->hdr_sz;
    curlen = mem->inmem_hi;
    if (expectlen == curlen) {
	debug(20, 3) ("CheckQuickAbort2: NO already finished\n");
	return 0;
    }
    if (EBIT_TEST(entry->flags, KEY_PRIVATE)) {
	debug(20, 3) ("CheckQuickAbort2: YES KEY_PRIVATE\n");
	return 1;
    }
    minlen = Config.quickAbort.min << 10;
    if (minlen < 0) {
	debug(20, 3) ("CheckQuickAbort2: NO disabled\n");
	return 0;
    }
    if (curlen > expectlen) {
	debug(20, 3) ("CheckQuickAbort2: YES bad content length\n");
	return 1;
    }
    if ((expectlen - curlen) < minlen) {
	debug(20, 3) ("CheckQuickAbort2: NO only little more left\n");
	return 0;
    }
    if ((expectlen - curlen) > (Config.quickAbort.max << 10)) {
	debug(20, 3) ("CheckQuickAbort2: YES too much left to go\n");
	return 1;
    }
    if (expectlen < 100) {
	debug(20, 3) ("CheckQuickAbort2: NO avoid FPE\n");
	return 0;
    }
    if ((curlen / (expectlen / 100)) > Config.quickAbort.pct) {
	debug(20, 3) ("CheckQuickAbort2: NO past point of no return\n");
	return 0;
    }
    debug(20, 3) ("CheckQuickAbort2: YES default, returning 1\n");
    return 1;
}

static void
CheckQuickAbort(StoreEntry * entry)
{
    if (entry == NULL)
	return;
    if (storePendingNClients(entry) > 0)
	return;
    if (entry->store_status != STORE_PENDING)
	return;
    if (EBIT_TEST(entry->flags, ENTRY_SPECIAL))
	return;
    if (CheckQuickAbort2(entry) == 0)
	return;
    statCounter.aborted_requests++;
    storeAbort(entry);
}

static void
storeClientCopyHeadersCB(void *data, mem_node_ref nr, ssize_t size)
{
    store_client *sc = data;
    assert(sc->header_cbdata);
    assert(sc->header_callback);
    stmemNodeUnref(&nr);
    /* XXX should cbdata lock/unlock the cbdata? */
    if (size < 0 || !memHaveHeaders(sc->entry->mem_obj)) {
	sc->header_callback(sc->header_cbdata, NULL);
	return;
    }
    sc->header_callback(sc->header_cbdata, sc->entry->mem_obj->reply);
}

/*
 * This is the eventual API which store clients should use to fetch the headers.
 */
void
storeClientCopyHeaders(store_client * sc, StoreEntry * e, STHCB * callback, void *callback_data)
{
    sc->header_callback = callback;
    sc->header_cbdata = callback_data;

    /* This kicks off either the memory read, waiting for the data to appear, or the disk read */
    storeClientRef(sc, e, 0, 0, SM_PAGE_SIZE, storeClientCopyHeadersCB, sc);
}
