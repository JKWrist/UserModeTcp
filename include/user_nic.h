#ifndef __USER_NIC_H__
#define __USER_NIC_H__


#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "user_tcp.h"

#include <net/netmap_user.h> 
#pragma pack(1)

#define NETMAP_WITH_LIBS

#define MAX_PKT_BURST	64
#define MAX_DEVICES		16

#define EXTRA_BUFS		512

#define ETHERNET_FRAME_SIZE		1514
#define ETHERNET_HEADER_LEN		14

#define IDLE_POLL_COUNT			10
#define IDLE_POLL_WAIT			1

typedef struct _user_nic_context {
	struct nm_desc *nmr;
	unsigned char snd_pktbuf[ETHERNET_FRAME_SIZE];
	unsigned char *rcv_pktbuf[MAX_PKT_BURST];
	uint16_t rcv_pkt_len[MAX_PKT_BURST];
	uint16_t snd_pkt_size;
	uint8_t dev_poll_flag;
	uint8_t idle_poll_count;
} user_nic_context;


typedef struct _user_nic_handler {
	int (*init)(user_thread_context *ctx, const char *ifname);
	int (*read)(user_nic_context *ctx, unsigned char **stream);
	int (*write)(user_nic_context *ctx, const void *stream, int length);
	unsigned char* (*get_wbuffer)(user_nic_context *ctx, int nif, uint16_t pktsize);
} user_nic_handler;

unsigned char* user_nic_get_wbuffer(user_nic_context *ctx, int nif, uint16_t pktsize);
unsigned char* user_nic_get_rbuffer(user_nic_context *ctx, int nif, uint16_t *len);

int user_nic_send_pkts(user_nic_context *ctx, int nif);
int user_nic_recv_pkts(user_nic_context *ctx, int ifidx);

int user_nic_read(user_nic_context *ctx, unsigned char **stream);
int user_nic_write(user_nic_context *ctx, const void *stream, int length);



int user_nic_init(user_thread_context *tctx, const char *ifname);
int user_nic_select(user_nic_context *ctx);


#if 0
extern user_nic_handler user_netmap_handler;
static user_nic_handler *user_current_handler = &user_netmap_handler;


#define USER_NIC_INIT(x, y)				user_current_handler->init(x, y)
#define USER_NIC_WRITE(x, y, z)			user_current_handler->write(x, y, z)
#define USER_NIC_READ(x, y)				user_current_handler->read(x, y)
#define USER_NIC_GET_WBUFFER(x, y, z) 	user_current_handler->get_wbuffer(x, y, z)
#endif


#endif