/*
 * FastCGI mux-demux for connections
 *
 * Copyright (C) 2019 HAProxy Technologies, Christopher Faulet <cfaulet@haproxy.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <import/ist.h>
#include <import/eb32tree.h>

#include <haproxy/api.h>
#include <haproxy/cfgparse.h>
#include <haproxy/connection.h>
#include <haproxy/dynbuf.h>
#include <haproxy/errors.h>
#include <haproxy/fcgi-app.h>
#include <haproxy/fcgi.h>
#include <haproxy/h1.h>
#include <haproxy/h1_htx.h>
#include <haproxy/http_htx.h>
#include <haproxy/htx.h>
#include <haproxy/list.h>
#include <haproxy/log.h>
#include <haproxy/mux_fcgi-t.h>
#include <haproxy/net_helper.h>
#include <haproxy/proxy.h>
#include <haproxy/regex.h>
#include <haproxy/sc_strm.h>
#include <haproxy/server.h>
#include <haproxy/session-t.h>
#include <haproxy/stconn.h>
#include <haproxy/stream.h>
#include <haproxy/trace.h>
#include <haproxy/version.h>

/* 32 buffers: one for the ring's root, rest for the mbuf itself */
#define FCGI_C_MBUF_CNT 32

/* Size for a record header (also size of empty record) */
#define FCGI_RECORD_HEADER_SZ 8

/* FCGI connection descriptor */
struct fcgi_conn {
	struct connection *conn;

	enum fcgi_conn_st state;              /* FCGI connection state */
	int16_t max_id;                       /* highest ID known on this connection, <0 before mgmt records */
	uint32_t streams_limit;               /* maximum number of concurrent streams the peer supports */
	uint32_t flags;                      /* Connection flags: FCGI_CF_* */

	int16_t  dsi;                        /* dmux stream ID (<0 = idle ) */
	uint16_t drl;                        /* demux record length (if dsi >= 0) */
	uint8_t  drt;                        /* demux record type (if dsi >= 0) */
	uint8_t  drp;                        /* demux record padding (if dsi >= 0) */

	struct buffer dbuf;                  /* demux buffer */
	struct buffer mbuf[FCGI_C_MBUF_CNT]; /* mux buffers (ring) */

	int timeout;                         /* idle timeout duration in ticks */
	int shut_timeout;                    /* idle timeout duration in ticks after shutdown */
	unsigned int nb_streams;             /* number of streams in the tree */
	unsigned int nb_sc;                  /* number of attached stream connectors */
	unsigned int nb_reserved;            /* number of reserved streams */
	unsigned int stream_cnt;             /* total number of streams seen */

	uint32_t term_evts_log;              /* Termination events log: first 4 events reported */

	struct proxy *proxy;                 /* the proxy this connection was created for */
	struct fcgi_app *app;                /* FCGI application used by this mux */
	struct task *task;                   /* timeout management task */
	struct eb_root streams_by_id;        /* all active streams by their ID */

	struct list send_list;               /* list of blocked streams requesting to send */

	struct buffer_wait buf_wait;         /* Wait list for buffer allocation */
	struct wait_event wait_event;        /* To be used if we're waiting for I/Os */
};


/* FCGI stream descriptor */
struct fcgi_strm {
	struct sedesc *sd;
	struct session *sess;
	struct fcgi_conn *fconn;

	int32_t id;                   /* stream ID */

	uint32_t flags;               /* Connection flags: FCGI_SF_* */
	enum fcgi_strm_st state;      /* FCGI stream state */
	int proto_status;             /* FCGI_PS_* */

	struct h1m h1m;               /* response parser state for H1 */

	struct buffer rxbuf;          /* receive buffer, always valid (buf_empty or real buffer) */

	struct eb32_node by_id;       /* place in fcgi_conn's streams_by_id */
	struct wait_event *subs;      /* Address of the wait_event the stream connector associated is waiting on */
	struct list send_list;        /* To be used when adding in fcgi_conn->send_list */
	struct tasklet *shut_tl;      /* deferred shutdown tasklet, to retry to close after we failed to by lack of space */
};

/* Flags representing all default FCGI parameters */
#define FCGI_SP_CGI_GATEWAY    0x00000001
#define FCGI_SP_DOC_ROOT       0x00000002
#define FCGI_SP_SCRIPT_NAME    0x00000004
#define FCGI_SP_PATH_INFO      0x00000008
#define FCGI_SP_REQ_URI        0x00000010
#define FCGI_SP_REQ_METH       0x00000020
#define FCGI_SP_REQ_QS         0x00000040
#define FCGI_SP_SRV_PORT       0x00000080
#define FCGI_SP_SRV_PROTO      0x00000100
#define FCGI_SP_SRV_NAME       0x00000200
#define FCGI_SP_REM_ADDR       0x00000400
#define FCGI_SP_REM_PORT       0x00000800
#define FCGI_SP_SCRIPT_FILE    0x00001000
#define FCGI_SP_PATH_TRANS     0x00002000
#define FCGI_SP_CONT_LEN       0x00004000
#define FCGI_SP_HTTPS          0x00008000
#define FCGI_SP_SRV_SOFT       0x00010000
#define FCGI_SP_MASK           0x0001FFFF
#define FCGI_SP_URI_MASK       (FCGI_SP_SCRIPT_NAME|FCGI_SP_PATH_INFO|FCGI_SP_REQ_QS)

/* FCGI parameters used when PARAMS record is sent */
struct fcgi_strm_params {
	uint32_t mask;
	struct ist docroot;
	struct ist scriptname;
	struct ist pathinfo;
	struct ist meth;
	struct ist uri;
	struct ist vsn;
	struct ist qs;
	struct ist srv_name;
	struct ist srv_port;
	struct ist rem_addr;
	struct ist rem_port;
	struct ist cont_len;
	struct ist srv_soft;
	int https;
	struct buffer *p;
};

/* Maximum amount of data we're OK with re-aligning for buffer optimizations */
#define MAX_DATA_REALIGN 1024

/* trace source and events */
static void fcgi_trace(enum trace_level level, uint64_t mask,
		       const struct trace_source *src,
		       const struct ist where, const struct ist func,
		       const void *a1, const void *a2, const void *a3, const void *a4);

/* The event representation is split like this :
 *   fconn - internal FCGI connection
 *   fstrm - internal FCGI stream
 *   strm  - application layer
 *   rx    - data receipt
 *   tx    - data transmission
 *   rsp   - response parsing
 */
static const struct trace_event fcgi_trace_events[] = {
#define           FCGI_EV_FCONN_NEW     (1ULL <<  0)
	{ .mask = FCGI_EV_FCONN_NEW,    .name = "fconn_new",        .desc = "new FCGI connection" },
#define           FCGI_EV_FCONN_RECV    (1ULL <<  1)
	{ .mask = FCGI_EV_FCONN_RECV,   .name = "fconn_recv",       .desc = "Rx on FCGI connection" },
#define           FCGI_EV_FCONN_SEND    (1ULL <<  2)
	{ .mask = FCGI_EV_FCONN_SEND,   .name = "fconn_send",       .desc = "Tx on FCGI connection" },
#define           FCGI_EV_FCONN_BLK     (1ULL <<  3)
	{ .mask = FCGI_EV_FCONN_BLK,    .name = "fconn_blk",        .desc = "FCGI connection blocked" },
#define           FCGI_EV_FCONN_WAKE    (1ULL <<  4)
	{ .mask = FCGI_EV_FCONN_WAKE,   .name = "fconn_wake",       .desc = "FCGI connection woken up" },
#define           FCGI_EV_FCONN_END     (1ULL <<  5)
	{ .mask = FCGI_EV_FCONN_END,    .name = "fconn_end",        .desc = "FCGI connection terminated" },
#define           FCGI_EV_FCONN_ERR     (1ULL <<  6)
	{ .mask = FCGI_EV_FCONN_ERR,    .name = "fconn_err",        .desc = "error on FCGI connection" },

#define           FCGI_EV_RX_FHDR       (1ULL <<  7)
	{ .mask = FCGI_EV_RX_FHDR,      .name = "rx_fhdr",          .desc = "FCGI record header received" },
#define           FCGI_EV_RX_RECORD     (1ULL <<  8)
	{ .mask = FCGI_EV_RX_RECORD,    .name = "rx_record",        .desc = "receipt of any FCGI record" },
#define           FCGI_EV_RX_EOI        (1ULL <<  9)
	{ .mask = FCGI_EV_RX_EOI,       .name = "rx_eoi",           .desc = "receipt of end of FCGI input" },
#define           FCGI_EV_RX_GETVAL     (1ULL << 10)
	{ .mask = FCGI_EV_RX_GETVAL,    .name = "rx_get_values",    .desc = "receipt of FCGI GET_VALUES_RESULT record" },
#define           FCGI_EV_RX_STDOUT     (1ULL << 11)
	{ .mask = FCGI_EV_RX_STDOUT,    .name = "rx_stdout",        .desc = "receipt of FCGI STDOUT record" },
#define           FCGI_EV_RX_STDERR     (1ULL << 12)
	{ .mask = FCGI_EV_RX_STDERR,    .name = "rx_stderr",        .desc = "receipt of FCGI STDERR record" },
#define           FCGI_EV_RX_ENDREQ     (1ULL << 13)
	{ .mask = FCGI_EV_RX_ENDREQ,    .name = "rx_end_req",       .desc = "receipt of FCGI END_REQUEST record" },

#define           FCGI_EV_TX_RECORD     (1ULL << 14)
	{ .mask = FCGI_EV_TX_RECORD,    .name = "tx_record",        .desc = "transmission of any FCGI record" },
#define           FCGI_EV_TX_EOI        (1ULL << 15)
	{ .mask = FCGI_EV_TX_EOI,       .name = "tx_eoi",           .desc = "transmission of FCGI end of input" },
#define           FCGI_EV_TX_BEGREQ     (1ULL << 16)
	{ .mask = FCGI_EV_TX_BEGREQ,    .name = "tx_begin_request", .desc = "transmission of FCGI BEGIN_REQUEST record" },
#define           FCGI_EV_TX_GETVAL     (1ULL << 17)
	{ .mask = FCGI_EV_TX_GETVAL,    .name = "tx_get_values",    .desc = "transmission of FCGI GET_VALUES record" },
#define           FCGI_EV_TX_PARAMS     (1ULL << 18)
	{ .mask = FCGI_EV_TX_PARAMS,    .name = "tx_params",        .desc = "transmission of FCGI PARAMS record" },
#define           FCGI_EV_TX_STDIN      (1ULL << 19)
	{ .mask = FCGI_EV_TX_STDIN,     .name = "tx_stding",        .desc = "transmission of FCGI STDIN record" },
#define           FCGI_EV_TX_ABORT      (1ULL << 20)
	{ .mask = FCGI_EV_TX_ABORT,     .name = "tx_abort",         .desc = "transmission of FCGI ABORT record" },

#define           FCGI_EV_RSP_DATA      (1ULL << 21)
	{ .mask = FCGI_EV_RSP_DATA,     .name = "rsp_data",         .desc = "parse any data of H1 response" },
#define           FCGI_EV_RSP_EOM       (1ULL << 22)
	{ .mask = FCGI_EV_RSP_EOM,      .name = "rsp_eom",          .desc = "reach the end of message of H1 response" },
#define           FCGI_EV_RSP_HDRS      (1ULL << 23)
	{ .mask = FCGI_EV_RSP_HDRS,     .name = "rsp_headers",      .desc = "parse headers of H1 response" },
#define           FCGI_EV_RSP_BODY      (1ULL << 24)
	{ .mask = FCGI_EV_RSP_BODY,     .name = "rsp_body",         .desc = "parse body part of H1 response" },
#define           FCGI_EV_RSP_TLRS      (1ULL << 25)
	{ .mask = FCGI_EV_RSP_TLRS,     .name = "rsp_trailerus",    .desc = "parse trailers of H1 response" },

#define           FCGI_EV_FSTRM_NEW     (1ULL << 26)
	{ .mask = FCGI_EV_FSTRM_NEW,    .name = "fstrm_new",        .desc = "new FCGI stream" },
#define           FCGI_EV_FSTRM_BLK     (1ULL << 27)
	{ .mask = FCGI_EV_FSTRM_BLK,    .name = "fstrm_blk",        .desc = "FCGI stream blocked" },
#define           FCGI_EV_FSTRM_END     (1ULL << 28)
	{ .mask = FCGI_EV_FSTRM_END,    .name = "fstrm_end",        .desc = "FCGI stream terminated" },
#define           FCGI_EV_FSTRM_ERR     (1ULL << 29)
	{ .mask = FCGI_EV_FSTRM_ERR,    .name = "fstrm_err",        .desc = "error on FCGI stream" },

#define           FCGI_EV_STRM_NEW      (1ULL << 30)
	{ .mask = FCGI_EV_STRM_NEW,     .name = "strm_new",         .desc = "app-layer stream creation" },
#define           FCGI_EV_STRM_RECV     (1ULL << 31)
	{ .mask = FCGI_EV_STRM_RECV,    .name = "strm_recv",        .desc = "receiving data for stream" },
#define           FCGI_EV_STRM_SEND     (1ULL << 32)
	{ .mask = FCGI_EV_STRM_SEND,    .name = "strm_send",        .desc = "sending data for stream" },
#define           FCGI_EV_STRM_FULL     (1ULL << 33)
	{ .mask = FCGI_EV_STRM_FULL,    .name = "strm_full",        .desc = "stream buffer full" },
#define           FCGI_EV_STRM_WAKE     (1ULL << 34)
	{ .mask = FCGI_EV_STRM_WAKE,    .name = "strm_wake",        .desc = "stream woken up" },
#define           FCGI_EV_STRM_SHUT     (1ULL << 35)
	{ .mask = FCGI_EV_STRM_SHUT,    .name = "strm_shut",        .desc = "stream shutdown" },
#define           FCGI_EV_STRM_END      (1ULL << 36)
	{ .mask = FCGI_EV_STRM_END,     .name = "strm_end",         .desc = "detaching app-layer stream" },
#define           FCGI_EV_STRM_ERR      (1ULL << 37)
	{ .mask = FCGI_EV_STRM_ERR,     .name = "strm_err",         .desc = "stream error" },

	{ }
};

static const struct name_desc fcgi_trace_lockon_args[4] = {
	/* arg1 */ { /* already used by the connection */ },
	/* arg2 */ { .name="fstrm", .desc="FCGI stream" },
	/* arg3 */ { },
	/* arg4 */ { }
};


static const struct name_desc fcgi_trace_decoding[] = {
#define FCGI_VERB_CLEAN    1
	{ .name="clean",    .desc="only user-friendly stuff, generally suitable for level \"user\"" },
#define FCGI_VERB_MINIMAL  2
	{ .name="minimal",  .desc="report only fconn/fstrm state and flags, no real decoding" },
#define FCGI_VERB_SIMPLE   3
	{ .name="simple",   .desc="add request/response status line or htx info when available" },
#define FCGI_VERB_ADVANCED 4
	{ .name="advanced", .desc="add header fields or record decoding when available" },
#define FCGI_VERB_COMPLETE 5
	{ .name="complete", .desc="add full data dump when available" },
	{ /* end */ }
};

static struct trace_source trace_fcgi __read_mostly = {
	.name = IST("fcgi"),
	.desc = "FastCGI multiplexer",
	.arg_def = TRC_ARG1_CONN,  // TRACE()'s first argument is always a connection
	.default_cb = fcgi_trace,
	.known_events = fcgi_trace_events,
	.lockon_args = fcgi_trace_lockon_args,
	.decoding = fcgi_trace_decoding,
	.report_events = ~0,  // report everything by default
};

#define TRACE_SOURCE &trace_fcgi
INITCALL1(STG_REGISTER, trace_register_source, TRACE_SOURCE);

/* FCGI connection and stream pools */
DECLARE_STATIC_POOL(pool_head_fcgi_conn, "fcgi_conn", sizeof(struct fcgi_conn));
DECLARE_STATIC_POOL(pool_head_fcgi_strm, "fcgi_strm", sizeof(struct fcgi_strm));

struct task *fcgi_timeout_task(struct task *t, void *context, unsigned int state);
static int fcgi_process(struct fcgi_conn *fconn);
/* fcgi_io_cb is exported to see it resolved in "show fd" */
struct task *fcgi_io_cb(struct task *t, void *ctx, unsigned int state);
static inline struct fcgi_strm *fcgi_conn_st_by_id(struct fcgi_conn *fconn, int id);
struct task *fcgi_deferred_shut(struct task *t, void *ctx, unsigned int state);
static struct fcgi_strm *fcgi_stconn_new(struct fcgi_conn *fconn, struct stconn *sc, struct session *sess);
static void fcgi_strm_notify_recv(struct fcgi_strm *fstrm);
static void fcgi_strm_notify_send(struct fcgi_strm *fstrm);
static void fcgi_strm_alert(struct fcgi_strm *fstrm);
static int fcgi_strm_send_abort(struct fcgi_conn *fconn, struct fcgi_strm *fstrm);
static int fcgi_dump_fcgi_conn_info(struct buffer *msg, struct fcgi_conn *fconn, const char *pfx);
static int fcgi_dump_fcgi_strm_info(struct buffer *msg, const struct fcgi_strm *fstrm, const char *pfx);

/* a dummy closed endpoint */
static const struct sedesc closed_ep = {
	.sc        = NULL,
	.flags     = SE_FL_DETACHED,
};

/* a dmumy management stream */
static const struct fcgi_strm *fcgi_mgmt_stream = &(const struct fcgi_strm){
	.sd        = (struct sedesc*)&closed_ep,
	.fconn     = NULL,
	.state     = FCGI_SS_CLOSED,
	.flags     = FCGI_SF_NONE,
	.id        = 0,
};

/* and a dummy idle stream for use with any unknown stream */
static const struct fcgi_strm *fcgi_unknown_stream = &(const struct fcgi_strm){
	.sd        = (struct sedesc*)&closed_ep,
	.fconn     = NULL,
	.state     = FCGI_SS_IDLE,
	.flags     = FCGI_SF_NONE,
	.id        = 0,
};

/* returns the stconn associated to the FCGI stream */
static forceinline struct stconn *fcgi_strm_sc(const struct fcgi_strm *fstrm)
{
	return fstrm->sd->sc;
}


/* the FCGI traces always expect that arg1, if non-null, is of type connection
 * (from which we can derive fconn), that arg2, if non-null, is of type fstrm,
 * and that arg3, if non-null, is a htx for rx/tx headers.
 */
static void fcgi_trace(enum trace_level level, uint64_t mask, const struct trace_source *src,
		       const struct ist where, const struct ist func,
		       const void *a1, const void *a2, const void *a3, const void *a4)
{
	const struct connection *conn = a1;
	struct fcgi_conn *fconn = conn ? conn->ctx : NULL;
	const struct fcgi_strm *fstrm = a2;
	const struct htx *htx = a3;
	const size_t     *val = a4;

	if (!fconn)
		fconn = (fstrm ? fstrm->fconn : NULL);

	if (!fconn || src->verbosity < FCGI_VERB_CLEAN)
		return;

	/* Display the response state if fstrm is defined */
	if (fstrm)
		chunk_appendf(&trace_buf, " [rsp:%s]", h1m_state_str(fstrm->h1m.state));

	if (src->verbosity == FCGI_VERB_CLEAN)
		return;

	/* Display the value to the 4th argument (level > STATE) */
	if (src->level > TRACE_LEVEL_STATE && val)
		chunk_appendf(&trace_buf, " - VAL=%lu", (long)*val);

	/* Display status-line if possible (verbosity > MINIMAL) */
	if (src->verbosity > FCGI_VERB_MINIMAL && htx && htx_nbblks(htx)) {
		const struct htx_blk *blk = __htx_get_head_blk(htx);
		const struct htx_sl  *sl  = htx_get_blk_ptr(htx, blk);
		enum htx_blk_type    type = htx_get_blk_type(blk);

		if (type == HTX_BLK_REQ_SL || type == HTX_BLK_RES_SL)
			chunk_appendf(&trace_buf, " - \"%.*s %.*s %.*s\"",
				      HTX_SL_P1_LEN(sl), HTX_SL_P1_PTR(sl),
				      HTX_SL_P2_LEN(sl), HTX_SL_P2_PTR(sl),
				      HTX_SL_P3_LEN(sl), HTX_SL_P3_PTR(sl));
	}

	/* Display fconn info and, if defined, fstrm info */
	chunk_appendf(&trace_buf, " - fconn=%p(%s,0x%08x)", fconn, fconn_st_to_str(fconn->state), fconn->flags);
	if (fstrm)
		chunk_appendf(&trace_buf, " fstrm=%p(%d,%s,0x%08x)", fstrm, fstrm->id, fstrm_st_to_str(fstrm->state), fstrm->flags);

	if (!fstrm || fstrm->id <= 0)
		chunk_appendf(&trace_buf, " dsi=%d", fconn->dsi);
	if (fconn->dsi >= 0 && (mask & FCGI_EV_RX_FHDR))
		chunk_appendf(&trace_buf, " drt=%s", fcgi_rt_str(fconn->drt));

	if (src->verbosity == FCGI_VERB_MINIMAL)
		return;

	/* Display mbuf and dbuf info (level > USER & verbosity > SIMPLE) */
	if (src->level > TRACE_LEVEL_USER) {
		if (src->verbosity == FCGI_VERB_COMPLETE ||
		    (src->verbosity == FCGI_VERB_ADVANCED && (mask & (FCGI_EV_FCONN_RECV|FCGI_EV_RX_RECORD))))
			chunk_appendf(&trace_buf, " dbuf=%u@%p+%u/%u",
				      (unsigned int)b_data(&fconn->dbuf), b_orig(&fconn->dbuf),
				      (unsigned int)b_head_ofs(&fconn->dbuf), (unsigned int)b_size(&fconn->dbuf));
		if (src->verbosity == FCGI_VERB_COMPLETE ||
		    (src->verbosity == FCGI_VERB_ADVANCED && (mask & (FCGI_EV_FCONN_SEND|FCGI_EV_TX_RECORD)))) {
			struct buffer *hmbuf = br_head(fconn->mbuf);
			struct buffer *tmbuf = br_tail(fconn->mbuf);

			chunk_appendf(&trace_buf, " .mbuf=[%u..%u|%u],h=[%u@%p+%u/%u],t=[%u@%p+%u/%u]",
				      br_head_idx(fconn->mbuf), br_tail_idx(fconn->mbuf), br_size(fconn->mbuf),
				      (unsigned int)b_data(hmbuf), b_orig(hmbuf),
				      (unsigned int)b_head_ofs(hmbuf), (unsigned int)b_size(hmbuf),
				      (unsigned int)b_data(tmbuf), b_orig(tmbuf),
				      (unsigned int)b_head_ofs(tmbuf), (unsigned int)b_size(tmbuf));
		}

		if (fstrm && (src->verbosity == FCGI_VERB_COMPLETE ||
			      (src->verbosity == FCGI_VERB_ADVANCED && (mask & (FCGI_EV_STRM_RECV|FCGI_EV_RSP_DATA)))))
			chunk_appendf(&trace_buf, " rxbuf=%u@%p+%u/%u",
				      (unsigned int)b_data(&fstrm->rxbuf), b_orig(&fstrm->rxbuf),
				      (unsigned int)b_head_ofs(&fstrm->rxbuf), (unsigned int)b_size(&fstrm->rxbuf));
	}

	/* Display htx info if defined (level > USER) */
	if (src->level > TRACE_LEVEL_USER && htx) {
		int full = 0;

		/* Full htx info (level > STATE && verbosity > SIMPLE) */
		if (src->level > TRACE_LEVEL_STATE) {
			if (src->verbosity == FCGI_VERB_COMPLETE)
				full = 1;
			else if (src->verbosity == FCGI_VERB_ADVANCED && (mask & (FCGI_EV_RSP_HDRS|FCGI_EV_TX_PARAMS)))
				full = 1;
		}

		chunk_memcat(&trace_buf, "\n\t", 2);
		htx_dump(&trace_buf, htx, full);
	}
}

/*****************************************************/
/* functions below are for dynamic buffer management */
/*****************************************************/

/* Indicates whether or not the we may call the fcgi_recv() function to attempt
 * to receive data into the buffer and/or demux pending data. The condition is
 * a bit complex due to some API limits for now. The rules are the following :
 *   - if an error or a shutdown was detected on the connection,
       we must not attempt to receive
 *   - if we're subscribed for receiving, no need to try again
 *   - if the demux buf failed to be allocated, we must not try to receive and
 *     we know there is nothing pending (we'll be woken up once allocated)
 *   - if the demux buf is full, we will not be able to receive.
 *   - otherwise we may attempt to receive
 */
static inline int fcgi_recv_allowed(const struct fcgi_conn *fconn)
{
	if (fconn->flags & (FCGI_CF_EOS|FCGI_CF_ERROR) || fconn->state == FCGI_CS_CLOSED)
		return 0;

	if ((fconn->wait_event.events & SUB_RETRY_RECV))
		return 0;

	if (!(fconn->flags & (FCGI_CF_DEM_DALLOC | FCGI_CF_DEM_DFULL)))
	    return 1;

	return 0;
}

/* Indicates whether it's worth waking up the I/O handler to restart demuxing.
 * Its conditions are the following:
 *   - if the buffer is empty and the connection is closed, there's nothing
 *     to demux
 *   - if a short read was reported, no need to try demuxing again
 *   - if some blocking conditions remain, no need to try again
 *   - otherwise it's safe to try demuxing again
 */
static inline int fcgi_may_demux(const struct fcgi_conn *fconn)
{
	if (fconn->state == FCGI_CS_CLOSED && !b_data(&fconn->dbuf))
		return 0;

	if (fconn->flags & FCGI_CF_DEM_SHORT_READ)
		return 0;

	if (fconn->flags & FCGI_CF_DEM_BLOCK_ANY)
		return 0;

	return 1;
}


/* restarts reading/processing on the connection if we can receive or demux
 * (both are called from the same tasklet).
 */
static inline void fcgi_conn_restart_reading(const struct fcgi_conn *fconn, int consider_buffer)
{
	if (!fcgi_recv_allowed(fconn) && !fcgi_may_demux(fconn))
		return;
	tasklet_wakeup(fconn->wait_event.tasklet);
}


/* Tries to grab a buffer and to re-enable processing on mux <target>. The
 * fcgi_conn flags are used to figure what buffer was requested. It returns 1 if
 * the allocation succeeds, in which case the connection is woken up, or 0 if
 * it's impossible to wake up and we prefer to be woken up later.
 */
static int fcgi_buf_available(void *target)
{
	struct fcgi_conn *fconn = target;
	struct fcgi_strm *fstrm;

	if ((fconn->flags & FCGI_CF_DEM_DALLOC) && b_alloc(&fconn->dbuf, DB_MUX_RX)) {
		TRACE_STATE("unblocking fconn, dbuf allocated", FCGI_EV_FCONN_RECV|FCGI_EV_FCONN_BLK|FCGI_EV_FCONN_WAKE, fconn->conn);
		fconn->flags &= ~FCGI_CF_DEM_DALLOC;
		fcgi_conn_restart_reading(fconn, 1);
		return 1;
	}

	if ((fconn->flags & FCGI_CF_MUX_MALLOC) && b_alloc(br_tail(fconn->mbuf), DB_MUX_TX)) {
		TRACE_STATE("unblocking fconn, mbuf allocated", FCGI_EV_FCONN_SEND|FCGI_EV_FCONN_BLK|FCGI_EV_FCONN_WAKE, fconn->conn);
		fconn->flags &= ~FCGI_CF_MUX_MALLOC;
		if (fconn->flags & FCGI_CF_DEM_MROOM) {
			fconn->flags &= ~FCGI_CF_DEM_MROOM;
			fcgi_conn_restart_reading(fconn, 1);
		}
		return 1;
	}

	if ((fconn->flags & FCGI_CF_DEM_SALLOC) &&
	    (fstrm = fcgi_conn_st_by_id(fconn, fconn->dsi)) && fcgi_strm_sc(fstrm) &&
	    b_alloc(&fstrm->rxbuf, DB_SE_RX)) {
		TRACE_STATE("unblocking fstrm, rxbuf allocated", FCGI_EV_STRM_RECV|FCGI_EV_FSTRM_BLK|FCGI_EV_STRM_WAKE, fconn->conn, fstrm);
		fconn->flags &= ~FCGI_CF_DEM_SALLOC;
		fcgi_conn_restart_reading(fconn, 1);
		fcgi_strm_notify_recv(fstrm);
		return 1;
	}

	return 0;
}

static inline struct buffer *fcgi_get_buf(struct fcgi_conn *fconn, struct buffer *bptr)
{
	struct buffer *buf = NULL;

	if (likely(!LIST_INLIST(&fconn->buf_wait.list)) &&
	    unlikely((buf = b_alloc(bptr, DB_MUX_RX)) == NULL)) {
		b_queue(DB_MUX_RX, &fconn->buf_wait, fconn, fcgi_buf_available);
	}
	return buf;
}

static inline void fcgi_release_buf(struct fcgi_conn *fconn, struct buffer *bptr)
{
	if (bptr->size) {
		b_free(bptr);
		offer_buffers(NULL, 1);
	}
}

static inline void fcgi_release_mbuf(struct fcgi_conn *fconn)
{
	struct buffer *buf;
	unsigned int count = 0;

	while (b_size(buf = br_head_pick(fconn->mbuf))) {
		b_free(buf);
		count++;
	}
	if (count)
		offer_buffers(NULL, count);
}

/* Returns the number of allocatable outgoing streams for the connection taking
 * the number reserved streams into account.
 */
static inline int fcgi_streams_left(const struct fcgi_conn *fconn)
{
	int ret;

	ret = (unsigned int)(0x7FFF - fconn->max_id) - fconn->nb_reserved - 1;
	if (ret < 0)
		ret = 0;
	return ret;
}

/* Returns the number of streams in use on a connection to figure if it's
 * idle or not. We check nb_sc and not nb_streams as the caller will want
 * to know if it was the last one after a detach().
 */
static int fcgi_used_streams(struct connection *conn)
{
	struct fcgi_conn *fconn = conn->ctx;

	return fconn->nb_sc;
}

/* Returns the number of concurrent streams available on the connection */
static int fcgi_avail_streams(struct connection *conn)
{
	struct server *srv = objt_server(conn->target);
	struct fcgi_conn *fconn = conn->ctx;
	int ret1, ret2;

	/* Don't open new stream if the connection is closed */
	if (fconn->state == FCGI_CS_CLOSED)
		return 0;

	/* May be negative if this setting has changed */
	ret1 = (fconn->streams_limit - fconn->nb_streams);

	/* we must also consider the limit imposed by stream IDs */
	ret2 = fcgi_streams_left(fconn);
	ret1 = MIN(ret1, ret2);
	if (ret1 > 0 && srv && srv->max_reuse >= 0) {
		ret2 = ((fconn->stream_cnt <= srv->max_reuse) ? srv->max_reuse - fconn->stream_cnt + 1: 0);
		ret1 = MIN(ret1, ret2);
	}
	return ret1;
}

/*****************************************************************/
/* functions below are dedicated to the mux setup and management */
/*****************************************************************/

/* Initializes the mux once it's attached. Only outgoing connections are
 * supported. So the context is already initialized before installing the
 * mux. <input> is always used as Input buffer and may contain data. It is the
 * caller responsibility to not reuse it anymore. Returns < 0 on error.
 */
static int fcgi_init(struct connection *conn, struct proxy *px, struct session *sess,
		     struct buffer *input)
{
	struct fcgi_conn *fconn;
	struct fcgi_strm *fstrm;
	struct fcgi_app *app = get_px_fcgi_app(px);
	struct task *t = NULL;
	void *conn_ctx = conn->ctx;

	TRACE_ENTER(FCGI_EV_FSTRM_NEW);

	if (!app) {
		TRACE_ERROR("No FCGI app found, don't create fconn", FCGI_EV_FCONN_NEW|FCGI_EV_FCONN_END|FCGI_EV_FCONN_ERR);
		goto fail_conn;
	}

	fconn = pool_alloc(pool_head_fcgi_conn);
	if (!fconn) {
		TRACE_ERROR("fconn allocation failure", FCGI_EV_FCONN_NEW|FCGI_EV_FCONN_END|FCGI_EV_FCONN_ERR);
		goto fail_conn;
	}

	fconn->shut_timeout = fconn->timeout = px->timeout.server;
	if (tick_isset(px->timeout.serverfin))
		fconn->shut_timeout = px->timeout.serverfin;

	fconn->flags = FCGI_CF_NONE;

	/* Retrieve useful info from the FCGI app */
	if (app->flags & FCGI_APP_FL_KEEP_CONN)
		fconn->flags |= FCGI_CF_KEEP_CONN;
	if (app->flags & FCGI_APP_FL_GET_VALUES)
		fconn->flags |= FCGI_CF_GET_VALUES;
	if (app->flags & FCGI_APP_FL_MPXS_CONNS)
		fconn->flags |= FCGI_CF_MPXS_CONNS;

	fconn->proxy = px;
	fconn->app = app;
	fconn->task = NULL;
	if (tick_isset(fconn->timeout)) {
		t = task_new_here();
		if (!t) {
			TRACE_ERROR("fconn task allocation failure", FCGI_EV_FCONN_NEW|FCGI_EV_FCONN_END|FCGI_EV_FCONN_ERR);
			goto fail;
		}

		fconn->task = t;
		t->process = fcgi_timeout_task;
		t->context = fconn;
		t->expire = tick_add(now_ms, fconn->timeout);
	}

	fconn->wait_event.tasklet = tasklet_new();
	if (!fconn->wait_event.tasklet)
		goto fail;
	fconn->wait_event.tasklet->process = fcgi_io_cb;
	fconn->wait_event.tasklet->context = fconn;
	fconn->wait_event.events = 0;

	/* Initialise the context. */
	fconn->state = FCGI_CS_INIT;
	fconn->conn = conn;
	fconn->streams_limit = app->maxreqs;
	fconn->max_id = -1;
	fconn->nb_streams = 0;
	fconn->nb_sc = 0;
	fconn->nb_reserved = 0;
	fconn->stream_cnt = 0;
	fconn->term_evts_log = 0;

	fconn->dbuf = *input;
	fconn->dsi = -1;

	br_init(fconn->mbuf, sizeof(fconn->mbuf) / sizeof(fconn->mbuf[0]));
	fconn->streams_by_id = EB_ROOT;
	LIST_INIT(&fconn->send_list);
	LIST_INIT(&fconn->buf_wait.list);

	conn->ctx = fconn;

	if (t)
		task_queue(t);

	/* FIXME: this is temporary, for outgoing connections we need to
	 * immediately allocate a stream until the code is modified so that the
	 * caller calls ->attach(). For now the outgoing sc is stored as
	 * conn->ctx by the caller and saved in conn_ctx.
	 */
	fstrm = fcgi_stconn_new(fconn, conn_ctx, sess);
	if (!fstrm)
		goto fail;


	/* Repare to read something */
	fcgi_conn_restart_reading(fconn, 1);
	TRACE_LEAVE(FCGI_EV_FCONN_NEW, conn);
	return 0;

  fail:
	task_destroy(t);
	tasklet_free(fconn->wait_event.tasklet);
	pool_free(pool_head_fcgi_conn, fconn);
  fail_conn:
	conn->ctx = conn_ctx; // restore saved ctx
	TRACE_DEVEL("leaving in error", FCGI_EV_FCONN_NEW|FCGI_EV_FCONN_END|FCGI_EV_FCONN_ERR);
	return -1;
}


static inline void fcgi_conn_report_term_evt(struct fcgi_conn *fconn, enum muxc_term_event_type type)
{
	enum term_event_loc loc = tevt_loc_muxc + 8; /* Always on backend side for now */

	fconn->term_evts_log = tevt_report_event(fconn->term_evts_log, loc, type);
}

/* Returns the next allocatable outgoing stream ID for the FCGI connection, or
 * -1 if no more is allocatable.
 */
static inline int32_t fcgi_conn_get_next_sid(const struct fcgi_conn *fconn)
{
	int32_t id = (fconn->max_id + 1) | 1;

	if ((id & 0x80000000U))
		id = -1;
	return id;
}

/* Returns the stream associated with id <id> or NULL if not found */
static inline struct fcgi_strm *fcgi_conn_st_by_id(struct fcgi_conn *fconn, int id)
{
	struct eb32_node *node;

	if (id == 0)
	 	return (struct fcgi_strm *)fcgi_mgmt_stream;

	if (id > fconn->max_id)
		return (struct fcgi_strm *)fcgi_unknown_stream;

	node = eb32_lookup(&fconn->streams_by_id, id);
	if (!node)
		return (struct fcgi_strm *)fcgi_unknown_stream;
	return container_of(node, struct fcgi_strm, by_id);
}

/* Release function. This one should be called to free all resources allocated
 * to the mux.
 */
static void fcgi_release(struct fcgi_conn *fconn)
{
	struct connection *conn = fconn->conn;

	TRACE_POINT(FCGI_EV_FCONN_END);

	b_dequeue(&fconn->buf_wait);

	fcgi_release_buf(fconn, &fconn->dbuf);
	fcgi_release_mbuf(fconn);

	if (fconn->task) {
		fconn->task->context = NULL;
		task_wakeup(fconn->task, TASK_WOKEN_OTHER);
		fconn->task = NULL;
	}
	tasklet_free(fconn->wait_event.tasklet);
	if (conn) {
		if (fconn->wait_event.events != 0)
			conn->xprt->unsubscribe(conn, conn->xprt_ctx, fconn->wait_event.events,
						&fconn->wait_event);
		fcgi_conn_report_term_evt(fconn, muxc_tevt_type_shutw);
	}

	pool_free(pool_head_fcgi_conn, fconn);

	if (conn) {
		conn->mux = NULL;
		conn->ctx = NULL;
		TRACE_DEVEL("freeing conn", FCGI_EV_FCONN_END, conn);

		conn_stop_tracking(conn);
		conn_full_close(conn);
		if (conn->destroy_cb)
			conn->destroy_cb(conn);
		conn_free(conn);
	}
}

/* Detect a pending read0 for a FCGI connection. It happens if a read0 was
 * already reported on a previous xprt->rcvbuf() AND a record parser failed
 * to parse pending data, confirming no more progress is possible because
 * we're facing a truncated frame. The function returns 1 to report a read0
 * or 0 otherwise.
 */
static int fcgi_conn_read0_pending(struct fcgi_conn *fconn)
{
	return !!(fconn->flags & FCGI_CF_END_REACHED);
}


/* Returns true if the FCGI connection must be release */
static inline int fcgi_conn_is_dead(struct fcgi_conn *fconn)
{
	if (eb_is_empty(&fconn->streams_by_id) &&               /* don't close if streams exist */
	    (!(fconn->flags & FCGI_CF_KEEP_CONN) ||             /* don't keep the connection alive */
	     (fconn->flags & FCGI_CF_ERROR) ||                  /* errors close immediately */
	     (fconn->state == FCGI_CS_CLOSED && !fconn->task) ||/* a timeout stroke earlier */
	     (!(fconn->conn->owner)) ||                         /* Nobody's left to take care of the connection, drop it now */
	     (!br_data(fconn->mbuf) &&                          /* mux buffer empty, also process clean events below */
	      (fconn->flags & FCGI_CF_EOS))))
	      return 1;
	return 0;
}


/********************************************************/
/* functions below are for the FCGI protocol processing */
/********************************************************/

/* Marks an error on the stream. */
static inline void fcgi_strm_error(struct fcgi_strm *fstrm)
{
	if (fstrm->id && fstrm->state != FCGI_SS_ERROR) {
		TRACE_POINT(FCGI_EV_FSTRM_ERR, fstrm->fconn->conn, fstrm);
		if (fstrm->state < FCGI_SS_ERROR) {
			fstrm->state = FCGI_SS_ERROR;
			TRACE_STATE("switching to ERROR", FCGI_EV_FSTRM_ERR, fstrm->fconn->conn, fstrm);
		}
		se_fl_set_error(fstrm->sd);
	}
}

/* Attempts to notify the data layer of recv availability */
static void fcgi_strm_notify_recv(struct fcgi_strm *fstrm)
{
	if (fstrm->subs && (fstrm->subs->events & SUB_RETRY_RECV)) {
		TRACE_POINT(FCGI_EV_STRM_WAKE, fstrm->fconn->conn, fstrm);
		tasklet_wakeup(fstrm->subs->tasklet);
		fstrm->subs->events &= ~SUB_RETRY_RECV;
		if (!fstrm->subs->events)
			fstrm->subs = NULL;
	}
}

/* Attempts to notify the data layer of send availability */
static void fcgi_strm_notify_send(struct fcgi_strm *fstrm)
{
	if (fstrm->subs && (fstrm->subs->events & SUB_RETRY_SEND)) {
		TRACE_POINT(FCGI_EV_STRM_WAKE, fstrm->fconn->conn, fstrm);
		fstrm->flags |= FCGI_SF_NOTIFIED;
		tasklet_wakeup(fstrm->subs->tasklet);
		fstrm->subs->events &= ~SUB_RETRY_SEND;
		if (!fstrm->subs->events)
			fstrm->subs = NULL;
	}
	else if (fstrm->flags & (FCGI_SF_WANT_SHUTR | FCGI_SF_WANT_SHUTW)) {
		TRACE_POINT(FCGI_EV_STRM_WAKE, fstrm->fconn->conn, fstrm);
		tasklet_wakeup(fstrm->shut_tl);
	}
}

/* Alerts the data layer, trying to wake it up by all means, following
 * this sequence :
 *   - if the fcgi stream' data layer is subscribed to recv, then it's woken up
 *     for recv
 *   - if its subscribed to send, then it's woken up for send
 *   - if it was subscribed to neither, its ->wake() callback is called
 * It is safe to call this function with a closed stream which doesn't have a
 * stream connector anymore.
 */
static void fcgi_strm_alert(struct fcgi_strm *fstrm)
{
	TRACE_POINT(FCGI_EV_STRM_WAKE, fstrm->fconn->conn, fstrm);
	if (fstrm->subs ||
	    (fstrm->flags & (FCGI_SF_WANT_SHUTR|FCGI_SF_WANT_SHUTW))) {
		fcgi_strm_notify_recv(fstrm);
		fcgi_strm_notify_send(fstrm);
	}
	else if (fcgi_strm_sc(fstrm) && fcgi_strm_sc(fstrm)->app_ops->wake != NULL) {
		TRACE_POINT(FCGI_EV_STRM_WAKE, fstrm->fconn->conn, fstrm);
		fcgi_strm_sc(fstrm)->app_ops->wake(fcgi_strm_sc(fstrm));
	}
}

/* Writes the 16-bit record size <len> at address <record> */
static inline void fcgi_set_record_size(void *record, uint16_t len)
{
	uint8_t *out = (record + 4);

	*out       = (len >> 8);
	*(out + 1) = (len & 0xff);
}

/* Writes the 16-bit stream id <id> at address <record> */
static inline void fcgi_set_record_id(void *record, uint16_t id)
{
	uint8_t *out = (record + 2);

	*out       = (id >> 8);
	*(out + 1) = (id & 0xff);
}

/* Marks a FCGI stream as CLOSED and decrement the number of active streams for
 * its connection if the stream was not yet closed. Please use this exclusively
 * before closing a stream to ensure stream count is well maintained.
 */
static inline void fcgi_strm_close(struct fcgi_strm *fstrm)
{
	if (fstrm->state != FCGI_SS_CLOSED) {
		TRACE_ENTER(FCGI_EV_FSTRM_END, fstrm->fconn->conn, fstrm);
		fstrm->fconn->nb_streams--;
		if (!fstrm->id)
			fstrm->fconn->nb_reserved--;
		if (fcgi_strm_sc(fstrm)) {
			if (!se_fl_test(fstrm->sd, SE_FL_EOS) && !b_data(&fstrm->rxbuf))
				fcgi_strm_notify_recv(fstrm);
		}
		fstrm->state = FCGI_SS_CLOSED;
		TRACE_STATE("switching to CLOSED", FCGI_EV_FSTRM_END, fstrm->fconn->conn, fstrm);
		TRACE_LEAVE(FCGI_EV_FSTRM_END, fstrm->fconn->conn, fstrm);
	}
}

/* Check fconn and fstrm flags to evaluate if EOI/EOS/ERR_PENDING/ERROR flags must
 * be set on the SE.
 */
static inline void fcgi_strm_propagate_term_flags(struct fcgi_conn *fconn, struct fcgi_strm *fstrm)
{
	if (fstrm->h1m.state == H1_MSG_DONE) {
		se_fl_set(fstrm->sd, SE_FL_EOI);
		/* Add EOS flag for tunnel */
		if (!(fstrm->h1m.flags & (H1_MF_VER_11|H1_MF_XFER_LEN))) {
			se_fl_set(fstrm->sd, SE_FL_EOS);
			se_report_term_evt(fstrm->sd, (fconn->flags & FCGI_CF_ERROR ? se_tevt_type_rcv_err : se_tevt_type_eos));
		}
	}
	if (fcgi_conn_read0_pending(fconn) || fstrm->state == FCGI_SS_CLOSED) {
		se_fl_set(fstrm->sd, SE_FL_EOS);
		if (!se_fl_test(fstrm->sd, SE_FL_EOI)) {
			se_fl_set(fstrm->sd, SE_FL_ERROR);
			se_report_term_evt(fstrm->sd, (fconn->flags & FCGI_CF_ERROR ? se_tevt_type_truncated_rcv_err : se_tevt_type_truncated_eos));
		}
		else
			se_report_term_evt(fstrm->sd, (fconn->flags & FCGI_CF_ERROR ? se_tevt_type_rcv_err : se_tevt_type_eos));
	}
	if (se_fl_test(fstrm->sd, SE_FL_ERR_PENDING))
		se_fl_set(fstrm->sd, SE_FL_ERROR);
}

/* Detaches a FCGI stream from its FCGI connection and releases it to the
 * fcgi_strm pool.
 */
static void fcgi_strm_destroy(struct fcgi_strm *fstrm)
{
	struct connection *conn = fstrm->fconn->conn;

	TRACE_ENTER(FCGI_EV_FSTRM_END, conn, fstrm);

	fcgi_strm_close(fstrm);
	eb32_delete(&fstrm->by_id);
	if (b_size(&fstrm->rxbuf)) {
		b_free(&fstrm->rxbuf);
		offer_buffers(NULL, 1);
	}
	if (fstrm->subs)
		fstrm->subs->events = 0;
	/* There's no need to explicitly call unsubscribe here, the only
	 * reference left would be in the fconn send_list/fctl_list, and if
	 * we're in it, we're getting out anyway
	 */
	LIST_DEL_INIT(&fstrm->send_list);
	tasklet_free(fstrm->shut_tl);
	BUG_ON(fstrm->sd && !se_fl_test(fstrm->sd, SE_FL_ORPHAN));
	sedesc_free(fstrm->sd);
	pool_free(pool_head_fcgi_strm, fstrm);

	TRACE_LEAVE(FCGI_EV_FSTRM_END, conn);
}

/* Allocates a new stream <id> for connection <fconn> and adds it into fconn's
 * stream tree. In case of error, nothing is added and NULL is returned. The
 * causes of errors can be any failed memory allocation. The caller is
 * responsible for checking if the connection may support an extra stream prior
 * to calling this function.
 */
static struct fcgi_strm *fcgi_strm_new(struct fcgi_conn *fconn, int id)
{
	struct fcgi_strm *fstrm;

	TRACE_ENTER(FCGI_EV_FSTRM_NEW, fconn->conn);

	fstrm = pool_alloc(pool_head_fcgi_strm);
	if (!fstrm) {
		TRACE_ERROR("fstrm allocation failure", FCGI_EV_FSTRM_NEW|FCGI_EV_FSTRM_ERR|FCGI_EV_FSTRM_END, fconn->conn);
		goto out;
	}

	fstrm->shut_tl = tasklet_new();
	if (!fstrm->shut_tl) {
		TRACE_ERROR("fstrm shut tasklet allocation failure", FCGI_EV_FSTRM_NEW|FCGI_EV_FSTRM_ERR|FCGI_EV_FSTRM_END, fconn->conn);
		pool_free(pool_head_fcgi_strm, fstrm);
		goto out;
	}
	fstrm->subs = NULL;
	fstrm->shut_tl->process = fcgi_deferred_shut;
	fstrm->shut_tl->context = fstrm;
	LIST_INIT(&fstrm->send_list);
	fstrm->fconn = fconn;
	fstrm->sd = NULL;
	fstrm->flags = FCGI_SF_NONE;
	fstrm->proto_status = 0;
	fstrm->state = FCGI_SS_IDLE;
	fstrm->rxbuf = BUF_NULL;

	h1m_init_res(&fstrm->h1m);
	fstrm->h1m.err_pos = -1; // don't care about errors on the request path
	fstrm->h1m.flags |= (H1_MF_NO_PHDR|H1_MF_CLEAN_CONN_HDR);

	fstrm->by_id.key = fstrm->id = id;
	if (id > 0)
		fconn->max_id = id;
	else
		fconn->nb_reserved++;

	eb32_insert(&fconn->streams_by_id, &fstrm->by_id);
	fconn->nb_streams++;
	fconn->stream_cnt++;

	TRACE_LEAVE(FCGI_EV_FSTRM_NEW, fconn->conn, fstrm);
	return fstrm;

  out:
	TRACE_DEVEL("leaving in error", FCGI_EV_FSTRM_NEW|FCGI_EV_FSTRM_ERR|FCGI_EV_FSTRM_END, fconn->conn);
	return NULL;
}

/* Allocates a new stream associated to stream connector <sc> on the FCGI connection
 * <fconn> and returns it, or NULL in case of memory allocation error or if the
 * highest possible stream ID was reached.
 */
static struct fcgi_strm *fcgi_stconn_new(struct fcgi_conn *fconn, struct stconn *sc,
					 struct session *sess)
{
	struct fcgi_strm *fstrm = NULL;

	TRACE_ENTER(FCGI_EV_FSTRM_NEW, fconn->conn);
	if (fconn->nb_streams >= fconn->streams_limit) {
		TRACE_ERROR("streams_limit reached", FCGI_EV_FSTRM_NEW|FCGI_EV_FSTRM_END|FCGI_EV_FSTRM_ERR, fconn->conn);
		goto out;
	}

	if (fcgi_streams_left(fconn) < 1) {
		TRACE_ERROR("!streams_left", FCGI_EV_FSTRM_NEW|FCGI_EV_FSTRM_END|FCGI_EV_FSTRM_ERR, fconn->conn);
		goto out;
	}

	/* Defer choosing the ID until we send the first message to create the stream */
	fstrm = fcgi_strm_new(fconn, 0);
	if (!fstrm) {
		TRACE_ERROR("fstream allocation failure", FCGI_EV_FSTRM_NEW|FCGI_EV_FSTRM_END|FCGI_EV_FSTRM_ERR, fconn->conn);
		goto out;
	}
	if (sc_attach_mux(sc, fstrm, fconn->conn) < 0)
		goto out;
	fstrm->sd = sc->sedesc;
	fstrm->sess = sess;
	fconn->nb_sc++;

	TRACE_LEAVE(FCGI_EV_FSTRM_NEW, fconn->conn, fstrm);
	return fstrm;

  out:
	TRACE_DEVEL("leaving on error", FCGI_EV_FSTRM_NEW|FCGI_EV_FSTRM_END|FCGI_EV_FSTRM_ERR, fconn->conn);
	fcgi_strm_destroy(fstrm);
	return NULL;
}

/* Wakes a specific stream and assign its stream connector some SE_FL_* flags among
 * SE_FL_ERR_PENDING and SE_FL_ERROR if needed. The stream's state is
 * automatically updated accordingly. If the stream is orphaned, it is
 * destroyed.
 */
static void fcgi_strm_wake_one_stream(struct fcgi_strm *fstrm)
{
	struct fcgi_conn *fconn = fstrm->fconn;

	TRACE_ENTER(FCGI_EV_STRM_WAKE, fconn->conn, fstrm);

	if (!fcgi_strm_sc(fstrm)) {
		/* this stream was already orphaned */
		fcgi_strm_destroy(fstrm);
		TRACE_DEVEL("leaving with no fstrm", FCGI_EV_STRM_WAKE, fconn->conn);
		return;
	}

	if (fcgi_conn_read0_pending(fconn)) {
		if (fstrm->state == FCGI_SS_OPEN) {
			fstrm->state = FCGI_SS_HREM;
			TRACE_STATE("switching to HREM", FCGI_EV_STRM_WAKE|FCGI_EV_FSTRM_END, fconn->conn, fstrm);
		}
		else if (fstrm->state == FCGI_SS_HLOC)
			fcgi_strm_close(fstrm);
	}

	if (fconn->state == FCGI_CS_CLOSED || (fconn->flags & (FCGI_CF_ERR_PENDING|FCGI_CF_ERROR))) {
		se_fl_set_error(fstrm->sd);
		fcgi_strm_propagate_term_flags(fconn, fstrm);
		if (fstrm->state < FCGI_SS_ERROR) {
			fstrm->state = FCGI_SS_ERROR;
			TRACE_STATE("switching to ERROR", FCGI_EV_STRM_WAKE|FCGI_EV_FSTRM_END, fconn->conn, fstrm);
		}
	}

	fcgi_strm_alert(fstrm);

	TRACE_LEAVE(FCGI_EV_STRM_WAKE, fconn->conn, fstrm);
}

/* Wakes unassigned streams (ID == 0) attached to the connection. */
static void fcgi_wake_unassigned_streams(struct fcgi_conn *fconn)
{
	struct eb32_node *node;
	struct fcgi_strm *fstrm;

	node = eb32_lookup(&fconn->streams_by_id, 0);
	while (node) {
		fstrm = container_of(node, struct fcgi_strm, by_id);
		if (fstrm->id > 0)
			break;
		node = eb32_next(node);
		fcgi_strm_wake_one_stream(fstrm);
	}
}

/* Wakes the streams attached to the connection, whose id is greater than <last>
 * or unassigned.
 */
static void fcgi_wake_some_streams(struct fcgi_conn *fconn, int last)
{
	struct eb32_node *node;
	struct fcgi_strm *fstrm;

	TRACE_ENTER(FCGI_EV_STRM_WAKE, fconn->conn);

	/* Wake all streams with ID > last */
	node = eb32_lookup_ge(&fconn->streams_by_id, last + 1);
	while (node) {
		fstrm = container_of(node, struct fcgi_strm, by_id);
		node = eb32_next(node);
		fcgi_strm_wake_one_stream(fstrm);
	}
	fcgi_wake_unassigned_streams(fconn);

	TRACE_LEAVE(FCGI_EV_STRM_WAKE, fconn->conn);
}

static int fcgi_set_default_param(struct fcgi_conn *fconn, struct fcgi_strm *fstrm,
				  struct htx *htx, struct htx_sl *sl,
				  struct fcgi_strm_params *params)
{
	struct connection *cli_conn = objt_conn(fstrm->sess->origin);
	const struct sockaddr_storage *src = (sc_check(fcgi_strm_sc(fstrm)) ? conn_src(fconn->conn) : sc_src(sc_opposite(fcgi_strm_sc(fstrm))));
	const struct sockaddr_storage *dst = (sc_check(fcgi_strm_sc(fstrm)) ? conn_dst(fconn->conn) : sc_dst(sc_opposite(fcgi_strm_sc(fstrm))));
	struct ist p;

	if (!sl)
		goto error;

	if (!(params->mask & FCGI_SP_DOC_ROOT))
		params->docroot = fconn->app->docroot;

	if (!(params->mask & FCGI_SP_REQ_METH)) {
		p  = htx_sl_req_meth(sl);
		params->meth = ist2(b_tail(params->p), p.len);
		chunk_istcat(params->p, p);
	}
	if (!(params->mask & FCGI_SP_REQ_URI)) {
		p = h1_get_uri(sl);
		params->uri = ist2(b_tail(params->p), p.len);
		chunk_istcat(params->p, p);
	}
	if (!(params->mask & FCGI_SP_SRV_PROTO)) {
		p  = htx_sl_req_vsn(sl);
		params->vsn = ist2(b_tail(params->p), p.len);
		chunk_istcat(params->p, p);
	}
	if (!(params->mask & FCGI_SP_SRV_PORT)) {
		char *end;
		int port = 0;
		if (dst)
			port = get_host_port(dst);
		end = ultoa_o(port, b_tail(params->p), b_room(params->p));
		if (!end)
			goto error;
		params->srv_port = ist2(b_tail(params->p), end - b_tail(params->p));
		params->p->data += params->srv_port.len;
	}
	if (!(params->mask & FCGI_SP_SRV_NAME)) {
		/* If no Host header found, use the server address to fill
		 * srv_name */
		if (!istlen(params->srv_name)) {
			char *ptr = NULL;

			if (dst)
				if (addr_to_str(dst, b_tail(params->p), b_room(params->p)) != -1)
					ptr = b_tail(params->p);
			if (ptr) {
				params->srv_name = ist(ptr);
				params->p->data += params->srv_name.len;
			}
		}
	}
	if (!(params->mask & FCGI_SP_REM_ADDR)) {
		char *ptr = NULL;

		if (src)
			if (addr_to_str(src, b_tail(params->p), b_room(params->p)) != -1)
				ptr = b_tail(params->p);
		if (ptr) {
			params->rem_addr = ist(ptr);
			params->p->data += params->rem_addr.len;
		}
	}
	if (!(params->mask & FCGI_SP_REM_PORT)) {
		char *end;
		int port = 0;
		if (src)
			port = get_host_port(src);
		end = ultoa_o(port, b_tail(params->p), b_room(params->p));
		if (!end)
			goto error;
		params->rem_port = ist2(b_tail(params->p), end - b_tail(params->p));
		params->p->data += params->rem_port.len;
	}
	if (!(params->mask & FCGI_SP_CONT_LEN)) {
		struct htx_blk *blk;
		enum htx_blk_type type;
		char *end;
		size_t len = 0;

		for (blk = htx_get_head_blk(htx); blk; blk = htx_get_next_blk(htx, blk)) {
			type = htx_get_blk_type(blk);

			if (type == HTX_BLK_TLR || type == HTX_BLK_EOT)
				break;
			if (type == HTX_BLK_DATA)
				len += htx_get_blksz(blk);
		}
		end = ultoa_o(len, b_tail(params->p), b_room(params->p));
		if (!end)
			goto error;
		params->cont_len = ist2(b_tail(params->p), end - b_tail(params->p));
		params->p->data += params->cont_len.len;
	}

	if (!(params->mask & FCGI_SP_HTTPS)) {
		if (cli_conn)
			params->https = conn_is_ssl(cli_conn);
	}

	if ((params->mask & FCGI_SP_URI_MASK) != FCGI_SP_URI_MASK) {
		/* one of scriptname, pathinfo or query_string is no set */
		struct http_uri_parser parser = http_uri_parser_init(params->uri);
		struct ist path = http_parse_path(&parser);
		int len;

		/* No scrit_name set but no valid path ==> error */
		if (!(params->mask & FCGI_SP_SCRIPT_NAME) && !istlen(path))
			goto error;

		/* If there is a query-string, Set it if not already set */
		if (!(params->mask & FCGI_SP_REQ_QS)) {
			struct ist qs = istfind(path, '?');

			/* Update the path length */
			path.len -= qs.len;

			/* Set the query-string skipping the '?', if any */
			if (istlen(qs))
				params->qs = istnext(qs);
		}

		/* If the script_name is set, don't try to deduce the path_info
		 * too. The opposite is not true.
		 */
		if (params->mask & FCGI_SP_SCRIPT_NAME) {
			params->mask |= FCGI_SP_PATH_INFO;
			goto end;
		}

		/* Decode the path. it must first be copied to keep the URI
		 * untouched.
		 */
		chunk_istcat(params->p, path);
		path.ptr = b_tail(params->p) - path.len;
		len = url_decode(ist0(path), 0);
		if (len < 0)
			goto error;
		path.len = len;

		/* script_name not set, preset it with the path for now */
		params->scriptname = path;

		/* If there is no regex to match the pathinfo, just to the last
		 * part and see if the index must be used.
		 */
		if (!fconn->app->pathinfo_re)
			goto check_index;

		/* If some special characters are found in the decoded path (\n
		 * or \0), the PATH_INFO regex cannot match. This is theoretically
		 * valid, but probably unexpected, to have such characters. So,
		 * to avoid any surprises, an error is triggered in this
		 * case.
		 */
		if (istchr(path, '\n') || istchr(path, '\0'))
			goto error;

		/* The regex does not match, just to the last part and see if
		 * the index must be used.
		 */
		if (!regex_exec_match2(fconn->app->pathinfo_re, path.ptr, len, MAX_MATCH, pmatch, 0))
			goto check_index;

		/* We must have at least 1 capture for the script name,
		 * otherwise we do nothing and jump to the last part.
		 */
		if (pmatch[1].rm_so == -1 || pmatch[1].rm_eo == -1)
			goto check_index;

		/* Finally we can set the script_name and the path_info. The
		 * path_info is set if not already defined, and if it was
		 * captured
		 */
		params->scriptname = ist2(path.ptr + pmatch[1].rm_so, pmatch[1].rm_eo - pmatch[1].rm_so);
		if (!(params->mask & FCGI_SP_PATH_INFO) && !(pmatch[2].rm_so == -1 || pmatch[2].rm_eo == -1))
			params->pathinfo = ist2(path.ptr + pmatch[2].rm_so, pmatch[2].rm_eo - pmatch[2].rm_so);

	  check_index:
		len = params->scriptname.len;
		/* the script_name if finished by a '/' so we can add the index
		 * part, if any.
		 */
		if (istlen(fconn->app->index) && params->scriptname.ptr[len-1] == '/') {
			struct ist sn = params->scriptname;

			params->scriptname = ist2(b_tail(params->p), len+fconn->app->index.len);
			chunk_istcat(params->p, sn);
			chunk_istcat(params->p, fconn->app->index);
		}
	}

	if (!(params->mask & FCGI_SP_SRV_SOFT)) {
		params->srv_soft = ist2(b_tail(params->p), 0);
		chunk_appendf(params->p, "HAProxy %s", haproxy_version);
		params->srv_soft.len = b_tail(params->p) - params->srv_soft.ptr;
	}

  end:
	return 1;
  error:
	return 0;
}

static int fcgi_encode_default_param(struct fcgi_conn *fconn, struct fcgi_strm *fstrm,
				     struct fcgi_strm_params *params, struct buffer *outbuf, int flag)
{
	struct fcgi_param p;

	if (params->mask & flag)
		return 1;

	chunk_reset(&trash);

	switch (flag) {
		case FCGI_SP_CGI_GATEWAY:
			p.n = ist("GATEWAY_INTERFACE");
			p.v = ist("CGI/1.1");
			goto encode;
		case FCGI_SP_DOC_ROOT:
			p.n = ist("DOCUMENT_ROOT");
			p.v = params->docroot;
			goto encode;
		case FCGI_SP_SCRIPT_NAME:
			p.n = ist("SCRIPT_NAME");
			p.v = params->scriptname;
			goto encode;
		case FCGI_SP_PATH_INFO:
			p.n = ist("PATH_INFO");
			p.v = params->pathinfo;
			goto encode;
		case FCGI_SP_REQ_URI:
			p.n = ist("REQUEST_URI");
			p.v = params->uri;
			goto encode;
		case FCGI_SP_REQ_METH:
			p.n = ist("REQUEST_METHOD");
			p.v = params->meth;
			goto encode;
		case FCGI_SP_REQ_QS:
			p.n = ist("QUERY_STRING");
			p.v = params->qs;
			goto encode;
		case FCGI_SP_SRV_NAME:
			p.n = ist("SERVER_NAME");
			p.v = params->srv_name;
			goto encode;
		case FCGI_SP_SRV_PORT:
			p.n = ist("SERVER_PORT");
			p.v = params->srv_port;
			goto encode;
		case FCGI_SP_SRV_PROTO:
			p.n = ist("SERVER_PROTOCOL");
			p.v = params->vsn;
			goto encode;
		case FCGI_SP_REM_ADDR:
			p.n = ist("REMOTE_ADDR");
			p.v = params->rem_addr;
			goto encode;
		case FCGI_SP_REM_PORT:
			p.n = ist("REMOTE_PORT");
			p.v = params->rem_port;
			goto encode;
		case FCGI_SP_SCRIPT_FILE:
			p.n = ist("SCRIPT_FILENAME");
			chunk_istcat(&trash, params->docroot);
			chunk_istcat(&trash, params->scriptname);
			p.v = ist2(b_head(&trash), b_data(&trash));
			goto encode;
		case FCGI_SP_PATH_TRANS:
			if (!istlen(params->pathinfo))
				goto skip;
			p.n = ist("PATH_TRANSLATED");
			chunk_istcat(&trash, params->docroot);
			chunk_istcat(&trash, params->pathinfo);
			p.v = ist2(b_head(&trash), b_data(&trash));
			goto encode;
		case FCGI_SP_CONT_LEN:
			p.n = ist("CONTENT_LENGTH");
			p.v = params->cont_len;
			goto encode;
		case FCGI_SP_HTTPS:
			if (!params->https)
				goto skip;
			p.n = ist("HTTPS");
			p.v = ist("on");
			goto encode;
		case FCGI_SP_SRV_SOFT:
			p.n = ist("SERVER_SOFTWARE");
			p.v = params->srv_soft;
			goto encode;
		default:
			goto skip;
	}

  encode:
	if (!istlen(p.v))
		goto skip;
	if (!fcgi_encode_param(outbuf, &p))
		return 0;
  skip:
	params->mask |= flag;
	return 1;
}

/* Sends a GET_VALUES record. Returns > 0 on success, 0 if it couldn't do
 * anything. It is highly unexpected, but if the record is larger than a buffer
 * and cannot be encoded in one time, an error is triggered and the connection is
 * closed. GET_VALUES record cannot be split.
 */
static int fcgi_conn_send_get_values(struct fcgi_conn *fconn)
{
	struct buffer outbuf;
	struct buffer *mbuf;
	struct fcgi_param max_reqs   = { .n = ist("FCGI_MAX_REQS"),   .v = ist("")};
	struct fcgi_param mpxs_conns = { .n = ist("FCGI_MPXS_CONNS"), .v = ist("")};
	int ret = 0;

	TRACE_ENTER(FCGI_EV_TX_RECORD|FCGI_EV_TX_GETVAL, fconn->conn);

	mbuf = br_tail(fconn->mbuf);
  retry:
	if (!fcgi_get_buf(fconn, mbuf)) {
		fconn->flags |= FCGI_CF_MUX_MALLOC;
		fconn->flags |= FCGI_CF_DEM_MROOM;
		TRACE_STATE("waiting for fconn mbuf ring allocation", FCGI_EV_TX_RECORD|FCGI_EV_FCONN_BLK, fconn->conn);
		ret = 0;
		goto end;
	}

	while (1) {
		outbuf = b_make(b_tail(mbuf), b_contig_space(mbuf), 0, 0);
		if (outbuf.size >= FCGI_RECORD_HEADER_SZ || !b_space_wraps(mbuf))
			break;
	  realign_again:
		b_slow_realign(mbuf, trash.area, b_data(mbuf));
	}

	if (outbuf.size < FCGI_RECORD_HEADER_SZ)
		goto full;

	/* vsn: 1(FCGI_VERSION), type: (9)FCGI_GET_VALUES, id: 0x0000,
	 *  len: 0x0000 (fill later), padding: 0x00, rsv: 0x00 */
	memcpy(outbuf.area, "\x01\x09\x00\x00\x00\x00\x00\x00", FCGI_RECORD_HEADER_SZ);
	outbuf.data = FCGI_RECORD_HEADER_SZ;

	/* Note: Don't send the param FCGI_MAX_CONNS because its value cannot be
	 *       handled by HAProxy.
	 */
	if (!fcgi_encode_param(&outbuf, &max_reqs) || !fcgi_encode_param(&outbuf, &mpxs_conns))
		goto full;

	/* update the record's size now */
	TRACE_PROTO("FCGI GET_VALUES record xferred", FCGI_EV_TX_RECORD|FCGI_EV_TX_GETVAL, fconn->conn, 0, 0, (size_t[]){outbuf.data-8});
	fcgi_set_record_size(outbuf.area, outbuf.data - FCGI_RECORD_HEADER_SZ);
	b_add(mbuf, outbuf.data);
	ret = 1;

  end:
	TRACE_LEAVE(FCGI_EV_TX_RECORD|FCGI_EV_TX_GETVAL, fconn->conn);
	return ret;
  full:
	/* Too large to be encoded. For GET_VALUES records, it is an error */
	if (!b_data(mbuf)) {
		TRACE_ERROR("GET_VALUES record too large", FCGI_EV_TX_RECORD|FCGI_EV_TX_GETVAL|FCGI_EV_FCONN_ERR, fconn->conn);
		goto fail;
	}

	if ((mbuf = br_tail_add(fconn->mbuf)) != NULL)
		goto retry;
	fconn->flags |= FCGI_CF_MUX_MFULL;
	fconn->flags |= FCGI_CF_DEM_MROOM;
	TRACE_STATE("mbuf ring full", FCGI_EV_TX_RECORD|FCGI_EV_FCONN_BLK, fconn->conn);
	ret = 0;
	goto end;
  fail:
	fconn->state = FCGI_CS_CLOSED;
	TRACE_STATE("switching to CLOSED", FCGI_EV_TX_RECORD|FCGI_EV_TX_GETVAL|FCGI_EV_FCONN_END, fconn->conn);
	TRACE_DEVEL("leaving on error", FCGI_EV_TX_RECORD|FCGI_EV_TX_GETVAL|FCGI_EV_FCONN_ERR, fconn->conn);
	return 0;
}

/* Processes a GET_VALUES_RESULT record. Returns > 0 on success, 0 if it
 * couldn't do anything. It is highly unexpected, but if the record is larger
 * than a buffer and cannot be decoded in one time, an error is triggered and
 * the connection is closed. GET_VALUES_RESULT record cannot be split.
 */
static int fcgi_conn_handle_values_result(struct fcgi_conn *fconn)
{
	struct buffer inbuf;
	struct buffer *dbuf;
	size_t offset;

	TRACE_ENTER(FCGI_EV_RX_RECORD|FCGI_EV_RX_GETVAL, fconn->conn);

	dbuf = &fconn->dbuf;

	/* Record too large to be fully decoded */
	if (b_size(dbuf) < (fconn->drl + fconn->drp))
		goto fail;

	/* process full record only */
	if (b_data(dbuf) < (fconn->drl + fconn->drp)) {
		fconn->flags |= FCGI_CF_DEM_SHORT_READ;
		TRACE_DEVEL("leaving on missing data", FCGI_EV_RX_RECORD|FCGI_EV_RX_GETVAL, fconn->conn);
		return 0;
	}

	if (unlikely(b_contig_data(dbuf, b_head_ofs(dbuf)) < fconn->drl)) {
		/* Realign the dmux buffer if the record wraps. It is unexpected
		 * at this stage because it should be the first record received
		 * from the FCGI application.
		 */
		b_slow_realign_ofs(dbuf, trash.area, 0);
	}

	inbuf = b_make(b_head(dbuf), b_data(dbuf), 0, fconn->drl);

	for (offset = 0; offset < b_data(&inbuf); ) {
		struct fcgi_param p;
		size_t ret;

		ret = fcgi_aligned_decode_param(&inbuf, offset, &p);
		if (!ret) {
			/* name or value too large to be decoded at once */
			TRACE_ERROR("error decoding GET_VALUES_RESULT param", FCGI_EV_RX_RECORD|FCGI_EV_RX_GETVAL|FCGI_EV_FCONN_ERR, fconn->conn);
			goto fail;
		}
		offset += ret;

		if (isteqi(p.n, ist("FCGI_MPXS_CONNS"))) {
			if (isteq(p.v, ist("1"))) {
				TRACE_STATE("set mpxs param", FCGI_EV_RX_RECORD|FCGI_EV_RX_GETVAL, fconn->conn, 0, 0, (size_t[]){1});
				fconn->flags |= FCGI_CF_MPXS_CONNS;
			}
			else {
				TRACE_STATE("set mpxs param", FCGI_EV_RX_RECORD|FCGI_EV_RX_GETVAL, fconn->conn, 0, 0, (size_t[]){0});
				fconn->flags &= ~FCGI_CF_MPXS_CONNS;
			}
		}
		else if (isteqi(p.n, ist("FCGI_MAX_REQS"))) {
			fconn->streams_limit = strl2ui(p.v.ptr, p.v.len);
			TRACE_STATE("set streams_limit", FCGI_EV_RX_RECORD|FCGI_EV_RX_GETVAL, fconn->conn, 0, 0, (size_t[]){fconn->streams_limit});
		}
		/*
		 * Ignore all other params
		 */
	}

	/* Reset the number of concurrent streams supported if the FCGI
	 * application does not support connection multiplexing
	 */
	if (!(fconn->flags & FCGI_CF_MPXS_CONNS)) {
		fconn->streams_limit = 1;
		TRACE_STATE("no mpxs for streams_limit to 1", FCGI_EV_RX_RECORD|FCGI_EV_RX_GETVAL, fconn->conn);
	}

	/* We must be sure to have read exactly the announced record length, no
	 * more no less
	 */
	if (offset != fconn->drl) {
		TRACE_ERROR("invalid GET_VALUES_RESULT record length", FCGI_EV_RX_RECORD|FCGI_EV_RX_GETVAL|FCGI_EV_FCONN_ERR, fconn->conn);
		goto fail;
	}

	TRACE_PROTO("FCGI GET_VALUES_RESULT record rcvd", FCGI_EV_RX_RECORD|FCGI_EV_RX_GETVAL, fconn->conn, 0, 0, (size_t[]){fconn->drl});
	b_del(&fconn->dbuf, fconn->drl + fconn->drp);
	fconn->drl = 0;
	fconn->drp = 0;
	fconn->state = FCGI_CS_RECORD_H;
	fcgi_wake_unassigned_streams(fconn);
	TRACE_STATE("switching to RECORD_H", FCGI_EV_RX_RECORD|FCGI_EV_RX_FHDR, fconn->conn);
	TRACE_LEAVE(FCGI_EV_RX_RECORD|FCGI_EV_RX_GETVAL, fconn->conn);
	return 1;
  fail:
	fconn->state = FCGI_CS_CLOSED;
	TRACE_STATE("switching to CLOSED", FCGI_EV_RX_RECORD|FCGI_EV_RX_GETVAL, fconn->conn);
	TRACE_DEVEL("leaving on error", FCGI_EV_RX_RECORD|FCGI_EV_RX_GETVAL|FCGI_EV_FCONN_ERR, fconn->conn);
	return 0;
}

/* Sends an ABORT_REQUEST record for each active streams. Closed streams are
 * excluded, as the streams which already received the end-of-stream. It returns
 * > 0 if the record was sent tp all streams. Otherwise it returns 0.
 */
static int fcgi_conn_send_aborts(struct fcgi_conn *fconn)
{
	struct eb32_node *node;
	struct fcgi_strm *fstrm;

	TRACE_ENTER(FCGI_EV_TX_RECORD, fconn->conn);

	node = eb32_lookup_ge(&fconn->streams_by_id, 1);
	while (node) {
		fstrm = container_of(node, struct fcgi_strm, by_id);
		node = eb32_next(node);
		if (fstrm->state != FCGI_SS_CLOSED &&
		    !(fstrm->flags & (FCGI_SF_ES_RCVD|FCGI_SF_ABRT_SENT)) &&
		    !fcgi_strm_send_abort(fconn, fstrm))
			return 0;
	}
	fconn->flags |= FCGI_CF_ABRTS_SENT;
	fcgi_conn_report_term_evt(fconn, muxc_tevt_type_graceful_shut);
	TRACE_STATE("aborts sent to all fstrms", FCGI_EV_TX_RECORD, fconn->conn);
	TRACE_LEAVE(FCGI_EV_TX_RECORD, fconn->conn);
	return 1;
}

/* Sends a BEGIN_REQUEST record. It returns > 0 on success, 0 if it couldn't do
 * anything. BEGIN_REQUEST record cannot be split. So we wait to have enough
 * space to proceed. It is small enough to be encoded in an empty buffer.
 */
static int fcgi_strm_send_begin_request(struct fcgi_conn *fconn, struct fcgi_strm *fstrm)
{
	struct buffer outbuf;
	struct buffer *mbuf;
	struct fcgi_begin_request rec = { .role = FCGI_RESPONDER, .flags = 0};
	int ret;

	TRACE_ENTER(FCGI_EV_TX_RECORD|FCGI_EV_TX_BEGREQ, fconn->conn, fstrm);

	mbuf = br_tail(fconn->mbuf);
  retry:
	if (!fcgi_get_buf(fconn, mbuf)) {
		fconn->flags |= FCGI_CF_MUX_MALLOC;
		fstrm->flags |= FCGI_SF_BLK_MROOM;
		TRACE_STATE("waiting for fconn mbuf ring allocation", FCGI_EV_TX_RECORD|FCGI_EV_FSTRM_BLK|FCGI_EV_FCONN_BLK, fconn->conn, fstrm);
		ret = 0;
		goto end;
	}

	while (1) {
		outbuf = b_make(b_tail(mbuf), b_contig_space(mbuf), 0, 0);
		if (outbuf.size >= FCGI_RECORD_HEADER_SZ || !b_space_wraps(mbuf))
			break;
	  realign_again:
		b_slow_realign(mbuf, trash.area, b_data(mbuf));
	}

	if (outbuf.size < FCGI_RECORD_HEADER_SZ)
		goto full;

	/* vsn: 1(FCGI_VERSION), type: (1)FCGI_BEGIN_REQUEST, id: fstrm->id,
	 *  len: 0x0008, padding: 0x00, rsv: 0x00 */
	memcpy(outbuf.area, "\x01\x01\x00\x00\x00\x08\x00\x00", FCGI_RECORD_HEADER_SZ);
	fcgi_set_record_id(outbuf.area, fstrm->id);
	outbuf.data = FCGI_RECORD_HEADER_SZ;

	if (fconn->flags & FCGI_CF_KEEP_CONN) {
		TRACE_STATE("keep connection opened", FCGI_EV_TX_RECORD|FCGI_EV_TX_BEGREQ, fconn->conn, fstrm);
		rec.flags |= FCGI_KEEP_CONN;
	}
	if (!fcgi_encode_begin_request(&outbuf, &rec))
		goto full;

	/* commit the record */
	TRACE_PROTO("FCGI BEGIN_REQUEST record xferred", FCGI_EV_TX_RECORD|FCGI_EV_TX_BEGREQ, fconn->conn, fstrm, 0, (size_t[]){0});
	b_add(mbuf, outbuf.data);
	fstrm->flags |= FCGI_SF_BEGIN_SENT;
	fstrm->state = FCGI_SS_OPEN;
	TRACE_STATE("switching to OPEN", FCGI_EV_TX_RECORD|FCGI_EV_TX_BEGREQ, fconn->conn, fstrm);
	ret = 1;

  end:
	TRACE_LEAVE(FCGI_EV_TX_RECORD|FCGI_EV_TX_BEGREQ, fconn->conn, fstrm);
	return ret;
  full:
	if ((mbuf = br_tail_add(fconn->mbuf)) != NULL)
		goto retry;
	fconn->flags |= FCGI_CF_MUX_MFULL;
	fstrm->flags |= FCGI_SF_BLK_MROOM;
	TRACE_STATE("mbuf ring full", FCGI_EV_TX_RECORD|FCGI_EV_FSTRM_BLK|FCGI_EV_FCONN_BLK, fconn->conn);
	ret = 0;
	goto end;
}

/* Sends an empty record of type <rtype>. It returns > 0 on success, 0 if it
 * couldn't do anything. Empty record cannot be split. So we wait to have enough
 * space to proceed. It is small enough to be encoded in an empty buffer.
 */
static int fcgi_strm_send_empty_record(struct fcgi_conn *fconn, struct fcgi_strm *fstrm,
				       enum fcgi_record_type rtype)
{
	struct buffer outbuf;
	struct buffer *mbuf;
	int ret;

	TRACE_ENTER(FCGI_EV_TX_RECORD, fconn->conn, fstrm);
	mbuf = br_tail(fconn->mbuf);
  retry:
	if (!fcgi_get_buf(fconn, mbuf)) {
		fconn->flags |= FCGI_CF_MUX_MALLOC;
		fstrm->flags |= FCGI_SF_BLK_MROOM;
		TRACE_STATE("waiting for fconn mbuf ring allocation", FCGI_EV_TX_RECORD|FCGI_EV_FSTRM_BLK|FCGI_EV_FCONN_BLK, fconn->conn, fstrm);
		ret = 0;
		goto end;
	}

	while (1) {
		outbuf = b_make(b_tail(mbuf), b_contig_space(mbuf), 0, 0);
		if (outbuf.size >= FCGI_RECORD_HEADER_SZ || !b_space_wraps(mbuf))
			break;
	  realign_again:
		b_slow_realign(mbuf, trash.area, b_data(mbuf));
	}

	if (outbuf.size < FCGI_RECORD_HEADER_SZ)
		goto full;

	/* vsn: 1(FCGI_VERSION), type: rtype, id: fstrm->id,
	 *  len: 0x0000, padding: 0x00, rsv: 0x00 */
	memcpy(outbuf.area, "\x01\x05\x00\x00\x00\x00\x00\x00", FCGI_RECORD_HEADER_SZ);
	outbuf.area[1] = rtype;
	fcgi_set_record_id(outbuf.area, fstrm->id);
	outbuf.data = FCGI_RECORD_HEADER_SZ;

	/* commit the record */
	b_add(mbuf, outbuf.data);
	ret = 1;

  end:
	TRACE_LEAVE(FCGI_EV_TX_RECORD, fconn->conn, fstrm);
	return ret;
  full:
	if ((mbuf = br_tail_add(fconn->mbuf)) != NULL)
		goto retry;
	fconn->flags |= FCGI_CF_MUX_MFULL;
	fstrm->flags |= FCGI_SF_BLK_MROOM;
	TRACE_STATE("mbuf ring full", FCGI_EV_TX_RECORD|FCGI_EV_FSTRM_BLK|FCGI_EV_FCONN_BLK, fconn->conn, fstrm);
	ret = 0;
	goto end;
}


/* Sends an empty PARAMS record. It relies on fcgi_strm_send_empty_record(). It
 * marks the end of params.
 */
static int fcgi_strm_send_empty_params(struct fcgi_conn *fconn, struct fcgi_strm *fstrm)
{
	int ret;

	TRACE_POINT(FCGI_EV_TX_RECORD|FCGI_EV_TX_PARAMS, fconn->conn, fstrm);
	ret = fcgi_strm_send_empty_record(fconn, fstrm, FCGI_PARAMS);
	if (ret) {
		fstrm->flags |= FCGI_SF_EP_SENT;
		TRACE_PROTO("FCGI PARAMS record xferred", FCGI_EV_TX_RECORD|FCGI_EV_TX_STDIN, fconn->conn, fstrm, 0, (size_t[]){0});
	}
	return ret;
}

/* Sends an empty STDIN record. It relies on fcgi_strm_send_empty_record(). It
 * marks the end of input. On success, all the request was successfully sent.
 */
static int fcgi_strm_send_empty_stdin(struct fcgi_conn *fconn, struct fcgi_strm *fstrm)
{
	int ret;

	TRACE_POINT(FCGI_EV_TX_RECORD|FCGI_EV_TX_STDIN|FCGI_EV_TX_EOI, fconn->conn, fstrm);
	ret = fcgi_strm_send_empty_record(fconn, fstrm, FCGI_STDIN);
	if (ret) {
		fstrm->flags |= FCGI_SF_ES_SENT;
		TRACE_PROTO("FCGI STDIN record xferred", FCGI_EV_TX_RECORD|FCGI_EV_TX_STDIN, fconn->conn, fstrm, 0, (size_t[]){0});
		TRACE_USER("FCGI request fully xferred", FCGI_EV_TX_RECORD|FCGI_EV_TX_STDIN|FCGI_EV_TX_EOI, fconn->conn, fstrm);
		TRACE_STATE("stdin data fully sent", FCGI_EV_TX_RECORD|FCGI_EV_TX_STDIN|FCGI_EV_TX_EOI, fconn->conn, fstrm);
	}
	return ret;
}

/* Sends an ABORT_REQUEST record. It relies on fcgi_strm_send_empty_record(). It
 * stops the request processing.
 */
static int fcgi_strm_send_abort(struct fcgi_conn *fconn, struct fcgi_strm *fstrm)
{
	int ret;

	TRACE_POINT(FCGI_EV_TX_RECORD|FCGI_EV_TX_ABORT, fconn->conn, fstrm);
	ret = fcgi_strm_send_empty_record(fconn, fstrm, FCGI_ABORT_REQUEST);
	if (ret) {
		fstrm->flags |= FCGI_SF_ABRT_SENT;
		se_report_term_evt(fstrm->sd, se_tevt_type_cancelled);
		TRACE_PROTO("FCGI ABORT record xferred", FCGI_EV_TX_RECORD|FCGI_EV_TX_ABORT, fconn->conn, fstrm, 0, (size_t[]){0});
		TRACE_USER("FCGI request aborted", FCGI_EV_TX_RECORD|FCGI_EV_TX_ABORT, fconn->conn, fstrm);
		TRACE_STATE("abort sent", FCGI_EV_TX_RECORD|FCGI_EV_TX_ABORT, fconn->conn, fstrm);
	}
	return ret;
}

/* Sends a PARAMS record. Returns > 0 on success, 0 if it couldn't do
 * anything. If there are too much K/V params to be encoded in a PARAMS record,
 * several records are sent. However, a K/V param cannot be split between 2
 * records.
 */
static size_t fcgi_strm_send_params(struct fcgi_conn *fconn, struct fcgi_strm *fstrm,
				    struct htx *htx)
{
	struct buffer outbuf;
	struct buffer *mbuf;
	struct htx_blk *blk;
	struct htx_sl *sl = NULL;
	struct fcgi_strm_params params;
	size_t total = 0;

	TRACE_ENTER(FCGI_EV_TX_RECORD|FCGI_EV_TX_PARAMS, fconn->conn, fstrm, htx);

	memset(&params, 0, sizeof(params));
	params.p = get_trash_chunk();

	mbuf = br_tail(fconn->mbuf);
  retry:
	if (!fcgi_get_buf(fconn, mbuf)) {
		fconn->flags |= FCGI_CF_MUX_MALLOC;
		fstrm->flags |= FCGI_SF_BLK_MROOM;
		TRACE_STATE("waiting for fconn mbuf ring allocation", FCGI_EV_TX_RECORD|FCGI_EV_FSTRM_BLK|FCGI_EV_FCONN_BLK, fconn->conn, fstrm);
		goto end;
	}

	while (1) {
		outbuf = b_make(b_tail(mbuf), b_contig_space(mbuf), 0, 0);
		if (outbuf.size >= FCGI_RECORD_HEADER_SZ || !b_space_wraps(mbuf))
			break;
	  realign_again:
		b_slow_realign(mbuf, trash.area, b_data(mbuf));
	}

	if (outbuf.size < FCGI_RECORD_HEADER_SZ)
		goto full;

	/* vsn: 1(FCGI_VERSION), type: (4)FCGI_PARAMS, id: fstrm->id,
	 *  len: 0x0000 (fill later), padding: 0x00, rsv: 0x00 */
	memcpy(outbuf.area, "\x01\x04\x00\x00\x00\x00\x00\x00", FCGI_RECORD_HEADER_SZ);
	fcgi_set_record_id(outbuf.area, fstrm->id);
	outbuf.data = FCGI_RECORD_HEADER_SZ;

	blk = htx_get_head_blk(htx);
	while (blk) {
		enum htx_blk_type type;
		uint32_t size = htx_get_blksz(blk);
		struct fcgi_param p;

		type = htx_get_blk_type(blk);
		switch (type) {
			case HTX_BLK_REQ_SL:
				sl = htx_get_blk_ptr(htx, blk);
				if (sl->info.req.meth == HTTP_METH_HEAD)
					fstrm->h1m.flags |= H1_MF_METH_HEAD;
				if (sl->flags & HTX_SL_F_VER_11)
					fstrm->h1m.flags |= H1_MF_VER_11;
				break;

			case HTX_BLK_HDR:
				p.n = htx_get_blk_name(htx, blk);
				p.v = htx_get_blk_value(htx, blk);

				if (istmatch(p.n, ist(":fcgi-"))) {
					p.n = istadv(p.n, 6);
					if (isteq(p.n, ist("gateway_interface")))
						params.mask |= FCGI_SP_CGI_GATEWAY;
					else if (isteq(p.n, ist("document_root"))) {
						params.mask |= FCGI_SP_DOC_ROOT;
						params.docroot = p.v;
					}
					else if (isteq(p.n, ist("script_name"))) {
						params.mask |= FCGI_SP_SCRIPT_NAME;
						params.scriptname = p.v;
					}
					else if (isteq(p.n, ist("path_info"))) {
						params.mask |= FCGI_SP_PATH_INFO;
						params.pathinfo = p.v;
					}
					else if (isteq(p.n, ist("request_uri"))) {
						params.mask |= FCGI_SP_REQ_URI;
						params.uri = p.v;
					}
					else if (isteq(p.n, ist("request_meth")))
						params.mask |= FCGI_SP_REQ_METH;
					else if (isteq(p.n, ist("query_string")))
						params.mask |= FCGI_SP_REQ_QS;
					else if (isteq(p.n, ist("server_name")))
						params.mask |= FCGI_SP_SRV_NAME;
					else if (isteq(p.n, ist("server_port")))
						params.mask |= FCGI_SP_SRV_PORT;
					else if (isteq(p.n, ist("server_protocol")))
						params.mask |= FCGI_SP_SRV_PROTO;
					else if (isteq(p.n, ist("remote_addr")))
						params.mask |= FCGI_SP_REM_ADDR;
					else if (isteq(p.n, ist("remote_port")))
						params.mask |= FCGI_SP_REM_PORT;
					else if (isteq(p.n, ist("script_filename")))
						params.mask |= FCGI_SP_SCRIPT_FILE;
					else if (isteq(p.n, ist("path_translated")))
						params.mask |= FCGI_SP_PATH_TRANS;
					else if (isteq(p.n, ist("https")))
						params.mask |= FCGI_SP_HTTPS;
					else if (isteq(p.n, ist("server_software")))
						params.mask |= FCGI_SP_SRV_SOFT;
				}
				else if (isteq(p.n, ist("content-length"))) {
					p.n = ist("CONTENT_LENGTH");
					params.mask |= FCGI_SP_CONT_LEN;
				}
				else if (isteq(p.n, ist("content-type")))
					p.n = ist("CONTENT_TYPE");
				else {
					struct ist n;

					if (isteq(p.n, ist("host")))
						params.srv_name = p.v;
					else if (isteq(p.n, ist("te"))) {
						/* "te" may only be sent with "trailers" if this value
						 * is present, otherwise it must be deleted.
						 */
						p.v = istist(p.v, ist("trailers"));
						if (!isttest(p.v) || (p.v.len > 8 && p.v.ptr[8] != ','))
							break;
						p.v = ist("trailers");
					}

					/* Skip header if same name is used to add the server name */
					if (isttest(fconn->proxy->server_id_hdr_name) && isteq(p.n, fconn->proxy->server_id_hdr_name))
						break;

					n = ist2(trash.area, 0);
					istcat(&n, ist("http_"), trash.size);
					istcat(&n, p.n, trash.size);
					p.n = n;
				}

				if (!fcgi_encode_param(&outbuf, &p)) {
					if (b_space_wraps(mbuf))
						goto realign_again;
					if (outbuf.data == FCGI_RECORD_HEADER_SZ)
						goto full;
					goto done;
				}
				break;

			case HTX_BLK_EOH:
				if (isttest(fconn->proxy->server_id_hdr_name)) {
					struct server *srv = objt_server(fconn->conn->target);

					if (!srv)
						goto done;

					p.n = ist2(trash.area, 0);
					istcat(&p.n, ist("http_"), trash.size);
					istcat(&p.n, fconn->proxy->server_id_hdr_name, trash.size);
					p.v = ist(srv->id);

					if (!fcgi_encode_param(&outbuf, &p)) {
						if (b_space_wraps(mbuf))
							goto realign_again;
						if (outbuf.data == FCGI_RECORD_HEADER_SZ)
							goto full;
					}
					TRACE_STATE("add server name header", FCGI_EV_TX_RECORD|FCGI_EV_TX_PARAMS, fconn->conn, fstrm);
				}
				goto done;

			default:
				break;
		}
		total += size;
		blk = htx_remove_blk(htx, blk);
	}

  done:
	if (!fcgi_set_default_param(fconn, fstrm, htx, sl, &params)) {
		TRACE_ERROR("error setting default params", FCGI_EV_TX_RECORD|FCGI_EV_STRM_ERR, fconn->conn, fstrm);
		goto error;
	}

	if (!fcgi_encode_default_param(fconn, fstrm, &params, &outbuf, FCGI_SP_CGI_GATEWAY) ||
	    !fcgi_encode_default_param(fconn, fstrm, &params, &outbuf, FCGI_SP_DOC_ROOT)    ||
	    !fcgi_encode_default_param(fconn, fstrm, &params, &outbuf, FCGI_SP_SCRIPT_NAME) ||
	    !fcgi_encode_default_param(fconn, fstrm, &params, &outbuf, FCGI_SP_PATH_INFO)   ||
	    !fcgi_encode_default_param(fconn, fstrm, &params, &outbuf, FCGI_SP_REQ_URI)     ||
	    !fcgi_encode_default_param(fconn, fstrm, &params, &outbuf, FCGI_SP_REQ_METH)    ||
	    !fcgi_encode_default_param(fconn, fstrm, &params, &outbuf, FCGI_SP_REQ_QS)      ||
	    !fcgi_encode_default_param(fconn, fstrm, &params, &outbuf, FCGI_SP_SRV_NAME)    ||
	    !fcgi_encode_default_param(fconn, fstrm, &params, &outbuf, FCGI_SP_SRV_PORT)    ||
	    !fcgi_encode_default_param(fconn, fstrm, &params, &outbuf, FCGI_SP_SRV_PROTO)   ||
	    !fcgi_encode_default_param(fconn, fstrm, &params, &outbuf, FCGI_SP_REM_ADDR)    ||
	    !fcgi_encode_default_param(fconn, fstrm, &params, &outbuf, FCGI_SP_REM_PORT)    ||
	    !fcgi_encode_default_param(fconn, fstrm, &params, &outbuf, FCGI_SP_SCRIPT_FILE) ||
	    !fcgi_encode_default_param(fconn, fstrm, &params, &outbuf, FCGI_SP_PATH_TRANS)  ||
	    !fcgi_encode_default_param(fconn, fstrm, &params, &outbuf, FCGI_SP_CONT_LEN)    ||
	    !fcgi_encode_default_param(fconn, fstrm, &params, &outbuf, FCGI_SP_SRV_SOFT)    ||
	    !fcgi_encode_default_param(fconn, fstrm, &params, &outbuf, FCGI_SP_HTTPS)) {
		TRACE_ERROR("error encoding default params", FCGI_EV_TX_RECORD|FCGI_EV_STRM_ERR, fconn->conn, fstrm);
		goto error;
	}

	/* update the record's size */
	TRACE_PROTO("FCGI PARAMS record xferred", FCGI_EV_TX_RECORD|FCGI_EV_TX_PARAMS, fconn->conn, fstrm, 0, (size_t[]){outbuf.data - FCGI_RECORD_HEADER_SZ});
	fcgi_set_record_size(outbuf.area, outbuf.data - FCGI_RECORD_HEADER_SZ);
	b_add(mbuf, outbuf.data);

  end:
	TRACE_LEAVE(FCGI_EV_TX_RECORD|FCGI_EV_TX_PARAMS, fconn->conn, fstrm, htx, (size_t[]){total});
	return total;
  full:
	if ((mbuf = br_tail_add(fconn->mbuf)) != NULL)
		goto retry;
	fconn->flags |= FCGI_CF_MUX_MFULL;
	fstrm->flags |= FCGI_SF_BLK_MROOM;
	TRACE_STATE("mbuf ring full", FCGI_EV_TX_RECORD|FCGI_EV_FSTRM_BLK|FCGI_EV_FCONN_BLK, fconn->conn, fstrm);
	if (total)
		goto error;
	goto end;

  error:
	htx->flags |= HTX_FL_PROCESSING_ERROR;
	TRACE_ERROR("processing error sending PARAMS record", FCGI_EV_TX_RECORD|FCGI_EV_STRM_ERR, fconn->conn, fstrm);
	fcgi_strm_error(fstrm);
	goto end;
}

/* Sends a STDIN record. Returns > 0 on success, 0 if it couldn't do
 * anything. STDIN records contain the request body.
 */
static size_t fcgi_strm_send_stdin(struct fcgi_conn *fconn, struct fcgi_strm *fstrm,
				   struct htx *htx, size_t count, struct buffer *buf)
{
	struct buffer outbuf;
	struct buffer *mbuf;
	struct htx_blk *blk;
	enum htx_blk_type type;
	uint32_t size, extra_bytes;
	size_t total = 0;

	extra_bytes = 0;

	TRACE_ENTER(FCGI_EV_TX_RECORD|FCGI_EV_TX_STDIN, fconn->conn, fstrm, htx, (size_t[]){count});
	if (!count)
		goto end;

	mbuf = br_tail(fconn->mbuf);
  retry:
	if (!fcgi_get_buf(fconn, mbuf)) {
		fconn->flags |= FCGI_CF_MUX_MALLOC;
		fstrm->flags |= FCGI_SF_BLK_MROOM;
		TRACE_STATE("waiting for fconn mbuf ring allocation", FCGI_EV_TX_RECORD|FCGI_EV_FSTRM_BLK|FCGI_EV_FCONN_BLK, fconn->conn, fstrm);
		goto end;
	}

	/* Perform some optimizations to reduce the number of buffer copies.
	 * First, if the mux's buffer is empty and the htx area contains exactly
	 * one data block of the same size as the requested count, and this
	 * count fits within the record size, then it's possible to simply swap
	 * the caller's buffer with the mux's output buffer and adjust offsets
	 * and length to match the entire DATA HTX block in the middle. In this
	 * case we perform a true zero-copy operation from end-to-end. This is
	 * the situation that happens all the time with large files. Second, if
	 * this is not possible, but the mux's output buffer is empty, we still
	 * have an opportunity to avoid the copy to the intermediary buffer, by
	 * making the intermediary buffer's area point to the output buffer's
	 * area. In this case we want to skip the HTX header to make sure that
	 * copies remain aligned and that this operation remains possible all
	 * the time. This goes for headers, data blocks and any data extracted
	 * from the HTX blocks.
	 */
	blk  = htx_get_head_blk(htx);
	if (!blk)
		goto end;
	type = htx_get_blk_type(blk);
	size = htx_get_blksz(blk);
	if (unlikely(size == count && htx_nbblks(htx) == 1 && type == HTX_BLK_DATA)) {
		void *old_area = mbuf->area;
		int eom = (htx->flags & HTX_FL_EOM);

		 /* Last block of the message: Reserve the size for the empty stdin record */
		if (eom)
			extra_bytes = FCGI_RECORD_HEADER_SZ;

		if (b_data(mbuf)) {
			/* Too bad there are data left there. We're willing to memcpy/memmove
			 * up to 1/4 of the buffer, which means that it's OK to copy a large
			 * record into a buffer containing few data if it needs to be realigned,
			 * and that it's also OK to copy few data without realigning. Otherwise
			 * we'll pretend the mbuf is full and wait for it to become empty.
			 */
			if (size + FCGI_RECORD_HEADER_SZ + extra_bytes <= b_room(mbuf) &&
			    (b_data(mbuf) <= b_size(mbuf) / 4 ||
			     (size <= b_size(mbuf) / 4 && size + FCGI_RECORD_HEADER_SZ + extra_bytes <= b_contig_space(mbuf))))
				goto copy;
			goto full;
		}

		TRACE_PROTO("sending stding data (zero-copy)", FCGI_EV_TX_RECORD|FCGI_EV_TX_STDIN, fconn->conn, fstrm, htx, (size_t[]){size});
		/* map a FCGI record to the HTX block so that we can put the
		 * record header there.
		 */
		*mbuf = b_make(buf->area, buf->size, sizeof(struct htx) + blk->addr - FCGI_RECORD_HEADER_SZ, size + FCGI_RECORD_HEADER_SZ);
		outbuf.area = b_head(mbuf);

		/* prepend a FCGI record header just before the DATA block */
		memcpy(outbuf.area, "\x01\x05\x00\x00\x00\x00\x00\x00", FCGI_RECORD_HEADER_SZ);
		fcgi_set_record_id(outbuf.area, fstrm->id);
		fcgi_set_record_size(outbuf.area, size);

		/* and exchange with our old area */
		buf->area = old_area;
		buf->data = buf->head = 0;
		total += size;

		htx = (struct htx *)buf->area;
		htx_reset(htx);
		if (eom)
			goto empty_stdin;
		goto end;
	}

  copy:
	while (1) {
		outbuf = b_make(b_tail(mbuf), b_contig_space(mbuf), 0, 0);
		if (outbuf.size >= FCGI_RECORD_HEADER_SZ + extra_bytes || !b_space_wraps(mbuf))
			break;
	  realign_again:
		b_slow_realign(mbuf, trash.area, b_data(mbuf));
	}

	if (outbuf.size < FCGI_RECORD_HEADER_SZ + extra_bytes)
		goto full;

	/* vsn: 1(FCGI_VERSION), type: (5)FCGI_STDIN, id: fstrm->id,
	 *  len: 0x0000 (fill later), padding: 0x00, rsv: 0x00 */
	memcpy(outbuf.area, "\x01\x05\x00\x00\x00\x00\x00\x00", FCGI_RECORD_HEADER_SZ);
	fcgi_set_record_id(outbuf.area, fstrm->id);
	outbuf.data = FCGI_RECORD_HEADER_SZ;

	blk = htx_get_head_blk(htx);
	while (blk && count) {
		enum htx_blk_type type = htx_get_blk_type(blk);
		uint32_t size = htx_get_blksz(blk);
		struct ist v;

		switch (type) {
			case HTX_BLK_DATA:
				TRACE_PROTO("sending stding data", FCGI_EV_TX_RECORD|FCGI_EV_TX_STDIN, fconn->conn, fstrm, htx, (size_t[]){size});
				v = htx_get_blk_value(htx, blk);

				if (htx_is_unique_blk(htx, blk) && (htx->flags & HTX_FL_EOM))
					extra_bytes = FCGI_RECORD_HEADER_SZ; /* Last block of the message */

				if (v.len > count) {
					v.len = count;
					extra_bytes = 0;
				}

				if (v.len + FCGI_RECORD_HEADER_SZ + extra_bytes > b_room(&outbuf)) {
					/* It doesn't fit at once. If it at least fits once split and
					 * the amount of data to move is low, let's defragment the
					 * buffer now.
					 */
					if (b_space_wraps(mbuf) &&
					    b_data(&outbuf) + v.len + extra_bytes <= b_room(mbuf) &&
					    b_data(mbuf) <= MAX_DATA_REALIGN)
						goto realign_again;
					v.len = (FCGI_RECORD_HEADER_SZ + extra_bytes > b_room(&outbuf)
						 ? 0
						 : b_room(&outbuf) - FCGI_RECORD_HEADER_SZ - extra_bytes);
				}
				if (!v.len || !chunk_memcat(&outbuf, v.ptr, v.len)) {
					if (outbuf.data == FCGI_RECORD_HEADER_SZ)
						goto full;
					goto done;
				}
				if (v.len != size) {
					total += v.len;
					count -= v.len;
					htx_cut_data_blk(htx, blk, v.len);
					goto done;
				}
				break;

			default:
				break;
		}
		total += size;
		count -= size;
		blk = htx_remove_blk(htx, blk);
	}

  done:
	/* update the record's size */
	TRACE_PROTO("FCGI STDIN record xferred", FCGI_EV_TX_RECORD|FCGI_EV_TX_STDIN, fconn->conn, fstrm, 0, (size_t[]){outbuf.data - FCGI_RECORD_HEADER_SZ});
	fcgi_set_record_size(outbuf.area, outbuf.data - FCGI_RECORD_HEADER_SZ);
	b_add(mbuf, outbuf.data);

	/* Send the empty stding here to finish the message */
	if (htx_is_empty(htx) && (htx->flags & HTX_FL_EOM)) {
	  empty_stdin:
		TRACE_PROTO("sending FCGI STDIN record", FCGI_EV_TX_RECORD|FCGI_EV_TX_STDIN, fconn->conn, fstrm, htx);
		if (!fcgi_strm_send_empty_stdin(fconn, fstrm)) {
			/* bytes already reserved for this record. It should not fail */
			htx->flags |= HTX_FL_PROCESSING_ERROR;
			TRACE_ERROR("processing error sending empty STDIN record", FCGI_EV_TX_RECORD|FCGI_EV_STRM_ERR, fconn->conn, fstrm);
			fcgi_strm_error(fstrm);
		}
	}

  end:
	TRACE_LEAVE(FCGI_EV_TX_RECORD|FCGI_EV_TX_STDIN, fconn->conn, fstrm, htx, (size_t[]){total});
	return total;
  full:
	if ((mbuf = br_tail_add(fconn->mbuf)) != NULL)
		goto retry;
	fconn->flags |= FCGI_CF_MUX_MFULL;
	fstrm->flags |= FCGI_SF_BLK_MROOM;
	TRACE_STATE("mbuf ring full", FCGI_EV_TX_RECORD|FCGI_EV_FSTRM_BLK|FCGI_EV_FCONN_BLK, fconn->conn, fstrm);
	goto end;
}

/* Processes a STDOUT record. Returns > 0 on success, 0 if it couldn't do
 * anything. STDOUT records contain the entire response. All the content is
 * copied in the stream's rxbuf. The parsing will be handled in fcgi_rcv_buf().
 */
static int fcgi_strm_handle_stdout(struct fcgi_conn *fconn, struct fcgi_strm *fstrm)
{
	struct buffer *dbuf;
	size_t ret;
	size_t max;

	TRACE_ENTER(FCGI_EV_RX_RECORD|FCGI_EV_RX_STDOUT, fconn->conn, fstrm);

	dbuf = &fconn->dbuf;

	/* Only padding remains */
	if (fconn->state == FCGI_CS_RECORD_P)
		goto end_transfer;

	if (b_data(dbuf) < (fconn->drl + fconn->drp) && !b_full(dbuf)) {
		fconn->flags |= FCGI_CF_DEM_SHORT_READ;
		goto fail; // incomplete record
	}

	if (!fcgi_get_buf(fconn, &fstrm->rxbuf)) {
		fconn->flags |= FCGI_CF_DEM_SALLOC;
		TRACE_STATE("waiting for fstrm rxbuf allocation", FCGI_EV_RX_RECORD|FCGI_EV_FSTRM_BLK, fconn->conn, fstrm);
		goto fail;
	}

	/*max = MIN(b_room(&fstrm->rxbuf), fconn->drl);*/
	max = buf_room_for_htx_data(&fstrm->rxbuf);
	if (!b_data(&fstrm->rxbuf))
		fstrm->rxbuf.head = sizeof(struct htx);
	if (max > fconn->drl)
		max = fconn->drl;

	ret = b_xfer(&fstrm->rxbuf, dbuf, max);
	if (!ret)
		goto fail;
	fconn->drl -= ret;
	TRACE_DATA("move some data to fstrm rxbuf", FCGI_EV_RX_RECORD|FCGI_EV_RX_STDOUT, fconn->conn, fstrm, 0, (size_t[]){ret});
	TRACE_PROTO("FCGI STDOUT record rcvd", FCGI_EV_RX_RECORD|FCGI_EV_RX_STDOUT, fconn->conn, fstrm, 0, (size_t[]){ret});

	if (!buf_room_for_htx_data(&fstrm->rxbuf)) {
		fconn->flags |= FCGI_CF_DEM_SFULL;
		TRACE_STATE("fstrm rxbuf full", FCGI_EV_RX_RECORD|FCGI_EV_FSTRM_BLK, fconn->conn, fstrm);
	}

	if (fconn->drl)
		goto fail;

  end_transfer:
	fconn->state = FCGI_CS_RECORD_P;
	fconn->drl += fconn->drp;
	fconn->drp = 0;
	ret = MIN(b_data(&fconn->dbuf), fconn->drl);
	b_del(&fconn->dbuf, ret);
	fconn->drl -= ret;
	if (fconn->drl)
		goto fail;

	fconn->state = FCGI_CS_RECORD_H;
	TRACE_STATE("switching to RECORD_H", FCGI_EV_RX_RECORD|FCGI_EV_RX_FHDR, fconn->conn, fstrm);
	TRACE_LEAVE(FCGI_EV_RX_RECORD|FCGI_EV_RX_STDOUT, fconn->conn, fstrm);
	return 1;
  fail:
	TRACE_DEVEL("leaving on missing data or error", FCGI_EV_RX_RECORD|FCGI_EV_RX_STDOUT, fconn->conn, fstrm);
	return 0;
}


/* Processes an empty STDOUT. Returns > 0 on success, 0 if it couldn't do
 * anything. It only skip the padding in fact, there is no payload for such
 * records. It marks the end of the response.
 */
static int fcgi_strm_handle_empty_stdout(struct fcgi_conn *fconn, struct fcgi_strm *fstrm)
{
	int ret;

	TRACE_ENTER(FCGI_EV_RX_RECORD|FCGI_EV_RX_STDOUT, fconn->conn, fstrm);

	if (b_data(&fconn->dbuf) < (fconn->drl + fconn->drp) && !b_full(&fconn->dbuf)) {
		fconn->flags |= FCGI_CF_DEM_SHORT_READ;
		goto fail; // incomplete record
	}

	fconn->state = FCGI_CS_RECORD_P;
	TRACE_STATE("switching to RECORD_P", FCGI_EV_RX_RECORD|FCGI_EV_RX_STDOUT, fconn->conn, fstrm);

	fconn->drl += fconn->drp;
	fconn->drp = 0;
	ret = MIN(b_data(&fconn->dbuf), fconn->drl);
	b_del(&fconn->dbuf, ret);
	fconn->drl -= ret;
	if (fconn->drl)
		goto fail;
	fconn->state = FCGI_CS_RECORD_H;
	fstrm->flags |= FCGI_SF_ES_RCVD;
	TRACE_PROTO("FCGI STDOUT record rcvd", FCGI_EV_RX_RECORD|FCGI_EV_RX_STDOUT, fconn->conn, fstrm, 0, (size_t[]){0});
	TRACE_STATE("stdout data fully send, switching to RECORD_H", FCGI_EV_RX_RECORD|FCGI_EV_RX_FHDR|FCGI_EV_RX_EOI, fconn->conn, fstrm);
	TRACE_LEAVE(FCGI_EV_RX_RECORD|FCGI_EV_RX_STDOUT, fconn->conn, fstrm);
	return 1;

  fail:
	TRACE_DEVEL("leaving on missing data or error", FCGI_EV_RX_RECORD|FCGI_EV_RX_STDOUT, fconn->conn, fstrm);
	return 0;
}

/* Processes a STDERR record. Returns > 0 on success, 0 if it couldn't do
 * anything.
 */
static int fcgi_strm_handle_stderr(struct fcgi_conn *fconn, struct fcgi_strm *fstrm)
{
	struct buffer *dbuf;
	struct buffer tag;
	size_t ret;

	TRACE_ENTER(FCGI_EV_RX_RECORD|FCGI_EV_RX_STDERR, fconn->conn, fstrm);
	dbuf = &fconn->dbuf;

	/* Only padding remains */
	if (fconn->state == FCGI_CS_RECORD_P || !fconn->drl)
		goto end_transfer;

	if (b_data(dbuf) < (fconn->drl + fconn->drp) && !b_full(dbuf)) {
		fconn->flags |= FCGI_CF_DEM_SHORT_READ;
		goto fail; // incomplete record
	}

	chunk_reset(&trash);
	ret = b_force_xfer(&trash, dbuf, MIN(b_room(&trash) - 2, fconn->drl));
	if (!ret)
		goto fail;
	fconn->drl -= ret;
	TRACE_PROTO("FCGI STDERR record rcvd", FCGI_EV_RX_RECORD|FCGI_EV_RX_STDERR, fconn->conn, fstrm, 0, (size_t[]){ret});

	trash.area[ret]   = '\n';
	trash.area[ret+1] = '\0';
	tag.area = fconn->app->name; tag.data = strlen(fconn->app->name);
	app_log(&fconn->app->loggers, &tag, LOG_ERR, "%s", trash.area);

	if (fconn->drl)
		goto fail;

  end_transfer:
	fconn->state = FCGI_CS_RECORD_P;
	fconn->drl += fconn->drp;
	fconn->drp = 0;
	ret = MIN(b_data(&fconn->dbuf), fconn->drl);
	b_del(&fconn->dbuf, ret);
	fconn->drl -= ret;
	if (fconn->drl)
		goto fail;
	fconn->state = FCGI_CS_RECORD_H;
	TRACE_STATE("switching to RECORD_H", FCGI_EV_RX_RECORD|FCGI_EV_RX_FHDR, fconn->conn, fstrm);
	TRACE_LEAVE(FCGI_EV_RX_RECORD|FCGI_EV_RX_STDERR, fconn->conn, fstrm);
	return 1;
  fail:
	TRACE_DEVEL("leaving on missing data or error", FCGI_EV_RX_RECORD|FCGI_EV_RX_STDERR, fconn->conn, fstrm);
	return 0;
}

/* Processes an END_REQUEST record. Returns > 0 on success, 0 if it couldn't do
 * anything. If the empty STDOUT record is not already received, this one marks
 * the end of the response. It is highly unexpected, but if the record is larger
 * than a buffer and cannot be decoded in one time, an error is triggered and
 * the connection is closed. END_REQUEST record cannot be split.
 */
static int fcgi_strm_handle_end_request(struct fcgi_conn *fconn, struct fcgi_strm *fstrm)
{
	struct buffer inbuf;
	struct buffer *dbuf;
	struct fcgi_end_request endreq;

	TRACE_ENTER(FCGI_EV_RX_RECORD|FCGI_EV_RX_ENDREQ, fconn->conn, fstrm);
	dbuf = &fconn->dbuf;

	/* Record too large to be fully decoded */
	if (b_size(dbuf) < (fconn->drl + fconn->drp)) {
		TRACE_ERROR("END_REQUEST record too large", FCGI_EV_RX_RECORD|FCGI_EV_RX_ENDREQ|FCGI_EV_FSTRM_ERR, fconn->conn, fstrm);
		goto fail;
	}

	/* process full record only */
	if (b_data(dbuf) < (fconn->drl + fconn->drp)) {
		fconn->flags |= FCGI_CF_DEM_SHORT_READ;
		TRACE_DEVEL("leaving on missing data", FCGI_EV_RX_RECORD|FCGI_EV_RX_ENDREQ, fconn->conn);
		return 0;
	}

	if (unlikely(b_contig_data(dbuf, b_head_ofs(dbuf)) < fconn->drl)) {
		/* Realign the dmux buffer if the record wraps. It is unexpected
		 * at this stage because it should be the first record received
		 * from the FCGI application.
		 */
		b_slow_realign_ofs(dbuf, trash.area, 0);
	}

	inbuf = b_make(b_head(dbuf), b_data(dbuf), 0, fconn->drl);

	if (!fcgi_decode_end_request(&inbuf, 0, &endreq)) {
		TRACE_ERROR("END_REQUEST record decoding failure", FCGI_EV_RX_RECORD|FCGI_EV_RX_ENDREQ|FCGI_EV_FSTRM_ERR, fconn->conn, fstrm);
		goto fail;
	}

	fstrm->flags |= FCGI_SF_ES_RCVD;
	TRACE_STATE("end of script reported", FCGI_EV_RX_RECORD|FCGI_EV_RX_ENDREQ|FCGI_EV_RX_EOI, fconn->conn, fstrm);
	TRACE_PROTO("FCGI END_REQUEST record rcvd", FCGI_EV_RX_RECORD|FCGI_EV_RX_ENDREQ, fconn->conn, fstrm, 0, (size_t[]){fconn->drl});
	fstrm->proto_status = endreq.errcode;
	fcgi_strm_close(fstrm);

	b_del(&fconn->dbuf, fconn->drl + fconn->drp);
	fconn->drl = 0;
	fconn->drp = 0;
	fconn->state = FCGI_CS_RECORD_H;
	TRACE_STATE("switching to RECORD_H", FCGI_EV_RX_RECORD|FCGI_EV_RX_FHDR, fconn->conn, fstrm);
	TRACE_LEAVE(FCGI_EV_RX_RECORD|FCGI_EV_RX_ENDREQ, fconn->conn, fstrm);
	return 1;

  fail:
	fcgi_strm_error(fstrm);
	TRACE_DEVEL("leaving on error", FCGI_EV_RX_RECORD|FCGI_EV_RX_ENDREQ|FCGI_EV_FSTRM_ERR, fconn->conn, fstrm);
	return 0;
}

/* process Rx records to be demultiplexed */
static void fcgi_process_demux(struct fcgi_conn *fconn)
{
	struct fcgi_strm *fstrm = NULL, *tmp_fstrm;
	struct fcgi_header hdr;
	int ret;

	TRACE_ENTER(FCGI_EV_FCONN_WAKE, fconn->conn);

	if (fconn->state == FCGI_CS_CLOSED)
		return;

	if (unlikely(fconn->state < FCGI_CS_RECORD_H)) {
		if (fconn->state == FCGI_CS_INIT) {
			TRACE_STATE("waiting FCGI GET_VALUES to be sent", FCGI_EV_RX_RECORD|FCGI_EV_RX_FHDR|FCGI_EV_RX_GETVAL, fconn->conn);
			return;
		}
		if (fconn->state == FCGI_CS_SETTINGS) {
			/* ensure that what is pending is a valid GET_VALUES_RESULT record. */
			TRACE_STATE("receiving FCGI record header", FCGI_EV_RX_RECORD|FCGI_EV_RX_FHDR, fconn->conn);
			ret = fcgi_decode_record_hdr(&fconn->dbuf, 0, &hdr);
			if (!ret) {
				fconn->flags |= FCGI_CF_DEM_SHORT_READ;
				TRACE_ERROR("header record decoding failure", FCGI_EV_RX_RECORD|FCGI_EV_RX_ENDREQ|FCGI_EV_FSTRM_ERR, fconn->conn, fstrm);
				goto fail;
			}
			b_del(&fconn->dbuf, ret);

			if (hdr.id || (hdr.type != FCGI_GET_VALUES_RESULT && hdr.type != FCGI_UNKNOWN_TYPE)) {
				fconn->state = FCGI_CS_CLOSED;
				TRACE_ERROR("unexpected record type or flags", FCGI_EV_RX_RECORD|FCGI_EV_RX_FHDR|FCGI_EV_RX_GETVAL|FCGI_EV_FCONN_ERR, fconn->conn);
				TRACE_STATE("switching to CLOSED", FCGI_EV_RX_RECORD|FCGI_EV_RX_FHDR|FCGI_EV_RX_GETVAL|FCGI_EV_FCONN_ERR, fconn->conn);
				goto fail;
			}
			goto new_record;
		}
	}

	/* process as many incoming records as possible below */
	while (1) {
		/* Make sure to clear DFULL if contents were deleted */
		if (!b_full(&fconn->dbuf))
			fconn->flags &= ~FCGI_CF_DEM_DFULL;

		if (!b_data(&fconn->dbuf)) {
			TRACE_DEVEL("no more Rx data", FCGI_EV_RX_RECORD, fconn->conn);
			fconn->flags |= FCGI_CF_DEM_SHORT_READ;
			break;
		}

		if (fconn->state == FCGI_CS_CLOSED) {
			TRACE_STATE("end of connection reported", FCGI_EV_RX_RECORD|FCGI_EV_RX_EOI, fconn->conn);
			break;
		}

		if (fconn->state == FCGI_CS_RECORD_H) {
			TRACE_PROTO("receiving FCGI record header", FCGI_EV_RX_RECORD|FCGI_EV_RX_FHDR, fconn->conn);
			ret = fcgi_decode_record_hdr(&fconn->dbuf, 0, &hdr);
			if (!ret) {
				fconn->flags |= FCGI_CF_DEM_SHORT_READ;
				break;
			}
			b_del(&fconn->dbuf, ret);

		  new_record:
			fconn->dsi = hdr.id;
			fconn->drt = hdr.type;
			fconn->drl = hdr.len;
			fconn->drp = hdr.padding;
			fconn->state = FCGI_CS_RECORD_D;
			TRACE_STATE("FCGI record header rcvd, switching to RECORD_D", FCGI_EV_RX_RECORD|FCGI_EV_RX_FHDR, fconn->conn);
		}

		/* Make sure to clear DFULL if contents were deleted */
		if (!b_full(&fconn->dbuf))
			fconn->flags &= ~FCGI_CF_DEM_DFULL;

		/* Only FCGI_CS_RECORD_D or FCGI_CS_RECORD_P */
		tmp_fstrm = fcgi_conn_st_by_id(fconn, fconn->dsi);

		if (tmp_fstrm != fstrm && fstrm && fcgi_strm_sc(fstrm) &&
		    (b_data(&fstrm->rxbuf) ||
		     fcgi_conn_read0_pending(fconn) ||
		     fstrm->state == FCGI_SS_CLOSED ||
		     (fstrm->flags & FCGI_SF_ES_RCVD) ||
		     se_fl_test(fstrm->sd, SE_FL_ERROR | SE_FL_ERR_PENDING | SE_FL_EOS))) {
			/* we may have to signal the upper layers */
			TRACE_DEVEL("notifying stream before switching SID", FCGI_EV_RX_RECORD|FCGI_EV_STRM_WAKE, fconn->conn, fstrm);
			se_fl_set(fstrm->sd, SE_FL_RCV_MORE);
			fcgi_strm_notify_recv(fstrm);
		}
		fstrm = tmp_fstrm;

		if (fstrm->state == FCGI_SS_CLOSED && fconn->dsi != 0) {
			/* ignore all record for closed streams */
			goto ignore_record;
		}
		if (fstrm->state == FCGI_SS_IDLE) {
			/* ignore all record for unknown streams */
			goto ignore_record;
		}

		switch (fconn->drt) {
			case FCGI_GET_VALUES_RESULT:
				TRACE_PROTO("receiving FCGI GET_VALUES_RESULT record", FCGI_EV_RX_RECORD|FCGI_EV_RX_GETVAL, fconn->conn);
				ret = fcgi_conn_handle_values_result(fconn);
				break;

			case FCGI_STDOUT:
				if (fstrm->flags & FCGI_SF_ES_RCVD)
					goto ignore_record;

				TRACE_PROTO("receiving FCGI STDOUT record", FCGI_EV_RX_RECORD|FCGI_EV_RX_STDOUT, fconn->conn, fstrm);
				if (fconn->drl)
					ret = fcgi_strm_handle_stdout(fconn, fstrm);
				else
					ret = fcgi_strm_handle_empty_stdout(fconn, fstrm);
				break;

			case FCGI_STDERR:
				TRACE_PROTO("receiving FCGI STDERR record", FCGI_EV_RX_RECORD|FCGI_EV_RX_STDERR, fconn->conn, fstrm);
				ret = fcgi_strm_handle_stderr(fconn, fstrm);
				break;

			case FCGI_END_REQUEST:
				TRACE_PROTO("receiving FCGI END_REQUEST record", FCGI_EV_RX_RECORD|FCGI_EV_RX_ENDREQ, fconn->conn, fstrm);
				ret = fcgi_strm_handle_end_request(fconn, fstrm);
				break;

			/* implement all extra record types here */
			default:
			  ignore_record:
				/* drop records that we ignore. They may be
				 * larger than the buffer so we drain all of
				 * their contents until we reach the end.
				 */
				if (b_data(&fconn->dbuf) < (fconn->drl + fconn->drp) && !b_full(&fconn->dbuf)) {
					fconn->flags |= FCGI_CF_DEM_SHORT_READ;
					ret = 0;
					break;
				}

				fconn->state = FCGI_CS_RECORD_P;
				fconn->drl += fconn->drp;
				fconn->drp = 0;
				ret = MIN(b_data(&fconn->dbuf), fconn->drl);
				TRACE_PROTO("receiving FCGI ignored record", FCGI_EV_RX_RECORD, fconn->conn, fstrm, 0, (size_t[]){ret});
				TRACE_STATE("switching to RECORD_P", FCGI_EV_RX_RECORD, fconn->conn, fstrm);
				b_del(&fconn->dbuf, ret);
				fconn->drl -= ret;
				ret = (fconn->drl == 0);
		}

		/* error or missing data condition met above ? */
		if (ret <= 0) {
			TRACE_DEVEL("insufficient data to proceed", FCGI_EV_RX_RECORD, fconn->conn, fstrm);
			break;
		}

		if (fconn->state != FCGI_CS_RECORD_H && !(fconn->drl+fconn->drp)) {
			fconn->state = FCGI_CS_RECORD_H;
			TRACE_STATE("switching to RECORD_H", FCGI_EV_RX_RECORD|FCGI_EV_RX_FHDR, fconn->conn);
		}
	}

 fail:
	if (fconn->state == FCGI_CS_CLOSED || (fconn->flags & FCGI_CF_DEM_SHORT_READ)) {
		if (fconn->flags & FCGI_CF_EOS)
			fconn->flags |= FCGI_CF_END_REACHED;
	}

	if (fconn->flags & FCGI_CF_ERROR)
		fcgi_conn_report_term_evt(fconn, ((eb_is_empty(&fconn->streams_by_id) && (fconn->state == FCGI_CS_RECORD_H))
						  ? muxc_tevt_type_rcv_err
						  : muxc_tevt_type_truncated_rcv_err));
	else if (fconn->flags & FCGI_CF_END_REACHED)
		fcgi_conn_report_term_evt(fconn, ((eb_is_empty(&fconn->streams_by_id) && (fconn->state == FCGI_CS_RECORD_H))
						  ? muxc_tevt_type_shutr
						  : muxc_tevt_type_truncated_shutr));

	/* Make sure to clear DFULL if contents were deleted */
	if (!b_full(&fconn->dbuf))
		fconn->flags &= ~FCGI_CF_DEM_DFULL;

	/* we can go here on missing data, blocked response or error */
	if (fstrm && fcgi_strm_sc(fstrm) &&
	    (b_data(&fstrm->rxbuf) ||
	     fcgi_conn_read0_pending(fconn) ||
	     fstrm->state == FCGI_SS_CLOSED ||
	     (fstrm->flags & FCGI_SF_ES_RCVD) ||
	     se_fl_test(fstrm->sd, SE_FL_ERROR | SE_FL_ERR_PENDING | SE_FL_EOS))) {
		/* we may have to signal the upper layers */
		TRACE_DEVEL("notifying stream before switching SID", FCGI_EV_RX_RECORD|FCGI_EV_STRM_WAKE, fconn->conn, fstrm);
		se_fl_set(fstrm->sd, SE_FL_RCV_MORE);
		fcgi_strm_notify_recv(fstrm);
	}

	fcgi_conn_restart_reading(fconn, 0);
}

/* process Tx records from streams to be multiplexed. Returns > 0 if it reached
 * the end.
 */
static int fcgi_process_mux(struct fcgi_conn *fconn)
{
	struct fcgi_strm *fstrm, *fstrm_back;

	TRACE_ENTER(FCGI_EV_FCONN_WAKE, fconn->conn);

	if (unlikely(fconn->state < FCGI_CS_RECORD_H)) {
		if (unlikely(fconn->state == FCGI_CS_INIT)) {
			if (!(fconn->flags & FCGI_CF_GET_VALUES)) {
				fconn->state = FCGI_CS_RECORD_H;
				TRACE_STATE("switching to RECORD_H", FCGI_EV_TX_RECORD|FCGI_EV_RX_RECORD|FCGI_EV_RX_FHDR, fconn->conn);
				fcgi_wake_unassigned_streams(fconn);
				goto mux;
			}
			TRACE_PROTO("sending FCGI GET_VALUES record", FCGI_EV_TX_RECORD|FCGI_EV_TX_GETVAL, fconn->conn);
			if (unlikely(!fcgi_conn_send_get_values(fconn)))
				goto fail;
			fconn->state = FCGI_CS_SETTINGS;
			TRACE_STATE("switching to SETTINGS", FCGI_EV_TX_RECORD|FCGI_EV_RX_RECORD|FCGI_EV_RX_GETVAL, fconn->conn);
		}
		/* need to wait for the other side */
		if (fconn->state < FCGI_CS_RECORD_H)
			goto done;
	}

  mux:
	list_for_each_entry_safe(fstrm, fstrm_back, &fconn->send_list, send_list) {
		if (fconn->state == FCGI_CS_CLOSED || fconn->flags & FCGI_CF_MUX_BLOCK_ANY)
			break;

		if (fstrm->flags & FCGI_SF_NOTIFIED)
			continue;

		/* If the sender changed his mind and unsubscribed, let's just
		 * remove the stream from the send_list.
		 */
		if (!(fstrm->flags & (FCGI_SF_WANT_SHUTR|FCGI_SF_WANT_SHUTW)) &&
		    (!fstrm->subs || !(fstrm->subs->events & SUB_RETRY_SEND))) {
			LIST_DEL_INIT(&fstrm->send_list);
			continue;
		}

		if (fstrm->subs && fstrm->subs->events & SUB_RETRY_SEND) {
			TRACE_POINT(FCGI_EV_STRM_WAKE, fconn->conn, fstrm);
			fstrm->flags &= ~FCGI_SF_BLK_ANY;
			fstrm->flags |= FCGI_SF_NOTIFIED;
			tasklet_wakeup(fstrm->subs->tasklet);
			fstrm->subs->events &= ~SUB_RETRY_SEND;
			if (!fstrm->subs->events)
				fstrm->subs = NULL;
		} else {
			/* it's the shut request that was queued */
			TRACE_POINT(FCGI_EV_STRM_WAKE, fconn->conn, fstrm);
			tasklet_wakeup(fstrm->shut_tl);
		}
	}

 fail:
	if (fconn->state == FCGI_CS_CLOSED) {
		if (fconn->stream_cnt - fconn->nb_reserved > 0) {
			fcgi_conn_send_aborts(fconn);
			if (fconn->flags & FCGI_CF_MUX_BLOCK_ANY) {
				TRACE_DEVEL("leaving in blocked situation", FCGI_EV_FCONN_WAKE|FCGI_EV_FCONN_BLK, fconn->conn);
				return 0;
			}
		}
	}

  done:
	TRACE_LEAVE(FCGI_EV_FCONN_WAKE, fconn->conn);
	return 1;
}


/* Attempt to read data, and subscribe if none available.
 * The function returns 1 if data has been received, otherwise zero.
 */
static int fcgi_recv(struct fcgi_conn *fconn)
{
	struct connection *conn = fconn->conn;
	struct buffer *buf;
	int max;
	size_t ret;

	TRACE_ENTER(FCGI_EV_FCONN_RECV, conn);

	if (fconn->wait_event.events & SUB_RETRY_RECV) {
		TRACE_DEVEL("leaving on sub_recv", FCGI_EV_FCONN_RECV, conn);
		return (b_data(&fconn->dbuf));
	}

	if (!fcgi_recv_allowed(fconn)) {
		TRACE_DEVEL("leaving on !recv_allowed", FCGI_EV_FCONN_RECV, conn);
		return 1;
	}

	buf = fcgi_get_buf(fconn, &fconn->dbuf);
	if (!buf) {
		TRACE_DEVEL("waiting for fconn dbuf allocation", FCGI_EV_FCONN_RECV|FCGI_EV_FCONN_BLK, conn);
		fconn->flags |= FCGI_CF_DEM_DALLOC;
		return 0;
	}

	if (!b_data(buf)) {
		/* try to pre-align the buffer like the
		 * rxbufs will be to optimize memory copies. We'll make
		 * sure that the record header lands at the end of the
		 * HTX block to alias it upon recv. We cannot use the
		 * head because rcv_buf() will realign the buffer if
		 * it's empty. Thus we cheat and pretend we already
		 * have a few bytes there.
		 */
		max = buf_room_for_htx_data(buf) + (fconn->state == FCGI_CS_RECORD_H ? FCGI_RECORD_HEADER_SZ : 0);
		buf->head = sizeof(struct htx) - (fconn->state == FCGI_CS_RECORD_H ? FCGI_RECORD_HEADER_SZ : 0);
	}
	else
		max = b_room(buf);

	ret = max ? conn->xprt->rcv_buf(conn, conn->xprt_ctx, buf, max, 0) : 0;

	if (max && !ret && fcgi_recv_allowed(fconn)) {
		TRACE_DATA("failed to receive data, subscribing", FCGI_EV_FCONN_RECV, conn);
		conn->xprt->subscribe(conn, conn->xprt_ctx, SUB_RETRY_RECV, &fconn->wait_event);
	}
	else if (ret) {
		fconn->flags &= ~FCGI_CF_DEM_SHORT_READ;
		TRACE_DATA("recv data", FCGI_EV_FCONN_RECV, conn, 0, 0, (size_t[]){ret});
	}

	if (conn_xprt_read0_pending(conn)) {
		TRACE_DATA("received read0", FCGI_EV_FCONN_RECV, conn);
		fconn->flags |= FCGI_CF_EOS;
	}
	if (conn->flags & CO_FL_ERROR &&
	    (!b_data(&fconn->dbuf) || (fconn->flags & FCGI_CF_DEM_SHORT_READ))) {
		TRACE_DATA("connection error", FCGI_EV_FCONN_RECV, conn);
		fconn->flags |= FCGI_CF_ERROR;
	}

	if (!b_data(buf)) {
		fcgi_release_buf(fconn, &fconn->dbuf);
		goto end;
	}

	if (ret == max) {
		TRACE_DEVEL("fconn dbuf full", FCGI_EV_FCONN_RECV|FCGI_EV_FCONN_BLK, conn);
		fconn->flags |= FCGI_CF_DEM_DFULL;
	}

end:
	TRACE_LEAVE(FCGI_EV_FCONN_RECV, conn);
	return !!ret || (fconn->flags & (FCGI_CF_EOS|FCGI_CF_ERROR));
}


/* Try to send data if possible.
 * The function returns 1 if data have been sent, otherwise zero.
 */
static int fcgi_send(struct fcgi_conn *fconn)
{
	struct connection *conn = fconn->conn;
	int done;
	int sent = 0;

	TRACE_ENTER(FCGI_EV_FCONN_SEND, conn);

	if (fconn->flags & (FCGI_CF_ERROR|FCGI_CF_ERR_PENDING)) {
		TRACE_DEVEL("leaving on connection error", FCGI_EV_FCONN_SEND, conn);
		if (fconn->flags & FCGI_CF_EOS)
			fconn->flags |= FCGI_CF_ERROR;
		b_reset(br_tail(fconn->mbuf));
		return 1;
	}


	if (conn->flags & CO_FL_WAIT_XPRT) {
		/* a handshake was requested */
		goto schedule;
	}

	/* This loop is quite simple : it tries to fill as much as it can from
	 * pending streams into the existing buffer until it's reportedly full
	 * or the end of send requests is reached. Then it tries to send this
	 * buffer's contents out, marks it not full if at least one byte could
	 * be sent, and tries again.
	 *
	 * The snd_buf() function normally takes a "flags" argument which may
	 * be made of a combination of CO_SFL_MSG_MORE to indicate that more
	 * data immediately comes and CO_SFL_STREAMER to indicate that the
	 * connection is streaming lots of data (used to increase TLS record
	 * size at the expense of latency). The former can be sent any time
	 * there's a buffer full flag, as it indicates at least one stream
	 * attempted to send and failed so there are pending data. An
	 * alternative would be to set it as long as there's an active stream
	 * but that would be problematic for ACKs until we have an absolute
	 * guarantee that all waiters have at least one byte to send. The
	 * latter should possibly not be set for now.
	 */

	done = 0;
	while (!done) {
		unsigned int flags = 0;
		unsigned int released = 0;
		struct buffer *buf;

		/* fill as much as we can into the current buffer */
		while (((fconn->flags & (FCGI_CF_MUX_MFULL|FCGI_CF_MUX_MALLOC)) == 0) && !done)
			done = fcgi_process_mux(fconn);

		if (fconn->flags & FCGI_CF_MUX_MALLOC)
			done = 1; // we won't go further without extra buffers

		if (conn->flags & CO_FL_ERROR)
			break;

		if (fconn->flags & (FCGI_CF_MUX_MFULL | FCGI_CF_DEM_MROOM))
			flags |= CO_SFL_MSG_MORE;

		for (buf = br_head(fconn->mbuf); b_size(buf); buf = br_del_head(fconn->mbuf)) {
			if (b_data(buf)) {
				int ret;

				ret = conn->xprt->snd_buf(conn, conn->xprt_ctx, buf, b_data(buf), flags);
				if (!ret) {
					done = 1;
					break;
				}
				sent = 1;
				TRACE_DATA("send data", FCGI_EV_FCONN_SEND, conn, 0, 0, (size_t[]){ret});
				b_del(buf, ret);
				if (b_data(buf)) {
					done = 1;
					break;
				}
			}
			b_free(buf);
			released++;
		}

		if (released)
			offer_buffers(NULL, released);

		/* wrote at least one byte, the buffer is not full anymore */
		if (fconn->flags & (FCGI_CF_MUX_MFULL | FCGI_CF_DEM_MROOM))
			TRACE_STATE("fconn mbuf ring not fill anymore", FCGI_EV_FCONN_SEND|FCGI_EV_FCONN_BLK, conn);
		fconn->flags &= ~(FCGI_CF_MUX_MFULL | FCGI_CF_DEM_MROOM);
	}

	if (conn->flags & CO_FL_ERROR) {
		fconn->flags |= FCGI_CF_ERR_PENDING;
		fcgi_conn_report_term_evt(fconn, muxc_tevt_type_snd_err);
		if (fconn->flags & FCGI_CF_EOS)
			fconn->flags |= FCGI_CF_ERROR;
		b_reset(br_tail(fconn->mbuf));
	}

	/* We're not full anymore, so we can wake any task that are waiting
	 * for us.
	 */
	if (!(fconn->flags & (FCGI_CF_MUX_MFULL | FCGI_CF_DEM_MROOM)) && fconn->state >= FCGI_CS_RECORD_H) {
		struct fcgi_strm *fstrm;

		list_for_each_entry(fstrm, &fconn->send_list, send_list) {
			if (fconn->state == FCGI_CS_CLOSED || fconn->flags & FCGI_CF_MUX_BLOCK_ANY)
				break;

			if (fstrm->flags & FCGI_SF_NOTIFIED)
				continue;

			/* If the sender changed his mind and unsubscribed, let's just
			 * remove the stream from the send_list.
			 */
			if (!(fstrm->flags & (FCGI_SF_WANT_SHUTR|FCGI_SF_WANT_SHUTW)) &&
			    (!fstrm->subs || !(fstrm->subs->events & SUB_RETRY_SEND))) {
				LIST_DEL_INIT(&fstrm->send_list);
				continue;
			}

			if (fstrm->subs && fstrm->subs->events & SUB_RETRY_SEND) {
				TRACE_DEVEL("waking up pending stream", FCGI_EV_FCONN_SEND|FCGI_EV_STRM_WAKE, conn, fstrm);
				fstrm->flags &= ~FCGI_SF_BLK_ANY;
				fstrm->flags |= FCGI_SF_NOTIFIED;
				tasklet_wakeup(fstrm->subs->tasklet);
				fstrm->subs->events &= ~SUB_RETRY_SEND;
				if (!fstrm->subs->events)
					fstrm->subs = NULL;
			} else {
				/* it's the shut request that was queued */
				TRACE_POINT(FCGI_EV_STRM_WAKE, fconn->conn, fstrm);
				tasklet_wakeup(fstrm->shut_tl);
			}
		}
	}
	/* We're done, no more to send */
	if (!br_data(fconn->mbuf)) {
		TRACE_DEVEL("leaving with everything sent", FCGI_EV_FCONN_SEND, conn);
		goto end;
	}
schedule:
	if (!(conn->flags & CO_FL_ERROR) && !(fconn->wait_event.events & SUB_RETRY_SEND)) {
		TRACE_STATE("more data to send, subscribing", FCGI_EV_FCONN_SEND, conn);
		conn->xprt->subscribe(conn, conn->xprt_ctx, SUB_RETRY_SEND, &fconn->wait_event);
	}

	TRACE_DEVEL("leaving with some data left to send", FCGI_EV_FCONN_SEND, conn);
end:
	return sent || (fconn->flags & (FCGI_CF_ERR_PENDING|FCGI_CF_ERROR));
}

/* this is the tasklet referenced in fconn->wait_event.tasklet */
struct task *fcgi_io_cb(struct task *t, void *ctx, unsigned int state)
{
	struct connection *conn;
	struct fcgi_conn *fconn = ctx;
	struct tasklet *tl = (struct tasklet *)t;
	int conn_in_list;
	int ret = 0;

	if (state & TASK_F_USR1) {
		/* the tasklet was idling on an idle connection, it might have
		 * been stolen, let's be careful!
		 */
		HA_SPIN_LOCK(IDLE_CONNS_LOCK, &idle_conns[tid].idle_conns_lock);
		if (tl->context == NULL) {
			/* The connection has been taken over by another thread,
			 * we're no longer responsible for it, so just free the
			 * tasklet, and do nothing.
			 */
			HA_SPIN_UNLOCK(IDLE_CONNS_LOCK, &idle_conns[tid].idle_conns_lock);
			tasklet_free(tl);
			return NULL;
		}
		conn = fconn->conn;
		TRACE_POINT(FCGI_EV_FCONN_WAKE, conn);

		conn_in_list = conn->flags & CO_FL_LIST_MASK;
		if (conn_in_list)
			conn_delete_from_tree(conn);

		HA_SPIN_UNLOCK(IDLE_CONNS_LOCK, &idle_conns[tid].idle_conns_lock);
	} else {
		/* we're certain the connection was not in an idle list */
		conn = fconn->conn;
		TRACE_ENTER(FCGI_EV_FCONN_WAKE, conn);
		conn_in_list = 0;
	}

	if (!(fconn->wait_event.events & SUB_RETRY_SEND))
		ret = fcgi_send(fconn);
	if (!(fconn->wait_event.events & SUB_RETRY_RECV))
		ret |= fcgi_recv(fconn);
	if (ret || b_data(&fconn->dbuf))
		ret = fcgi_process(fconn);

	/* If we were in an idle list, we want to add it back into it,
	 * unless fcgi_process() returned -1, which mean it has destroyed
	 * the connection (testing !ret is enough, if fcgi_process() wasn't
	 * called then ret will be 0 anyway.
	 */
	if (ret < 0)
		t = NULL;

	if (!ret && conn_in_list) {
		struct server *srv = __objt_server(conn->target);

		HA_SPIN_LOCK(IDLE_CONNS_LOCK, &idle_conns[tid].idle_conns_lock);
		_srv_add_idle(srv, conn, conn_in_list == CO_FL_SAFE_LIST);
		HA_SPIN_UNLOCK(IDLE_CONNS_LOCK, &idle_conns[tid].idle_conns_lock);
	}
	return t;
}

/* callback called on any event by the connection handler.
 * It applies changes and returns zero, or < 0 if it wants immediate
 * destruction of the connection (which normally doesn not happen in FCGI).
 */
static int fcgi_process(struct fcgi_conn *fconn)
{
	struct connection *conn = fconn->conn;

	TRACE_POINT(FCGI_EV_FCONN_WAKE, conn);

	if (!(fconn->flags & FCGI_CF_DEM_BLOCK_ANY) &&
	    (b_data(&fconn->dbuf) || (fconn->flags & FCGI_CF_EOS))) {
		do {
			fcgi_process_demux(fconn);

			/* hint: if we ended up aligned on a record, we've very
			 * likely reached the end, no point trying again.
			 */
			if (fconn->state == FCGI_CS_RECORD_H)
				break;

			if (!fcgi_recv_allowed(fconn))
				break;

			/* OK, it's worth trying to grab a few more records */
			fcgi_recv(fconn);

		} while ((b_data(&fconn->dbuf) && fcgi_may_demux(fconn)) || (fconn->flags & FCGI_CF_EOS));

		/* now's time to wake the task up */
		fcgi_conn_restart_reading(fconn, 0);

		if (fconn->state == FCGI_CS_CLOSED || (fconn->flags & FCGI_CF_ERROR))
			b_reset(&fconn->dbuf);
	}
	fcgi_send(fconn);

	if (unlikely(fconn->proxy->flags & (PR_FL_DISABLED|PR_FL_STOPPED))) {
		/* frontend is stopping, reload likely in progress, let's try
		 * to announce a graceful shutdown if not yet done. We don't
		 * care if it fails, it will be tried again later.
		 */
		TRACE_STATE("proxy stopped, sending ABORT to all streams", FCGI_EV_FCONN_WAKE|FCGI_EV_TX_RECORD, conn);
		if (!(fconn->flags & (FCGI_CF_ABRTS_SENT|FCGI_CF_ABRTS_FAILED))) {
			if (fconn->stream_cnt - fconn->nb_reserved > 0)
				fcgi_conn_send_aborts(fconn);
		}
	}

	/*
	 * If we received early data, and the handshake is done, wake
	 * any stream that was waiting for it.
	 */
	if (!(fconn->flags & FCGI_CF_WAIT_FOR_HS) &&
	    (conn->flags & (CO_FL_EARLY_SSL_HS | CO_FL_WAIT_XPRT | CO_FL_EARLY_DATA)) == CO_FL_EARLY_DATA) {
		struct eb32_node *node;
		struct fcgi_strm *fstrm;

		fconn->flags |= FCGI_CF_WAIT_FOR_HS;
		node = eb32_lookup_ge(&fconn->streams_by_id, 1);

		while (node) {
			fstrm = container_of(node, struct fcgi_strm, by_id);
			if (fcgi_strm_sc(fstrm) && se_fl_test(fstrm->sd, SE_FL_WAIT_FOR_HS))
				fcgi_strm_notify_recv(fstrm);
			node = eb32_next(node);
		}
	}

	if ((fconn->flags & FCGI_CF_ERROR) || fcgi_conn_read0_pending(fconn) ||
	    fconn->state == FCGI_CS_CLOSED || (fconn->flags & FCGI_CF_ABRTS_FAILED) ||
	    eb_is_empty(&fconn->streams_by_id)) {
		fcgi_wake_some_streams(fconn, 0);

		if (eb_is_empty(&fconn->streams_by_id)) {
			/* no more stream, kill the connection now */
			fcgi_release(fconn);
			TRACE_DEVEL("leaving after releasing the connection", FCGI_EV_FCONN_WAKE);
			return -1;
		}
	}

	if (!b_data(&fconn->dbuf))
		fcgi_release_buf(fconn, &fconn->dbuf);

	if (fconn->state == FCGI_CS_CLOSED  || (fconn->flags & FCGI_CF_ABRTS_FAILED) ||
	    (!br_data(fconn->mbuf) && ((fconn->flags & FCGI_CF_MUX_BLOCK_ANY) || LIST_ISEMPTY(&fconn->send_list))))
		fcgi_release_mbuf(fconn);

	if (fconn->task) {
		fconn->task->expire = tick_add(now_ms, (fconn->state == FCGI_CS_CLOSED ? fconn->shut_timeout : fconn->timeout));
		task_queue(fconn->task);
	}

	fcgi_send(fconn);
	TRACE_LEAVE(FCGI_EV_FCONN_WAKE, conn);
	return 0;
}


/* wake-up function called by the connection layer (mux_ops.wake) */
static int fcgi_wake(struct connection *conn)
{
	struct fcgi_conn *fconn = conn->ctx;

	TRACE_POINT(FCGI_EV_FCONN_WAKE, conn);
	return (fcgi_process(fconn));
}


static int fcgi_ctl(struct connection *conn, enum mux_ctl_type mux_ctl, void *output)
{
	struct fcgi_conn *fconn = conn->ctx;
	int ret = 0;

	switch (mux_ctl) {
	case MUX_CTL_STATUS:
		if (!(conn->flags & CO_FL_WAIT_XPRT))
			ret |= MUX_STATUS_READY;
		return ret;
	case MUX_CTL_EXIT_STATUS:
		return MUX_ES_UNKNOWN;
	case MUX_CTL_GET_NBSTRM:
		return fconn->nb_streams;
	case MUX_CTL_GET_MAXSTRM:
		return fconn->streams_limit;
	case MUX_CTL_TEVTS:
		return fconn->term_evts_log;
	default:
		return -1;
	}
}

static int fcgi_sctl(struct stconn *sc, enum mux_sctl_type mux_sctl, void *output)
{
	int ret = 0;
	struct fcgi_strm *fstrm = __sc_mux_strm(sc);
	union mux_sctl_dbg_str_ctx *dbg_ctx;
	struct buffer *buf;

	switch (mux_sctl) {
	case MUX_SCTL_SID:
		if (output)
			*((int64_t *)output) = fstrm->id;
		return ret;
	case MUX_SCTL_DBG_STR:
		dbg_ctx = output;
		buf = get_trash_chunk();

		if (dbg_ctx->arg.debug_flags & MUX_SCTL_DBG_STR_L_MUXS)
			fcgi_dump_fcgi_strm_info(buf, fstrm, NULL);

		if (dbg_ctx->arg.debug_flags & MUX_SCTL_DBG_STR_L_MUXC)
			fcgi_dump_fcgi_conn_info(buf, fstrm->fconn, NULL);

		if (dbg_ctx->arg.debug_flags & MUX_SCTL_DBG_STR_L_CONN)
			chunk_appendf(buf, " conn.flg=%#08x conn.err_code=%u conn.evts=%s",
				      fstrm->fconn->conn->flags, fstrm->fconn->conn->err_code,
				      tevt_evts2str(fstrm->fconn->conn->term_evts_log));

		/* other layers not implemented */
		dbg_ctx->ret.buf = *buf;
		return ret;
	case MUX_SCTL_TEVTS:
		return fstrm->sd->term_evts_log;
	default:
		return -1;
	}
}

/* Connection timeout management. The principle is that if there's no receipt
 * nor sending for a certain amount of time, the connection is closed. If the
 * MUX buffer still has lying data or is not allocatable, the connection is
 * immediately killed. If it's allocatable and empty, we attempt to send a
 * ABORT records.
 */
struct task *fcgi_timeout_task(struct task *t, void *context, unsigned int state)
{
	struct fcgi_conn *fconn = context;
	int expired = tick_is_expired(t->expire, now_ms);

	TRACE_ENTER(FCGI_EV_FCONN_WAKE, (fconn ? fconn->conn : NULL));

	if (fconn) {
		HA_SPIN_LOCK(IDLE_CONNS_LOCK, &idle_conns[tid].idle_conns_lock);

		/* Somebody already stole the connection from us, so we should not
		 * free it, we just have to free the task.
		 */
		if (!t->context) {
			HA_SPIN_UNLOCK(IDLE_CONNS_LOCK, &idle_conns[tid].idle_conns_lock);
			fconn = NULL;
			goto do_leave;
		}

		if (!expired) {
			HA_SPIN_UNLOCK(IDLE_CONNS_LOCK, &idle_conns[tid].idle_conns_lock);
			TRACE_DEVEL("leaving (not expired)", FCGI_EV_FCONN_WAKE, fconn->conn);
			return t;
		}

		/* We're about to destroy the connection, so make sure nobody attempts
		 * to steal it from us.
		 */
		if (fconn->conn->flags & CO_FL_LIST_MASK)
			conn_delete_from_tree(fconn->conn);

		HA_SPIN_UNLOCK(IDLE_CONNS_LOCK, &idle_conns[tid].idle_conns_lock);

		fcgi_conn_report_term_evt(fconn, muxc_tevt_type_tout);
	}

do_leave:
	task_destroy(t);

	if (!fconn) {
		/* resources were already deleted */
		TRACE_DEVEL("leaving (not more fconn)", FCGI_EV_FCONN_WAKE);
		return NULL;
	}

	fconn->task = NULL;
	fconn->state = FCGI_CS_CLOSED;
	fcgi_wake_some_streams(fconn, 0);

	if (br_data(fconn->mbuf)) {
		/* don't even try to send aborts, the buffer is stuck */
		fconn->flags |= FCGI_CF_ABRTS_FAILED;
		goto end;
	}

	/* try to send but no need to insist */
	if (!fcgi_conn_send_aborts(fconn))
		fconn->flags |= FCGI_CF_ABRTS_FAILED;

	if (br_data(fconn->mbuf) && !(fconn->flags & FCGI_CF_ABRTS_FAILED) &&
	    conn_xprt_ready(fconn->conn)) {
		unsigned int released = 0;
		struct buffer *buf;

		for (buf = br_head(fconn->mbuf); b_size(buf); buf = br_del_head(fconn->mbuf)) {
			if (b_data(buf)) {
				int ret = fconn->conn->xprt->snd_buf(fconn->conn, fconn->conn->xprt_ctx,
								     buf, b_data(buf), 0);
				if (!ret)
					break;
				b_del(buf, ret);
				if (b_data(buf))
					break;
				b_free(buf);
				released++;
			}
		}

		if (released)
			offer_buffers(NULL, released);
	}

  end:
	/* either we can release everything now or it will be done later once
	 * the last stream closes.
	 */
	if (eb_is_empty(&fconn->streams_by_id))
		fcgi_release(fconn);

	TRACE_LEAVE(FCGI_EV_FCONN_WAKE);
	return NULL;
}


/*******************************************/
/* functions below are used by the streams */
/*******************************************/

/* Append the description of what is present in error snapshot <es> into <out>.
 * The description must be small enough to always fit in a buffer. The output
 * buffer may be the trash so the trash must not be used inside this function.
 */
static void fcgi_show_error_snapshot(struct buffer *out, const struct error_snapshot *es)
{
	chunk_appendf(out,
		      "  FCGI connection flags 0x%08x, FCGI stream flags 0x%08x\n"
		      "  H1 msg state %s(%d), H1 msg flags 0x%08x\n"
		      "  H1 chunk len %lld bytes, H1 body len %lld bytes :\n",
		      es->ctx.h1.c_flags, es->ctx.h1.s_flags,
		      h1m_state_str(es->ctx.h1.state), es->ctx.h1.state,
		      es->ctx.h1.m_flags, es->ctx.h1.m_clen, es->ctx.h1.m_blen);
}
/*
 * Capture a bad response and archive it in the proxy's structure.  By default
 * it tries to report the error position as h1m->err_pos. However if this one is
 * not set, it will then report h1m->next, which is the last known parsing
 * point. The function is able to deal with wrapping buffers. It always displays
 * buffers as a contiguous area starting at buf->p. The direction is determined
 * thanks to the h1m's flags.
 */
static void fcgi_strm_capture_bad_message(struct fcgi_conn *fconn, struct fcgi_strm *fstrm,
					  struct h1m *h1m, struct buffer *buf)
{
	struct session *sess = fstrm->sess;
	struct proxy *proxy = fconn->proxy;
	struct proxy *other_end;
	union error_snapshot_ctx ctx;

	if (fcgi_strm_sc(fstrm) && sc_strm(fcgi_strm_sc(fstrm))) {
		if (sess == NULL)
			sess = __sc_strm(fcgi_strm_sc(fstrm))->sess;
		if (!(h1m->flags & H1_MF_RESP))
			other_end = __sc_strm(fcgi_strm_sc(fstrm))->be;
		else
			other_end = sess->fe;
	} else
		other_end = NULL;
	/* http-specific part now */
	ctx.h1.state   = h1m->state;
	ctx.h1.c_flags = fconn->flags;
	ctx.h1.s_flags = fstrm->flags;
	ctx.h1.m_flags = h1m->flags;
	ctx.h1.m_clen  = h1m->curr_len;
	ctx.h1.m_blen  = h1m->body_len;

	proxy_capture_error(proxy, 1, other_end, fconn->conn->target, sess, buf, 0, 0,
			    (h1m->err_pos >= 0) ? h1m->err_pos : h1m->next,
			    &ctx, fcgi_show_error_snapshot);
}

static size_t fcgi_strm_parse_headers(struct fcgi_strm *fstrm, struct h1m *h1m, struct htx *htx,
				      struct buffer *buf, size_t *ofs, size_t max)
{
	int ret;

	TRACE_ENTER(FCGI_EV_RSP_DATA|FCGI_EV_RSP_HDRS, fstrm->fconn->conn, fstrm, 0, (size_t[]){max});
	ret = h1_parse_msg_hdrs(h1m, NULL, htx, buf, *ofs, max);
	if (ret <= 0) {
		TRACE_DEVEL("leaving on missing data or error", FCGI_EV_RSP_DATA|FCGI_EV_RSP_HDRS, fstrm->fconn->conn, fstrm);
		if (htx->flags & HTX_FL_PARSING_ERROR) {
			TRACE_ERROR("parsing error, reject H1 response", FCGI_EV_RSP_DATA|FCGI_EV_RSP_HDRS|FCGI_EV_FSTRM_ERR, fstrm->fconn->conn, fstrm);
			fcgi_strm_error(fstrm);
			fcgi_strm_capture_bad_message(fstrm->fconn, fstrm, h1m, buf);
		}
		ret = 0;
		goto end;
	}

	/* Reject any message with an unknown transfer-encoding. In fact if any
	 * encoding other than "chunked". A 422-Unprocessable-Content is
	 * returned for an invalid request, a 502-Bad-Gateway for an invalid
	 * response.
	 */
	if (h1m->flags & H1_MF_TE_OTHER) {
		htx->flags |= HTX_FL_PARSING_ERROR;
		TRACE_ERROR("Unknown transfer-encoding", FCGI_EV_RSP_DATA|FCGI_EV_RSP_HDRS|FCGI_EV_FSTRM_ERR, fstrm->fconn->conn, fstrm);
		fcgi_strm_error(fstrm);
		fcgi_strm_capture_bad_message(fstrm->fconn, fstrm, h1m, buf);
		ret = 0;
		goto end;
	}

	*ofs += ret;
  end:
	TRACE_LEAVE(FCGI_EV_RSP_DATA|FCGI_EV_RSP_HDRS, fstrm->fconn->conn, fstrm, 0, (size_t[]){ret});
	return ret;

}

static size_t fcgi_strm_parse_data(struct fcgi_strm *fstrm, struct h1m *h1m, struct htx **htx,
				   struct buffer *buf, size_t *ofs, size_t max, struct buffer *htxbuf)
{
	size_t ret;

	TRACE_ENTER(FCGI_EV_RSP_DATA|FCGI_EV_RSP_BODY, fstrm->fconn->conn, fstrm, 0, (size_t[]){max});
	ret = h1_parse_msg_data(h1m, htx, buf, *ofs, max, htxbuf);
	if (!ret) {
		TRACE_DEVEL("leaving on missing data or error", FCGI_EV_RSP_DATA|FCGI_EV_RSP_BODY, fstrm->fconn->conn, fstrm);
		if ((*htx)->flags & HTX_FL_PARSING_ERROR) {
			TRACE_ERROR("parsing error, reject H1 response", FCGI_EV_RSP_DATA|FCGI_EV_RSP_BODY|FCGI_EV_FSTRM_ERR, fstrm->fconn->conn, fstrm);
			fcgi_strm_error(fstrm);
			fcgi_strm_capture_bad_message(fstrm->fconn, fstrm, h1m, buf);
		}
		goto end;
	}
	*ofs += ret;
  end:
	TRACE_LEAVE(FCGI_EV_RSP_DATA|FCGI_EV_RSP_BODY, fstrm->fconn->conn, fstrm, 0, (size_t[]){ret});
	return ret;
}

static size_t fcgi_strm_parse_trailers(struct fcgi_strm *fstrm, struct h1m *h1m, struct htx *htx,
				       struct buffer *buf, size_t *ofs, size_t max)
{
	int ret;

	TRACE_ENTER(FCGI_EV_RSP_DATA|FCGI_EV_RSP_TLRS, fstrm->fconn->conn, fstrm, 0, (size_t[]){max});
	ret = h1_parse_msg_tlrs(h1m, htx, buf, *ofs, max);
	if (ret <= 0) {
		TRACE_DEVEL("leaving on missing data or error", FCGI_EV_RSP_DATA|FCGI_EV_RSP_TLRS, fstrm->fconn->conn, fstrm);
		if (htx->flags & HTX_FL_PARSING_ERROR) {
			TRACE_ERROR("parsing error, reject H1 response", FCGI_EV_RSP_DATA|FCGI_EV_RSP_TLRS|FCGI_EV_FSTRM_ERR, fstrm->fconn->conn, fstrm);
			fcgi_strm_error(fstrm);
			fcgi_strm_capture_bad_message(fstrm->fconn, fstrm, h1m, buf);
		}
		ret = 0;
		goto end;
	}
	*ofs += ret;
  end:
	TRACE_LEAVE(FCGI_EV_RSP_DATA|FCGI_EV_RSP_TLRS, fstrm->fconn->conn, fstrm, 0, (size_t[]){ret});
	return ret;
}

static size_t fcgi_strm_parse_response(struct fcgi_strm *fstrm, struct buffer *buf, size_t count)
{
	struct fcgi_conn *fconn = fstrm->fconn;
	struct htx *htx;
	struct h1m *h1m = &fstrm->h1m;
	size_t ret, data, total = 0;

	htx = htx_from_buf(buf);
	TRACE_ENTER(FCGI_EV_RSP_DATA, fconn->conn, fstrm, htx, (size_t[]){count});

	data = htx->data;
	if (fstrm->state == FCGI_SS_ERROR)
		goto end;

	do {
		size_t used = htx_used_space(htx);

		if (h1m->state <= H1_MSG_LAST_LF) {
			TRACE_PROTO("parsing response headers", FCGI_EV_RSP_DATA|FCGI_EV_RSP_HDRS, fconn->conn, fstrm);
			ret = fcgi_strm_parse_headers(fstrm, h1m, htx, &fstrm->rxbuf, &total, count);
			if (!ret)
				break;

			TRACE_USER("rcvd H1 response headers", FCGI_EV_RSP_DATA|FCGI_EV_RSP_HDRS, fconn->conn, fstrm, htx);

			if ((h1m->flags & (H1_MF_VER_11|H1_MF_XFER_LEN)) == H1_MF_VER_11) {
				struct htx_blk *blk = htx_get_head_blk(htx);
				struct htx_sl *sl;

				if (!blk)
					break;
				sl = htx_get_blk_ptr(htx, blk);
				sl->flags |= HTX_SL_F_XFER_LEN;
				htx->extra = 0;
			}
		}
		else if (h1m->state < H1_MSG_TRAILERS) {
			TRACE_PROTO("parsing response payload", FCGI_EV_RSP_DATA|FCGI_EV_RSP_BODY, fconn->conn, fstrm);
			fcgi_strm_parse_data(fstrm, h1m, &htx, &fstrm->rxbuf, &total, count, buf);

			if (!(h1m->flags & H1_MF_XFER_LEN) && fstrm->state != FCGI_SS_ERROR &&
			    (fstrm->flags & FCGI_SF_ES_RCVD) && b_data(&fstrm->rxbuf) == total) {
				TRACE_DEVEL("end of data", FCGI_EV_RSP_DATA, fconn->conn, fstrm);
				if (htx_is_empty(htx) && !htx_add_endof(htx, HTX_BLK_EOT))
					break;
				htx->flags |= HTX_FL_EOM;
				h1m->state = H1_MSG_DONE;
				TRACE_USER("H1 response fully rcvd", FCGI_EV_RSP_DATA|FCGI_EV_RSP_EOM, fconn->conn, fstrm, htx);
			}

			if (h1m->state < H1_MSG_TRAILERS)
				break;

			TRACE_PROTO("rcvd response payload data", FCGI_EV_RSP_DATA|FCGI_EV_RSP_BODY, fconn->conn, fstrm, htx);
		}
		else if (h1m->state == H1_MSG_TRAILERS) {
			TRACE_PROTO("parsing response trailers", FCGI_EV_RSP_DATA|FCGI_EV_RSP_TLRS, fconn->conn, fstrm);
			fcgi_strm_parse_trailers(fstrm, h1m, htx, &fstrm->rxbuf, &total, count);
			if (h1m->state != H1_MSG_DONE)
				break;

			TRACE_PROTO("rcvd H1 response trailers", FCGI_EV_RSP_DATA|FCGI_EV_RSP_TLRS, fconn->conn, fstrm, htx);
		}
		else if (h1m->state == H1_MSG_DONE) {
			TRACE_USER("H1 response fully rcvd", FCGI_EV_RSP_DATA|FCGI_EV_RSP_EOM, fconn->conn, fstrm, htx);
			if (b_data(&fstrm->rxbuf) > total) {
				htx->flags |= HTX_FL_PARSING_ERROR;
				TRACE_PROTO("too much data, parsing error", FCGI_EV_RSP_DATA, fconn->conn, fstrm);
				fcgi_strm_error(fstrm);
			}
			break;
		}
		else {
			htx->flags |= HTX_FL_PROCESSING_ERROR;
			TRACE_ERROR("unexpected processing error", FCGI_EV_RSP_DATA|FCGI_EV_STRM_ERR, fconn->conn, fstrm);
			fcgi_strm_error(fstrm);
			break;
		}

		count -= htx_used_space(htx) - used;
	} while (fstrm->state != FCGI_SS_ERROR);

	if (fstrm->state == FCGI_SS_ERROR) {
		b_reset(&fstrm->rxbuf);
		htx_to_buf(htx, buf);
		TRACE_DEVEL("leaving on error", FCGI_EV_RSP_DATA|FCGI_EV_STRM_ERR, fconn->conn, fstrm);
		return 0;
	}

	b_del(&fstrm->rxbuf, total);

  end:
	htx_to_buf(htx, buf);
	ret = htx->data - data;
	TRACE_LEAVE(FCGI_EV_RSP_DATA, fconn->conn, fstrm, htx, (size_t[]){ret});
	return ret;
}

/*
 * Attach a new stream to a connection
 * (Used for outgoing connections)
 */
static int fcgi_attach(struct connection *conn, struct sedesc *sd, struct session *sess)
{
	struct fcgi_strm *fstrm;
	struct fcgi_conn *fconn = conn->ctx;

	TRACE_ENTER(FCGI_EV_FSTRM_NEW, conn);
	fstrm = fcgi_stconn_new(fconn, sd->sc, sess);
	if (!fstrm)
		goto err;

	/* the connection is not idle anymore, let's mark this */
	HA_ATOMIC_AND(&fconn->wait_event.tasklet->state, ~TASK_F_USR1);
	xprt_set_used(conn, conn->xprt, conn->xprt_ctx);

	TRACE_LEAVE(FCGI_EV_FSTRM_NEW, conn, fstrm);
	return 0;

  err:
	TRACE_DEVEL("leaving on error", FCGI_EV_FSTRM_NEW|FCGI_EV_FSTRM_ERR, conn);
	return -1;
}

/* Retrieves the first valid stream connector from this connection, or returns NULL.
 * We have to scan because we may have some orphan streams. It might be
 * beneficial to scan backwards from the end to reduce the likeliness to find
 * orphans.
 */
static struct stconn *fcgi_get_first_sc(const struct connection *conn)
{
	struct fcgi_conn *fconn = conn->ctx;
	struct fcgi_strm *fstrm;
	struct eb32_node *node;

	node = eb32_first(&fconn->streams_by_id);
	while (node) {
		fstrm = container_of(node, struct fcgi_strm, by_id);
		if (fcgi_strm_sc(fstrm))
			return fcgi_strm_sc(fstrm);
		node = eb32_next(node);
	}
	return NULL;
}

/*
 * Destroy the mux and the associated connection, if it is no longer used
 */
static void fcgi_destroy(void *ctx)
{
	struct fcgi_conn *fconn = ctx;

	TRACE_POINT(FCGI_EV_FCONN_END, fconn->conn);
	if (eb_is_empty(&fconn->streams_by_id)) {
		BUG_ON(fconn->conn->ctx != fconn);
		fcgi_release(fconn);
	}
}

/*
 * Detach the stream from the connection and possibly release the connection.
 */
static void fcgi_detach(struct sedesc *sd)
{
	struct fcgi_strm *fstrm = sd->se;
	struct fcgi_conn *fconn;
	struct session *sess;

	TRACE_ENTER(FCGI_EV_STRM_END, (fstrm ? fstrm->fconn->conn : NULL), fstrm);

	if (!fstrm) {
		TRACE_LEAVE(FCGI_EV_STRM_END);
		return;
	}

	/* there's no txbuf so we're certain no to be able to send anything */
	fstrm->flags &= ~FCGI_SF_NOTIFIED;

	sess = fstrm->sess;
	fconn = fstrm->fconn;
	fconn->nb_sc--;

	if (fstrm->proto_status == FCGI_PS_CANT_MPX_CONN) {
		fconn->flags &= ~FCGI_CF_MPXS_CONNS;
		fconn->streams_limit = 1;
	}
	else if (fstrm->proto_status == FCGI_PS_OVERLOADED ||
		 fstrm->proto_status == FCGI_PS_UNKNOWN_ROLE) {
		fconn->flags &= ~FCGI_CF_KEEP_CONN;
		fconn->state = FCGI_CS_CLOSED;
	}

	/* this stream may be blocked waiting for some data to leave, so orphan
	 * it in this case.
	 */
	if (!(fconn->flags & (FCGI_CF_ERR_PENDING|FCGI_CF_ERROR)) && // FIXME: Be sure for ERR_PENDING
	    (fconn->state != FCGI_CS_CLOSED) &&
	    (fstrm->flags & (FCGI_SF_BLK_MBUSY|FCGI_SF_BLK_MROOM)) &&
	    (fstrm->subs || (fstrm->flags & (FCGI_SF_WANT_SHUTR|FCGI_SF_WANT_SHUTW)))) {
		TRACE_DEVEL("leaving on stream blocked", FCGI_EV_STRM_END|FCGI_EV_FSTRM_BLK, fconn->conn, fstrm);
		return;
	}

	if ((fconn->flags & FCGI_CF_DEM_BLOCK_ANY && fstrm->id == fconn->dsi)) {
		/* unblock the connection if it was blocked on this stream. */
		fconn->flags &= ~FCGI_CF_DEM_BLOCK_ANY;
		fcgi_conn_restart_reading(fconn, 1);
	}

	fcgi_strm_destroy(fstrm);

	if (!(fconn->flags & (FCGI_CF_EOS|FCGI_CF_ERR_PENDING|FCGI_CF_ERROR)) &&
	    (fconn->flags & FCGI_CF_KEEP_CONN)) {
		if (fconn->conn->flags & CO_FL_PRIVATE) {
			/* Add the connection in the session serverlist, if not already done */
			if (!session_add_conn(sess, fconn->conn))
				fconn->conn->owner = NULL;

			if (eb_is_empty(&fconn->streams_by_id)) {
				if (!fconn->conn->owner) {
					/* Session insertion above has failed and connection is idle, remove it. */
					fconn->conn->mux->destroy(fconn);
					TRACE_DEVEL("outgoing connection killed", FCGI_EV_STRM_END|FCGI_EV_FCONN_ERR);
					return;
				}

				/* mark that the tasklet may lose its context to another thread and
				 * that the handler needs to check it under the idle conns lock.
				 */
				HA_ATOMIC_OR(&fconn->wait_event.tasklet->state, TASK_F_USR1);

				/* Ensure session can keep a new idle connection. */
				if (session_check_idle_conn(sess, fconn->conn) != 0) {
					fconn->conn->mux->destroy(fconn);
					TRACE_DEVEL("outgoing connection killed", FCGI_EV_STRM_END|FCGI_EV_FCONN_ERR);
					return;
				}
			}
		}
		else {
			if (eb_is_empty(&fconn->streams_by_id)) {
				/* If the connection is owned by the session, first remove it
				 * from its list
				 */
				if (fconn->conn->owner) {
					session_unown_conn(fconn->conn->owner, fconn->conn);
					fconn->conn->owner = NULL;
				}

				/* mark that the tasklet may lose its context to another thread and
				 * that the handler needs to check it under the idle conns lock.
				 */
				HA_ATOMIC_OR(&fconn->wait_event.tasklet->state, TASK_F_USR1);
				xprt_set_idle(fconn->conn, fconn->conn->xprt, fconn->conn->xprt_ctx);

				if (!srv_add_to_idle_list(objt_server(fconn->conn->target), fconn->conn, 1)) {
					/* The server doesn't want it, let's kill the connection right away */
					fconn->conn->mux->destroy(fconn);
					TRACE_DEVEL("outgoing connection killed", FCGI_EV_STRM_END|FCGI_EV_FCONN_ERR);
					return;
				}
				/* At this point, the connection has been added to the
				 * server idle list, so another thread may already have
				 * hijacked it, so we can't do anything with it.
				 */
				TRACE_DEVEL("reusable idle connection", FCGI_EV_STRM_END, fconn->conn);
				return;
			}
			else if (!fconn->conn->hash_node->node.node.leaf_p &&
				 fcgi_avail_streams(fconn->conn) > 0 && objt_server(fconn->conn->target) &&
				 !LIST_INLIST(&fconn->conn->sess_el)) {
				srv_add_to_avail_list(__objt_server(fconn->conn->target), fconn->conn);
			}
		}
	}

	/* We don't want to close right now unless we're removing the last
	 * stream and the connection is in error.
	 */
	if (fcgi_conn_is_dead(fconn)) {
		/* no more stream will come, kill it now */
		TRACE_DEVEL("leaving, killing dead connection", FCGI_EV_STRM_END, fconn->conn);
		fcgi_release(fconn);
	}
	else if (fconn->task) {
		fconn->task->expire = tick_add(now_ms, (fconn->state == FCGI_CS_CLOSED ? fconn->shut_timeout : fconn->timeout));
		task_queue(fconn->task);
		TRACE_DEVEL("leaving, refreshing connection's timeout", FCGI_EV_STRM_END, fconn->conn);
	}
	else
		TRACE_DEVEL("leaving", FCGI_EV_STRM_END, fconn->conn);
}


/* Performs a synchronous or asynchronous shutr(). */
static void fcgi_do_shutr(struct fcgi_strm *fstrm)
{
	struct fcgi_conn *fconn = fstrm->fconn;

	TRACE_ENTER(FCGI_EV_STRM_SHUT, fconn->conn, fstrm);

	if (fstrm->state == FCGI_SS_CLOSED)
		goto done;

	/* a connstream may require us to immediately kill the whole connection
	 * for example because of a "tcp-request content reject" rule that is
	 * normally used to limit abuse.
	 */
	if (se_fl_test(fstrm->sd, SE_FL_KILL_CONN) &&
	    !(fconn->flags & (FCGI_CF_ABRTS_SENT|FCGI_CF_ABRTS_FAILED))) {
		TRACE_STATE("stream wants to kill the connection", FCGI_EV_STRM_SHUT, fconn->conn, fstrm);
		fconn->state = FCGI_CS_CLOSED;
	}
	else if (fstrm->flags & FCGI_SF_BEGIN_SENT) {
		TRACE_STATE("no headers sent yet, trying a retryable abort", FCGI_EV_STRM_SHUT, fconn->conn, fstrm);
		if (!(fstrm->flags & (FCGI_SF_ES_SENT|FCGI_SF_ABRT_SENT)) &&
		    !fcgi_strm_send_abort(fconn, fstrm))
			goto add_to_list;
	}

	fcgi_strm_close(fstrm);

	if (!(fconn->wait_event.events & SUB_RETRY_SEND))
		tasklet_wakeup(fconn->wait_event.tasklet);
  done:
	fstrm->flags &= ~FCGI_SF_WANT_SHUTR;
	TRACE_LEAVE(FCGI_EV_STRM_SHUT, fconn->conn, fstrm);
	return;

  add_to_list:
	/* Let the handler know we want to shutr, and add ourselves to the
	 * send list if not yet done. fcgi_deferred_shut() will be
	 * automatically called via the shut_tl tasklet when there's room
	 * again.
	 */
	if (!LIST_INLIST(&fstrm->send_list)) {
		if (fstrm->flags & (FCGI_SF_BLK_MBUSY|FCGI_SF_BLK_MROOM)) {
			LIST_APPEND(&fconn->send_list, &fstrm->send_list);
		}
	}
	fstrm->flags |= FCGI_SF_WANT_SHUTR;
	TRACE_LEAVE(FCGI_EV_STRM_SHUT, fconn->conn, fstrm);
	return;
}

/* Performs a synchronous or asynchronous shutw(). */
static void fcgi_do_shutw(struct fcgi_strm *fstrm)
{
	struct fcgi_conn *fconn = fstrm->fconn;

	TRACE_ENTER(FCGI_EV_STRM_SHUT, fconn->conn, fstrm);

	if (fstrm->state != FCGI_SS_HLOC || fstrm->state == FCGI_SS_CLOSED)
		goto done;

	if (fstrm->state != FCGI_SS_ERROR && (fstrm->flags & FCGI_SF_BEGIN_SENT)) {
		if (!(fstrm->flags & (FCGI_SF_ES_SENT|FCGI_SF_ABRT_SENT)) &&
		    !fcgi_strm_send_abort(fconn, fstrm))
			goto add_to_list;

		if (fstrm->state == FCGI_SS_HREM)
			fcgi_strm_close(fstrm);
		else
			fstrm->state = FCGI_SS_HLOC;
	} else {
		/* a connstream may require us to immediately kill the whole connection
		 * for example because of a "tcp-request content reject" rule that is
		 * normally used to limit abuse.
		 */
		if (se_fl_test(fstrm->sd, SE_FL_KILL_CONN) &&
		    !(fconn->flags & (FCGI_CF_ABRTS_SENT|FCGI_CF_ABRTS_FAILED))) {
			TRACE_STATE("stream wants to kill the connection", FCGI_EV_STRM_SHUT, fconn->conn, fstrm);
			fconn->state = FCGI_CS_CLOSED;
		}

		fcgi_strm_close(fstrm);
	}

	if (!(fconn->wait_event.events & SUB_RETRY_SEND))
		tasklet_wakeup(fconn->wait_event.tasklet);
  done:
	fstrm->flags &= ~FCGI_SF_WANT_SHUTW;
	TRACE_LEAVE(FCGI_EV_STRM_SHUT, fconn->conn, fstrm);
	return;

  add_to_list:
	/* Let the handler know we want to shutr, and add ourselves to the
	 * send list if not yet done. fcgi_deferred_shut() will be
	 * automatically called via the shut_tl tasklet when there's room
	 * again.
	 */
	if (!LIST_INLIST(&fstrm->send_list)) {
		if (fstrm->flags & (FCGI_SF_BLK_MBUSY|FCGI_SF_BLK_MROOM)) {
			LIST_APPEND(&fconn->send_list, &fstrm->send_list);
		}
	}
	fstrm->flags |= FCGI_SF_WANT_SHUTW;
	TRACE_LEAVE(FCGI_EV_STRM_SHUT, fconn->conn, fstrm);
	return;
}

/* This is the tasklet referenced in fstrm->shut_tl, it is used for
 * deferred shutdowns when the fcgi_detach() was done but the mux buffer was full
 * and prevented the last record from being emitted.
 */
struct task *fcgi_deferred_shut(struct task *t, void *ctx, unsigned int state)
{
	struct fcgi_strm *fstrm = ctx;
	struct fcgi_conn *fconn = fstrm->fconn;

	TRACE_ENTER(FCGI_EV_STRM_SHUT, fconn->conn, fstrm);

	if (fstrm->flags & FCGI_SF_NOTIFIED) {
		/* some data processing remains to be done first */
		goto end;
	}

	if (fstrm->flags & FCGI_SF_WANT_SHUTW)
		fcgi_do_shutw(fstrm);

	if (fstrm->flags & FCGI_SF_WANT_SHUTR)
		fcgi_do_shutr(fstrm);

	if (!(fstrm->flags & (FCGI_SF_WANT_SHUTR|FCGI_SF_WANT_SHUTW))) {
		/* We're done trying to send, remove ourself from the send_list */
		LIST_DEL_INIT(&fstrm->send_list);

		if (!fcgi_strm_sc(fstrm)) {
			fcgi_strm_destroy(fstrm);
			if (fcgi_conn_is_dead(fconn))
				fcgi_release(fconn);
		}
	}
 end:
	TRACE_LEAVE(FCGI_EV_STRM_SHUT);
	return t;
}

static void fcgi_shut(struct stconn *sc, unsigned int mode, struct se_abort_info *reason)
{
	struct fcgi_strm *fstrm = __sc_mux_strm(sc);

	TRACE_ENTER(FCGI_EV_STRM_SHUT, fstrm->fconn->conn, fstrm);
	if (mode & (SE_SHW_SILENT|SE_SHW_NORMAL))
		fcgi_do_shutw(fstrm);
	if (mode & SE_SHR_RESET)
		fcgi_do_shutr(fstrm);
	TRACE_LEAVE(FCGI_EV_STRM_SHUT, fstrm->fconn->conn, fstrm);
}

/* Called from the upper layer, to subscribe <es> to events <event_type>. The
 * event subscriber <es> is not allowed to change from a previous call as long
 * as at least one event is still subscribed. The <event_type> must only be a
 * combination of SUB_RETRY_RECV and SUB_RETRY_SEND. It always returns 0.
 */
static int fcgi_subscribe(struct stconn *sc, int event_type, struct wait_event *es)
{
	struct fcgi_strm *fstrm = __sc_mux_strm(sc);
	struct fcgi_conn *fconn = fstrm->fconn;

	BUG_ON(event_type & ~(SUB_RETRY_SEND|SUB_RETRY_RECV));
	BUG_ON(fstrm->subs && fstrm->subs != es);

	es->events |= event_type;
	fstrm->subs = es;

	if (event_type & SUB_RETRY_RECV)
		TRACE_DEVEL("unsubscribe(recv)", FCGI_EV_STRM_RECV, fconn->conn, fstrm);

	if (event_type & SUB_RETRY_SEND) {
		TRACE_DEVEL("unsubscribe(send)", FCGI_EV_STRM_SEND, fconn->conn, fstrm);
		if (!LIST_INLIST(&fstrm->send_list))
			LIST_APPEND(&fconn->send_list, &fstrm->send_list);
	}
	return 0;
}

/* Called from the upper layer, to unsubscribe <es> from events <event_type>
 * (undo fcgi_subscribe). The <es> pointer is not allowed to differ from the one
 * passed to the subscribe() call. It always returns zero.
 */
static int fcgi_unsubscribe(struct stconn *sc, int event_type, struct wait_event *es)
{
	struct fcgi_strm *fstrm = __sc_mux_strm(sc);
	struct fcgi_conn *fconn = fstrm->fconn;

	BUG_ON(event_type & ~(SUB_RETRY_SEND|SUB_RETRY_RECV));
	BUG_ON(fstrm->subs && fstrm->subs != es);

	es->events &= ~event_type;
	if (!es->events)
		fstrm->subs = NULL;

	if (event_type & SUB_RETRY_RECV)
		TRACE_DEVEL("subscribe(recv)", FCGI_EV_STRM_RECV, fconn->conn, fstrm);

	if (event_type & SUB_RETRY_SEND) {
		TRACE_DEVEL("subscribe(send)", FCGI_EV_STRM_SEND, fconn->conn, fstrm);
		fstrm->flags &= ~FCGI_SF_NOTIFIED;
		if (!(fstrm->flags & (FCGI_SF_WANT_SHUTR|FCGI_SF_WANT_SHUTW)))
			LIST_DEL_INIT(&fstrm->send_list);
	}
	return 0;
}

/* Called from the upper layer, to receive data
 *
 * The caller is responsible for defragmenting <buf> if necessary. But <flags>
 * must be tested to know the calling context. If CO_RFL_BUF_FLUSH is set, it
 * means the caller wants to flush input data (from the mux buffer and the
 * channel buffer) to be able to use kernel splicing or any kind of mux-to-mux
 * xfer. If CO_RFL_KEEP_RECV is set, the mux must always subscribe for read
 * events before giving back. CO_RFL_BUF_WET is set if <buf> is congested with
 * data scheduled for leaving soon. CO_RFL_BUF_NOT_STUCK is set to instruct the
 * mux it may optimize the data copy to <buf> if necessary. Otherwise, it should
 * copy as much data as possible.
 */
static size_t fcgi_rcv_buf(struct stconn *sc, struct buffer *buf, size_t count, int flags)
{
	struct fcgi_strm *fstrm = __sc_mux_strm(sc);
	struct fcgi_conn *fconn = fstrm->fconn;
	size_t ret = 0;

	TRACE_ENTER(FCGI_EV_STRM_RECV, fconn->conn, fstrm);

	if (!(fconn->flags & FCGI_CF_DEM_SALLOC))
		ret = fcgi_strm_parse_response(fstrm, buf, count);
	else
		TRACE_STATE("fstrm rxbuf not allocated", FCGI_EV_STRM_RECV|FCGI_EV_FSTRM_BLK, fconn->conn, fstrm);

	if (b_data(&fstrm->rxbuf)) {
		/* If the channel buffer is not empty, consider the mux is
		 * blocked because it needs more room. But if the channel buffer
		 * is empty, it means partial data were received and the mux
		 * needs to receive more data to be able to parse it.
		 */
		if (b_data(buf))
			se_fl_set(fstrm->sd, SE_FL_RCV_MORE | SE_FL_WANT_ROOM);
	}
	else {
		se_fl_clr(fstrm->sd, SE_FL_RCV_MORE | SE_FL_WANT_ROOM);
		fcgi_strm_propagate_term_flags(fconn, fstrm);
		fcgi_release_buf(fconn, &fstrm->rxbuf);
	}

	if (ret && fconn->dsi == fstrm->id) {
		/* demux is blocking on this stream's buffer */
		fconn->flags &= ~FCGI_CF_DEM_SFULL;
		fcgi_conn_restart_reading(fconn, 1);
	}

	TRACE_LEAVE(FCGI_EV_STRM_RECV, fconn->conn, fstrm);
	return ret;
}


/* Called from the upper layer, to send data from buffer <buf> for no more than
 * <count> bytes. Returns the number of bytes effectively sent. Some status
 * flags may be updated on the stream connector.
 */
static size_t fcgi_snd_buf(struct stconn *sc, struct buffer *buf, size_t count, int flags)
{
	struct fcgi_strm *fstrm = __sc_mux_strm(sc);
	struct fcgi_conn *fconn = fstrm->fconn;
	size_t total = 0;
	size_t ret;
	struct htx *htx = NULL;
	struct htx_sl *sl;
	struct htx_blk *blk;
	uint32_t bsize;

	TRACE_ENTER(FCGI_EV_STRM_SEND, fconn->conn, fstrm, 0, (size_t[]){count});

	/* If we were not just woken because we wanted to send but couldn't,
	 * and there's somebody else that is waiting to send, do nothing,
	 * we will subscribe later and be put at the end of the list
	 */
	if (!(fstrm->flags & FCGI_SF_NOTIFIED) && !LIST_ISEMPTY(&fconn->send_list)) {
		TRACE_STATE("other streams already waiting, going to the queue and leaving", FCGI_EV_STRM_SEND|FCGI_EV_FSTRM_BLK, fconn->conn, fstrm);
		return 0;
	}
	fstrm->flags &= ~FCGI_SF_NOTIFIED;

	if (fconn->state < FCGI_CS_RECORD_H) {
		TRACE_STATE("connection not ready, leaving", FCGI_EV_STRM_SEND|FCGI_EV_FSTRM_BLK, fconn->conn, fstrm);
		return 0;
	}

	htx = htxbuf(buf);
	if (fstrm->id == 0) {
		int32_t id = fcgi_conn_get_next_sid(fconn);

		if (id < 0) {
			fcgi_strm_close(fstrm);
			se_fl_set(fstrm->sd, SE_FL_ERROR);
			TRACE_DEVEL("couldn't get a stream ID, leaving in error", FCGI_EV_STRM_SEND|FCGI_EV_FSTRM_ERR|FCGI_EV_STRM_ERR, fconn->conn, fstrm);
			return 0;
		}

		eb32_delete(&fstrm->by_id);
		fstrm->by_id.key = fstrm->id = id;
		fconn->max_id = id;
		fconn->nb_reserved--;
		eb32_insert(&fconn->streams_by_id, &fstrm->by_id);


		/* Check if length of the body is known or if the message is
		 * full. Otherwise, the request is invalid.
		 */
		sl = http_get_stline(htx);
		if (!sl || (!(sl->flags & HTX_SL_F_CLEN) && !(htx->flags & HTX_FL_EOM))) {
			htx->flags |= HTX_FL_PARSING_ERROR;
			fcgi_strm_error(fstrm);
			goto done;
		}
	}

	if (!(fstrm->flags & FCGI_SF_BEGIN_SENT)) {
		TRACE_PROTO("sending FCGI BEGIN_REQUEST record", FCGI_EV_TX_RECORD|FCGI_EV_TX_BEGREQ, fconn->conn, fstrm);
		if (!fcgi_strm_send_begin_request(fconn, fstrm))
			goto done;
	}

	if (!(fstrm->flags & FCGI_SF_OUTGOING_DATA) && count)
		fstrm->flags |= FCGI_SF_OUTGOING_DATA;

	while (fstrm->state < FCGI_SS_HLOC && !(fstrm->flags & FCGI_SF_BLK_ANY) &&
	       count && !htx_is_empty(htx)) {
		blk = ASSUME_NONNULL(htx_get_head_blk(htx));
		bsize = htx_get_blksz(blk);

		switch (htx_get_blk_type(blk)) {
			case HTX_BLK_REQ_SL:
			case HTX_BLK_HDR:
				TRACE_USER("sending FCGI PARAMS record", FCGI_EV_TX_RECORD|FCGI_EV_TX_PARAMS, fconn->conn, fstrm, htx);
				ret = fcgi_strm_send_params(fconn, fstrm, htx);
				if (!ret) {
					goto done;
				}
				total += ret;
				count -= ret;
				break;

			case HTX_BLK_EOH:
				if (!(fstrm->flags & FCGI_SF_EP_SENT)) {
					TRACE_PROTO("sending FCGI PARAMS record", FCGI_EV_TX_RECORD|FCGI_EV_TX_PARAMS, fconn->conn, fstrm, htx);
					ret = fcgi_strm_send_empty_params(fconn, fstrm);
					if (!ret)
						goto done;
				}
				if (htx_is_unique_blk(htx, blk) && (htx->flags & HTX_FL_EOM)) {
					TRACE_PROTO("sending FCGI STDIN record", FCGI_EV_TX_RECORD|FCGI_EV_TX_STDIN, fconn->conn, fstrm, htx);
					ret = fcgi_strm_send_empty_stdin(fconn, fstrm);
					if (!ret)
						goto done;
				}
				goto remove_blk;

			case HTX_BLK_DATA:
				TRACE_PROTO("sending FCGI STDIN record", FCGI_EV_TX_RECORD|FCGI_EV_TX_STDIN, fconn->conn, fstrm, htx);
				ret = fcgi_strm_send_stdin(fconn, fstrm, htx, count, buf);
				if (ret > 0) {
					htx = htx_from_buf(buf);
					total += ret;
					count -= ret;
					if (ret < bsize)
						goto done;
				}
				break;

			case HTX_BLK_EOT:
				if (htx_is_unique_blk(htx, blk) && (htx->flags & HTX_FL_EOM)) {
					TRACE_PROTO("sending FCGI STDIN record", FCGI_EV_TX_RECORD|FCGI_EV_TX_STDIN, fconn->conn, fstrm, htx);
					ret = fcgi_strm_send_empty_stdin(fconn, fstrm);
					if (!ret)
						goto done;
				}
				__fallthrough;

			default:
			  remove_blk:
				htx_remove_blk(htx, blk);
				total += bsize;
				count -= bsize;
				break;
		}
	}

  done:
	if (fstrm->state >= FCGI_SS_HLOC) {
		/* trim any possibly pending data after we close (extra CR-LF,
		 * unprocessed trailers, abnormal extra data, ...)
		 */
		total += count;
		count = 0;
	}

	if (fstrm->state == FCGI_SS_ERROR) {
		TRACE_DEVEL("reporting error to the app-layer stream", FCGI_EV_STRM_SEND|FCGI_EV_FSTRM_ERR|FCGI_EV_STRM_ERR, fconn->conn, fstrm);
		se_fl_set_error(fstrm->sd);
		se_report_term_evt(fstrm->sd, se_tevt_type_snd_err);
		if (!(fstrm->flags & FCGI_SF_BEGIN_SENT) || fcgi_strm_send_abort(fconn, fstrm))
			fcgi_strm_close(fstrm);
	}

	if (htx)
		htx_to_buf(htx, buf);

	if (total > 0) {
		if (!(fconn->wait_event.events & SUB_RETRY_SEND)) {
			TRACE_DEVEL("data queued, waking up fconn sender", FCGI_EV_STRM_SEND|FCGI_EV_FCONN_SEND|FCGI_EV_FCONN_WAKE, fconn->conn, fstrm);
			tasklet_wakeup(fconn->wait_event.tasklet);
		}

		/* Ok we managed to send something, leave the send_list */
		if (!(fstrm->flags & (FCGI_SF_WANT_SHUTR|FCGI_SF_WANT_SHUTW)))
			LIST_DEL_INIT(&fstrm->send_list);
	}

	TRACE_LEAVE(FCGI_EV_STRM_SEND, fconn->conn, fstrm, htx, (size_t[]){total});
	return total;
}

/* appends some info about stream <fstrm> to buffer <msg>, or does nothing if
 * <fstrm> is NULL. Returns non-zero if the stream is considered suspicious. May
 * emit multiple lines, each new one being prefixed with <pfx>, if <pfx> is not
 * NULL, otherwise a single line is used.
 */
static int fcgi_dump_fcgi_strm_info(struct buffer *msg, const struct fcgi_strm *fstrm, const char *pfx)
{
	int ret = 0;

	if (!fstrm)
		return ret;

	chunk_appendf(msg, " .id=%d .flg=0x%04x .rxbuf=%u@%p+%u/%u .sc=%p",
		      fstrm->id, fstrm->flags,
		      (unsigned int)b_data(&fstrm->rxbuf), b_orig(&fstrm->rxbuf),
		      (unsigned int)b_head_ofs(&fstrm->rxbuf), (unsigned int)b_size(&fstrm->rxbuf),
		      fcgi_strm_sc(fstrm));

	if (pfx && fstrm->subs)
		chunk_appendf(msg, "\n%s", pfx);

	chunk_appendf(msg, " .sd.flg=0x%08x .sd.evts=%s", se_fl_get(fstrm->sd), tevt_evts2str(fstrm->sd->term_evts_log));
	if (!se_fl_test(fstrm->sd, SE_FL_ORPHAN))
		chunk_appendf(msg, " .sc.flg=0x%08x .sc.app=%p .sc.evts=%s",
			      fcgi_strm_sc(fstrm)->flags, fcgi_strm_sc(fstrm)->app, tevt_evts2str(fcgi_strm_sc(fstrm)->term_evts_log));

	if (pfx && fstrm->subs)
		chunk_appendf(msg, "\n%s", pfx);

	chunk_appendf(msg, " .subs=%p", fstrm->subs);
	if (fstrm->subs) {
		chunk_appendf(msg, "(ev=%d tl=%p", fstrm->subs->events, fstrm->subs->tasklet);
		chunk_appendf(msg, " tl.calls=%d tl.ctx=%p tl.fct=",
			      fstrm->subs->tasklet->calls,
			      fstrm->subs->tasklet->context);
		if (fstrm->subs->tasklet->calls >= 1000000)
			ret = 1;
		resolve_sym_name(msg, NULL, fstrm->subs->tasklet->process);
		chunk_appendf(msg, ")");
	}
	return ret;
}

/* appends some info about connection <fconn> to buffer <msg>, or does nothing
 * if <fconn> is NULL. Returns non-zero if the connection is considered
 * suspicious.  May emit multiple lines, each new one being prefixed with <pfx>,
 * if <pfx> is not NULL, otherwise a single line is used.
 */
static int fcgi_dump_fcgi_conn_info(struct buffer *msg, struct fcgi_conn *fconn, const char *pfx)
{
	const struct buffer *hmbuf, *tmbuf;
	struct fcgi_strm *fstrm = NULL;
	struct eb32_node *node;
	int send_cnt = 0;
	int tree_cnt = 0;
	int orph_cnt = 0;
	int ret = 0;

	if (!fconn)
		return ret;

	list_for_each_entry(fstrm, &fconn->send_list, send_list)
		send_cnt++;

	fstrm = NULL;
	node = eb32_first(&fconn->streams_by_id);
	while (node) {
		fstrm = container_of(node, struct fcgi_strm, by_id);
		tree_cnt++;
		if (!fcgi_strm_sc(fstrm))
			orph_cnt++;
		node = eb32_next(node);
	}

	hmbuf = br_head(fconn->mbuf);
	tmbuf = br_tail(fconn->mbuf);
	chunk_appendf(msg, " fconn.st0=%d .maxid=%d .flg=0x%04x .nbst=%u .nbcs=%u .evts=%s",
		      fconn->state, fconn->max_id, fconn->flags,
		      fconn->nb_streams, fconn->nb_sc,
		      tevt_evts2str(fconn->term_evts_log));

	if (pfx)
		chunk_appendf(msg, "\n%s", pfx);

	chunk_appendf(msg, ".send_cnt=%d .tree_cnt=%d .orph_cnt=%d  .sub=%d .dsi=%d .dbuf=%u@%p+%u/%u",
		      send_cnt, tree_cnt, orph_cnt, fconn->wait_event.events, fconn->dsi,
		      (unsigned int)b_data(&fconn->dbuf), b_orig(&fconn->dbuf),
		      (unsigned int)b_head_ofs(&fconn->dbuf), (unsigned int)b_size(&fconn->dbuf));


	if (pfx)
		chunk_appendf(msg, "\n%s", pfx);

	chunk_appendf(msg, ".mbuf=[%u..%u|%u],h=[%u@%p+%u/%u],t=[%u@%p+%u/%u]",
		      br_head_idx(fconn->mbuf), br_tail_idx(fconn->mbuf), br_size(fconn->mbuf),
		      (unsigned int)b_data(hmbuf), b_orig(hmbuf),
		      (unsigned int)b_head_ofs(hmbuf), (unsigned int)b_size(hmbuf),
		      (unsigned int)b_data(tmbuf), b_orig(tmbuf),
		      (unsigned int)b_head_ofs(tmbuf), (unsigned int)b_size(tmbuf));

	chunk_appendf(msg, " .task=%p", fconn->task);
	if (fconn->task) {
		chunk_appendf(msg, " .exp=%s",
			      fconn->task->expire ? tick_is_expired(fconn->task->expire, now_ms) ? "<PAST>" :
			      human_time(TICKS_TO_MS(fconn->task->expire - now_ms), TICKS_TO_MS(1000)) : "<NEVER>");
	}

	return ret;
}



/* for debugging with CLI's "show fd" command */
static int fcgi_show_fd(struct buffer *msg, struct connection *conn)
{
	struct fcgi_conn *fconn = conn->ctx;
	struct fcgi_strm *fstrm = NULL;
	struct eb32_node *node;
	int ret = 0;

	if (!fconn)
		return ret;

	ret |= fcgi_dump_fcgi_conn_info(msg, fconn, NULL);

	node = eb32_first(&fconn->streams_by_id);
	if (node) {
		fstrm = container_of(node, struct fcgi_strm, by_id);
		chunk_appendf(msg, " last_fstrm=%p", fstrm);
		ret |= fcgi_dump_fcgi_strm_info(msg, fstrm, NULL);
	}

	return ret;
}


/* for debugging with CLI's "show sess" command. May emit multiple lines, each
 * new one being prefixed with <pfx>, if <pfx> is not NULL, otherwise a single
 * line is used. Each field starts with a space so it's safe to print it after
 * existing fields.
 */
static int fcgi_show_sd(struct buffer *msg, struct sedesc *sd, const char *pfx)
{
	struct fcgi_strm *fstrm = sd->se;
	int ret = 0;

	if (!fstrm)
		return ret;

	chunk_appendf(msg, " fstrm=%p", fstrm);
	ret |= fcgi_dump_fcgi_strm_info(msg, fstrm, pfx);
	if (pfx)
		chunk_appendf(msg, "\n%s", pfx);
	chunk_appendf(msg, " fconn=%p", fstrm->fconn);
	ret |= fcgi_dump_fcgi_conn_info(msg, fstrm->fconn, pfx);
	return ret;
}

/* Migrate the the connection to the current thread.
 * Return 0 if successful, non-zero otherwise.
 * Expected to be called with the old thread lock held.
 */
static int fcgi_takeover(struct connection *conn, int orig_tid, int release)
{
	struct fcgi_conn *fcgi = conn->ctx;
	struct task *task;
	struct task *new_task = NULL;
	struct tasklet *new_tasklet = NULL;

	/* Pre-allocate tasks so that we don't have to roll back after the xprt
	 * has been migrated.
	 */
	if (!release) {
		/* If the connection is attached to a buffer_wait (extremely
		 * rare), it will be woken up at any instant by its own thread
		 * and we can't undo it anyway, so let's give up on this one.
		 * It's not interesting anyway since it's not usable right now.
		 */
		if (LIST_INLIST(&fcgi->buf_wait.list))
			goto fail;

		new_task = task_new_here();
		new_tasklet = tasklet_new();
		if (!new_task || !new_tasklet)
			goto fail;
	}

	if (fd_takeover(conn->handle.fd, conn) != 0)
		goto fail;

	if (conn->xprt->takeover && conn->xprt->takeover(conn, conn->xprt_ctx, orig_tid, release) != 0) {
		/* We failed to takeover the xprt, even if the connection may
		 * still be valid, flag it as error'd, as we have already
		 * taken over the fd, and wake the tasklet, so that it will
		 * destroy it.
		 */
		conn->flags |= CO_FL_ERROR;
		tasklet_wakeup_on(fcgi->wait_event.tasklet, orig_tid);
		goto fail;
	}

	if (fcgi->wait_event.events)
		fcgi->conn->xprt->unsubscribe(fcgi->conn, fcgi->conn->xprt_ctx,
		    fcgi->wait_event.events, &fcgi->wait_event);

	task = fcgi->task;
	if (task) {
		/* only assign a task if there was already one, otherwise
		 * the preallocated new task will be released.
		 */
		task->context = NULL;
		fcgi->task = NULL;
		__ha_barrier_store();
		task_kill(task);

		fcgi->task = new_task;
		new_task = NULL;
		if (!release) {
			fcgi->task->process = fcgi_timeout_task;
			fcgi->task->context = fcgi;
		}
	}

	/* To let the tasklet know it should free itself, and do nothing else,
	 * set its context to NULL;
	 */
	fcgi->wait_event.tasklet->context = NULL;
	tasklet_wakeup_on(fcgi->wait_event.tasklet, orig_tid);

	fcgi->wait_event.tasklet = new_tasklet;
	if (!release) {
		fcgi->wait_event.tasklet->process = fcgi_io_cb;
		fcgi->wait_event.tasklet->context = fcgi;
		fcgi->conn->xprt->subscribe(fcgi->conn, fcgi->conn->xprt_ctx,
		                            SUB_RETRY_RECV, &fcgi->wait_event);
	}

	if (release) {
		/* we're being called for a server deletion and are running
		 * under thread isolation. That's the only way we can
		 * unregister a possible subscription of the original
		 * connection from its owner thread's queue, as this involves
		 * manipulating thread-unsafe areas. Note that it is not
		 * possible to just call b_dequeue() here as it would update
		 * the current thread's bufq_map and not the original one.
		 */
		BUG_ON(!thread_isolated());
		if (LIST_INLIST(&fcgi->buf_wait.list))
			_b_dequeue(&fcgi->buf_wait, orig_tid);
	}

	if (new_task)
		__task_free(new_task);
	return 0;
 fail:
	if (new_task)
		__task_free(new_task);
	tasklet_free(new_tasklet);
	return -1;
}

/****************************************/
/* MUX initialization and instantiation */
/****************************************/

/* The mux operations */
static const struct mux_ops mux_fcgi_ops = {
	.init          = fcgi_init,
	.wake          = fcgi_wake,
	.attach        = fcgi_attach,
	.get_first_sc  = fcgi_get_first_sc,
	.detach        = fcgi_detach,
	.destroy       = fcgi_destroy,
	.avail_streams = fcgi_avail_streams,
	.used_streams  = fcgi_used_streams,
	.rcv_buf       = fcgi_rcv_buf,
	.snd_buf       = fcgi_snd_buf,
	.subscribe     = fcgi_subscribe,
	.unsubscribe   = fcgi_unsubscribe,
	.shut          = fcgi_shut,
	.ctl           = fcgi_ctl,
	.sctl           = fcgi_sctl,
	.show_fd       = fcgi_show_fd,
	.show_sd       = fcgi_show_sd,
	.takeover      = fcgi_takeover,
	.flags         = MX_FL_HTX|MX_FL_HOL_RISK|MX_FL_NO_UPG,
	.name          = "FCGI",
};


/* this mux registers FCGI proto */
static struct mux_proto_list mux_proto_fcgi =
{ .token = IST("fcgi"), .mode = PROTO_MODE_HTTP, .side = PROTO_SIDE_BE, .mux = &mux_fcgi_ops };

INITCALL1(STG_REGISTER, register_mux_proto, &mux_proto_fcgi);

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
