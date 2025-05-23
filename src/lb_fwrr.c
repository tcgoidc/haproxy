/*
 * Fast Weighted Round Robin load balancing algorithm.
 *
 * Copyright 2000-2009 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <import/eb32tree.h>
#include <haproxy/api.h>
#include <haproxy/backend.h>
#include <haproxy/queue.h>
#include <haproxy/server-t.h>
#include <haproxy/global.h>


static inline void fwrr_remove_from_tree(struct server *s, int tgid);
static inline void fwrr_queue_by_weight(struct eb_root *root, struct server *s, int tgid);
static inline void fwrr_dequeue_srv(struct server *s, int tgid);
static void fwrr_get_srv(struct server *s, int tgid);
static void fwrr_queue_srv(struct server *s, int tgid);


/* This function updates the server trees according to server <srv>'s new
 * state. It should be called when server <srv>'s status changes to down.
 * It is not important whether the server was already down or not. It is not
 * important either that the new state is completely down (the caller may not
 * know all the variables of a server's state).
 *
 * The server's lock must be held. The lbprm's lock will be used.
 */
static void fwrr_set_server_status_down(struct server *srv)
{
	struct proxy *p = srv->proxy;
	int i;

	if (!srv_lb_status_changed(srv))
		return;

	if (srv_willbe_usable(srv))
		goto out_update_state;

	HA_RWLOCK_WRLOCK(LBPRM_LOCK, &p->lbprm.lock);

	if (!srv_currently_usable(srv))
		/* server was already down */
		goto out_update_backend;

	for (i = 0; i < global.nbtgroups; i++) {
		HA_RWLOCK_WRLOCK(LBPRM_LOCK, &p->per_tgrp[i].lbprm.fwrr.lock);
		fwrr_dequeue_srv(srv, i + 1);
		fwrr_remove_from_tree(srv, i + 1);
		HA_RWLOCK_WRUNLOCK(LBPRM_LOCK, &p->per_tgrp[i].lbprm.fwrr.lock);
	}

	if (srv->flags & SRV_F_BACKUP) {
		p->lbprm.fwrr.next_weight_bck -= srv->cur_eweight;
		p->lbprm.tot_wbck = p->lbprm.fwrr.next_weight_bck;
		p->srv_bck--;

		if (srv == p->lbprm.fbck) {
			/* we lost the first backup server in a single-backup
			 * configuration, we must search another one.
			 */
			struct server *srv2 = p->lbprm.fbck;
			do {
				srv2 = srv2->next;
			} while (srv2 &&
				 !((srv2->flags & SRV_F_BACKUP) &&
				   srv_willbe_usable(srv2)));
			p->lbprm.fbck = srv2;
		}
	} else {
		p->lbprm.fwrr.next_weight_act -= srv->cur_eweight;
		p->lbprm.tot_wact = p->lbprm.fwrr.next_weight_act;
		p->srv_act--;
	}


out_update_backend:
	/* check/update tot_used, tot_weight */
	update_backend_weight(p);
	HA_RWLOCK_WRUNLOCK(LBPRM_LOCK, &p->lbprm.lock);

 out_update_state:
	srv_lb_commit_status(srv);
}

/* This function updates the server trees according to server <srv>'s new
 * state. It should be called when server <srv>'s status changes to up.
 * It is not important whether the server was already down or not. It is not
 * important either that the new state is completely UP (the caller may not
 * know all the variables of a server's state). This function will not change
 * the weight of a server which was already up.
 *
 * The server's lock must be held. The lbprm's lock will be used.
 */
static void fwrr_set_server_status_up(struct server *srv)
{
	struct proxy *p = srv->proxy;
	struct fwrr_group *grp;
	int next_weight;
	int i;

	if (!srv_lb_status_changed(srv))
		return;

	if (!srv_willbe_usable(srv))
		goto out_update_state;

	HA_RWLOCK_WRLOCK(LBPRM_LOCK, &p->lbprm.lock);

	if (srv_currently_usable(srv))
		/* server was already up */
		goto out_update_backend;


	if (srv->flags & SRV_F_BACKUP) {
		p->lbprm.fwrr.next_weight_bck += srv->next_eweight;
		next_weight = p->lbprm.tot_wbck = p->lbprm.fwrr.next_weight_bck;
		p->srv_bck++;

		if (!(p->options & PR_O_USE_ALL_BK)) {
			if (!p->lbprm.fbck) {
				/* there was no backup server anymore */
				p->lbprm.fbck = srv;
			} else {
				/* we may have restored a backup server prior to fbck,
				 * in which case it should replace it.
				 */
				struct server *srv2 = srv;
				do {
					srv2 = srv2->next;
				} while (srv2 && (srv2 != p->lbprm.fbck));
				if (srv2)
					p->lbprm.fbck = srv;
			}
		}
	} else {
		p->lbprm.fwrr.next_weight_act += srv->next_eweight;
		next_weight = p->lbprm.tot_wact = p->lbprm.fwrr.next_weight_act;
		p->srv_act++;
	}

	/* note that eweight cannot be 0 here */
	for (i = 0; i < global.nbtgroups; i++) {
		HA_RWLOCK_WRLOCK(LBPRM_LOCK, &p->per_tgrp[i].lbprm.fwrr.lock);
		grp = (srv->flags & SRV_F_BACKUP) ? &p->per_tgrp[i].lbprm.fwrr.bck : &p->per_tgrp[i].lbprm.fwrr.act;
		fwrr_get_srv(srv, i + 1);
		srv->per_tgrp[i].npos = grp->curr_pos + (next_weight + grp->curr_weight - grp->curr_pos) / srv->next_eweight;
		fwrr_queue_srv(srv, i + 1);
		HA_RWLOCK_WRUNLOCK(LBPRM_LOCK, &p->per_tgrp[i].lbprm.fwrr.lock);
	}

out_update_backend:
	/* check/update tot_used, tot_weight */
	update_backend_weight(p);
	HA_RWLOCK_WRUNLOCK(LBPRM_LOCK, &p->lbprm.lock);

 out_update_state:
	srv_lb_commit_status(srv);
}

/* This function must be called after an update to server <srv>'s effective
 * weight. It may be called after a state change too.
 *
 * The server's lock must be held. The lbprm's lock will be used.
 */
static void fwrr_update_server_weight(struct server *srv)
{
	int old_state, new_state;
	struct proxy *p = srv->proxy;
	struct fwrr_group *grp;
	int next_weight;
	int i;

	if (!srv_lb_status_changed(srv))
		return;

	/* If changing the server's weight changes its state, we simply apply
	 * the procedures we already have for status change. If the state
	 * remains down, the server is not in any tree, so it's as easy as
	 * updating its values. If the state remains up with different weights,
	 * there are some computations to perform to find a new place and
	 * possibly a new tree for this server.
	 */
	 
	old_state = srv_currently_usable(srv);
	new_state = srv_willbe_usable(srv);

	if (!old_state && !new_state) {
		srv_lb_commit_status(srv);
		return;
	}
	else if (!old_state && new_state) {
		fwrr_set_server_status_up(srv);
		return;
	}
	else if (old_state && !new_state) {
		fwrr_set_server_status_down(srv);
		return;
	}

	HA_RWLOCK_WRLOCK(LBPRM_LOCK, &p->lbprm.lock);

	if (srv->flags & SRV_F_BACKUP) {
		p->lbprm.fwrr.next_weight_bck = p->lbprm.fwrr.next_weight_bck - srv->cur_eweight + srv->next_eweight;
		next_weight = p->lbprm.tot_wbck = p->lbprm.fwrr.next_weight_bck;
	} else {
		p->lbprm.fwrr.next_weight_act = p->lbprm.fwrr.next_weight_act - srv->cur_eweight + srv->next_eweight;
		next_weight = p->lbprm.tot_wact = p->lbprm.fwrr.next_weight_act;
	}

	for (i = 0; i < global.nbtgroups; i++) {
		HA_RWLOCK_WRLOCK(LBPRM_LOCK, &p->per_tgrp[i].lbprm.fwrr.lock);
		grp = (srv->flags & SRV_F_BACKUP) ? &p->per_tgrp[i].lbprm.fwrr.bck : &p->per_tgrp[i].lbprm.fwrr.act;
		if (srv->lb_tree == grp->init) {
			fwrr_dequeue_srv(srv, i + 1);
			fwrr_queue_by_weight(grp->init, srv, i + 1);
		}
		else if (!srv->lb_tree) {
			/* FIXME: server was down. This is not possible right now but
			 * may be needed soon for slowstart or graceful shutdown.
			 */
			fwrr_dequeue_srv(srv, i + 1);
			fwrr_get_srv(srv, i + 1);
			srv->per_tgrp[i].npos = grp->curr_pos + (next_weight + grp->curr_weight - grp->curr_pos) / srv->next_eweight;
			fwrr_queue_srv(srv, i + 1);
		} else {
			/* The server is either active or in the next queue. If it's
			 * still in the active queue and it has not consumed all of its
			 * places, let's adjust its next position.
			 */
			fwrr_get_srv(srv, i + 1);

			if (srv->next_eweight > 0) {
				int prev_next = srv->per_tgrp[i].npos;
				int step = next_weight / srv->next_eweight;

				srv->per_tgrp[i].npos = srv->per_tgrp[i].lpos + step;
				srv->per_tgrp[i].rweight = 0;

				if (srv->per_tgrp[i].npos > prev_next)
					srv->per_tgrp[i].npos = prev_next;
				if (srv->per_tgrp[i].npos < grp->curr_pos + 2)
					srv->per_tgrp[i].npos = grp->curr_pos + step;
			} else {
				/* push it into the next tree */
				srv->per_tgrp[i].npos = grp->curr_pos + grp->curr_weight;
			}

			fwrr_dequeue_srv(srv, i + 1);
			fwrr_queue_srv(srv, i + 1);
		}
		HA_RWLOCK_WRUNLOCK(LBPRM_LOCK, &p->per_tgrp[i].lbprm.fwrr.lock);
	}

	update_backend_weight(p);
	HA_RWLOCK_WRUNLOCK(LBPRM_LOCK, &p->lbprm.lock);

	srv_lb_commit_status(srv);
}

/* Remove a server from a tree. It must have previously been dequeued. This
 * function is meant to be called when a server is going down or has its
 * weight disabled.
 *
 * The lbprm's lock must be held. The server's lock is not used.
 */
static inline void fwrr_remove_from_tree(struct server *s, int tgid)
{
	s->per_tgrp[tgid - 1].lb_tree = NULL;
}

/* Queue a server in the weight tree <root>, assuming the weight is >0.
 * We want to sort them by inverted weights, because we need to place
 * heavy servers first in order to get a smooth distribution.
 *
 * The lbprm's lock must be held. The server's lock is not used.
 */
static inline void fwrr_queue_by_weight(struct eb_root *root, struct server *s, int tgid)
{
	s->per_tgrp[tgid - 1].lb_node.key = SRV_EWGHT_MAX - s->next_eweight;
	eb32_insert(root, &s->per_tgrp[tgid - 1].lb_node);
	s->per_tgrp[tgid - 1].lb_tree = root;
}

/* This function is responsible for building the weight trees in case of fast
 * weighted round-robin. It also sets p->lbprm.wdiv to the eweight to uweight
 * ratio. Both active and backup groups are initialized.
 */
void fwrr_init_server_groups(struct proxy *p)
{
	struct server *srv;
	struct eb_root init_head = EB_ROOT;
	int i, j;

	p->lbprm.set_server_status_up   = fwrr_set_server_status_up;
	p->lbprm.set_server_status_down = fwrr_set_server_status_down;
	p->lbprm.update_server_eweight  = fwrr_update_server_weight;

	p->lbprm.wdiv = BE_WEIGHT_SCALE;
	for (srv = p->srv; srv; srv = srv->next) {
		srv->next_eweight = (srv->uweight * p->lbprm.wdiv + p->lbprm.wmult - 1) / p->lbprm.wmult;
		srv_lb_commit_status(srv);
	}

	recount_servers(p);
	update_backend_weight(p);

	for (i = 0; i < global.nbtgroups; i++) {
		/* prepare the active servers group */
		p->per_tgrp[i].lbprm.fwrr.act.curr_pos =
			p->per_tgrp[i].lbprm.fwrr.act.curr_weight =
			p->lbprm.fwrr.next_weight_act = p->lbprm.tot_wact;
		p->per_tgrp[i].lbprm.fwrr.act.curr =
			p->per_tgrp[i].lbprm.fwrr.act.t0 =
			p->per_tgrp[i].lbprm.fwrr.act.t1 = init_head;
		p->per_tgrp[i].lbprm.fwrr.act.init = &p->per_tgrp[i].lbprm.fwrr.act.t0;
		p->per_tgrp[i].lbprm.fwrr.act.next = &p->per_tgrp[i].lbprm.fwrr.act.t1;

		/* prepare the backup servers group */
		p->per_tgrp[i].lbprm.fwrr.bck.curr_pos =
			p->per_tgrp[i].lbprm.fwrr.bck.curr_weight =
			p->lbprm.fwrr.next_weight_bck = p->lbprm.tot_wbck;
		p->per_tgrp[i].lbprm.fwrr.bck.curr =
			p->per_tgrp[i].lbprm.fwrr.bck.t0 =
			p->per_tgrp[i].lbprm.fwrr.bck.t1 = init_head;
		p->per_tgrp[i].lbprm.fwrr.bck.init = &p->per_tgrp[i].lbprm.fwrr.bck.t0;
		p->per_tgrp[i].lbprm.fwrr.bck.next = &p->per_tgrp[i].lbprm.fwrr.bck.t1;

		/* queue active and backup servers in two distinct groups */
		j = 0;
		for (srv = p->srv; srv; srv = srv->next) {
			j++;
			if (!srv_currently_usable(srv))
				continue;
			if (j <= i)
				continue;
			fwrr_queue_by_weight((srv->flags & SRV_F_BACKUP) ?
					p->per_tgrp[i].lbprm.fwrr.bck.init :
					p->per_tgrp[i].lbprm.fwrr.act.init,
					srv, i + 1);
		}
		j = 0;
		for (srv = p->srv; srv && j < i; srv = srv->next) {
			j++;
			if (!srv_currently_usable(srv))
				continue;
			fwrr_queue_by_weight((srv->flags & SRV_F_BACKUP) ?
					p->per_tgrp[i].lbprm.fwrr.bck.init :
					p->per_tgrp[i].lbprm.fwrr.act.init,
					srv, i + 1);
		}
	}
}

/* simply removes a server from a weight tree.
 *
 * The lbprm's lock must be held. The server's lock is not used.
 */
static inline void fwrr_dequeue_srv(struct server *s, int tgid)
{
	eb32_delete(&s->per_tgrp[tgid - 1].lb_node);
}

/* queues a server into the appropriate group and tree depending on its
 * backup status, and ->npos. If the server is disabled, simply assign
 * it to the NULL tree.
 *
 * The lbprm's lock must be held. The server's lock is not used.
 */
static void fwrr_queue_srv(struct server *s, int tgid)
{
	struct proxy *p = s->proxy;
	struct fwrr_group *grp;
	int next_weight;

	if (s->flags & SRV_F_BACKUP) {
		grp = &p->per_tgrp[tgid - 1].lbprm.fwrr.bck;
		next_weight = p->lbprm.fwrr.next_weight_bck;
	} else {
		grp = &p->per_tgrp[tgid - 1].lbprm.fwrr.act;
		next_weight = p->lbprm.fwrr.next_weight_act;
	}

	/* Delay everything which does not fit into the window and everything
	 * which does not fit into the theoretical new window.
	 */
	if (!srv_willbe_usable(s)) {
		fwrr_remove_from_tree(s, tgid);
	}
	else if (s->next_eweight <= 0 ||
	    s->per_tgrp[tgid - 1].npos >= 2 * grp->curr_weight ||
	    s->per_tgrp[tgid - 1].npos >= grp->curr_weight + next_weight) {
		/* put into next tree, and readjust npos in case we could
		 * finally take this back to current. */
		s->per_tgrp[tgid - 1].npos -= grp->curr_weight;
		fwrr_queue_by_weight(grp->next, s, tgid);
	}
	else {
		/* The sorting key is stored in units of s->npos * user_weight
		 * in order to avoid overflows. As stated in backend.h, the
		 * lower the scale, the rougher the weights modulation, and the
		 * higher the scale, the lower the number of servers without
		 * overflow. With this formula, the result is always positive,
		 * so we can use eb32_insert().
		 */
		s->per_tgrp[tgid - 1].lb_node.key = SRV_UWGHT_RANGE * s->per_tgrp[tgid - 1].npos +
			(unsigned)(SRV_EWGHT_MAX + s->per_tgrp[tgid - 1].rweight - s->next_eweight) / BE_WEIGHT_SCALE;

		eb32_insert(&grp->curr, &s->per_tgrp[tgid - 1].lb_node);
		s->per_tgrp[tgid - 1].lb_tree = &grp->curr;
	}
}

/* prepares a server when extracting it from the "init" tree.
 *
 * The lbprm's lock must be held. The server's lock is not used.
 */
static inline void fwrr_get_srv_init(struct server *s, int tgid)
{
	s->per_tgrp[tgid - 1].npos = s->per_tgrp[tgid - 1].rweight = 0;
}

/* prepares a server when extracting it from the "next" tree.
 *
 * The lbprm's lock must be held. The server's lock is not used.
 */
static inline void fwrr_get_srv_next(struct server *s, int tgid)
{
	struct fwrr_group *grp = (s->flags & SRV_F_BACKUP) ?
		&s->proxy->per_tgrp[tgid - 1].lbprm.fwrr.bck :
		&s->proxy->per_tgrp[tgid - 1].lbprm.fwrr.act;

	s->per_tgrp[tgid - 1].npos += grp->curr_weight;
}

/* prepares a server when it was marked down.
 *
 * The lbprm's lock must be held. The server's lock is not used.
 */
static inline void fwrr_get_srv_down(struct server *s, int tgid)
{
	struct fwrr_group *grp = (s->flags & SRV_F_BACKUP) ?
		&s->proxy->per_tgrp[tgid - 1].lbprm.fwrr.bck :
		&s->proxy->per_tgrp[tgid - 1].lbprm.fwrr.act;

	s->per_tgrp[tgid - 1].npos = grp->curr_pos;
}

/* prepares a server when extracting it from its tree.
 *
 * The lbprm's lock must be held. The server's lock is not used.
 */
static void fwrr_get_srv(struct server *s, int tgid)
{
	struct proxy *p = s->proxy;

	struct fwrr_group *grp = (s->flags & SRV_F_BACKUP) ?
	    &p->per_tgrp[tgid - 1].lbprm.fwrr.bck :
	    &p->per_tgrp[tgid - 1].lbprm.fwrr.act;

	if (s->per_tgrp[tgid - 1].lb_tree == grp->init) {
		fwrr_get_srv_init(s, tgid);
	}
	else if (s->per_tgrp[tgid - 1].lb_tree == grp->next) {
		fwrr_get_srv_next(s, tgid);
	}
	else if (s->per_tgrp[tgid - 1].lb_tree == NULL) {
		fwrr_get_srv_down(s, tgid);
	}
}

/* switches trees "init" and "next" for FWRR group <grp>. "init" should be empty
 * when this happens, and "next" filled with servers sorted by weights.
 *
 * The lbprm's lock must be held. The server's lock is not used.
 */
static inline void fwrr_switch_trees(struct fwrr_group *grp, int next_weight)
{
	struct eb_root *swap;
	swap = grp->init;
	grp->init = grp->next;
	grp->next = swap;
	grp->curr_weight = next_weight;
	grp->curr_pos = grp->curr_weight;
}

/* return next server from the current tree in FWRR group <grp>, or a server
 * from the "init" tree if appropriate. If both trees are empty, return NULL.
 *
 * The lbprm's lock must be held. The server's lock is not used.
 */
static struct server *fwrr_get_server_from_group(struct fwrr_group *grp)
{
	struct eb32_node *node1;
	struct eb32_node *node2;
	struct srv_per_tgroup *per_tgrp;
	struct server *s1 = NULL;
	struct server *s2 = NULL;

	node1 = eb32_first(&grp->curr);
	if (node1) {
		per_tgrp = eb32_entry(node1, struct srv_per_tgroup, lb_node);
		s1 = per_tgrp->server;
		if (s1->cur_eweight && s1->per_tgrp[tgid - 1].npos <= grp->curr_pos)
			return s1;
	}

	/* Either we have no server left, or we have a hole. We'll look in the
	 * init tree or a better proposal. At this point, if <s1> is non-null,
	 * it is guaranteed to remain available as the tree is locked.
	 */
	node2 = eb32_first(grp->init);
	if (node2) {
		per_tgrp = eb32_entry(node2, struct srv_per_tgroup, lb_node);
		s2 = per_tgrp->server;
		if (s2->cur_eweight) {
			fwrr_get_srv_init(s2, tgid);
			return s2;
		}
	}
	return s1;
}

/* Computes next position of server <s> in the group. Nothing is done if <s>
 * has a zero weight. 
 *
 * The lbprm's lock must be held to protect lpos/npos/rweight.
 */
static inline void fwrr_update_position(struct fwrr_group *grp, struct server *s, int next_weight)
{
	unsigned int eweight = *(volatile unsigned int *)&s->cur_eweight;

	if (!eweight)
		return;

	if (!s->per_tgrp[tgid - 1].npos) {
		/* first time ever for this server */
		s->per_tgrp[tgid - 1].npos     = grp->curr_pos;
	}

	s->per_tgrp[tgid - 1].lpos     = s->per_tgrp[tgid - 1].npos;
	s->per_tgrp[tgid - 1].npos    += next_weight / eweight;
	s->per_tgrp[tgid - 1].rweight += next_weight % eweight;

	if (s->per_tgrp[tgid - 1].rweight >= eweight) {
		s->per_tgrp[tgid - 1].rweight -= eweight;
		s->per_tgrp[tgid - 1].npos++;
	}
}

/* Return next server from the current tree in backend <p>, or a server from
 * the init tree if appropriate. If both trees are empty, return NULL.
 * Saturated servers are skipped and requeued.
 *
 * The lbprm's lock will be used in R/W mode. The server's lock is not used.
 */
struct server *fwrr_get_next_server(struct proxy *p, struct server *srvtoavoid)
{
	struct server *srv, *full, *avoided;
	struct fwrr_group *grp;
	int switched;
	int next_weight;

	HA_RWLOCK_WRLOCK(LBPRM_LOCK, &p->per_tgrp[tgid - 1].lbprm.fwrr.lock);
	if (p->srv_act) {
		grp = &p->per_tgrp[tgid - 1].lbprm.fwrr.act;
		next_weight = p->lbprm.fwrr.next_weight_act;
	} else if (p->lbprm.fbck) {
		srv = p->lbprm.fbck;
		goto out;
	}
	else if (p->srv_bck) {
		next_weight = p->lbprm.fwrr.next_weight_bck;
		grp = &p->per_tgrp[tgid - 1].lbprm.fwrr.bck;
	} else {
		srv = NULL;
		goto out;
	}

	switched = 0;
	avoided = NULL;
	full = NULL; /* NULL-terminated list of saturated servers */
	while (1) {
		/* if we see an empty group, let's first try to collect weights
		 * which might have recently changed.
		 */
		if (!grp->curr_weight)
			grp->curr_pos = grp->curr_weight = next_weight;

		/* get first server from the "current" tree. When the end of
		 * the tree is reached, we may have to switch, but only once.
		 */
		while (1) {
			srv = fwrr_get_server_from_group(grp);
			if (srv)
				break;
			if (switched) {
				if (avoided) {
					srv = avoided;
					goto take_this_one;
				}
				goto requeue_servers;
			}
			switched = 1;
			fwrr_switch_trees(grp, next_weight);
		}

		/* OK, we have a server. However, it may be saturated, in which
		 * case we don't want to reconsider it for now. We'll update
		 * its position and dequeue it anyway, so that we can move it
		 * to a better place afterwards.
		 */
		fwrr_update_position(grp, srv, next_weight);
		fwrr_dequeue_srv(srv, tgid);
		grp->curr_pos++;
		if (!srv->maxconn || (!srv->queueslength && srv->served < srv_dynamic_maxconn(srv))) {
			/* make sure it is not the server we are trying to exclude... */
			if (srv != srvtoavoid || avoided)
				break;

			avoided = srv; /* ...but remember that is was selected yet avoided */
		}

		/* the server is saturated or avoided, let's chain it for later reinsertion.
		 */
		srv->per_tgrp[tgid - 1].next_full = full;
		full = srv;
	}

 take_this_one:
	/* OK, we got the best server, let's update it */
	fwrr_queue_srv(srv, tgid);

 requeue_servers:
	/* Requeue all extracted servers. If full==srv then it was
	 * avoided (unsuccessfully) and chained, omit it now. The
	 * only way to get there is by having <avoided>==NULL or
	 * <avoided>==<srv>.
	 */
	if (unlikely(full != NULL)) {
		if (switched) {
			/* the tree has switched, requeue all extracted servers
			 * into "init", because their place was lost, and only
			 * their weight matters.
			 */
			do {
				if (likely(full != srv))
					fwrr_queue_by_weight(grp->init, full, tgid);
				full = full->per_tgrp[tgid - 1].next_full;
			} while (full);
		} else {
			/* requeue all extracted servers just as if they were consumed
			 * so that they regain their expected place.
			 */
			do {
				if (likely(full != srv))
					fwrr_queue_srv(full, tgid);
				full = full->per_tgrp[tgid - 1].next_full;
			} while (full);
		}
	}
 out:
	HA_RWLOCK_WRUNLOCK(LBPRM_LOCK, &p->per_tgrp[tgid - 1].lbprm.fwrr.lock);
	return srv;
}

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
