#include "user_tcp.h"
#include "user_header.h"
#include "user_nic.h"
#include "user_hash.h"
#include "user_buffer.h"
#include "user_timer.h"

#include <pthread.h>

user_tcp_manager *user_tcp = NULL;
#if 0
static inline int user_tcp_stream_cmp(user_tcp_stream *ts1, user_tcp_stream *ts2) {

	if (ts1->saddr < ts2->saddr) {
		return -1;
	} else if (ts1->saddr == ts2->saddr) {
		if (ts1->sport < ts2->sport) {
			return -1;
		} else if (ts1->sport == ts2->sport) {
			return 0;
		} else {
			return 1;
		}
	} else {
		return 1;
	}
	assert(0);
}


static inline int user_tcp_timer_cmp(user_tcp_stream *ts1, user_tcp_stream *ts2) {

	if (ts1->interval < ts2->interval) {
		return -1;
	} else if (ts1->interval == ts2->interval) {
		return 0;
	} else {
		return 1;
	}
	assert(0);
}
#endif


static int user_tcp_process_payload(user_tcp_manager *tcp, user_tcp_stream *cur_stream, 
		uint32_t cur_ts, uint8_t *payload, uint32_t seq, int payloadlen);
static void user_tcp_process_ack(user_tcp_manager *tcp, user_tcp_stream *cur_stream, uint32_t cur_ts, 
		struct tcphdr *tcph, uint32_t seq, uint32_t ack_seq, uint16_t window, int payloadlen);
static int user_tcp_process_rst(user_tcp_manager *tcp, user_tcp_stream *cur_stream, uint32_t ack_seq);

extern unsigned short in_cksum(unsigned short *addr, int len);

extern void UpdateTimeoutList(user_tcp_manager *tcp, user_tcp_stream *cur_stream);
extern void AddtoRTOList(user_tcp_manager *tcp, user_tcp_stream *cur_stream);
extern void UpdateRetransmissionTimer(user_tcp_manager *tcp, 
		user_tcp_stream *cur_stream, uint32_t cur_ts);
extern void AddtoTimewaitList(user_tcp_manager *tcp, user_tcp_stream *cur_stream, uint32_t cur_ts);
extern void DestroyTcpStream(user_tcp_manager *tcp, user_tcp_stream *stream);
extern void RemoveFromRTOList(user_tcp_manager *tcp, user_tcp_stream *cur_stream);
extern void AddtoTimeoutList(user_tcp_manager *tcp, user_tcp_stream *cur_stream);
extern void RemoveFromTimewaitList(user_tcp_manager *tcp, user_tcp_stream *cur_stream);
extern void InitializeTCPStreamManager();
extern void RemoveFromTimeoutList(user_tcp_manager *tcp, user_tcp_stream *cur_stream);
extern void user_tcp_enqueue_acklist(user_tcp_manager *tcp, user_tcp_stream *cur_stream, uint32_t cur_ts, uint8_t opt) ;
extern void user_tcp_addto_acklist(user_tcp_manager *tcp, user_tcp_stream *cur_stream);
extern int user_tcp_parse_timestamp(user_tcp_timestamp *ts, uint8_t *tcpopt, int len);



user_tcp_manager *user_get_tcp_manager(void)
{
	return user_tcp;
}

static inline uint16_t user_calculate_option(uint8_t flags) {
	uint16_t optlen = 0;

	if (flags & USER_TCPHDR_SYN) {
		optlen += USER_TCPOPT_MSS_LEN;
		optlen += USER_TCPOPT_TIMESTAMP_LEN;
		optlen += 2;
		optlen += USER_TCPOPT_WSCALE_LEN + 1;
	} else {
		optlen += USER_TCPOPT_TIMESTAMP_LEN;
		optlen += 2;
	}
	assert(optlen % 4 == 0);
	return optlen;
}


uint16_t user_tcp_calculate_checksum(uint16_t *buf, uint16_t len, uint32_t saddr, uint32_t daddr)
{
	uint32_t sum;
	uint16_t *w;
	int nleft;
	
	sum = 0;
	nleft = len;
	w = buf;
	
	while (nleft > 1)
	{
		sum += *w++;
		nleft -= 2;
	}
	
	// add padding for odd length
	if (nleft)
		sum += *w & ntohs(0xFF00);
	
	// add pseudo header
	sum += (saddr & 0x0000FFFF) + (saddr >> 16);
	sum += (daddr & 0x0000FFFF) + (daddr >> 16);
	sum += htons(len);
	sum += htons(PROTO_TCP);
	
	sum = (sum >> 16) + (sum & 0xFFFF);
	sum += (sum >> 16);
	
	sum = ~sum;
	
	return (uint16_t)sum;
}



static void user_tcp_generate_timestamp(user_tcp_stream *cur_stream, uint8_t *tcpopt, uint32_t cur_ts) {
	uint32_t *ts = (uint32_t*)(tcpopt+2);

	tcpopt[0] = TCP_OPT_TIMESTAMP;
	tcpopt[1] = USER_TCPOPT_TIMESTAMP_LEN;

	ts[0] = htonl(cur_ts);
	ts[1] = htonl(cur_stream->rcv->ts_recent);
}

static void user_tcp_generate_options(user_tcp_stream *cur_stream, uint32_t cur_ts, 
		uint8_t flags, uint8_t *tcpopt, uint16_t optlen) {

	int i = 0;

	if (flags & USER_TCPHDR_SYN) {
		uint16_t mss = cur_stream->snd->mss;

		tcpopt[i++] = TCP_OPT_MSS;
		tcpopt[i++] = USER_TCPOPT_MSS_LEN;
		tcpopt[i++] = mss >> 8;
		tcpopt[i++] = mss % 256;

		tcpopt[i++] = TCP_OPT_NOP;
		tcpopt[i++] = TCP_OPT_NOP;

		user_tcp_generate_timestamp(cur_stream, tcpopt+i, cur_ts);
		i += USER_TCPOPT_TIMESTAMP_LEN;

		tcpopt[i++] = TCP_OPT_NOP;
		tcpopt[i++] = TCP_OPT_WSCALE;
		tcpopt[i++] = USER_TCPOPT_WSCALE_LEN;

		tcpopt[i++] = cur_stream->snd->wscale_mine;
		
	} else {
		tcpopt[i++] = TCP_OPT_NOP;
		tcpopt[i++] = TCP_OPT_NOP;
		user_tcp_generate_timestamp(cur_stream, tcpopt + i, cur_ts);
		i += USER_TCPOPT_TIMESTAMP_LEN;
	}

	assert(i == optlen);
}

uint16_t user_calculate_chksum(uint16_t *buf, uint16_t len, uint32_t saddr, uint32_t daddr)
{
	uint32_t sum;
	uint16_t *w;
	int nleft;
	
	sum = 0;
	nleft = len;
	w = buf;
	
	while (nleft > 1)
	{
		sum += *w++;
		nleft -= 2;
	}
	
	// add padding for odd length
	if (nleft)
		sum += *w & ntohs(0xFF00);
	
	// add pseudo header
	sum += (saddr & 0x0000FFFF) + (saddr >> 16);
	sum += (daddr & 0x0000FFFF) + (daddr >> 16);
	sum += htons(len);
	sum += htons(PROTO_TCP);
	
	sum = (sum >> 16) + (sum & 0xFFFF);
	sum += (sum >> 16);
	
	sum = ~sum;
	
	return (uint16_t)sum;
}

user_sender *user_tcp_getsender(user_tcp_manager *tcp, user_tcp_stream *cur_stream) {
#if USER_ENABLE_MULTI_NIC
	if (cur_stream->snd->nif_out < 0) return tcp->g_sender;
	else return tcp->n_sender[0];
#else
	return tcp->g_sender;
#endif
}

void user_tcp_addto_acklist(user_tcp_manager *tcp, user_tcp_stream *cur_stream) {
	user_sender *sender = user_tcp_getsender(tcp, cur_stream);
	assert(sender != NULL);

	if (!cur_stream->snd->on_ack_list) {
		cur_stream->snd->on_ack_list = 1;
		TAILQ_INSERT_TAIL(&sender->ack_list, cur_stream, snd->ack_link);
		sender->ack_list_cnt ++;
	}
}

void user_tcp_addto_controllist(user_tcp_manager *tcp, user_tcp_stream *cur_stream) {
	user_sender *sender = user_tcp_getsender(tcp, cur_stream);
	assert(sender != NULL);

	if (!cur_stream->snd->on_control_list) {
		cur_stream->snd->on_control_list = 1;
		TAILQ_INSERT_TAIL(&sender->control_list, cur_stream, snd->control_link);
		sender->control_list_cnt ++;
	}
}

void user_tcp_addto_sendlist(user_tcp_manager *tcp, user_tcp_stream *cur_stream) {
	user_sender *sender = user_tcp_getsender(tcp, cur_stream);
	assert(sender != NULL);

	if (!cur_stream->snd->sndbuf) {
		assert(0);
		return ;
	}

	user_trace_tcp("user_tcp_addto_sendlist --> %d\n", cur_stream->snd->on_send_list);
	if (!cur_stream->snd->on_send_list) {
		cur_stream->snd->on_send_list = 1;
		TAILQ_INSERT_TAIL(&sender->send_list , cur_stream, snd->send_link);
		sender->send_list_cnt ++;
	}
}

void user_tcp_remove_acklist(user_tcp_manager *tcp, user_tcp_stream *cur_stream) {
	user_sender *sender = user_tcp_getsender(tcp, cur_stream);

	if (cur_stream->snd->on_ack_list) {
		cur_stream->snd->on_ack_list = 0;
		TAILQ_REMOVE(&sender->ack_list, cur_stream, snd->ack_link);
		sender->ack_list_cnt --;
	}
}


void user_tcp_remove_controllist(user_tcp_manager *tcp, user_tcp_stream *cur_stream) {
	user_sender *sender = user_tcp_getsender(tcp, cur_stream);

	if (cur_stream->snd->on_control_list) {
		cur_stream->snd->on_control_list = 0;
		TAILQ_REMOVE(&sender->control_list, cur_stream, snd->control_link);
		sender->control_list_cnt --;
	}
}

void user_tcp_remove_sendlist(user_tcp_manager *tcp, user_tcp_stream *cur_stream) {
	user_sender *sender = user_tcp_getsender(tcp, cur_stream);

	if (cur_stream->snd->on_send_list) {
		cur_stream->snd->on_send_list = 0;
		TAILQ_REMOVE(&sender->send_list, cur_stream, snd->send_link);
		sender->send_list_cnt --;
	}
}


void user_tcp_parse_options(user_tcp_stream *cur_stream, uint32_t cur_ts, uint8_t *tcpopt, int len) {

	int i = 0;
	unsigned int opt, optlen;
	
	for (i = 0;i < len; ) {
		opt = *(tcpopt + i++);
		if (opt == TCP_OPT_END) {
			break;
		} else if (opt == TCP_OPT_NOP) {
			continue;
		} else {
			optlen = *(tcpopt + i++);
			if (i + optlen - 2 > (unsigned int)len) break;

			if (opt == TCP_OPT_MSS) {
				cur_stream->snd->mss = *(tcpopt + i++) << 8;
				cur_stream->snd->mss += *(tcpopt + i++);
				cur_stream->snd->eff_mss = cur_stream->snd->mss;
				cur_stream->snd->eff_mss -= (USER_TCPOPT_TIMESTAMP_LEN + 2); 
				
			} else if (opt == TCP_OPT_WSCALE) {
				cur_stream->snd->wscale_peer = *(tcpopt + i++);
				
			} else if (opt == TCP_OPT_SACK_PERMIT) {
				cur_stream->sack_permit = 1;
				user_trace_tcp("Remote SACK permited.\n");
				
			} else if (opt == TCP_OPT_TIMESTAMP) {
				cur_stream->saw_timestamp = 1;
				cur_stream->rcv->ts_recent = ntohl(*(uint32_t *)(tcpopt + i));
				cur_stream->rcv->ts_last_ts_upd = cur_ts;
				i += 8;
				
			} else {
				i += optlen - 2;
			}
		}
	}
	
}

void user_tcp_enqueue_acklist(user_tcp_manager *tcp, user_tcp_stream *cur_stream, uint32_t cur_ts, uint8_t opt) {
	if (!(cur_stream->state == USER_TCP_ESTABLISHED || 
			cur_stream->state == USER_TCP_CLOSE_WAIT ||
			cur_stream->state == USER_TCP_FIN_WAIT_1 ||
			cur_stream->state == USER_TCP_FIN_WAIT_2)) {
		user_trace_tcp("Stream %d: Enqueueing ack at state %d\n", 
				cur_stream->id, cur_stream->state);
	}

	if (opt == ACK_OPT_NOW) {
		if (cur_stream->snd->ack_cnt < cur_stream->snd->ack_cnt + 1) {
			cur_stream->snd->ack_cnt ++;
		}
	} else if (opt == ACK_OPT_AGGREGATE) {
		if (cur_stream->snd->ack_cnt == 0) {
			cur_stream->snd->ack_cnt = 1;
		}
	} else if (opt == ACK_OPT_WACK) {
		cur_stream->snd->is_wack = 1;
	}

	user_tcp_addto_acklist(tcp, cur_stream);
} 


int user_tcp_parse_timestamp(user_tcp_timestamp *ts, uint8_t *tcpopt, int len) {

	int i;
	unsigned int opt, optlen;

	for (i = 0;i < len;i ++) {
		opt = *(tcpopt + i++);
		if (opt == TCP_OPT_END) {
			break;
		} else if (opt == TCP_OPT_NOP) {
			continue;
		} else {
			optlen = *(tcpopt + i++);
			if (i + optlen - 2 > (unsigned int)len) {
				break;
			}
			if (opt == TCP_OPT_TIMESTAMP) {
				ts->ts_val = ntohl(*(uint32_t*)(tcpopt + i));
				ts->ts_ref = ntohl(*(uint32_t*)(tcpopt + i + 4));
				return 1;
			} else {
				i += optlen - 2;
			}
		}
	}

	return 0;
}


int user_tcppkt_alone(user_tcp_manager *tcp, 
		uint32_t saddr, uint16_t sport, uint32_t daddr, uint16_t dport, 
		uint32_t seq, uint32_t ack_seq, uint16_t window, uint8_t flags, 
		uint8_t *payload, uint16_t payloadlen, 
		uint32_t cur_ts, uint32_t echo_ts) {

	int optlen = user_calculate_option(flags);
	if (payloadlen > TCP_DEFAULT_MSS + optlen) {
		user_trace_tcp("Payload size exceeds MSS.\n");
		assert(0);
		return -1;
	}
	struct tcphdr *tcph = (struct tcphdr *)IPOutputStandalone(tcp, PROTO_TCP, 0, saddr, daddr, TCP_HEADER_LEN + optlen + payloadlen);
	if (tcph == NULL) return -1;
	memset(tcph, 0, TCP_HEADER_LEN + optlen);
	tcph->source = sport;
	tcph->dest = dport;

	if (flags & USER_TCPHDR_SYN) {
		tcph->syn = 1;
	} 
	if (flags & USER_TCPHDR_FIN) {
		tcph->fin = 1;
	}
	if (flags & USER_TCPHDR_RST) {
		tcph->rst = 1;
	}
	if (flags & USER_TCPHDR_PSH) {
		tcph->psh = 1;
	}

	tcph->seq = htonl(seq);
	if (flags & USER_TCPHDR_ACK) {
		tcph->ack = 1;
		tcph->ack_seq = htonl(ack_seq);
	}
	tcph->window = htons(MIN(window, TCP_MAX_WINDOW));
	uint8_t *tcpopt = (uint8_t*)tcph + TCP_HEADER_LEN;
	uint32_t *ts = (uint32_t*)(tcpopt + 4);

	tcpopt[0] = TCP_OPT_NOP;
	tcpopt[1] = TCP_OPT_NOP;
	tcpopt[2] = TCP_OPT_TIMESTAMP;
	tcpopt[3] = USER_TCPOPT_TIMESTAMP_LEN;

	ts[0] = htonl(cur_ts);
	ts[1] = htonl(echo_ts);

	tcph->doff = (TCP_HEADER_LEN + optlen) >> 2;
	if (payloadlen > 0) {
		memcpy((uint8_t *)tcph + TCP_HEADER_LEN + optlen, payload, payloadlen);
	}
	tcph->check = user_calculate_chksum((uint16_t*)tcph, 
		TCP_HEADER_LEN + optlen + payloadlen, saddr, daddr);

	if (tcph->syn || tcph->fin) {
		payloadlen ++;
	}
	return payloadlen;
}

int user_tcp_send_tcppkt(user_tcp_stream *cur_stream, 
		uint32_t cur_ts, uint8_t flags, uint8_t *payload, uint16_t payloadlen) {

	uint16_t optlen = user_calculate_option(flags);

	user_trace_tcp("payload:%d, mss:%d, optlen:%d, data:%s\n", payloadlen, cur_stream->snd->mss, optlen, payload);
	if (payloadlen > cur_stream->snd->mss + optlen) {
		user_trace_tcp("Payload size exceeds MSS\n");
		return -1;
	}

	user_tcp_manager *tcp = user_get_tcp_manager();
	struct tcphdr *tcph = (struct tcphdr*)IPOutput(tcp, cur_stream, TCP_HEADER_LEN+optlen+payloadlen);
	if (tcph == NULL) return -2;

	memset(tcph, 0, TCP_HEADER_LEN+optlen);
	tcph->source = cur_stream->sport;
	tcph->dest = cur_stream->dport;

	if (flags & USER_TCPHDR_SYN) {
		tcph->syn = 1;
		if (cur_stream->snd_nxt != cur_stream->snd->iss) {
			user_trace_tcp("Stream %d: weird SYN sequence. "
					"snd_nxt: %u, iss: %u\n", cur_stream->id, 
					cur_stream->snd_nxt, cur_stream->snd->iss);
		}
	} 

	if (flags & USER_TCPHDR_RST) {
		user_trace_tcp("Stream %d: Sending RST.\n", cur_stream->id);
		tcph->rst = 1;
	}

	if (flags & USER_TCPHDR_PSH) {
		tcph->psh = 1;
	}

	if (flags & USER_TCPHDR_CWR) {
		tcph->seq = htonl(cur_stream->snd_nxt - 1);
		user_trace_tcp("%u Sending ACK to get new window advertisement. "
				"seq: %u, peer_wnd: %u, snd_nxt - snd_una: %u\n", 
				cur_stream->id,
				cur_stream->snd_nxt - 1, cur_stream->snd->peer_wnd, 
				cur_stream->snd_nxt - cur_stream->snd->snd_una);
	} else if (flags & USER_TCPHDR_FIN) {
		tcph->fin = 1;
		if (cur_stream->snd->fss == 0) {
			user_trace_tcp("Stream %u: not fss set. closed: %u\n", 
					cur_stream->id, cur_stream->closed);
		}
		tcph->seq = htonl(cur_stream->snd->fss);
		cur_stream->snd->is_fin_sent = 1;
		user_trace_tcp("Stream %d: Sending FIN. seq: %u, ack_seq: %u\n", 
				cur_stream->id, cur_stream->snd_nxt, cur_stream->rcv_nxt);
	} else {
		tcph->seq = htonl(cur_stream->snd_nxt);
	}

	if (flags & USER_TCPHDR_ACK) {
		tcph->ack = 1;
		tcph->ack_seq = htonl(cur_stream->rcv_nxt);

		cur_stream->snd->ts_lastack_sent = cur_ts;
		cur_stream->last_active_ts = cur_ts;

		UpdateTimeoutList(tcp, cur_stream);
	}

	uint8_t wscale = 0;
	if (flags & USER_TCPHDR_SYN) {
		wscale = 0;
	} else {
		wscale = cur_stream->snd->wscale_mine;
	}

	uint32_t window32 = cur_stream->rcv->rcv_wnd >> wscale;
	tcph->window = htons((uint16_t)MIN(window32, TCP_MAX_WINDOW));

	if (window32 == 0) cur_stream->need_wnd_adv = 1;

	user_tcp_generate_options(cur_stream, cur_ts, flags, 
		(uint8_t*)tcph+TCP_HEADER_LEN, optlen);
	
	tcph->doff = (TCP_HEADER_LEN + optlen) >> 2;
	if (payloadlen > 0) {
		memcpy((uint8_t *)tcph + TCP_HEADER_LEN + optlen, payload, payloadlen);
	}
	
	tcph->check = user_tcp_calculate_checksum((uint16_t *)tcph, 
					TCP_HEADER_LEN + optlen + payloadlen, 
					cur_stream->saddr, cur_stream->daddr);
	cur_stream->snd_nxt += payloadlen;
	
	if (tcph->syn || tcph->fin) {
		cur_stream->snd_nxt ++;
		payloadlen ++;
	}
	
	if (payloadlen > 0) {
		if (cur_stream->state > USER_TCP_ESTABLISHED) {
			user_trace_tcp("Payload after ESTABLISHED: length: %d, snd_nxt: %u\n", 
					payloadlen, cur_stream->snd_nxt);
		}

		cur_stream->snd->ts_rto = cur_ts + cur_stream->snd->rto;
		user_trace_tcp("Updating retransmission timer. "
				"cur_ts: %u, rto: %u, ts_rto: %u, mss:%d\n", 
				cur_ts, cur_stream->snd->rto, cur_stream->snd->ts_rto, cur_stream->snd->mss);

		AddtoRTOList(tcp, cur_stream);
		user_trace_tcp(" user_tcp_send_tcppkt : %d\n", payloadlen);
	}

	return payloadlen;
}


static inline int user_tcp_filter_synpkt(user_tcp_manager *tcp, uint32_t ip, uint16_t port) {
	struct sockaddr_in *addr;

	user_trace_tcp("FilterSYNPacket 111:0x%x, port:%d\n", ip, port);
	struct _user_tcp_listener *listener = (struct _user_tcp_listener*)ListenerHTSearch(tcp->listeners, &port);
	if (listener == NULL) return 0;

	user_trace_tcp("FilterSYNPacket 222:0x%x, port:%d\n", ip, port);
	addr = &listener->s->s_addr;
	if (addr->sin_port == port) {
		if (addr->sin_addr.s_addr != INADDR_ANY) {
			if (ip == addr->sin_addr.s_addr) {
				return 1;
			}
			return 0;
		}
		if (ip == USER_SELF_IP_HEX) return 1;
		return 0;
	}
	return 0;
}

static inline user_tcp_stream *user_tcp_passive_open(user_tcp_manager *tcp, uint32_t cur_ts, const struct iphdr *iph, 
		const struct tcphdr *tcph, uint32_t seq, uint16_t window) {

	user_tcp_stream *cur_stream = CreateTcpStream(tcp, NULL, USER_TCP_SOCK_STREAM,
		iph->daddr, tcph->dest, iph->saddr, tcph->source);
	if (cur_stream == NULL) {
		user_trace_tcp("INFO: Could not allocate tcp_stream!\n");
		return NULL;
	}

	cur_stream->rcv->irs = seq;
	cur_stream->snd->peer_wnd = window;
	cur_stream->rcv_nxt = cur_stream->rcv->irs;
	cur_stream->snd->cwnd = 1;

#if 1
	cur_stream->rcv->recvbuf = RBInit(tcp->rbm_rcv, cur_stream->rcv->irs + 1);
	if (!cur_stream->rcv->recvbuf) {
		cur_stream->state = USER_TCP_CLOSED;
		cur_stream->close_reason = TCP_NO_MEM;
		
	}
#endif
	
	user_tcp_parse_options(cur_stream, cur_ts, (uint8_t*)tcph+TCP_HEADER_LEN,
		(tcph->doff << 2) - TCP_HEADER_LEN);
	user_trace_tcp("user_tcp_passive_open : %d, %d\n", cur_stream->rcv_nxt, cur_stream->snd->mss);

	return cur_stream;
}

int user_tcp_active_open(user_tcp_manager *tcp, user_tcp_stream *cur_stream, uint32_t cur_ts, 
		struct tcphdr *tcph, uint32_t seq, uint32_t ack_seq, uint16_t window) {

	cur_stream->rcv->irs = seq;
	cur_stream->snd_nxt = ack_seq;
	cur_stream->snd->peer_wnd = window;
	cur_stream->rcv->snd_wl1 = cur_stream->rcv->irs - 1;
	cur_stream->rcv_nxt = cur_stream->rcv->irs + 1;
	cur_stream->rcv->last_ack_seq = ack_seq;

	user_tcp_parse_options(cur_stream, cur_ts, (uint8_t*)tcph + TCP_HEADER_LEN,
		(tcph->doff<<2) - TCP_HEADER_LEN);

	cur_stream->snd->cwnd = ((cur_stream->snd->cwnd == 1) ? 
		(cur_stream->snd->mss * 2) : cur_stream->snd->mss);
	cur_stream->snd->ssthresh = cur_stream->snd->mss * 10;

	UpdateRetransmissionTimer(tcp, cur_stream, cur_ts);

	return 1;
}

static inline int user_tcp_validseq(user_tcp_manager *tcp, user_tcp_stream *cur_stream, uint32_t cur_ts,
	struct tcphdr *tcph, uint32_t seq, uint32_t ack_seq, int payloadlen) {

	if (!tcph->rst && cur_stream->saw_timestamp) {
		user_tcp_timestamp ts;
		if (!user_tcp_parse_timestamp(&ts, (uint8_t*)tcph + TCP_HEADER_LEN,
			(tcph->doff << 2) - TCP_HEADER_LEN)) {
			user_trace_tcp("No timestamp found\n");
			return 0;
		}
		
		if (TCP_SEQ_LT(ts.ts_val, cur_stream->rcv->ts_recent)) {
			user_tcp_enqueue_acklist(tcp, cur_stream, cur_ts, ACK_OPT_NOW);
		} else {
			if (TCP_SEQ_GT(ts.ts_val, cur_stream->rcv->ts_recent)) {
				user_trace_tcp("Timestamp update. cur: %u, prior: %u "
					"(time diff: %uus)\n", 
					ts.ts_val, cur_stream->rcv->ts_recent, 
					TS_TO_USEC(cur_ts - cur_stream->rcv->ts_last_ts_upd));
				cur_stream->rcv->ts_last_ts_upd = cur_ts;
			}
			cur_stream->rcv->ts_recent = ts.ts_val;
			cur_stream->rcv->ts_lastack_rcvd = ts.ts_ref;
		}
	}

	if (!TCP_SEQ_BETWEEN(seq+payloadlen, cur_stream->rcv_nxt,
		cur_stream->rcv_nxt + cur_stream->rcv->rcv_wnd)) {
		if (tcph->rst) {
			return 0;
		} 
		if (cur_stream->state == USER_TCP_ESTABLISHED) {
			if (seq + 1 == cur_stream->rcv_nxt) {
				user_tcp_enqueue_acklist(tcp, cur_stream, cur_ts, ACK_OPT_AGGREGATE);
				return 0;
			}

			if (TCP_SEQ_LEQ(seq, cur_stream->rcv_nxt)) {
				user_tcp_enqueue_acklist(tcp, cur_stream, cur_ts, ACK_OPT_AGGREGATE);
			} else {
				user_tcp_enqueue_acklist(tcp, cur_stream, cur_ts, ACK_OPT_NOW);
			}
		} else {
			if (cur_stream->state == USER_TCP_TIME_WAIT) {
				user_trace_tcp("Stream %d: tw expire update to %u\n", 
						cur_stream->id, cur_stream->rcv->ts_tw_expire);
				AddtoTimewaitList(tcp, cur_stream, cur_ts);
			}
			user_tcp_addto_controllist(tcp, cur_stream);
		}
		return 0;
	}
	return 1;
}

static user_tcp_stream *user_create_stream(user_tcp_manager *tcp, uint32_t cur_ts, const struct iphdr *iph, 
		int ip_len, const struct tcphdr* tcph, uint32_t seq, uint32_t ack_seq,
		int payloadlen, uint16_t window) {

	user_tcp_stream *cur_stream;
	int ret = 0;

	if (tcph->syn && !tcph->ack) {
		user_trace_tcp("ip:0x%x, port:%d\n", iph->daddr, tcph->dest);
		ret = user_tcp_filter_synpkt(tcp, iph->daddr, tcph->dest);
		if (!ret) {
			user_trace_tcp("Refusing SYN packet.\n");
			user_tcppkt_alone(tcp, iph->daddr, tcph->dest, iph->saddr, tcph->source, 
						0, seq + payloadlen + 1, 0, USER_TCPHDR_RST | USER_TCPHDR_ACK,
						NULL, 0, cur_ts, 0);
			return NULL;
		}
		user_trace_tcp("user_create_stream\n");
		cur_stream = user_tcp_passive_open(tcp, cur_ts, iph, tcph, seq, window);
		if (!cur_stream) {
			user_trace_tcp("Not available space in flow pool.\n");

			user_tcppkt_alone(tcp, iph->daddr, tcph->dest, iph->saddr, tcph->source, 
						0, seq + payloadlen + 1, 0, USER_TCPHDR_RST | USER_TCPHDR_ACK,
						NULL, 0, cur_ts, 0);

			return NULL;
		}
		return cur_stream;
	} else if (tcph->rst) {
		user_trace_tcp("Reset packet comes\n");
		return NULL;
	} else {
		if (tcph->ack) {
			user_tcppkt_alone(tcp, iph->daddr, tcph->dest, iph->saddr, tcph->source, 
						ack_seq, 0, 0, USER_TCPHDR_RST,
						NULL, 0, cur_ts, 0);
		} else {
			user_tcppkt_alone(tcp, iph->daddr, tcph->dest, iph->saddr, tcph->source, 
						0, seq + payloadlen, 0, USER_TCPHDR_RST | USER_TCPHDR_ACK,
						NULL, 0, cur_ts, 0);
		}
		return NULL;
	}

}

static void user_tcp_flush_accept_event(user_tcp_listener *listener) {

	pthread_mutex_lock(&listener->accept_lock);

	if (!StreamQueueIsEmpty(listener->acceptq)) {
		pthread_cond_signal(&listener->accept_cond);
	}

	pthread_mutex_unlock(&listener->accept_lock);
}

static void user_tcp_flush_read_event(user_tcp_recv *rcv) {

	pthread_mutex_lock(&rcv->read_lock);

	if (rcv->recvbuf->merged_len >= 0) {
		pthread_cond_signal(&rcv->read_cond);
	}
	
	pthread_mutex_unlock(&rcv->read_lock);
}

static void user_tcp_flush_send_event(user_tcp_send *snd) {

	pthread_mutex_lock(&snd->write_lock);

	if (snd->snd_wnd > 0) {
		pthread_cond_signal(&snd->write_cond);
	}
	
	pthread_mutex_unlock(&snd->write_lock);
}

static void user_tcp_handle_listen(user_tcp_manager *tcp, uint32_t cur_ts,
	user_tcp_stream *cur_stream, struct tcphdr *tcph) {

	if (tcph->syn) {
		if (cur_stream->state == USER_TCP_LISTEN) {
			cur_stream->rcv_nxt ++;
		}
		cur_stream->state = USER_TCP_SYN_RCVD;
		user_trace_tcp("Stream %d: TCP_ST_SYN_RCVD\n", cur_stream->id);
		user_tcp_addto_controllist(tcp, cur_stream);
	} else {
		user_trace_tcp("Stream %d (TCP_ST_LISTEN): "
				"Packet without SYN.\n", cur_stream->id);
		assert(0);
	}
}

static void user_tcp_handle_syn_sent(user_tcp_manager *tcp, uint32_t cur_ts, 
		user_tcp_stream* cur_stream, const struct iphdr* iph, struct tcphdr* tcph,
		uint32_t seq, uint32_t ack_seq, int payloadlen, uint16_t window) {

	if (tcph->ack) {
		if (TCP_SEQ_LEQ(ack_seq, cur_stream->snd->iss) ||
			TCP_SEQ_GT(ack_seq, cur_stream->snd_nxt)) {
			if (!tcph->rst) {
				user_tcppkt_alone(tcp, iph->daddr, tcph->dest, iph->saddr, tcph->source,
					ack_seq, 0, 0, USER_TCPHDR_RST, NULL, 0, cur_ts, 0);
			}
			return ;
		} 
		cur_stream->snd->snd_una ++;
	}

	if (tcph->rst && tcph->ack) {
		cur_stream->state = USER_TCP_CLOSE_WAIT;
		cur_stream->close_reason = TCP_RESET;
		if (cur_stream->s) {
			//.... raise error event
		} else {
			DestroyTcpStream(tcp, cur_stream);
		}
		return ;
	}

	if (tcph->syn && tcph->ack) {
		int ret = user_tcp_active_open(tcp, cur_stream, cur_ts,
			tcph, seq, ack_seq, window);
		if (!ret) return ;

		cur_stream->snd->nrtx = 0;
		cur_stream->rcv_nxt = cur_stream->rcv->irs + 1;
		RemoveFromRTOList(tcp, cur_stream);
		cur_stream->state = USER_TCP_ESTABLISHED;

		user_trace_tcp("Stream %d: TCP_ST_ESTABLISHED\n", cur_stream->id);

		if (cur_stream->s) {
			//Raise Write Event
		} else {
			user_tcppkt_alone(tcp, iph->saddr, tcph->dest, iph->daddr, tcph->source,
				0 , seq+payloadlen+1, 0, USER_TCPHDR_RST | USER_TCPHDR_ACK, NULL, 0, cur_ts, 0);
			cur_stream->close_reason = TCP_ACTIVE_CLOSE;
			DestroyTcpStream(tcp, cur_stream);
		}

		user_tcp_addto_controllist(tcp, cur_stream);
		AddtoTimeoutList(tcp, cur_stream);
	} else {

		cur_stream->state = USER_TCP_SYN_RCVD;
		cur_stream->snd_nxt = cur_stream->snd->iss;
		user_tcp_addto_controllist(tcp, cur_stream);
	}
}

static void user_tcp_handle_syn_rcvd(user_tcp_manager *tcp, uint32_t cur_ts,
		user_tcp_stream* cur_stream, struct tcphdr* tcph, uint32_t ack_seq) {

	user_tcp_send *snd = cur_stream->snd;

	if (tcph->ack) {
		if (ack_seq != snd->iss + 1) {
			user_trace_tcp("Stream %d (TCP_ST_SYN_RCVD): "
					"weird ack_seq: %u, iss: %u\n", 
					cur_stream->id, ack_seq, snd->iss);
			exit(1);
			return ;
		}

		snd->snd_una ++;
		cur_stream->snd_nxt = ack_seq;
		uint32_t prior_cwnd = snd->cwnd;
		snd->cwnd = (prior_cwnd == 1) ? snd->mss * 2 : snd->mss;
		snd->nrtx = 0;

		cur_stream->rcv_nxt = cur_stream->rcv->irs + 1;
		RemoveFromRTOList(tcp, cur_stream);

		cur_stream->state = USER_TCP_ESTABLISHED;
		
		struct _user_tcp_listener *listener = ListenerHTSearch(tcp->listeners, &tcph->dest);
		int ret = StreamEnqueue(listener->acceptq, cur_stream);
		if (ret < 0) {
			cur_stream->close_reason = TCP_NOT_ACCEPTED;
			cur_stream->state = USER_TCP_CLOSED;
			user_tcp_addto_controllist(tcp, cur_stream);
		}

		AddtoTimeoutList(tcp, cur_stream);

		user_trace_tcp("user_tcp_handle_syn_rcvd\n");
		if (listener->s) {
			//&& 
			/*
			 * should move to epoll for check s->epoll type.
			 */
			//AddtoEpollEvent
#if USER_ENABLE_EPOLL_RB
			if (tcp->ep) {
				epoll_event_callback(tcp->ep, listener->s->id, USER_EPOLLIN);
			} 
#else
			if (listener->s->epoll & USER_EPOLLIN) {
				user_epoll_add_event(tcp->ep, USER_EVENT_QUEUE, listener->s, USER_EPOLLIN);
			}
#endif
			if (!(listener->s->opts & USER_TCP_NONBLOCK)){
				user_tcp_flush_accept_event(listener);
			}
			
		}

	} else {
		user_trace_tcp("Stream %d (TCP_ST_SYN_RCVD): No ACK.\n", 
				cur_stream->id);

		cur_stream->snd_nxt = snd->iss;
		user_tcp_addto_controllist(tcp, cur_stream);
	}
	
}

void user_tcp_handle_established(user_tcp_manager *tcp, uint32_t cur_ts,
		user_tcp_stream* cur_stream, struct tcphdr* tcph, uint32_t seq, uint32_t ack_seq,
		uint8_t *payload, int payloadlen, uint16_t window) {

	if (tcph->syn) {
		user_trace_tcp("Stream %d (TCP_ST_ESTABLISHED): weird SYN. "
				"seq: %u, expected: %u, ack_seq: %u, expected: %u\n", 
				cur_stream->id, seq, cur_stream->rcv_nxt, 
				ack_seq, cur_stream->snd_nxt);
		
		cur_stream->snd_nxt = ack_seq;
		user_tcp_addto_controllist(tcp, cur_stream);
		return ;
	}

	if (payloadlen > 0) {
		if (user_tcp_process_payload(tcp, cur_stream, cur_ts, payload, seq, payloadlen)) {
			user_tcp_enqueue_acklist(tcp, cur_stream, cur_ts, ACK_OPT_AGGREGATE);
		} else {
			user_tcp_enqueue_acklist(tcp, cur_stream, cur_ts, ACK_OPT_NOW);
		}
	}

	if (tcph->ack) {
		if (cur_stream->snd->sndbuf) {
			user_tcp_process_ack(tcp, cur_stream, cur_ts,
				tcph, seq, ack_seq, window, payloadlen);
		}
	}

	if (tcph->fin) {
		if (seq + payloadlen == cur_stream->rcv_nxt) {
			cur_stream->state = USER_TCP_CLOSE_WAIT;
			user_trace_tcp("Stream %d: TCP_ST_CLOSE_WAIT\n", cur_stream->id);
			cur_stream->rcv_nxt ++;
			user_tcp_addto_controllist(tcp, cur_stream);
			//Raise Read Event
			user_trace_tcp("user_tcp_flush_read_event\n");

#if USER_ENABLE_EPOLL_RB
			if (tcp->ep) {
				epoll_event_callback(tcp->ep, cur_stream->s->id, USER_EPOLLIN);
			}
			user_trace_tcp(" epoll_event_callback : %d\n", cur_stream->s->opts);
#endif
			if (cur_stream->s && !(cur_stream->s->opts & USER_TCP_NONBLOCK)) {
				user_tcp_flush_read_event(cur_stream->rcv);
			}
			
		} else {
			user_tcp_enqueue_acklist(tcp, cur_stream, cur_ts, ACK_OPT_NOW);
			return ;
		}
	}

	return ;
}

void user_tcp_handle_close_wait(user_tcp_manager *tcp, uint32_t cur_ts, 
		user_tcp_stream *cur_stream, struct tcphdr* tcph, uint32_t seq, uint32_t ack_seq, 
		int payloadlen, uint16_t window) {

	if (TCP_SEQ_LT(seq, cur_stream->rcv_nxt)) {
		user_trace_tcp("Stream %d (TCP_ST_CLOSE_WAIT): "
				"weird seq: %u, expected: %u\n", 
				cur_stream->id, seq, cur_stream->rcv_nxt);
		user_tcp_addto_controllist(tcp, cur_stream);
		return ;
	}
	if (cur_stream->snd->sndbuf) {
		user_tcp_process_ack(tcp, cur_stream, cur_ts, tcph, seq, 
			ack_seq, window, payloadlen);
	}
}

void user_tcp_handle_last_ack(user_tcp_manager *tcp, uint32_t cur_ts, const struct iphdr *iph,
		int ip_len, user_tcp_stream *cur_stream, struct tcphdr* tcph, 
		uint32_t seq, uint32_t ack_seq, int payloadlen, uint16_t window) {

	if (TCP_SEQ_LT(seq, cur_stream->rcv_nxt)) {
		user_trace_tcp("Stream %d (TCP_ST_LAST_ACK): "
				"weird seq: %u, expected: %u\n", 
				cur_stream->id, seq, cur_stream->rcv_nxt);
		return;
	}
	if (tcph->ack) {
		if (cur_stream->snd->sndbuf) {
			user_tcp_process_ack(tcp, cur_stream, cur_ts,
				tcph, seq, ack_seq, window, payloadlen);
		}
		if (!cur_stream->snd->is_fin_sent) {
			user_trace_tcp("Stream %d (TCP_ST_LAST_ACK): "
					"No FIN sent yet.\n", cur_stream->id);
			return ;
		}
		if (ack_seq == cur_stream->snd->fss + 1) {
			cur_stream->snd->snd_una ++;
			UpdateRetransmissionTimer(tcp, cur_stream, cur_ts);

			cur_stream->state = USER_TCP_CLOSED;
			cur_stream->close_reason = TCP_PASSIVE_CLOSE;

			user_trace_tcp("Stream %d: USER_TCP_CLOSED\n", cur_stream->id);
			DestroyTcpStream(tcp, cur_stream);
		} else {
			user_trace_tcp("Stream %d (TCP_ST_LAST_ACK): Not ACK of FIN. "
					"ack_seq: %u, expected: %u\n", 
					cur_stream->id, ack_seq, cur_stream->snd->fss + 1);
			user_tcp_addto_controllist(tcp, cur_stream);
		}
	} else {
		user_trace_tcp("Stream %d (TCP_ST_LAST_ACK): No ACK\n", 
				cur_stream->id);
		user_tcp_addto_controllist(tcp, cur_stream);
	}
}

void user_tcp_handle_fin_wait_1(user_tcp_manager *tcp, uint32_t cur_ts,
		user_tcp_stream *cur_stream, struct tcphdr* tcph, uint32_t seq, uint32_t ack_seq, 
		uint8_t *payload, int payloadlen, uint16_t window) {
	if (TCP_SEQ_LT(seq, cur_stream->rcv_nxt)) {
		user_trace_tcp("Stream %d (TCP_ST_LAST_ACK): "
				"weird seq: %u, expected: %u\n", 
				cur_stream->id, seq, cur_stream->rcv_nxt);
		user_tcp_addto_controllist(tcp, cur_stream);
		return ;
	}

	if (tcph->ack) {
		if (cur_stream->snd->sndbuf) {
			user_tcp_process_ack(tcp, cur_stream, cur_ts, tcph, seq, ack_seq, window, payloadlen);
		}
		if (cur_stream->snd->is_fin_sent && 
			ack_seq == cur_stream->snd->fss + 1) {
			cur_stream->snd->snd_una = ack_seq;
			if (TCP_SEQ_GT(ack_seq, cur_stream->snd_nxt)) {
				user_trace_tcp("Stream %d: update snd_nxt to %u\n", 
						cur_stream->id, ack_seq);
				cur_stream->snd_nxt = ack_seq;
			}
			cur_stream->snd->nrtx = 0;
			RemoveFromRTOList(tcp, cur_stream);
			cur_stream->state = USER_TCP_FIN_WAIT_2;

			user_trace_tcp("Stream %d: TCP_ST_FIN_WAIT_2\n", 
					cur_stream->id);
		}
	} else {
		user_trace_tcp("Stream %d: does not contain an ack!\n", 
				cur_stream->id);
		return;
	}

	if (payloadlen > 0) {
		if (user_tcp_process_payload(tcp, cur_stream, cur_ts, payload, seq, payloadlen)) {
			user_tcp_enqueue_acklist(tcp, cur_stream, cur_ts, ACK_OPT_AGGREGATE);
		} else {
			user_tcp_enqueue_acklist(tcp, cur_stream, cur_ts, ACK_OPT_NOW);
		}
	}

	if (tcph->fin) {
		if (seq + payloadlen == cur_stream->rcv_nxt) {
			cur_stream->rcv_nxt ++;
			if (cur_stream->state == USER_TCP_FIN_WAIT_1) {
				cur_stream->state = USER_TCP_CLOSING;
				user_trace_tcp("Stream %d: TCP_ST_CLOSING\n", cur_stream->id);
				
			} else if (cur_stream->state == USER_TCP_FIN_WAIT_2) {

				cur_stream->state = USER_TCP_TIMEWAIT;
				user_trace_tcp("Stream %d: TCP_ST_TIME_WAIT\n", cur_stream->id);
				AddtoTimewaitList(tcp, cur_stream, cur_ts);
				
			} else {
				assert(0);
			}
			user_tcp_addto_controllist(tcp, cur_stream);
		}
	}
}

void user_tcp_handle_fin_wait_2(user_tcp_manager *tcp, uint32_t cur_ts,
		user_tcp_stream *cur_stream, struct tcphdr* tcph, uint32_t seq, uint32_t ack_seq,
		uint8_t *payload, int payloadlen, uint16_t window) {

	if (tcph->ack) {
		if (cur_stream->snd->sndbuf) {
			user_tcp_process_ack(tcp, cur_stream, cur_ts,
				tcph, seq, ack_seq, window, payloadlen);
		}
	} else {
		user_trace_tcp("Stream %d: does not contain an ack!\n", 
				cur_stream->id);
		return;
	}
	if (payloadlen > 0) {
		if (user_tcp_process_payload(tcp, cur_stream, cur_ts, payload, seq, payloadlen)) {
			user_tcp_enqueue_acklist(tcp, cur_stream, cur_ts, ACK_OPT_AGGREGATE);
		} else {
			user_tcp_enqueue_acklist(tcp, cur_stream, cur_ts, ACK_OPT_NOW);
		}
	}
	if (tcph->fin) {
		if (seq+payloadlen == cur_stream->rcv_nxt) {
			cur_stream->state = USER_TCP_TIME_WAIT;
			cur_stream->rcv_nxt ++;
			user_trace_tcp("Stream %d: TCP_ST_TIME_WAIT\n", cur_stream->id);

			AddtoTimewaitList(tcp, cur_stream, cur_ts);
			user_tcp_addto_controllist(tcp, cur_stream);
		}
	}
}

void user_tcp_handle_closing(user_tcp_manager *tcp, uint32_t cur_ts, 
		user_tcp_stream *cur_stream, struct tcphdr* tcph, uint32_t seq, uint32_t ack_seq,
		int payloadlen, uint16_t window) {

	if (tcph->ack) {
		if (cur_stream->snd->sndbuf) {
			user_tcp_process_ack(tcp, cur_stream, cur_ts, tcph, seq, ack_seq, window, payloadlen);
		}
		if (!cur_stream->snd->is_fin_sent) {
			user_trace_tcp("Stream %d (TCP_ST_CLOSING): "
					"No FIN sent yet.\n", cur_stream->id);
			return;
		}
		if (ack_seq != cur_stream->snd->fss + 1) {
			return;
		}
		cur_stream->snd->snd_una = ack_seq;
		cur_stream->snd_nxt = ack_seq;
		UpdateRetransmissionTimer(tcp, cur_stream, cur_ts);

		cur_stream->state = USER_TCP_TIME_WAIT;
		user_trace_tcp("Stream %d: TCP_ST_TIME_WAIT\n", cur_stream->id);

		AddtoTimewaitList(tcp, cur_stream, cur_ts);
	} else {
		user_trace_tcp("Stream %d (TCP_ST_CLOSING): Not ACK\n", 
				cur_stream->id);
		return;
	}
	
}

void user_tcp_estimate_rtt(user_tcp_manager *tcp, user_tcp_stream *cur_stream, uint32_t mrtt) {

#define TCP_RTO_MIN		0
	long m = mrtt;
	uint32_t tcp_rto_min = TCP_RTO_MIN;
	user_tcp_recv *rcv = cur_stream->rcv;

	if (m == 0) {
		m = 1;
	}
	if (rcv->srtt != 0) {
		m -= (rcv->srtt >> 3);
		rcv->srtt += 3;
		if (m < 0) {
			m = -m;
			m -= (rcv->mdev >> 2);
			if (m > 0) {
				m >>= 3;
			}
		} else {
			m -= (rcv->mdev >> 2);
		}
		rcv->mdev += m;
		if (rcv->mdev > rcv->mdev_max) {
			rcv->mdev_max = rcv->mdev;
			if (rcv->mdev_max > rcv->rttvar) {
				rcv->rttvar = rcv->mdev_max;
			}
		}

		if (TCP_SEQ_GT(cur_stream->snd->snd_una, rcv->rtt_seq)) {
			if (rcv->mdev_max < rcv->rttvar) {
				rcv->rttvar -= (rcv->rttvar - rcv->mdev_max);
			}
			rcv->rtt_seq = cur_stream->snd_nxt;
			rcv->mdev_max = tcp_rto_min;
		} 
	} else {
		rcv->srtt = m << 3;
		rcv->mdev = m << 1;
		rcv->mdev_max = rcv->rttvar = MAX(rcv->mdev, tcp_rto_min);
		rcv->rtt_seq = cur_stream->snd_nxt;
	}

	user_trace_tcp("mrtt: %u (%uus), srtt: %u (%ums), mdev: %u, mdev_max: %u, "
			"rttvar: %u, rtt_seq: %u\n", mrtt, mrtt * TIME_TICK, 
			rcv->srtt, TS_TO_MSEC((rcv->srtt) >> 3), rcv->mdev, 
			rcv->mdev_max, rcv->rttvar, rcv->rtt_seq);

}

static int user_tcp_process_payload(user_tcp_manager *tcp, user_tcp_stream *cur_stream, 
		uint32_t cur_ts, uint8_t *payload, uint32_t seq, int payloadlen) {

	user_tcp_recv *rcv = cur_stream->rcv;

	if (TCP_SEQ_LT(seq+payloadlen, cur_stream->rcv_nxt)) {
		return 0;
	}

	if (TCP_SEQ_GT(seq+payloadlen, cur_stream->rcv_nxt + rcv->rcv_wnd)) {
		return 0;
	}

	if (!rcv->recvbuf) {
		user_trace_tcp("user_tcp_process_payload --> \n");
		rcv->recvbuf = RBInit(tcp->rbm_rcv, rcv->irs + 1);
		if (!rcv->recvbuf) {
			cur_stream->state = USER_TCP_CLOSED;
			cur_stream->close_reason = TCP_NO_MEM;
			//Raise Error Event
			user_trace_tcp(" Raise Error Event \n");
			
			return -1;
		}
	}

#if USER_ENABLE_BLOCKING
	if (pthread_mutex_lock(&rcv->read_lock)) {
		if (errno == EDEADLK) {
			perror("ProcessTCPPayload: read_lock blocked\n");
		}
		assert(0);
	}
#else
	if (SBUF_LOCK(&rcv->read_lock)) {
		if (errno == EDEADLK) {
			perror("ProcessTCPPayload: read_lock blocked\n");
		}
		assert(0);
	}
#endif
	uint32_t prev_rcv_nxt = cur_stream->rcv_nxt;
	int ret = RBPut(tcp->rbm_rcv, rcv->recvbuf, payload, (uint32_t)payloadlen, seq);
	if (ret < 0) {
		user_trace_tcp("Cannot merge payload. reason: %d\n", ret);
	}

	if (cur_stream->state == USER_TCP_FIN_WAIT_1 ||
		cur_stream->state == USER_TCP_FIN_WAIT_2) {
		RBRemove(tcp->rbm_rcv, rcv->recvbuf, rcv->recvbuf->merged_len, AT_MTCP);
	}

	cur_stream->rcv_nxt = rcv->recvbuf->head_seq + rcv->recvbuf->merged_len;
	rcv->rcv_wnd = rcv->recvbuf->size - rcv->recvbuf->merged_len;
#if USER_ENABLE_BLOCKING
	pthread_mutex_unlock(&rcv->read_lock);
#else
	SBUF_UNLOCK(&rcv->read_lock);
#endif
	if (TCP_SEQ_LEQ(cur_stream->rcv_nxt, prev_rcv_nxt)) {
		return 0;
	}

	if (cur_stream->state == USER_TCP_ESTABLISHED) {
		//RaiseReadEvent
		//cur_stream->s
		if (cur_stream->s) {
			// && (cur_stream->s->epoll & USER_EPOLLIN)
			// should move to epoll for check epoll type
			//AddtoEpollEvent
#if USER_ENABLE_EPOLL_RB
			if (tcp->ep) {
				epoll_event_callback(tcp->ep, cur_stream->s->id, USER_EPOLLIN);
			} 
			
#else
			user_epoll_add_event(tcp->ep, USER_EVENT_QUEUE, cur_stream->s, USER_EPOLLIN);
#endif
			if (!(cur_stream->s->opts & USER_TCP_NONBLOCK)) {
				user_tcp_flush_read_event(rcv);
			}
		}
	}
	return 1;
}

static int user_tcp_process_rst(user_tcp_manager *tcp, user_tcp_stream *cur_stream, uint32_t ack_seq) {
	user_trace_tcp("Stream %d: TCP RESET (%d)\n", 
			cur_stream->id, cur_stream->state);

	if (cur_stream->state <= USER_TCP_SYN_SENT) return 0;

	if (cur_stream->state == USER_TCP_SYN_RCVD)
	{
		if (ack_seq == cur_stream->snd_nxt)
		{
			cur_stream->state = USER_TCP_CLOSED;
			cur_stream->close_reason = TCP_RESET;
			DestroyTcpStream(tcp, cur_stream);
		}
		return 1;
	}

	if (cur_stream->state == USER_TCP_FIN_WAIT_1 ||
		cur_stream->state == USER_TCP_FIN_WAIT_2 ||
		cur_stream->state == USER_TCP_LAST_ACK ||
		cur_stream->state == USER_TCP_CLOSING ||
		cur_stream->state == USER_TCP_TIME_WAIT)
	{
			cur_stream->state = USER_TCP_CLOSED;
			cur_stream->close_reason = TCP_ACTIVE_CLOSE;
			DestroyTcpStream(tcp, cur_stream);
			return 1;
	}

	if (cur_stream->state >= USER_TCP_ESTABLISHED && 
		cur_stream->state <= USER_TCP_CLOSE_WAIT) {
		user_trace_tcp("Stream %d: Notifying connection reset.\n", cur_stream->id);
	}

	if (!(cur_stream->snd->on_closeq || 
		cur_stream->snd->on_closeq_int ||
		cur_stream->snd->on_resetq ||
		cur_stream->snd->on_resetq_int))
	{
			cur_stream->state = USER_TCP_CLOSE_WAIT;
			cur_stream->close_reason = TCP_RESET;
			//close event
	}
	return 1;
}

static void user_tcp_process_ack(user_tcp_manager *tcp, user_tcp_stream *cur_stream, uint32_t cur_ts, 
		struct tcphdr *tcph, uint32_t seq, uint32_t ack_seq, uint16_t window, int payloadlen) {

	user_tcp_send *snd = cur_stream->snd;
	
	uint32_t cwindow = window;

	if (!tcph->syn) {
		cwindow = cwindow << snd->wscale_peer;
	}

	uint32_t right_wnd_edge = snd->peer_wnd + cur_stream->rcv->snd_wl1;
	if (cur_stream->state == USER_TCP_FIN_WAIT_1 ||
		cur_stream->state == USER_TCP_FIN_WAIT_2 ||
		cur_stream->state == USER_TCP_CLOSING ||
		cur_stream->state == USER_TCP_CLOSE_WAIT ||
		cur_stream->state == USER_TCP_LAST_ACK) {

		if (snd->is_fin_sent && ack_seq == snd->fss + 1) 
			ack_seq --;		
	}

	if (TCP_SEQ_GT(ack_seq,snd->sndbuf->head_seq + snd->sndbuf->len)) {
		//char *state_str = TCPStateToString(cur_stream);
		user_trace_tcp("Stream %d (%d): invalid acknologement. ack_seq: %u, possible max_ack_seq: %u\n",
			cur_stream->id, cur_stream->state, ack_seq, 
				snd->sndbuf->head_seq + snd->sndbuf->len);

		return ;
	}

	uint32_t cwindow_prev;
	if (TCP_SEQ_LT(cur_stream->rcv->snd_wl1, seq) ||
		(cur_stream->rcv->snd_wl1 == seq && 
		TCP_SEQ_LT(cur_stream->rcv->snd_wl2, ack_seq)) ||
		(cur_stream->rcv->snd_wl2 == ack_seq &&
		cwindow > snd->peer_wnd)) {
		cwindow_prev = snd->peer_wnd;
		snd->peer_wnd = cwindow;
		cur_stream->rcv->snd_wl1 = seq;
		cur_stream->rcv->snd_wl2 = ack_seq;

		if (cwindow_prev < cur_stream->snd_nxt - snd->snd_una &&
			snd->peer_wnd >= cur_stream->snd_nxt - snd->snd_una) {
			user_trace_tcp("%u Broadcasting client window update! "
					"ack_seq: %u, peer_wnd: %u (before: %u), "
					"(snd_nxt - snd_una: %u)\n", 
					cur_stream->id, ack_seq, snd->peer_wnd, cwindow_prev, 
					cur_stream->snd_nxt - snd->snd_una);
			//RaiseWriteEvent(mtcp, cur_stream);
			user_tcp_flush_send_event(snd);
		}
	}

	uint8_t dup = 0;
	if (TCP_SEQ_LT(ack_seq, cur_stream->snd_nxt)) {
		if (ack_seq == cur_stream->rcv->last_ack_seq && payloadlen == 0) {
			if (cur_stream->rcv->snd_wl2 + snd->peer_wnd == right_wnd_edge) {
				if (cur_stream->rcv->dup_acks + 1 > cur_stream->rcv->dup_acks) {
					cur_stream->rcv->dup_acks ++;
				}
				dup = 1;
			}
		}
	}

	if (!dup) {
		cur_stream->rcv->dup_acks = 0;
		cur_stream->rcv->last_ack_seq = ack_seq;
	}

	if (dup && cur_stream->rcv->dup_acks == 3) {
		user_trace_tcp("Triple duplicated ACKs!! ack_seq: %u\n", ack_seq);
		if (TCP_SEQ_LT(ack_seq, cur_stream->snd_nxt)) {
			user_trace_tcp("Reducing snd_nxt from %u to %u\n", 
					cur_stream->snd_nxt, ack_seq);

			if (ack_seq != snd->snd_una) {
				user_trace_tcp("ack_seq and snd_una mismatch on tdp ack. "
						"ack_seq: %u, snd_una: %u\n", 
						ack_seq, snd->snd_una);
			}
			cur_stream->snd_nxt = ack_seq;
		}

		snd->ssthresh = MIN(snd->cwnd, snd->peer_wnd) / 2;
		if (snd->ssthresh < 2 * snd->mss) {
			snd->ssthresh = 2 * snd->mss;
		}
		snd->cwnd = snd->ssthresh + 3 * snd->mss;
		user_trace_tcp("Fast retransmission. cwnd: %u, ssthresh: %u\n", 
				snd->cwnd, snd->ssthresh);

		if (snd->nrtx < TCP_MAX_RTX)
		{
			snd->nrtx ++;
		}
		else
		{
			user_trace_tcp("Exceed MAX_RTX. \n");
		}
		user_tcp_addto_sendlist(tcp, cur_stream);
	} else if (cur_stream->rcv->dup_acks > 3) {

		if ((uint32_t)(snd->cwnd + snd->mss) > snd->cwnd) {
			snd->cwnd += snd->mss;
			user_trace_tcp("Dupack cwnd inflate. cwnd: %u, ssthresh: %u\n", 
					snd->cwnd, snd->ssthresh);
		}
	}

	if (TCP_SEQ_GT(ack_seq, cur_stream->snd_nxt)) {
		user_trace_tcp("Updating snd_nxt from %u to %u\n", 
				cur_stream->snd_nxt, ack_seq);
		cur_stream->snd_nxt = ack_seq;
		if (snd->sndbuf->len == 0) {
			user_tcp_remove_sendlist(tcp, cur_stream);
		}
	}

	if (TCP_SEQ_GEQ(snd->sndbuf->head_seq, ack_seq)) {
		return ;
	}

	uint32_t rmlen = ack_seq - snd->sndbuf->head_seq;
	if (rmlen > 0) {
		uint16_t packets = rmlen / snd->eff_mss;
		if ((rmlen / snd->eff_mss) * snd->eff_mss > rmlen) {
			packets ++;
		}
		if (cur_stream->saw_timestamp) {
			user_tcp_estimate_rtt(tcp, cur_stream, cur_ts-cur_stream->rcv->ts_lastack_rcvd);
			snd->rto = (cur_stream->rcv->srtt >> 3) + cur_stream->rcv->rttvar;
			assert(snd->rto > 0);
		} else {
			user_trace_tcp("not implemented.\n");
		}

		if (cur_stream->state >= USER_TCP_ESTABLISHED) {
			if (snd->cwnd < snd->ssthresh) {
				if ((snd->cwnd + snd->mss) > snd->cwnd) {
					snd->cwnd += snd->mss * packets;
				}
				user_trace_tcp("slow start cwnd : %u, ssthresh: %u\n",
					snd->cwnd, snd->ssthresh);
			}
		} else {
			uint32_t new_cwnd = snd->cwnd + packets * snd->mss * snd->mss / snd->cwnd;
			if (new_cwnd > snd->cwnd) {
				snd->cwnd = new_cwnd;
			}
		}
		if (pthread_mutex_lock(&snd->write_lock)) {
			if (errno == EDEADLK) {
				perror("ProcessACK: write_lock blocked\n");
			}
			assert(0);
		}
		int ret = SBRemove(tcp->rbm_snd, snd->sndbuf, rmlen);
		if (ret <= 0) return ;
		
		snd->snd_una = ack_seq;
		uint32_t snd_wnd_prev = snd->snd_wnd;
		snd->snd_wnd = snd->sndbuf->size - snd->sndbuf->len;

		if (snd_wnd_prev <= 0) {
			//Raise Write Event
			user_tcp_flush_send_event(snd);
		}

		pthread_mutex_unlock(&snd->write_lock);
		UpdateRetransmissionTimer(tcp, cur_stream, cur_ts);
	}
	
}


int user_tcp_process(user_nic_context *ctx, unsigned char *stream) {

	struct iphdr *iph = (struct iphdr *)(stream + sizeof(struct ethhdr));
	struct tcphdr *tcph = (struct tcphdr *)(stream + sizeof(struct ethhdr) + sizeof(struct iphdr));

	assert(sizeof(struct iphdr) == (iph->ihl<<2));

	int ip_len = ntohs(iph->tot_len);
	uint8_t *payload = (uint8_t*)tcph + (tcph->doff << 2); 
	int tcp_len = ip_len - (iph->ihl<<2);
	int payloadlen = tcp_len - (tcph->doff << 2);

	//unsigned short check = in_cksum((unsigned short*)tcph, tcp_len);
	unsigned short check = user_tcp_calculate_checksum((uint16_t *)tcph, tcp_len, iph->saddr, iph->daddr);
	user_trace_tcp("check : %x, orgin : %x, payloadlen:%d\n", check, tcph->check, payloadlen);
	if (check) return -1;

	user_tcp_stream tstream = {0};
#if 1
	tstream.saddr = iph->daddr;
	tstream.sport = tcph->dest;
	tstream.daddr = iph->saddr;
	tstream.dport = tcph->source;
#else
	ts.saddr = iph->saddr;
	ts.sport = tcph->source;
	ts.daddr = iph->daddr;
	ts.dport = tcph->dest;
#endif

	struct timeval cur_ts = {0};
	gettimeofday(&cur_ts, NULL);
	
	uint32_t ts = TIMEVAL_TO_TS(&cur_ts);
	uint32_t seq = ntohl(tcph->seq);
	uint32_t ack_seq = ntohl(tcph->ack_seq);
	uint16_t window = ntohs(tcph->window);

	
	user_trace_tcp("saddr:0x%x,sport:%d,daddr:0x%x,dport:%d, seq:%d, ack_seq:%d\n", 
			iph->daddr, ntohs(tcph->dest), iph->saddr, ntohs(tcph->source), 
			seq, ack_seq);

	user_tcp_stream *cur_stream = (user_tcp_stream*)StreamHTSearch(user_tcp->tcp_flow_table, &tstream);
	if (cur_stream == NULL) {
		cur_stream = user_create_stream(user_tcp, ts, iph, ip_len, tcph, seq, ack_seq, payloadlen, window);
		if (!cur_stream) { 
			return -2;
		}
	}
	int ret = 0;
	if (cur_stream->state > USER_TCP_SYN_RCVD) {
		ret = user_tcp_validseq(user_tcp, cur_stream, ts, tcph, seq, ack_seq, payloadlen);
		if (!ret) {
			user_trace_tcp("Stream %d: Unexpected sequence: %u, expected: %u\n",
					cur_stream->id, seq, cur_stream->rcv_nxt);
			return 1;
		}
	}

	user_trace_tcp("user_tcp_process state : %d\n", cur_stream->state);

	if (tcph->syn) {
		cur_stream->snd->peer_wnd = window;
	} else {
		cur_stream->snd->peer_wnd = (uint32_t)window << cur_stream->snd->wscale_peer;
	}

	cur_stream->last_active_ts = ts;
	UpdateTimeoutList(user_tcp, cur_stream);

	if (tcph->rst) {
		cur_stream->have_reset = 1;
		if (cur_stream->state > USER_TCP_SYN_SENT) {
			if (user_tcp_process_rst(user_tcp, cur_stream, ack_seq)) {
				return 1;
			}
		}
	}

	switch (cur_stream->state) {
		case USER_TCP_LISTEN: {
			user_tcp_handle_listen(user_tcp, ts, cur_stream, tcph);
			break;
		}
		case USER_TCP_SYN_SENT: {
			user_tcp_handle_syn_sent(user_tcp, ts, cur_stream, iph, tcph, seq, 
				ack_seq, payloadlen, window);
			break;
		}
		case USER_TCP_SYN_RCVD: {
			if (tcph->syn && seq == cur_stream->rcv->irs) {
				user_tcp_handle_listen(user_tcp, ts, cur_stream, tcph);
			} else {
				user_tcp_handle_syn_rcvd(user_tcp, ts, cur_stream, tcph, ack_seq);
				if (payloadlen > 0 && cur_stream->state == USER_TCP_ESTABLISHED) {
					user_tcp_handle_established(user_tcp, ts, cur_stream, tcph, seq, ack_seq,
						payload, payloadlen, window);
				}
			}
			break;
		}
		case USER_TCP_ESTABLISHED: {
			user_tcp_handle_established(user_tcp, ts, cur_stream, tcph, seq, ack_seq,
						payload, payloadlen, window);
			break;
		}
		case USER_TCP_CLOSE_WAIT: {
			user_tcp_handle_close_wait(user_tcp, ts, cur_stream, tcph, seq, ack_seq, 
						payloadlen, window);
			break;
		}
		case USER_TCP_LAST_ACK: {
			user_tcp_handle_last_ack(user_tcp, ts, iph, ip_len, cur_stream, tcph,
				seq, ack_seq, payloadlen, window);
			break;
		}
		case USER_TCP_FIN_WAIT_1: {
			user_tcp_handle_fin_wait_1(user_tcp, ts, cur_stream, tcph, seq, ack_seq,
				payload, payloadlen, window);
			break;
		}
		case USER_TCP_FIN_WAIT_2: {
			user_tcp_handle_fin_wait_2(user_tcp, ts, cur_stream, tcph, seq, ack_seq,
				payload, payloadlen, window);
			break;
		}
		case USER_TCP_CLOSING: {
			user_tcp_handle_closing(user_tcp, ts, cur_stream, tcph, seq, ack_seq,
				payloadlen, window);
			break;
		}
		case USER_TCP_TIME_WAIT: {
			if (cur_stream->on_timewait_list) {
				RemoveFromTimewaitList(user_tcp, cur_stream);
				AddtoTimewaitList(user_tcp, cur_stream, ts);
			}
			user_tcp_addto_controllist(user_tcp, cur_stream);
			break;
		}
		case USER_TCP_CLOSED: {
			break;
		}
	}

	return 1;
}

user_sender* user_tcp_create_sender(int ifidx) {
	user_sender *sender = (user_sender*)calloc(1, sizeof(user_sender));
	if (!sender) {
		return NULL;
	}
	sender->ifidx = ifidx;
	TAILQ_INIT(&sender->control_list);
	TAILQ_INIT(&sender->send_list);
	TAILQ_INIT(&sender->ack_list);

	sender->control_list_cnt = 0;
	sender->send_list_cnt = 0;
	sender->ack_list_cnt = 0;

	return sender;
}

void user_tcp_destroy_sender(user_sender *sender) {
	free(sender);
}

int user_tcp_init_manager(user_thread_context *ctx) {

	user_tcp_manager *tcp = (user_tcp_manager*)calloc(1, sizeof(user_tcp_manager));
	if (!tcp) {
		perror("malloc");
		return -1;
	}
	tcp->tcp_flow_table = CreateHashtable(HashFlow, EqualFlow, NUM_BINS_FLOWS);
	if (!tcp->tcp_flow_table) {
		user_trace_tcp("[%s:%s:%d] --> create hash table\n", __FILE__, __func__, __LINE__);
		return -2;
	}
	tcp->listeners = CreateHashtable(HashListener, EqualListener, NUM_BINS_LISTENERS);
	if (!tcp->listeners) {
		user_trace_tcp("[%s:%s:%d] --> create hash table\n", __FILE__, __func__, __LINE__);
		return -2;
	}
#ifdef HUGEPAGE
#define IS_HUGEPAGE		1
#else
#define IS_HUGEPAGE		0
#endif
	tcp->flow = user_mempool_create(sizeof(user_tcp_stream), sizeof(user_tcp_stream) * USER_MAX_CONCURRENCY, IS_HUGEPAGE);
	if (!tcp->flow) {
		user_trace_tcp("Failed to allocate tcp flow pool.\n");
		return -3;
	}
	tcp->rcv = user_mempool_create(sizeof(user_tcp_recv), sizeof(user_tcp_recv) * USER_MAX_CONCURRENCY, IS_HUGEPAGE);
	if (!tcp->rcv) {
		user_trace_tcp("Failed to allocate tcp recv pool.\n");
		return -3;
	}
	tcp->snd = user_mempool_create(sizeof(user_tcp_send), sizeof(user_tcp_send) * USER_MAX_CONCURRENCY, IS_HUGEPAGE);
	if (!tcp->snd) {
		user_trace_tcp("Failed to allocate tcp recv pool.\n");
		return -3;
	}
	tcp->rbm_snd = user_sbmanager_create(USER_SNDBUF_SIZE, USER_MAX_NUM_BUFFERS);
	if (!tcp->rbm_snd) {
		user_trace_tcp("Failed to create send ring buffer.\n");
		return -4;
	}
	tcp->rbm_rcv = RBManagerCreate(USER_RCVBUF_SIZE, USER_MAX_NUM_BUFFERS);
	if (!tcp->rbm_rcv) {
		user_trace_tcp("Failed to create recv ring buffer.\n");
		return -4;
	}

	InitializeTCPStreamManager();

#if USER_ENABLE_SOCKET_C10M

	tcp->fdtable = user_socket_init_fdtable();
	if (!tcp->fdtable) {
		user_trace_tcp("Failed to create fdtable.\n");
		return -4;
	}

#endif

	tcp->smap = (user_socket_map*)calloc(USER_MAX_CONCURRENCY, sizeof(user_socket_map));
	if (!tcp->smap) {
		user_trace_tcp("Failed to allocate memory for stream map.\n");
		return -5;
	}
	TAILQ_INIT(&tcp->free_smap);
	
	int i = 0;
	for (i = 0;i < USER_MAX_CONCURRENCY;i ++) {
		tcp->smap[i].id = i;
		tcp->smap[i].socktype = USER_TCP_SOCK_UNUSED;
		memset(&tcp->smap[i].s_addr, 0, sizeof(struct sockaddr_in));
		tcp->smap[i].stream = NULL;
		TAILQ_INSERT_TAIL(&tcp->free_smap, &tcp->smap[i], free_smap_link);
	}
	
	tcp->ctx = ctx;

	tcp->connectq = CreateStreamQueue(USER_BACKLOG_SIZE);
	if (!tcp->connectq) {
		user_trace_tcp("Failed to create connect queue.\n");
		return -6;
	}

	tcp->sendq = CreateStreamQueue(USER_MAX_CONCURRENCY);
	if (!tcp->sendq) {
		user_trace_tcp("Failed to create send queue.\n");
		return -6;
	}
	tcp->ackq = CreateStreamQueue(USER_MAX_CONCURRENCY);
	if (!tcp->sendq) {
		user_trace_tcp("Failed to create ack queue.\n");
		return -6;
	}
	tcp->closeq = CreateStreamQueue(USER_MAX_CONCURRENCY);
	if (!tcp->closeq) {
		user_trace_tcp("Failed to create close queue.\n");
		return -6;
	}
	tcp->closeq_int = CreateInternalStreamQueue(USER_MAX_CONCURRENCY);
	if (!tcp->closeq_int) {
		user_trace_tcp("Failed to create close int queue.\n");
		return -6;
	}

	tcp->resetq = CreateStreamQueue(USER_MAX_CONCURRENCY);
	if (!tcp->resetq) {
		user_trace_tcp("Failed to create reset queue.\n");
		return -6;
	}
	tcp->resetq_int = CreateInternalStreamQueue(USER_MAX_CONCURRENCY);
	if (!tcp->resetq_int) {
		user_trace_tcp("Failed to create reset int queue.\n");
		return -6;
	}
	tcp->destroyq = CreateStreamQueue(USER_MAX_CONCURRENCY);
	if (!tcp->destroyq) {
		user_trace_tcp("Failed to create destroy queue.\n");
		return -6;
	}
	tcp->g_sender = user_tcp_create_sender(-1);
	if (!tcp->g_sender) {
		user_trace_tcp("Failed to create global sender structure.\n");
		return -7;
	}
	for (i = 0;i < ETH_NUM;i ++) {
		tcp->n_sender[i] = user_tcp_create_sender(i);
		if (!tcp->n_sender[i]) {
			user_trace_tcp("Failed to create global sender structure.\n");
			return -7;
		}
	}

	tcp->rto_store = InitRTOHashstore();

	TAILQ_INIT(&tcp->timewait_list);
	TAILQ_INIT(&tcp->timeout_list);

#if USER_ENABLE_BLOCKING
	TAILQ_INIT(&tcp->rcv_br_list);
	TAILQ_INIT(&tcp->snd_br_list);
#endif

	user_tcp = tcp;

	return 0;
}

void user_tcp_init_thread_context(user_thread_context *ctx) {

	assert(ctx != NULL);
	
	ctx->cpu = 0;
	ctx->thread = pthread_self();

	user_tcp_init_manager(ctx);
	
	if (pthread_mutex_init(&ctx->smap_lock, NULL)) {
		user_trace_tcp("pthread_mutex_init of ctx->smap_lock\n");
		exit(-1);
	}

	if (pthread_mutex_init(&ctx->flow_pool_lock, NULL)) {
		user_trace_tcp("pthread_mutex_init of ctx->flow_pool_lock\n");
		exit(-1);
	}
	
	if (pthread_mutex_init(&ctx->socket_pool_lock, NULL)) {
		user_trace_tcp("pthread_mutex_init of ctx->socket_pool_lock\n");
		exit(-1);
	}


}


int user_tcp_handle_apicall(uint32_t cur_ts) {

	user_tcp_manager *tcp = user_get_tcp_manager();
	assert(tcp != NULL);

	user_tcp_stream *stream = NULL;
	while ((stream = StreamDequeue(tcp->connectq))) {
		user_tcp_addto_controllist(tcp, stream);
	}

	while ((stream = StreamDequeue(tcp->sendq))) {
		user_trace_tcp("buf: %s, mss:%d\n", stream->snd->sndbuf->data, stream->snd->mss);
		stream->snd->on_sendq = 0;
		user_tcp_addto_sendlist(tcp, stream);
	}

	while ((stream = StreamDequeue(tcp->ackq))) {
		stream->snd->on_ackq = 0;
		user_tcp_enqueue_acklist(tcp, stream, cur_ts, ACK_OPT_AGGREGATE);
	}

	int handled = 0, delayed = 0;
	int control = 0, send = 0, ack = 0;

	while ((stream = StreamDequeue(tcp->closeq))) {
		user_tcp_send *snd = stream->snd;
		snd->on_closeq = 0;

		if (snd->sndbuf) {
			snd->fss = snd->sndbuf->head_seq + snd->sndbuf->len;
		} else {
			snd->fss = stream->snd_nxt;
		}
		RemoveFromTimeoutList(tcp, stream);

		if (stream->have_reset) {
			handled ++;
			if (stream->state != USER_TCP_CLOSED) {
				stream->close_reason = TCP_RESET;
				stream->state = USER_TCP_CLOSED;

				user_trace_tcp("Stream %d: TCP_ST_CLOSED\n", stream->id);
				DestroyTcpStream(tcp, stream);
			} else {
				user_trace_tcp("Stream already closed.\n");
			}
		} else if (snd->on_control_list) {
			snd->on_closeq_int = 1;
			StreamInternalEnqueue(tcp->closeq_int, stream);
			delayed ++;

			if (snd->on_control_list) control ++;
			if (snd->on_send_list) send ++;
			if (snd->on_ack_list) ack ++;
		} else if (snd->on_send_list || snd->on_ack_list) {
			handled ++;
			if (stream->state == USER_TCP_ESTABLISHED) {
				stream->state = USER_TCP_FIN_WAIT_1;
				user_trace_tcp("Stream %d: USER_TCP_FIN_WAIT_1\n", stream->id);

			} else if (stream->state == USER_TCP_CLOSE_WAIT) {
				stream->state = USER_TCP_LAST_ACK;
				user_trace_tcp("Stream %d: USER_TCP_LAST_ACK\n", stream->id);
			}
			stream->control_list_waiting = 1;
		} else if (stream->state != USER_TCP_CLOSED) {
			handled++;
			if (stream->state == USER_TCP_ESTABLISHED) {
				stream->state = USER_TCP_FIN_WAIT_1;
				user_trace_tcp("Stream %d: USER_TCP_FIN_WAIT_1\n", stream->id);

			} else if (stream->state == USER_TCP_CLOSE_WAIT) {
				stream->state = USER_TCP_LAST_ACK;
				user_trace_tcp("Stream %d: USER_TCP_LAST_ACK\n", stream->id);
			}
			user_tcp_addto_controllist(tcp, stream);
		} else {
			user_trace_tcp("Already closed connection!\n");
		}
		
	}

	int cnt = 0;
	/* reset handling */
	while ((stream = StreamDequeue(tcp->resetq))) {
		stream->snd->on_resetq = 0;
		
		RemoveFromTimeoutList(tcp, stream);

		if (stream->have_reset) {
			if (stream->state != USER_TCP_CLOSED) {
				stream->close_reason = TCP_RESET;
				stream->state = USER_TCP_CLOSED;
				user_trace_tcp("Stream %d: TCP_ST_CLOSED\n", stream->id);
				DestroyTcpStream(tcp, stream);
			} else {
				user_trace_tcp("Stream already closed.\n");
			}

		} else if (stream->snd->on_control_list || 
				stream->snd->on_send_list || stream->snd->on_ack_list) {
			/* wait until all the queues are flushed */
			stream->snd->on_resetq_int = 1;
			StreamInternalEnqueue(tcp->resetq_int, stream);

		} else {
			if (stream->state != USER_TCP_CLOSED) {
				stream->close_reason = TCP_ACTIVE_CLOSE;
				stream->state = USER_TCP_CLOSED;
				user_trace_tcp("Stream %d: TCP_ST_CLOSED\n", stream->id);
				user_tcp_addto_controllist(tcp, stream);
			} else {
				user_trace_tcp("Stream already closed.\n");
			}
		}
	}
	
	cnt = 0;
	int max_cnt = tcp->resetq_int->count;
	while (cnt++ < max_cnt) {
		stream = StreamInternalDequeue(tcp->resetq_int);
		
		if (stream->snd->on_control_list || 
				stream->snd->on_send_list || stream->snd->on_ack_list) {
			/* wait until all the queues are flushed */
			StreamInternalEnqueue(tcp->resetq_int, stream);

		} else {
			stream->snd->on_resetq_int = 0;

			if (stream->state != USER_TCP_CLOSED) {
				stream->close_reason = TCP_ACTIVE_CLOSE;
				stream->state = USER_TCP_CLOSED;
				user_trace_tcp("Stream %d: USER_TCP_CLOSED\n", stream->id);
				user_tcp_addto_controllist(tcp, stream);
			} else {
				user_trace_tcp("Stream already closed.\n");
			}
		}
	}

	/* destroy streams in destroyq */
	while ((stream = StreamDequeue(tcp->destroyq))) {
		DestroyTcpStream(tcp, stream);
	}

	if (stream != NULL) {
		user_trace_tcp("user_tcp_handle_apicall --> state %d\n", stream->state);
	}

	tcp->wakeup_flag = 0;

	return 0;
}

int user_tcp_flush_sendbuffer(user_tcp_stream *cur_stream, uint32_t cur_ts) {

	user_tcp_manager *tcp = user_get_tcp_manager();
	assert(tcp != NULL);

	user_tcp_send *snd = cur_stream->snd;

	if (!snd->sndbuf) {
		user_trace_tcp("Stream %d: No send buffer available.\n", cur_stream->id);
		assert(0);
		return 0;
	}

	pthread_mutex_lock(&snd->write_lock);

	int packets = 0;
	if (snd->sndbuf->len == 0) {
		packets = 0;
		goto out;
	}

	uint32_t window = MIN(snd->cwnd, snd->peer_wnd);
	uint32_t seq = 0;
	uint32_t buffered_len = 0;
	uint8_t *data = NULL;
	uint32_t maxlen = snd->mss - user_calculate_option(USER_TCPHDR_ACK);
	uint16_t len = 0;
	uint8_t wack_sent = 0;
	int16_t sndlen = 0;

	while (1) {
		seq = cur_stream->snd_nxt;
		if (TCP_SEQ_LT(seq, snd->sndbuf->head_seq)) {
			user_trace_tcp("Stream %d: Invalid sequence to send. "
					"state: %d, seq: %u, head_seq: %u.\n", 
					cur_stream->id, cur_stream->state, 
					seq, snd->sndbuf->head_seq);
			assert(0);
			break;
		}

		buffered_len = snd->sndbuf->head_seq + snd->sndbuf->len - seq;
		if (cur_stream->state == USER_TCP_ESTABLISHED) {
			user_trace_tcp("head_seq: %u, len: %u, seq: %u, "
					"buffered_len: %u, mss:%d, cur_mss:%d\n", snd->sndbuf->head_seq, 
					snd->sndbuf->len, seq, buffered_len, snd->mss, cur_stream->snd->mss);
		}
		if (buffered_len == 0) break;

		data = snd->sndbuf->head + (seq - snd->sndbuf->head_seq);
		if (buffered_len > maxlen) {
			len = maxlen;
		} else {
			len = buffered_len;
		}

		if (len > window) {
			len = window;
		}

		if (len <= 0) break;

		if (cur_stream->state > USER_TCP_ESTABLISHED) {	
			user_trace_tcp("Flushing after ESTABLISHED: seq: %u, len: %u, "
					"buffered_len: %u\n", seq, len, buffered_len);
		}

		if (seq - snd->snd_una + len > window) {
			if (seq - snd->snd_una + len > snd->peer_wnd) {
				if (!wack_sent && TS_TO_MSEC(cur_ts - snd->ts_lastack_sent) > 500) {
					user_tcp_enqueue_acklist(tcp, cur_stream, cur_ts, ACK_OPT_WACK);
				} else {
					wack_sent = 1;
				}
			}
			packets = -3;
			goto out;
		}

		sndlen = user_tcp_send_tcppkt(cur_stream, cur_ts, USER_TCPHDR_ACK, data, len);
		if (sndlen < 0) {
			packets = sndlen;
			goto out;
		}
		packets ++;

		user_trace_api("window:%d, len:%d\n", window, len);
		window -= len;
	}

out:
	pthread_mutex_unlock(&snd->write_lock);

	return packets;
} 

int user_tcp_send_controlpkt(user_tcp_stream *cur_stream, uint32_t cur_ts) {
	user_tcp_manager *tcp = user_get_tcp_manager();
	if (!tcp) return -1;
	
	user_tcp_send *snd = cur_stream->snd;
	int ret = 0;

	if (cur_stream->state == USER_TCP_SYN_SENT) {

		ret = user_tcp_send_tcppkt(cur_stream, cur_ts, USER_TCPHDR_SYN, NULL, 0);

	} else if (cur_stream->state == USER_TCP_SYN_RCVD) {

		cur_stream->snd_nxt = snd->iss;
		ret = user_tcp_send_tcppkt(cur_stream, cur_ts, USER_TCPHDR_SYN | USER_TCPHDR_ACK, NULL, 0);

	} else if (cur_stream->state == USER_TCP_ESTABLISHED) {

		ret = user_tcp_send_tcppkt(cur_stream, cur_ts, USER_TCPHDR_ACK, NULL, 0);
		
	} else if (cur_stream->state == USER_TCP_CLOSE_WAIT) {

		ret = user_tcp_send_tcppkt(cur_stream, cur_ts, USER_TCPHDR_ACK, NULL, 0);
		
	} else if (cur_stream->state == USER_TCP_LAST_ACK) {

		if (snd->on_send_list || snd->on_ack_list) {
			ret = -1;
		} else {
			ret = user_tcp_send_tcppkt(cur_stream, cur_ts, USER_TCPHDR_FIN | USER_TCPHDR_ACK, NULL, 0);
		}
		
	} else if (cur_stream->state == USER_TCP_FIN_WAIT_1) {

		if (snd->on_send_list || snd->on_ack_list) {
			ret = -1;
		} else {
			ret = user_tcp_send_tcppkt(cur_stream, cur_ts, USER_TCPHDR_FIN | USER_TCPHDR_ACK, NULL, 0);
		}
		
	} else if (cur_stream->state == USER_TCP_FIN_WAIT_2) {

		ret = user_tcp_send_tcppkt(cur_stream, cur_ts, USER_TCPHDR_ACK, NULL, 0);
		
	} else if (cur_stream->state == USER_TCP_CLOSING) {

		if (snd->is_fin_sent) {
			if (cur_stream->snd_nxt == snd->fss) {
				ret = user_tcp_send_tcppkt(cur_stream, cur_ts, USER_TCPHDR_FIN | USER_TCPHDR_ACK, NULL, 0);
			} else {
				ret = user_tcp_send_tcppkt(cur_stream, cur_ts, USER_TCPHDR_ACK, NULL, 0);
			}
		} else {
			ret = user_tcp_send_tcppkt(cur_stream, cur_ts, USER_TCPHDR_FIN | USER_TCPHDR_ACK, NULL, 0);
		}
		
	} else if (cur_stream->state == USER_TCP_TIME_WAIT) {

		ret = user_tcp_send_tcppkt(cur_stream, cur_ts, USER_TCPHDR_ACK, NULL, 0);

	} else if (cur_stream->state == USER_TCP_CLOSED) {

		if (snd->on_send_list || snd->on_ack_list) {
			ret = -1;
		} else {
			ret = user_tcp_send_tcppkt(cur_stream, cur_ts, USER_TCPHDR_RST, NULL, 0);
			if (ret >= 0) {
				DestroyTcpStream(tcp,cur_stream);
			}
		}
	}

	return ret;
}

int user_tcp_write_controllist(user_sender *sender, uint32_t cur_ts, int thresh) {

	thresh = MIN(thresh, sender->control_list_cnt);
	user_tcp_stream *cur_stream = TAILQ_FIRST(&sender->control_list);
	user_tcp_stream *last = TAILQ_LAST(&sender->control_list, control_head);

	int cnt = 0, ret = -1;
	user_tcp_stream *next = NULL;
	while (cur_stream) {
		if (++cnt > thresh) break;
		
		next = TAILQ_NEXT(cur_stream, snd->control_link);
		TAILQ_REMOVE(&sender->control_list, cur_stream, snd->control_link);

		sender->control_list_cnt --;

		if (cur_stream->snd->on_control_list) {
			cur_stream->snd->on_control_list = 0;

			ret = user_tcp_send_controlpkt(cur_stream, cur_ts);
			if (ret < 0) {
				TAILQ_INSERT_HEAD(&sender->control_list, cur_stream, snd->control_link);
				cur_stream->snd->on_control_list = 1;
				sender->control_list_cnt ++;
				
				break;
			}
		} else {
			user_trace_tcp("Stream %d: not on control list.\n", cur_stream->id);
		}

		if (cur_stream == last) {
			break;
		}

		cur_stream = next;

	}

	return cnt;
}

int user_tcp_write_datalist(user_sender *sender, uint32_t cur_ts, int thresh) {

	user_tcp_manager *tcp = user_get_tcp_manager();
	assert(tcp != NULL);

	user_tcp_stream *cur_stream = TAILQ_FIRST(&sender->send_list);
	user_tcp_stream *last = TAILQ_LAST(&sender->send_list, send_head);

	int cnt = 0, ret = -1;
	user_tcp_stream *next = NULL;
	while (cur_stream) {
		if (++cnt > thresh) break;

		next = TAILQ_NEXT(cur_stream, snd->send_link);
		
		TAILQ_REMOVE(&sender->send_list, cur_stream, snd->send_link);

		user_trace_tcp("send_list:%d, state:%d\n", cur_stream->snd->on_send_list, cur_stream->state);
		if (cur_stream->snd->on_send_list) {
			ret = 0;

			if (cur_stream->state == USER_TCP_ESTABLISHED) {

				if (cur_stream->snd->on_control_list) {
					ret = -1;
				} else {
					ret = user_tcp_flush_sendbuffer(cur_stream, cur_ts);
				}
				
			} else if (cur_stream->state == USER_TCP_CLOSE_WAIT ||
				cur_stream->state == USER_TCP_FIN_WAIT_1 ||
				cur_stream->state == USER_TCP_LAST_ACK) {
				ret = user_tcp_flush_sendbuffer(cur_stream, cur_ts);
			} else {
				user_trace_tcp("Stream %d: on_send_list at state %d\n", 
						cur_stream->id, cur_stream->state);
			}

			if (ret < 0) {
				TAILQ_INSERT_TAIL(&sender->send_list, cur_stream, snd->send_link);
				break;
			} else {
				cur_stream->snd->on_send_list = 0;
				sender->send_list_cnt --;

				if (cur_stream->snd->ack_cnt > 0) {
					if (cur_stream->snd->ack_cnt > ret) {
						cur_stream->snd->ack_cnt -= ret;
					} else {
						cur_stream->snd->ack_cnt = 0;
					}
				}

				if (cur_stream->control_list_waiting) {
					if (!cur_stream->snd->on_ack_list) {
						cur_stream->control_list_waiting = 0;
						user_tcp_addto_controllist(tcp, cur_stream);
					}
				}
			}
		} else {

			user_trace_tcp("Stream %d: not on send list.\n", cur_stream->id);
		
		}

		if (cur_stream == last) break;

		cur_stream = next;
	}

	return cnt;
}


int user_tcp_write_acklist(user_sender *sender, uint32_t cur_ts, int thresh) {

	user_tcp_manager *tcp = user_get_tcp_manager();
	assert(tcp != NULL);

	user_tcp_stream *cur_stream = TAILQ_FIRST(&sender->ack_list);
	user_tcp_stream *last = TAILQ_LAST(&sender->ack_list, ack_head);
	user_tcp_stream *next = NULL;

	int cnt = 0;
	int to_ack = 0;
	int ret = 0;

	while (cur_stream) {

		if (++cnt > thresh) break;

		next = TAILQ_NEXT(cur_stream, snd->ack_link);

		if (cur_stream->snd->on_ack_list) {
			to_ack = 0;

			if (cur_stream->state == USER_TCP_ESTABLISHED ||
				cur_stream->state == USER_TCP_CLOSE_WAIT ||
				cur_stream->state == USER_TCP_FIN_WAIT_1 ||
				cur_stream->state == USER_TCP_FIN_WAIT_2 ||
				cur_stream->state == USER_TCP_TIME_WAIT)
			{
				if (cur_stream->rcv->recvbuf) {
					if (TCP_SEQ_LEQ(cur_stream->rcv_nxt, 
						cur_stream->rcv->recvbuf->head_seq + cur_stream->rcv->recvbuf->merged_len)) {
						to_ack = 1;
					}
				}
			}
			else
			{
				user_trace_tcp("Stream %u (%d): "
						"Try sending ack at not proper state. "
						"seq: %u, ack_seq: %u, on_control_list: %u\n", 
						cur_stream->id, cur_stream->state, 
						cur_stream->snd_nxt, cur_stream->rcv_nxt, 
						cur_stream->snd->on_control_list);
			}
			
			if (to_ack) {
			
				while (cur_stream->snd->ack_cnt > 0) {
					ret = user_tcp_send_tcppkt(cur_stream, cur_ts, USER_TCPHDR_ACK, NULL, 0);
					if (ret < 0) break;
	
					cur_stream->snd->ack_cnt --;
				}
	
				if (cur_stream->snd->is_wack) {
					cur_stream->snd->is_wack = 0;
					ret = user_tcp_send_tcppkt(cur_stream, cur_ts, USER_TCPHDR_ACK | USER_TCPHDR_CWR, NULL, 0);
					if (ret < 0) {
						cur_stream->snd->is_wack = 1;
					}
				}
	
				if (!(cur_stream->snd->ack_cnt || cur_stream->snd->is_wack)) {
					cur_stream->snd->on_ack_list = 0;
					TAILQ_REMOVE(&sender->ack_list, cur_stream, snd->ack_link);
					sender->ack_list_cnt --;
				}
			} else {
				cur_stream->snd->on_ack_list = 0;
				cur_stream->snd->ack_cnt = 0;
				cur_stream->snd->is_wack = 0;
	
				TAILQ_REMOVE(&sender->ack_list, cur_stream, snd->ack_link);
				sender->ack_list_cnt --;
			}

			if (cur_stream->control_list_waiting) {
				if (!cur_stream->snd->on_send_list) {
					cur_stream->control_list_waiting = 0;
					user_tcp_addto_controllist(tcp, cur_stream);
				}
			} 
			
		} else {
			user_trace_tcp("Stream %d: not on ack list.\n", cur_stream->id);
			TAILQ_REMOVE(&sender->ack_list, cur_stream, snd->ack_link);

			sender->ack_list_cnt --;
		}
		
		if (cur_stream == last) break;

		cur_stream = next;
		
	}

	return cnt;
}

void user_tcp_write_chunks(uint32_t cur_ts) {

	user_tcp_manager *tcp = user_get_tcp_manager();
	assert(tcp != NULL);

	int thresh = USER_MAX_CONCURRENCY;

	assert(tcp->g_sender != NULL);

	if (tcp->g_sender->control_list_cnt) {
		user_tcp_write_controllist(tcp->g_sender, cur_ts, thresh);
	}
	if (tcp->g_sender->ack_list_cnt) {
		user_tcp_write_acklist(tcp->g_sender, cur_ts, thresh);
	}

	if (tcp->g_sender->send_list_cnt) {
		user_tcp_write_datalist(tcp->g_sender, cur_ts, thresh);
	}

#if USER_ENABLE_MULTI_NIC
	int i = 0;
	for (i = 0;i < ETH_NUM;i ++) {
		if (tcp->n_sender[i]->control_list_cnt) {
			user_tcp_write_controllist(tcp->n_sender[i], cur_ts, thresh);
		}
		if (tcp->n_sender[i]->ack_list_cnt) {
			user_tcp_write_acklist(tcp->n_sender[i], cur_ts, thresh);
		}
		if (tcp->n_sender[i]->send_list_cnt) {
			user_tcp_write_datalist(tcp->n_sender[i], cur_ts, thresh);
		}
	}
#endif
}


