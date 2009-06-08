/*
 * This file is part of Firestorm NIDS
 * Copyright (c) Gianni Tedesco 2008
 * This program is released under the terms of the GNU GPL version 3
 *
 * TODO:
 *  o Put state data in to DCB
 *  o Handle ICMP errors
 *  o Reassembly
 *  o Application layer infrastructure
 *  o check for broadcasts if possible
*/
#include <firestorm.h>
#include <f_packet.h>
#include <f_decode.h>
#include <pkt/ip.h>
#include <pkt/tcp.h>
#include <pkt/icmp.h>
#include <p_ipv4.h>

#include "tcpip.h"

#define STATE_DEBUG 1
#define SEGMENT_DEBUG 1
#define STREAM_DEBUG 1

#if STATE_DEBUG
#define dmesg mesg
#define dhex_dump hex_dump
#else
#define dmesg(x...) do{}while(0);
#define dhex_dump(x...) do{}while(0);
#endif

#if SEGMENT_DEBUG
#include <stdio.h>
#include <stdarg.h>
#endif

static const uint8_t minttl = 1;

#define TCP_PAWS_24DAYS (60 * 60 * 24 * 24)
#define TCP_PAWS_MSL 60
#define TCP_PAWS_WINDOW 60

#define TCP_TMO_SYN1 (90 * TIMESTAMP_HZ)

struct tcpseg {
	struct tcpflow *tf;
	timestamp_t ts;
	const struct pkt_iphdr *iph;
	const struct pkt_tcphdr *tcph;
	uint32_t ack, seq, win, seq_end;
	uint16_t hash, len;
	uint32_t tsval;
	unsigned int saw_tstamp;
	uint8_t *payload;
	struct tcp_state *snd, *rcv;
};

static void dbg_segment(struct tcpseg *cur)
{
#if SEGMENT_DEBUG
	static const char tcpflags[] = "FSRPAUEC";
	uint8_t x, i;
	char ackbuf[16];
	char fstr[9];
	ipstr_t sip, dip;

	iptostr(sip, cur->iph->saddr);
	iptostr(dip, cur->iph->daddr);

	for(i = 0, x = 1; x; i++, x <<= 1)
		fstr[i] = (cur->tcph->flags & x) ? tcpflags[i] : '*';

	fstr[i] = '\0';

	if ( cur->tcph->flags & TCP_ACK ) {
		snprintf(ackbuf, sizeof(ackbuf), " a:%x", cur->ack);
	}else{
		ackbuf[0] = '\0';
	}

	mesg(M_DEBUG, "\033[36m[%s] %s:%u %s:%u s:%x%s w:%u l:%u\033[0m", fstr,
		sip, sys_be16(cur->tcph->sport),
		dip, sys_be16(cur->tcph->dport),
		cur->seq, ackbuf, cur->win, cur->len);
#endif
}

static void dbg_stream(const char *label, struct tcp_state *s)
{
#if STREAM_DEBUG
	if ( NULL == s )
		return;
	mesg(M_DEBUG, "\033[34m%s: su=%.8x sn=%.8x n=%.8x wup=%.8x w=%u\033[0m",
		label, s->snd_una, s->snd_nxt,
		s->rcv_nxt, s->rcv_wup, s->rcv_wnd);
#endif
}

static void state_err(struct tcpseg *cur, const char *msg)
{
#if STATE_DEBUG
	ipstr_t sip, dip;

	iptostr(sip, cur->iph->saddr);
	iptostr(dip, cur->iph->daddr);
	mesg(M_ERR, "%s:%u -> %s:%u - %s",
		sip, sys_be16(cur->tcph->sport),
		dip, sys_be16(cur->tcph->dport), msg);
#endif
}

/* Wrap-safe seq/ack calculations */
static int between(uint32_t s1, uint32_t s2, uint32_t s3)
{
	return s3 - s2 >= s1 - s2; /* is s2<=s1<=s3 ? */
}

static uint32_t tcp_receive_window(struct tcp_state *s)
{
	int32_t win = s->rcv_wup + s->rcv_wnd - s->rcv_nxt;
	if ( win < 0 )
		win = 0;
	return (uint32_t)win;
}

static int tcp_sequence(struct tcp_state *s, uint32_t seq, uint32_t end_seq)
{
	return !tcp_before(end_seq, s->rcv_wup) &&
		!tcp_after(seq, s->rcv_nxt + tcp_receive_window(s));
}

/* Hash function.
 * Hashes to the same value even when source and destinations are inverted.
 */
_constfn static uint16_t tcp_hashfn(uint32_t saddr, uint32_t daddr,
					uint16_t sport, uint16_t dport)
{
	uint32_t h;
	h = ((saddr ^ sport) ^ (daddr ^ dport));
	h ^= h >> 16;
	h ^= h >> 8;
	h %= TCPHASH;
	return h;
}

/* HASH: Unlink a session from the session hash */
static void tcp_hash_unlink(struct tcp_session *s)
{
	if (s->hash_next)
		s->hash_next->hash_pprev = s->hash_pprev;
	*s->hash_pprev = s->hash_next;
}

/* HASH: Link a session in to the TCP session hash */
static void tcp_hash_link(struct tcpflow *tf, struct tcp_session *s,
				uint16_t bucket)
{
	if ((s->hash_next = tf->hash[bucket]))
		s->hash_next->hash_pprev = &s->hash_next;
	tf->hash[bucket] = s;
	s->hash_pprev = &tf->hash[bucket];
}

/* HASH: Move to front of hash collision chain */
static void tcp_hash_mtf(struct tcpflow *tf, struct tcp_session *s,
				uint16_t bucket)
{
	tcp_hash_unlink(s);
	tcp_hash_link(tf, s, bucket);
}

/* Find a TCP session given a packet */
static struct tcp_session *tcp_collide(struct tcp_session *s,
					const struct pkt_iphdr *iph,
					const struct pkt_tcphdr *tcph,
					unsigned int *to_server)
{
	for (; s; s = s->hash_next) {
		if (	s->s_addr == iph->saddr &&
			s->c_addr == iph->daddr &&
			s->s_port == tcph->sport &&
			s->c_port == tcph->dport ) {
			*to_server = 0;
			return s;
		}
		if (	s->c_addr == iph->saddr &&
			s->s_addr == iph->daddr &&
			s->c_port == tcph->sport &&
			s->s_port == tcph->dport ) {
			*to_server = 1;
			return s;
		}
	}

	return NULL;
}

/* Parse TCP options just for timestamps */
static int tcp_fast_options(struct tcpseg *cur)
{
	const struct pkt_tcphdr *t = cur->tcph;
	char *tmp, *end;
	size_t ofs = t->doff << 2;

	/* Return if we don't have any */
	if ( ofs <= sizeof(struct pkt_tcphdr))
		return 0;

	/* Work out where they begin and end */
	tmp = end = (char *)t;
	tmp += sizeof(struct pkt_tcphdr);
	end += ofs;

	while ( tmp < end ) {
		size_t step;

		/* XXX: We continue past an EOL. Is that right? */
		switch ( *tmp ) {
		case TCPOPT_EOL:
		case TCPOPT_NOP:
			tmp++;
			continue;
		}

		if ( tmp+1 >= end )
			break;

		switch ( *tmp ) {
		case TCPOPT_TIMESTAMP:
			if ( tmp + 10 >= end )
				break;
			cur->tsval = sys_be32(*((uint32_t *)(tmp + 2)));
			return 1;
		}

		step = *(tmp + 1);
		if ( step < 2 ) {
			dmesg(M_DEBUG, "Malicious tcp options");
			step = 2;
		}
		tmp += step;
	}

	return 0;
}

/* This will parse TCP options for SYN packets */
static void tcp_syn_options(struct tcp_state *s,
				const struct pkt_tcphdr *t,
				uint32_t sec)
{
	uint8_t *tmp, *end;
	size_t ofs = t->doff << 2;

	/* Return if we don't have any */
	if ( ofs <= sizeof(struct pkt_tcphdr))
		return;

	/* Work out where they begin and end */
	tmp = end = (uint8_t *)t;
	tmp += sizeof(struct pkt_tcphdr);
	end += ofs;

	while ( tmp < end ) {
		size_t step;

		/* XXX: We continue past an EOL. Is that right? */
		switch(*tmp) {
		case TCPOPT_EOL:
		case TCPOPT_NOP:
			tmp++;
			continue;
		}

		if ( tmp + 1 >= end )
			break;

		/* Deal with fixed size options */
		switch ( *tmp ) {
		case TCPOPT_SACK_PERMITTED:
			s->flags |= TF_SACK_OK;
			break;
		case TCPOPT_TIMESTAMP:
			s->flags |= TF_TSTAMP_OK;

			/* Only check the bit we want */
			if ( tmp + 10 >= end )
				break;

			s->ts_recent = sys_be32(*((uint32_t *)(tmp + 2)));
			s->ts_recent_stamp = sec;

			break;
		case TCPOPT_WSCALE:
			if ( tmp + 2 >= end )
				break;

			s->flags |= TF_WSCALE_OK;

			/* rfc1323: must log error and limit to 14 */
			s->scale = *(tmp + 2);
			if ( s->scale > 14 )
				s->scale = 14;
			break;
		}

		step = *(tmp + 1);
		if ( step < 2 ) {
			dmesg(M_WARN, "Malicious tcp options");
			step = 2;
		}

		tmp += step;
	}
}

static void tcp_free(struct tcpflow *tf, struct tcp_session *s)
{
	tcp_hash_unlink(s);
	list_del(&s->lru);
	if ( s->s_wnd )
		objcache_free(tf->sstate_cache, s->s_wnd);
	memset(s, 0xa5, sizeof(*s));
	objcache_free(tf->session_cache, s);
	tf->num_active--;
}

/* TMO: Check timeouts */
#if 0
static void tcp_tmo_check(struct tcpflow *tf, timestamp_t now)
{
	struct tcp_session *s;

	/* Actually, the generic list API made this more ugly */
	while ( !list_empty(&tf->syn1) ) {
		s = list_entry(tf->syn1.prev, struct tcp_session, tmo);
		if ( !time_before(s->expire, now) )
			return;

		tcp_free(tf, s);
		tf->num_timeouts++;
	}
}

/* TMO: Set expiry */
static void tcp_expire(struct tcpflow *tf,
				struct tcp_session *s,
				timestamp_t t)
{
	s->expire = t;
	list_move(&tf->syn1, &s->tmo);
}
#endif

static void init_wnd(struct tcpseg *cur, struct tcp_state *s)
{
	memset(s, 0, sizeof(*s));
	s->snd_una = cur->seq + 1;
	s->snd_nxt = s->snd_una + 1;
	s->rcv_wnd = cur->win;
	s->rcv_wup = s->rcv_nxt;
	tcp_syn_options(s, cur->tcph, cur->ts / TIMESTAMP_HZ);
}

static struct tcp_session *new_session(struct tcpseg *cur)
{
	struct tcp_session *s;

	/* Track syn packets only for now. This could be re-jiggled for
	 * flow accounting:
	 *  - move this check after allocation
	 *  - for stray packets: don't transition + keep server/client zeroed
	 */
	if ( (cur->tcph->flags & (TCP_SYN|TCP_ACK|TCP_FIN|TCP_RST))
			!= TCP_SYN ) {
		state_err(cur, "not a valid syn packet");
		return NULL;
	}

	s = objcache_alloc(cur->tf->session_cache);
	if ( s == NULL ) {
		mesg(M_CRIT, "tcp OOM");
		return NULL;
	}

	dmesg(M_DEBUG, "#1 - syn: half-state allocated");

	s->c_addr = cur->iph->saddr;
	s->s_addr = cur->iph->daddr;
	s->c_port = cur->tcph->sport;
	s->s_port = cur->tcph->dport;

	s->state = TCP_SESSION_S1;

	/* stats */
	cur->tf->num_active++;
	if ( cur->tf->num_active > cur->tf->max_active )
		cur->tf->max_active = cur->tf->num_active;

	/* Setup initial window tracking state machine */
	init_wnd(cur, &s->c_wnd);
	s->s_wnd = NULL;

	/* link it all up and set up timeouts*/
	list_add(&s->lru, &cur->tf->tmo_30);
	tcp_hash_link(cur->tf, s, cur->hash);

	return s;
}

static void s1_processing(struct tcpseg *cur, struct tcp_session *s)
{
	if ( cur->tcph->flags & (TCP_FIN|TCP_RST) ) {
		/* FIXME: apply seq check */
		mesg(M_DEBUG, "connection refused");
		s->state = TCP_SESSION_C;
		return;
	}

	if ( cur->tcph->flags & TCP_SYN ) {
		dmesg(M_DEBUG, "#2 syn+ack");
		s->s_wnd = objcache_alloc(cur->tf->sstate_cache);
		assert(NULL != s->s_wnd);
		cur->snd = s->s_wnd;
		init_wnd(cur, s->s_wnd);
		s->state = TCP_SESSION_S2;
	}
}

static void s2_processing(struct tcpseg *cur, struct tcp_session *s)
{
	if ( cur->tcph->flags & TCP_ACK ) {
		dmesg(M_DEBUG, "#3 ack");
		s->state = TCP_SESSION_S3;
	}
}

static void e_processing(struct tcpseg *cur, struct tcp_session *s)
{
	dhex_dump(cur->payload, cur->len, 16);
	if ( cur->tcph->flags & TCP_FIN) {
		if ( cur->snd == &s->c_wnd ) {
			dmesg(M_DEBUG, "client close");
			s->state = TCP_SESSION_CF1;
		}else{
			dmesg(M_DEBUG, "server close");
			s->state = TCP_SESSION_SF1;
		}
	}
}

static void f1_processing(struct tcpseg *cur, struct tcp_session *s)
{
	struct tcp_state *closer;

	if ( s->state == TCP_SESSION_CF1 )
		closer = &s->c_wnd;
	else
		closer = s->s_wnd;

	if ( cur->snd == closer ) {
		dmesg(M_DEBUG, "fin resend?");
		return;
	}

	if ( cur->tcph->flags & TCP_ACK ) {
		dmesg(M_DEBUG, "ack for fin");
		s->state++;
	}

	if ( cur->tcph->flags & TCP_FIN) {
		dmesg(M_DEBUG, "simultaneous close");
		s->state++;
	}
}

static void f2_processing(struct tcpseg *cur, struct tcp_session *s)
{
	struct tcp_state *closer;

	if ( s->state == TCP_SESSION_CF2 )
		closer = &s->c_wnd;
	else
		closer = s->s_wnd;

	if ( cur->snd != closer && cur->tcph->flags & TCP_FIN ) {
		dmesg(M_DEBUG, "final fin");
		s->state++;
	}
}

static void f3_processing(struct tcpseg *cur, struct tcp_session *s)
{
	struct tcp_state *closer;

	if ( s->state == TCP_SESSION_CF3 )
		closer = &s->c_wnd;
	else
		closer = s->s_wnd;

	if ( cur->snd == closer && cur->tcph->flags & TCP_ACK ) {
		dmesg(M_DEBUG, "teardown");
		s->state = TCP_SESSION_C;
	}
}

static void state_track(struct tcpseg *cur, struct tcp_session *s)
{
	switch(s->state) {
	case TCP_SESSION_S1:
		if ( cur->snd != &s->c_wnd )
			s1_processing(cur, s);
		else
			dmesg(M_DEBUG, "syn resend?");
		break;
	case TCP_SESSION_S2:
		if ( cur->snd == &s->c_wnd )
			s2_processing(cur, s);
		else
			dmesg(M_DEBUG, "syn+ack resend?");
		break;
	case TCP_SESSION_S3:
		if ( cur->snd == &s->c_wnd )
			dmesg(M_DEBUG, "client sent first data");
		else
			dmesg(M_DEBUG, "server sent first data");
		s->state = TCP_SESSION_E;
		/* fall through */
	case TCP_SESSION_E:
		e_processing(cur, s);
		break;
	case TCP_SESSION_CF1:
	case TCP_SESSION_SF1:
		f1_processing(cur, s);
		break;
	case TCP_SESSION_CF2:
	case TCP_SESSION_SF2:
		f2_processing(cur, s);
		break;
	case TCP_SESSION_CF3:
	case TCP_SESSION_SF3:
		f3_processing(cur, s);
		break;
	case TCP_SESSION_C:
		dmesg(M_DEBUG, "2MSL timeout");
		break;
	}

	/* TODO: update timeouts */
	list_move(&cur->tf->lru, &s->lru);
}

static int tcp_csum(struct tcpseg *cur)
{
	struct tcp_phdr ph;
	uint16_t *tmp;
	uint32_t sum = 0;
	uint16_t csum, len;
	int i;

	len = sys_be16(cur->iph->tot_len) - (cur->iph->ihl << 2);

	/* Make pseudo-header */
	ph.sip = cur->iph->saddr;
	ph.dip = cur->iph->daddr;
	ph.zero = 0;
	ph.proto = cur->iph->protocol;
	ph.tcp_len = sys_be16(len);

	/* Checksum the pseudo-header */
	tmp = (uint16_t *)&ph;
	for(i = 0; i < 6; i++)
		sum += tmp[i];

	/* Checksum the header+data */
	tmp = (uint16_t *)cur->tcph;
	for(i = 0; i < (len >> 1); i++)
		sum += tmp[i];

	/* Deal with last byte (if odd number of bytes) */
	if ( len & 1 ) {
		union {
			uint8_t b[2];
			uint16_t s;
		}f;

		f.b[0] = ((uint8_t *)cur->tcph)[len - 1];
		f.b[1] = 0;
		sum += f.s;
	}

	sum = (sum & 0xffff) + (sum >> 16);

	csum = ~sum & 0xffff;

	return (csum == 0);
}

static void seg_init(struct tcpseg *cur, struct ip_flow_state *ipfs,
		pkt_t pkt, struct tcp_dcb *dcb)
{
	cur->tf = &ipfs->tcpflow;

	cur->ts = pkt->pkt_ts;
	cur->iph = dcb->tcp_iph;
	cur->tcph = dcb->tcp_hdr;
	cur->ack = sys_be32(cur->tcph->ack);
	cur->seq = sys_be32(cur->tcph->seq);
	cur->win = sys_be16(cur->tcph->win);
	cur->hash = tcp_hashfn(cur->iph->saddr, cur->iph->daddr,
				cur->tcph->sport, cur->tcph->dport);
	cur->len = sys_be16(cur->iph->tot_len) -
			(cur->iph->ihl << 2) -
			(cur->tcph->doff << 2);
	cur->seq_end = cur->seq + cur->len;
	cur->tsval = 0;
	cur->saw_tstamp = 0;
	cur->payload = (uint8_t *)cur->tcph + (cur->tcph->doff << 2);

	cur->tf->num_segments++;

	dbg_segment(cur);
}

void _tcpflow_track(flow_state_t sptr, pkt_t pkt, dcb_t dcb_ptr)
{
	unsigned int to_server;
	struct tcp_session *s;
	struct tcpseg cur;

	seg_init(&cur, sptr, pkt, (struct tcp_dcb *)dcb_ptr);

	if ( cur.iph->ttl < minttl ) {
		cur.tf->num_ttl_errs++;
		state_err(&cur, "TTL evasion");
		return;
	}

	if ( !tcp_csum(&cur) ) {
		cur.tf->num_csum_errs++;
		state_err(&cur, "bad checksum");
		dhex_dump(cur.payload, cur.len, 16);
		return;
	}

	s = tcp_collide(cur.tf->hash[cur.hash],
			cur.iph, cur.tcph, &to_server);
	if ( s == NULL ) {
		s = new_session(&cur);
		if ( s == NULL )
			return;
	}else{
		/* Figure out which side is which */
		if ( to_server ) {
			cur.snd = &s->c_wnd;
			cur.rcv = s->s_wnd;
		}else{
			cur.snd = s->s_wnd;
			cur.rcv = &s->c_wnd;
		}

		tcp_hash_mtf(cur.tf, s, cur.hash);

		state_track(&cur, s);
	}

	dbg_stream("client", &s->c_wnd);
	dbg_stream("server", s->s_wnd);
	if ( s->state == TCP_SESSION_C ) {
		tcp_free(cur.tf, s);
		mesg(M_DEBUG, "freed session state");
	}
	printf("\n\n");
}

void _tcpflow_dtor(struct tcpflow *tf)
{
	mesg(M_INFO,"tcpstream: max_active=%u num_active=%u",
		tf->max_active, tf->num_active);
	mesg(M_INFO,"tcpstream: %u segments processed",
		tf->num_segments);
//	objcache_fini(&tf->session_cache);
//	objcache_fini(&tf->server_cache);
//	objcache_fini(&tf->sstate_cache);
}

int _tcpflow_ctor(struct tcpflow *tf)
{
	INIT_LIST_HEAD(&tf->lru);
	INIT_LIST_HEAD(&tf->tmo_30);

	dmesg(M_INFO, "tcpflow: %u bytes state", sizeof(*tf));

	tf->session_cache = objcache_init("tcp_session",
						sizeof(struct tcp_session));
	if ( tf->session_cache == NULL )
		return 0;

	tf->server_cache = objcache_init("tcp_server",
						sizeof(struct tcp_server));
	if ( tf->server_cache == NULL )
		return 0;

	tf->sstate_cache = objcache_init("tcp_state",
						sizeof(struct tcp_state));
	if ( tf->sstate_cache == NULL )
		return 0;

	return 1;
}
