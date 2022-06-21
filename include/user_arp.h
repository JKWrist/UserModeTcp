#ifndef __USER_ARP_H__
#define __USER_ARP_H__

#include "user_header.h"

#define MAX_ARPENTRY 256

typedef struct _user_arp_entry
{
  uint32_t ip;
  int8_t prefix;
  uint32_t ip_mask;
  uint32_t ip_masked;
  unsigned char haddr[ETH_ALEN];
} user_arp_entry;

typedef struct _user_arp_table
{
  user_arp_entry *entry;
  int entries;
} user_arp_table;

unsigned char *GetDestinationHWaddr(uint32_t dip);
int GetOutputInterface(uint32_t daddr);

int user_arp_register_entry(uint32_t ip, const unsigned char *haddr);
int user_arp_process(user_nic_context *ctx, unsigned char *stream);
int user_arp_init_table(void);

int str2mac(char *mac, char *str);

#endif
