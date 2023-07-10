#include "user_header.h"
#include "user_nic.h"

void user_udp_pkt(struct udppkt *udp, struct udppkt *udp_rt)
{
    memcpy(udp_rt, udp, sizeof(struct udppkt));
    memcpy(udp_rt->eh.h_dest, udp->eh.h_source, ETH_ALEN);
    memcpy(udp_rt->eh.h_source, udp->eh.h_dest, ETH_ALEN);
    memcpy(&udp_rt->ip.saddr, &udp->ip.daddr, sizeof(udp->ip.saddr));
    memcpy(&udp_rt->ip.daddr, &udp->ip.saddr, sizeof(udp->ip.saddr));
    memcpy(&udp_rt->udp.source, &udp->udp.dest, sizeof(udp->udp.source));
    memcpy(&udp_rt->udp.dest, &udp->udp.source, sizeof(udp->udp.dest));
}

int user_udp_process(user_nic_context *ctx, unsigned char *stream)
{
    struct udppkt *udph = (struct udppkt *) stream;
    //struct in_addr addr;
    //addr.s_addr = udph->ip.saddr;

    int udp_length = ntohs(udph->udp.len);
    udph->body[udp_length - 8] = '\0';
    //printf("%s:%d:length:%d, ip_len:%d --> %s \n", inet_ntoa(addr), udph->udp.source,
    //udp_length, ntohs(udph->ip.tot_len), udph->body);

    struct udppkt udph_rt;
    user_udp_pkt(udph, &udph_rt);
    user_nic_write(ctx, &udph_rt, sizeof(struct udppkt));
    return 0;
}
