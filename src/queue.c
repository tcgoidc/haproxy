/*
 * Queue management functions.
 *
 * Copyright 2000-2009 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

/* Short explanation on the locking, which is far from being trivial : a
 * pendconn is a list element which necessarily is associated with an existing
 * stream. It has pendconn->strm always valid. A pendconn may only be in one of
 * these three states :
 *   - unlinked : in this case it is an empty list head ;
 *   - linked into the server's queue ;
 *   - linked into the proxy's queue.
 *
 * A stream does not necessarily have such a pendconn. Thus the pendconn is
 * designated by the stream->pend_pos pointer. This results in some properties :
 *   - pendconn->strm->pend_pos is never NULL for any valid pendconn
 *   - if p->node.node.leaf_p is NULL, the element is unlinked,
 *     otherwise it necessarily belongs to one of the other lists ; this may
 *     not be atomically checked under threads though ;
 *   - pendconn->px is never NULL if pendconn->list is not empty
 *   - pendconn->srv is never NULL if pendconn->list is in the server's queue,
 *     and is always NULL if pendconn->list is in the backend's queue or empty.
 *   - pendconn->target is NULL while the element is queued, and points to the
 *     assigned server when the pendconn is picked.
 *
 * Threads complicate the design a little bit but rules remain simple :
 *   - the server's queue lock must be held at least when manipulating the
 *     server's queue, which is when adding a pendconn to the queue and when
 *     removing a pendconn from the queue. It protects the queue's integrity.
 *
 *   - the proxy's queue lock must be held at least when manipulating the
 *     proxy's queue, which is when adding a pendconn to the queue and when
 *     removing a pendconn from the queue. It protects the queue's integrity.
 *
 *   - both locks are compatible and may be held at the same time.
 *
 *   - a pendconn_add() is only performed by the stream which will own the
 *     pendconn ; the pendconn is allocated at this moment and returned ; it is
 *     added to either the server or the proxy's queue while holding this
s *     queue's lock.
 *
 *   - the pendconn is then met by a thread walking over the proxy or server's
 *     queue with the respective lock held. This lock is exclusive and the
 *     pendconn can only appear in one queue so by definition a single thread
 *     may find this pendconn at a time.
 *
 *   - the pendconn is unlinked either by its own stream upon success/abort/
 *     free, or by another one offering it its server slot. This is achieved by
 *     pendconn_process_next_strm() under either the server or proxy's lock,
 *     pendconn_redistribute() under the server's lock, or pendconn_unlink()
 *     under either the proxy's or the server's lock depending
 *     on the queue the pendconn is attached to.
 *
 *   - no single operation except the pendconn initialisation prior to the
 *     insertion are performed without eithre a queue lock held or the element
 *     being unlinked and visible exclusively to its stream.
 *
 *   - pendconn_process_next_strm() assign ->target so that the stream knows
 *     what server to work with (via pendconn_dequeue() which sets it on
 *     strm->target).
 *
 *   - a pendconn doesn't switch between queues, it stays where it is.
 */

#include <import/eb32tree.h>
#include <haproxy/api.h>
#include <haproxy/backend.h>
#include <haproxy/counters.h>
#include <haproxy/http_rules.h>
#include <haproxy/pool.h>
#include <haproxy/queue.h>
#include <haproxy/sample.h>
#include <haproxy/server-t.h>
#include <haproxy/stream.h>
#include <haproxy/task.h>
#include <haproxy/tcp_rules.h>
#include <haproxy/thread.h>
#include <haproxy/time.h>
#include <haproxy/tools.h>


#define NOW_OFFSET_BOUNDARY()          ((now_ms - (TIMER_LOOK_BACK >> 12)) & 0xfffff)
#define KEY_CLASS(key)                 ((u32)key & 0xfff00000)
#define KEY_OFFSET(key)                ((u32)key & 0x000fffff)
#define KEY_CLASS_OFFSET_BOUNDARY(key) (KEY_CLASS(key) | NOW_OFFSET_BOUNDARY())
#define MAKE_KEY(class, offset)        (((u32)(class + 0x7ff) << 20) | ((u32)(now_ms + offset) & 0xfffff))

DECLARE_POOL(pool_head_pendconn, "pendconn", sizeof(struct pendconn));

/* returns the effective dynamic maxconn for a server, considering the minconn
 * and the proxy's usage relative to its dynamic connections limit. It is
 * expected that 0 < s->minconn <= s->maxconn when this is called. If the
 * server is currently warming up, the slowstart is also applied to the
 * resulting value, which can be lower than minconn in this case, but never
 * less than 1.
 */
unsigned int srv_dynamic_maxconn(const struct server *s)
{
	unsigned int max;

	if (s->proxy->beconn >= s->proxy->fullconn)
		/* no fullconn or proxy is full */
		max = s->maxconn;
	else if (s->minconn == s->maxconn)
		/* static limit */
		max = s->maxconn;
	else max = MAX(s->minconn,
		       s->proxy->beconn * s->maxconn / s->proxy->fullconn);

	if ((s->cur_state == SRV_ST_STARTING) &&
	    ns_to_sec(now_ns) < s->last_change + s->slowstart &&
	    ns_to_sec(now_ns) >= s->last_change) {
		unsigned int ratio;
		ratio = 100 * (ns_to_sec(now_ns) - s->last_change) / s->slowstart;
		max = MAX(1, max * ratio / 100);
	}
	return max;
}

/* Remove the pendconn from the server's queue. At this stage, the connection
 * is not really dequeued. It will be done during the process_stream. It is
 * up to the caller to atomically decrement the pending counts.
 *
 * The caller must own the lock on the server queue. The pendconn must still be
 * queued (p->node.leaf_p != NULL) and must be in a server (p->srv != NULL).
 */
static void __pendconn_unlink_srv(struct pendconn *p)
{
	p->strm->logs.srv_queue_pos += _HA_ATOMIC_LOAD(&p->queue->idx) - p->queue_idx;
	eb32_delete(&p->node);
}

/* Remove the pendconn from the proxy's queue. At this stage, the connection
 * is not really dequeued. It will be done during the process_stream. It is
 * up to the caller to atomically decrement the pending counts.
 *
 * The caller must own the lock on the proxy queue. The pendconn must still be
 * queued (p->node.leaf_p != NULL) and must be in the proxy (p->srv == NULL).
 */
static void __pendconn_unlink_prx(struct pendconn *p)
{
	p->strm->logs.prx_queue_pos += _HA_ATOMIC_LOAD(&p->queue->idx) - p->queue_idx;
	eb32_delete(&p->node);
}

/* Locks the queue the pendconn element belongs to. This relies on both p->px
 * and p->srv to be properly initialized (which is always the case once the
 * element has been added).
 */
static inline void pendconn_queue_lock(struct pendconn *p)
{
	HA_SPIN_LOCK(QUEUE_LOCK, &p->queue->lock);
}

/* Unlocks the queue the pendconn element belongs to. This relies on both p->px
 * and p->srv to be properly initialized (which is always the case once the
 * element has been added).
 */
static inline void pendconn_queue_unlock(struct pendconn *p)
{
	HA_SPIN_UNLOCK(QUEUE_LOCK, &p->queue->lock);
}

/* Removes the pendconn from the server/proxy queue. At this stage, the
 * connection is not really dequeued. It will be done during process_stream().
 * This function takes all the required locks for the operation. The pendconn
 * must be valid, though it doesn't matter if it was already unlinked. Prefer
 * pendconn_cond_unlink() to first check <p>. It also forces a serialization
 * on p->del_lock to make sure another thread currently waking it up finishes
 * first.
 */
void pendconn_unlink(struct pendconn *p)
{
	struct queue  *q  = p->queue;
	struct proxy  *px = q->px;
	struct server *sv = q->sv;
	uint oldidx;
	int done = 0;

	oldidx = _HA_ATOMIC_LOAD(&p->queue->idx);
	HA_SPIN_LOCK(QUEUE_LOCK, &q->lock);
	HA_SPIN_LOCK(QUEUE_LOCK, &p->del_lock);

	if (p->node.node.leaf_p) {
		eb32_delete(&p->node);
		done = 1;
	}

	HA_SPIN_UNLOCK(QUEUE_LOCK, &p->del_lock);
	HA_SPIN_UNLOCK(QUEUE_LOCK, &q->lock);

	if (done) {
		oldidx -= p->queue_idx;
		if (sv) {
			p->strm->logs.srv_queue_pos += oldidx;
			_HA_ATOMIC_DEC(&sv->queueslength);
		}
		else {
			p->strm->logs.prx_queue_pos += oldidx;
			_HA_ATOMIC_DEC(&px->queueslength);
		}

		_HA_ATOMIC_DEC(&q->length);
		_HA_ATOMIC_DEC(&px->totpend);
	}
}

/* Retrieve the first pendconn from tree <pendconns>. Classes are always
 * considered first, then the time offset. The time does wrap, so the
 * lookup is performed twice, one to retrieve the first class and a second
 * time to retrieve the earliest time in this class.
 */
static struct pendconn *pendconn_first(struct eb_root *pendconns)
{
	struct eb32_node *node, *node2 = NULL;
	u32 key;

	node = eb32_first(pendconns);
	if (!node)
		return NULL;

	key = KEY_CLASS_OFFSET_BOUNDARY(node->key);
	node2 = eb32_lookup_ge(pendconns, key);

	if (!node2 ||
	    KEY_CLASS(node2->key) != KEY_CLASS(node->key)) {
		/* no other key in the tree, or in this class */
		return eb32_entry(node, struct pendconn, node);
	}

	/* found a better key */
	return eb32_entry(node2, struct pendconn, node);
}

/* Process the next pending connection from either a server or a proxy, and
 * returns a strictly positive value on success (see below). If no pending
 * connection is found, 0 is returned.  Note that neither <srv> nor <px> may be
 * NULL.  Priority is given to the oldest request in the queue if both <srv> and
 * <px> have pending requests. This ensures that no request will be left
 * unserved.  The <px> queue is not considered if the server (or a tracked
 * server) is not RUNNING, is disabled, or has a null weight (server going
 * down). The <srv> queue is still considered in this case, because if some
 * connections remain there, it means that some requests have been forced there
 * after it was seen down (eg: due to option persist).  The stream is
 * immediately marked as "assigned", and both its <srv> and <srv_conn> are set
 * to <srv>.
 *
 * The proxy's queue will be consulted only if px_ok is non-zero.
 *
 * This function must only be called if the server queue is locked _AND_ the
 * proxy queue is not. Today it is only called by process_srv_queue.
 * When a pending connection is dequeued, this function returns 1 if a pendconn
 * is dequeued, otherwise 0.
 */
static int pendconn_process_next_strm(struct server *srv, struct proxy *px, int px_ok, int tgrp)
{
	struct pendconn *p = NULL;
	struct pendconn *pp = NULL;
	u32 pkey, ppkey;
	int served;
	int maxconn;
	int got_it = 0;

	p = NULL;
	if (srv->per_tgrp[tgrp - 1].queue.length)
		p = pendconn_first(&srv->per_tgrp[tgrp - 1].queue.head);

	pp = NULL;
	if (px_ok && px->per_tgrp[tgrp - 1].queue.length) {
		/* the lock only remains held as long as the pp is
		 * in the proxy's queue.
		 */
		HA_SPIN_LOCK(QUEUE_LOCK,  &px->per_tgrp[tgrp - 1].queue.lock);
		pp = pendconn_first(&px->per_tgrp[tgrp - 1].queue.head);
		if (!pp)
			HA_SPIN_UNLOCK(QUEUE_LOCK,  &px->per_tgrp[tgrp - 1].queue.lock);
	}

	if (!p && !pp)
		return 0;

	served = _HA_ATOMIC_LOAD(&srv->served);
	maxconn = srv_dynamic_maxconn(srv);

	while (served < maxconn && !got_it)
		got_it = _HA_ATOMIC_CAS(&srv->served, &served, served + 1);

	/* No more slot available, give up */
	if (!got_it) {
		if (pp)
			HA_SPIN_UNLOCK(QUEUE_LOCK, &px->per_tgrp[tgrp - 1].queue.lock);
		return 0;
	}

	/*
	 * Now we know we'll have something available.
	 * Let's try to allocate a slot on the server.
	 */
	if (!pp)
		goto use_p; /*  p != NULL */
	else if (!p)
		goto use_pp; /* pp != NULL */

	/* p != NULL && pp != NULL*/

	if (KEY_CLASS(p->node.key) < KEY_CLASS(pp->node.key))
		goto use_p;

	if (KEY_CLASS(pp->node.key) < KEY_CLASS(p->node.key))
		goto use_pp;

	pkey  = KEY_OFFSET(p->node.key);
	ppkey = KEY_OFFSET(pp->node.key);

	if (pkey < NOW_OFFSET_BOUNDARY())
		pkey += 0x100000; // key in the future

	if (ppkey < NOW_OFFSET_BOUNDARY())
		ppkey += 0x100000; // key in the future

	if (pkey <= ppkey)
		goto use_p;

 use_pp:
	/* we'd like to release the proxy lock ASAP to let other threads
	 * work with other servers. But for this we must first hold the
	 * pendconn alive to prevent a removal from its owning stream.
	 */
	HA_SPIN_LOCK(QUEUE_LOCK, &pp->del_lock);

	/* now the element won't go, we can release the proxy */
	__pendconn_unlink_prx(pp);
	HA_SPIN_UNLOCK(QUEUE_LOCK, &px->per_tgrp[tgrp - 1].queue.lock);

	pp->strm_flags |= SF_ASSIGNED;
	pp->target = srv;
	stream_add_srv_conn(pp->strm, srv);

	/* we must wake the task up before releasing the lock as it's the only
	 * way to make sure the task still exists. The pendconn cannot vanish
	 * under us since the task will need to take the lock anyway and to wait
	 * if it wakes up on a different thread.
	 */
	task_wakeup(pp->strm->task, TASK_WOKEN_RES);
	HA_SPIN_UNLOCK(QUEUE_LOCK, &pp->del_lock);

	_HA_ATOMIC_DEC(&px->per_tgrp[tgrp - 1].queue.length);
	_HA_ATOMIC_INC(&px->per_tgrp[tgrp - 1].queue.idx);
	_HA_ATOMIC_DEC(&px->queueslength);
	return 1;

 use_p:
	/* we don't need the px queue lock anymore, we have the server's lock */
	if (pp)
		HA_SPIN_UNLOCK(QUEUE_LOCK, &px->per_tgrp[tgrp - 1].queue.lock);

	p->strm_flags |= SF_ASSIGNED;
	p->target = srv;
	stream_add_srv_conn(p->strm, srv);

	/* we must wake the task up before releasing the lock as it's the only
	 * way to make sure the task still exists. The pendconn cannot vanish
	 * under us since the task will need to take the lock anyway and to wait
	 * if it wakes up on a different thread.
	 */
	task_wakeup(p->strm->task, TASK_WOKEN_RES);
	__pendconn_unlink_srv(p);

	_HA_ATOMIC_DEC(&srv->per_tgrp[tgrp - 1].queue.length);
	_HA_ATOMIC_INC(&srv->per_tgrp[tgrp - 1].queue.idx);
	_HA_ATOMIC_DEC(&srv->queueslength);
	return 1;
}

/* Manages a server's connection queue. This function will try to dequeue as
 * many pending streams as possible, and wake them up.
 */
int process_srv_queue(struct server *s)
{
	struct server *ref = s->track ? s->track : s;
	struct proxy  *p = s->proxy;
	uint64_t non_empty_tgids = all_tgroups_mask;
	int maxconn;
	int done = 0;
	int px_ok;
	int cur_tgrp;

	/* if a server is not usable or backup and must not be used
	 * to dequeue backend requests.
	 */
	px_ok = srv_currently_usable(ref) &&
	        (!(s->flags & SRV_F_BACKUP) ||
	         (!p->srv_act &&
	          (s == p->lbprm.fbck || (p->options & PR_O_USE_ALL_BK))));

	/* let's repeat that under the lock on each round. Threads competing
	 * for the same server will give up, knowing that at least one of
	 * them will check the conditions again before quitting. In order
	 * to avoid the deadly situation where one thread spends its time
	 * dequeueing for others, we limit the number of rounds it does.
	 * However we still re-enter the loop for one pass if there's no
	 * more served, otherwise we could end up with no other thread
	 * trying to dequeue them.
	 *
	 * There's one racy part: we don't want to have more than one thread
	 * in charge of dequeuing, hence the dequeung flag. We cannot rely
	 * on a trylock here because it would compete against pendconn_add()
	 * and would occasionally leave entries in the queue that are never
	 * dequeued. Nobody else uses the dequeuing flag so when seeing it
	 * non-null, we're certain that another thread is waiting on it.
	 *
	 * We'll dequeue MAX_SELF_USE_QUEUE items from the queue corresponding
	 * to our thread group, then we'll get one from a different one, to
	 * be sure those actually get processed too.
	 */
	while (non_empty_tgids != 0
	       && (done < global.tune.maxpollevents || !s->served) &&
	       s->served < (maxconn = srv_dynamic_maxconn(s))) {
	       int self_served;
	       int to_dequeue;

	       /*
		* self_served contains the number of times we dequeued items
		* from our own thread-group queue.
		*/
	       self_served = _HA_ATOMIC_LOAD(&s->per_tgrp[tgid - 1].self_served) % (MAX_SELF_USE_QUEUE + 1);
	       if ((self_served == MAX_SELF_USE_QUEUE && non_empty_tgids != (1UL << (tgid - 1))) ||
		    !(non_empty_tgids & (1UL << (tgid - 1)))) {
			unsigned int old_served, new_served;

			/*
			 * We want to dequeue from another queue. The last
			 * one we used is stored in last_other_tgrp_served.
			 */
			old_served = _HA_ATOMIC_LOAD(&s->per_tgrp[tgid - 1].last_other_tgrp_served);
			do {
				new_served = old_served + 1;

				/*
				 * Find the next tgrp to dequeue from.
				 * If we're here then we know there is
				 * at least one tgrp that is not the current
				 * tgrp that we can dequeue from, so that
				 * loop will end eventually.
				 */
				while (new_served == tgid ||
				       new_served == global.nbtgroups + 1 ||
				       !(non_empty_tgids & (1UL << (new_served - 1)))) {
					if (new_served == global.nbtgroups + 1)
						new_served = 1;
					else
						new_served++;
				}
			} while (!_HA_ATOMIC_CAS(&s->per_tgrp[tgid - 1].last_other_tgrp_served, &old_served, new_served) && __ha_cpu_relax());
			cur_tgrp = new_served;
			to_dequeue = 1;
		} else {
			cur_tgrp = tgid;
			if (self_served == MAX_SELF_USE_QUEUE)
				self_served = 0;
			to_dequeue = MAX_SELF_USE_QUEUE - self_served;
		}
		if (HA_ATOMIC_XCHG(&s->per_tgrp[cur_tgrp - 1].dequeuing, 1)) {
			non_empty_tgids &= ~(1UL << (cur_tgrp - 1));
			continue;
		}

		HA_SPIN_LOCK(QUEUE_LOCK, &s->per_tgrp[cur_tgrp - 1].queue.lock);
		while (to_dequeue > 0 && s->served < maxconn) {
			/*
			 * pendconn_process_next_strm() will increment
			 * the served field, only if it is < maxconn.
			 */
			if (!pendconn_process_next_strm(s, p, px_ok, cur_tgrp)) {
				non_empty_tgids &= ~(1UL << (cur_tgrp - 1));
				break;
			}
			to_dequeue--;
			if (cur_tgrp == tgid)
				_HA_ATOMIC_INC(&s->per_tgrp[tgid - 1].self_served);
			done++;
			if (done >= global.tune.maxpollevents)
				break;
		}
		HA_ATOMIC_STORE(&s->per_tgrp[cur_tgrp - 1].dequeuing, 0);
		HA_SPIN_UNLOCK(QUEUE_LOCK, &s->per_tgrp[cur_tgrp - 1].queue.lock);
	}

	if (done) {
		_HA_ATOMIC_SUB(&p->totpend, done);
		_HA_ATOMIC_ADD(&p->served, done);
		__ha_barrier_atomic_store();
		if (p->lbprm.server_take_conn)
			p->lbprm.server_take_conn(s);
	}
	if (s->served == 0 && p->served == 0 && !HA_ATOMIC_LOAD(&p->ready_srv)) {
		int i;

		/*
		 * If there is no task running on the server, and the proxy,
		 * let it known that we are ready, there is a small race
		 * condition if a task was being added just before we checked
		 * the proxy queue. It will look for that server, and use it
		 * if nothing is currently running, as there would be nobody
		 * to wake it up.
		 */
		_HA_ATOMIC_STORE(&p->ready_srv, s);
		/*
		 * Maybe a stream was added to the queue just after we
		 * checked, but before we set ready_srv so it would not see it,
		 * just in case try to run one more stream.
		 */
		for (i = 0; i < global.nbtgroups; i++) {
			HA_SPIN_LOCK(QUEUE_LOCK, &s->per_tgrp[i].queue.lock);
			if (pendconn_process_next_strm(s, p, px_ok, i + 1)) {
				HA_SPIN_UNLOCK(QUEUE_LOCK, &s->per_tgrp[i].queue.lock);
				_HA_ATOMIC_SUB(&p->totpend, 1);
				_HA_ATOMIC_ADD(&p->served, 1);
				done++;
				break;
			}
			HA_SPIN_UNLOCK(QUEUE_LOCK, &s->per_tgrp[i].queue.lock);
		}
	}
	return done;
}

/* Adds the stream <strm> to the pending connection queue of server <strm>->srv
 * or to the one of <strm>->proxy if srv is NULL. All counters and back pointers
 * are updated accordingly. Returns NULL if no memory is available, otherwise the
 * pendconn itself. If the stream was already marked as served, its flag is
 * cleared. It is illegal to call this function with a non-NULL strm->srv_conn.
 * The stream's queue position is counted with an offset of -1 because we want
 * to make sure that being at the first position in the queue reports 1.
 *
 * The queue is sorted by the composition of the priority_class, and the current
 * timestamp offset by strm->priority_offset. The timestamp is in milliseconds
 * and truncated to 20 bits, so will wrap every 17m28s575ms.
 * The offset can be positive or negative, and an offset of 0 puts it in the
 * middle of this range (~ 8 min). Note that this also means if the adjusted
 * timestamp wraps around, the request will be misinterpreted as being of
 * the highest priority for that priority class.
 *
 * This function must be called by the stream itself, so in the context of
 * process_stream.
 */
struct pendconn *pendconn_add(struct stream *strm)
{
	struct pendconn *p;
	struct proxy    *px;
	struct server   *srv;
	struct queue    *q;
	unsigned int *max_ptr;
	unsigned int *queueslength;
	unsigned int old_max, new_max;

	p = pool_alloc(pool_head_pendconn);
	if (!p)
		return NULL;

	p->target     = NULL;
	p->node.key   = MAKE_KEY(strm->priority_class, strm->priority_offset);
	p->strm       = strm;
	p->strm_flags = strm->flags;
	HA_SPIN_INIT(&p->del_lock);
	strm->pend_pos = p;

	px = strm->be;
	if (strm->flags & SF_ASSIGNED)
		srv = objt_server(strm->target);
	else
		srv = NULL;

	if (srv) {
		q = &srv->per_tgrp[tgid - 1].queue;
		max_ptr = &srv->counters.nbpend_max;
		queueslength = &srv->queueslength;
	}
	else {
		q = &px->per_tgrp[tgid - 1].queue;
		max_ptr = &px->be_counters.nbpend_max;
		queueslength = &px->queueslength;
	}

	p->queue = q;
	p->queue_idx  = _HA_ATOMIC_LOAD(&q->idx) - 1; // for logging only
	new_max = _HA_ATOMIC_ADD_FETCH(queueslength, 1);
	_HA_ATOMIC_INC(&q->length);
	old_max = _HA_ATOMIC_LOAD(max_ptr);
	while (new_max > old_max) {
		if (likely(_HA_ATOMIC_CAS(max_ptr, &old_max, new_max)))
			break;
	}
	__ha_barrier_atomic_store();

	HA_SPIN_LOCK(QUEUE_LOCK, &q->lock);
	eb32_insert(&q->head, &p->node);
	HA_SPIN_UNLOCK(QUEUE_LOCK, &q->lock);

	_HA_ATOMIC_INC(&px->totpend);
	return p;
}

/* Redistribute pending connections when a server goes down. The number of
 * connections redistributed is returned. It will take the server queue lock
 * and does not use nor depend on other locks.
 */
int pendconn_redistribute(struct server *s)
{
	struct pendconn *p;
	struct eb32_node *node, *nodeb;
	struct proxy *px = s->proxy;
	int px_xferred = 0;
	int xferred = 0;
	int i;

	/* The REDISP option was specified. We will ignore cookie and force to
	 * balance or use the dispatcher.
	 */
	if (!(s->cur_admin & SRV_ADMF_MAINT) &&
	    (s->proxy->options & (PR_O_REDISP|PR_O_PERSIST)) != PR_O_REDISP)
		goto skip_srv_queue;

	for (i = 0; i < global.nbtgroups; i++) {
		struct queue *queue = &s->per_tgrp[i].queue;
		int local_xferred = 0;

		HA_SPIN_LOCK(QUEUE_LOCK, &queue->lock);
		for (node = eb32_first(&queue->head); node; node = nodeb) {
			nodeb =	eb32_next(node);

			p = eb32_entry(node, struct pendconn, node);
			if (p->strm_flags & SF_FORCE_PRST)
				continue;

			/* it's left to the dispatcher to choose a server */
			__pendconn_unlink_srv(p);
			if (!(s->proxy->options & PR_O_REDISP))
				p->strm_flags &= ~(SF_DIRECT | SF_ASSIGNED);

			task_wakeup(p->strm->task, TASK_WOKEN_RES);
			local_xferred++;
		}
		HA_SPIN_UNLOCK(QUEUE_LOCK, &queue->lock);
		xferred += local_xferred;
		if (local_xferred)
			_HA_ATOMIC_SUB(&queue->length, local_xferred);
	}

	if (xferred) {
		_HA_ATOMIC_SUB(&s->queueslength, xferred);
		_HA_ATOMIC_SUB(&s->proxy->totpend, xferred);
	}

 skip_srv_queue:
	if (px->lbprm.tot_wact || px->lbprm.tot_wbck)
		goto done;

	for (i = 0; i < global.nbtgroups; i++) {
		struct queue *queue = &px->per_tgrp[i].queue;
		int local_xferred = 0;

		HA_SPIN_LOCK(QUEUE_LOCK, &queue->lock);
		for (node = eb32_first(&queue->head); node; node = nodeb) {
			nodeb =	eb32_next(node);
			p = eb32_entry(node, struct pendconn, node);

			/* force-persist streams may occasionally appear in the
			 * proxy's queue, and we certainly don't want them here!
			 */
			p->strm_flags &= ~SF_FORCE_PRST;
			__pendconn_unlink_prx(p);

			task_wakeup(p->strm->task, TASK_WOKEN_RES);
			local_xferred++;
		}
		HA_SPIN_UNLOCK(QUEUE_LOCK, &queue->lock);
		if (local_xferred)
			_HA_ATOMIC_SUB(&queue->length, local_xferred);
		px_xferred += local_xferred;
	}

	if (px_xferred) {
		_HA_ATOMIC_SUB(&px->queueslength, px_xferred);
		_HA_ATOMIC_SUB(&px->totpend, px_xferred);
	}
 done:
	return xferred + px_xferred;
}

/* Try to dequeue pending connection attached to the stream <strm>. It must
 * always exists here. If the pendconn is still linked to the server or the
 * proxy queue, nothing is done and the function returns 1. Otherwise,
 * <strm>->flags and <strm>->target are updated, the pendconn is released and 0
 * is returned.
 *
 * This function must be called by the stream itself, so in the context of
 * process_stream.
 */
int pendconn_dequeue(struct stream *strm)
{
	struct pendconn *p;
	int is_unlinked;

	/* unexpected case because it is called by the stream itself and
	 * only the stream can release a pendconn. So it is only
	 * possible if a pendconn is released by someone else or if the
	 * stream is supposed to be queued but without its associated
	 * pendconn. In both cases it is a bug! */
	BUG_ON(!strm->pend_pos);

	p = strm->pend_pos;

	/* note below : we need to grab the queue's lock to check for emptiness
	 * because we don't want a partial process_srv_queue() or redistribute()
	 * to be called in parallel and show an empty list without having the
	 * time to finish. With this we know that if we see the element
	 * unlinked, these functions were completely done.
	 */
	pendconn_queue_lock(p);
	is_unlinked = !p->node.node.leaf_p;
	pendconn_queue_unlock(p);

	/* serialize to make sure the element was finished processing */
	HA_SPIN_LOCK(QUEUE_LOCK, &p->del_lock);
	HA_SPIN_UNLOCK(QUEUE_LOCK, &p->del_lock);

	if (!is_unlinked)
		return 1;

	/* the pendconn is not queued anymore and will not be so we're safe
	 * to proceed.
	 */
	strm->flags &= ~(SF_DIRECT | SF_ASSIGNED);
	strm->flags |= p->strm_flags & (SF_DIRECT | SF_ASSIGNED);

	/* the entry might have been redistributed to another server */
	if (!(strm->flags & SF_ASSIGNED))
		sockaddr_free(&strm->scb->dst);

	if (p->target) {
		/* a server picked this pendconn, it must skip LB */
		stream_set_srv_target(strm, p->target);
		strm->flags |= SF_ASSIGNED;
	}

	strm->pend_pos = NULL;
	pool_free(pool_head_pendconn, p);
	return 0;
}

static enum act_return action_set_priority_class(struct act_rule *rule, struct proxy *px,
                                                 struct session *sess, struct stream *s, int flags)
{
	struct sample *smp;

	smp = sample_fetch_as_type(px, sess, s, SMP_OPT_DIR_REQ|SMP_OPT_FINAL, rule->arg.expr, SMP_T_SINT);
	if (!smp)
		return ACT_RET_CONT;

	s->priority_class = queue_limit_class(smp->data.u.sint);
	return ACT_RET_CONT;
}

static enum act_return action_set_priority_offset(struct act_rule *rule, struct proxy *px,
                                                  struct session *sess, struct stream *s, int flags)
{
	struct sample *smp;

	smp = sample_fetch_as_type(px, sess, s, SMP_OPT_DIR_REQ|SMP_OPT_FINAL, rule->arg.expr, SMP_T_SINT);
	if (!smp)
		return ACT_RET_CONT;

	s->priority_offset = queue_limit_offset(smp->data.u.sint);

	return ACT_RET_CONT;
}

static enum act_parse_ret parse_set_priority_class(const char **args, int *arg, struct proxy *px,
                                                   struct act_rule *rule, char **err)
{
	unsigned int where = 0;

	rule->arg.expr = sample_parse_expr((char **)args, arg, px->conf.args.file,
	                                   px->conf.args.line, err, &px->conf.args, NULL);
	if (!rule->arg.expr)
		return ACT_RET_PRS_ERR;

	if (px->cap & PR_CAP_FE)
		where |= SMP_VAL_FE_HRQ_HDR;
	if (px->cap & PR_CAP_BE)
		where |= SMP_VAL_BE_HRQ_HDR;

	if (!(rule->arg.expr->fetch->val & where)) {
		memprintf(err,
			  "fetch method '%s' extracts information from '%s', none of which is available here",
			  args[0], sample_src_names(rule->arg.expr->fetch->use));
		free(rule->arg.expr);
		return ACT_RET_PRS_ERR;
	}

	rule->action     = ACT_CUSTOM;
	rule->action_ptr = action_set_priority_class;
	return ACT_RET_PRS_OK;
}

static enum act_parse_ret parse_set_priority_offset(const char **args, int *arg, struct proxy *px,
                                                    struct act_rule *rule, char **err)
{
	unsigned int where = 0;

	rule->arg.expr = sample_parse_expr((char **)args, arg, px->conf.args.file,
	                                   px->conf.args.line, err, &px->conf.args, NULL);
	if (!rule->arg.expr)
		return ACT_RET_PRS_ERR;

	if (px->cap & PR_CAP_FE)
		where |= SMP_VAL_FE_HRQ_HDR;
	if (px->cap & PR_CAP_BE)
		where |= SMP_VAL_BE_HRQ_HDR;

	if (!(rule->arg.expr->fetch->val & where)) {
		memprintf(err,
			  "fetch method '%s' extracts information from '%s', none of which is available here",
			  args[0], sample_src_names(rule->arg.expr->fetch->use));
		free(rule->arg.expr);
		return ACT_RET_PRS_ERR;
	}

	rule->action     = ACT_CUSTOM;
	rule->action_ptr = action_set_priority_offset;
	return ACT_RET_PRS_OK;
}

static struct action_kw_list tcp_cont_kws = {ILH, {
	{ "set-priority-class", parse_set_priority_class },
	{ "set-priority-offset", parse_set_priority_offset },
	{ /* END */ }
}};

INITCALL1(STG_REGISTER, tcp_req_cont_keywords_register, &tcp_cont_kws);

static struct action_kw_list http_req_kws = {ILH, {
	{ "set-priority-class", parse_set_priority_class },
	{ "set-priority-offset", parse_set_priority_offset },
	{ /* END */ }
}};

INITCALL1(STG_REGISTER, http_req_keywords_register, &http_req_kws);

static int
smp_fetch_priority_class(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	if (!smp->strm)
		return 0;

	smp->data.type = SMP_T_SINT;
	smp->data.u.sint = smp->strm->priority_class;

	return 1;
}

static int
smp_fetch_priority_offset(const struct arg *args, struct sample *smp, const char *kw, void *private)
{
	if (!smp->strm)
		return 0;

	smp->data.type = SMP_T_SINT;
	smp->data.u.sint = smp->strm->priority_offset;

	return 1;
}


static struct sample_fetch_kw_list smp_kws = {ILH, {
	{ "prio_class", smp_fetch_priority_class, 0, NULL, SMP_T_SINT, SMP_USE_INTRN, },
	{ "prio_offset", smp_fetch_priority_offset, 0, NULL, SMP_T_SINT, SMP_USE_INTRN, },
	{ /* END */},
}};

INITCALL1(STG_REGISTER, sample_register_fetches, &smp_kws);

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
