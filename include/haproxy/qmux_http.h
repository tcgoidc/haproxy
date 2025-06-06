#ifndef _HAPROXY_MUX_QUIC_HTTP_H
#define _HAPROXY_MUX_QUIC_HTTP_H

#ifdef USE_QUIC

#include <haproxy/buf.h>
#include <haproxy/mux_quic.h>

size_t qcs_http_rcv_buf(struct qcs *qcs, struct buffer *buf, size_t count,
                        char *fin);

int qcs_http_handle_standalone_fin(struct qcs *qcs);

size_t qcs_http_snd_buf(struct qcs *qcs, struct buffer *buf, size_t count,
                        char *fin);

#endif /* USE_QUIC */

#endif /* _HAPROXY_MUX_QUIC_HTTP_H */
