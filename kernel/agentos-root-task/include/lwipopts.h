#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS               1
#define SYS_LIGHTWEIGHT_PROT 0
#define MEM_LIBC_MALLOC      0
#define MEM_ALIGNMENT        4
#define MEM_SIZE             49152
#define MEMP_NUM_TCP_PCB     8
#define MEMP_NUM_TCP_PCB_LISTEN 4
#define MEMP_NUM_PBUF        16
#define MEMP_NUM_ARP_QUEUE   5
#define PBUF_POOL_SIZE       16
#define PBUF_POOL_BUFSIZE    1536
#define LWIP_TCP             1
#define TCP_WND              (4 * TCP_MSS)
#define TCP_MSS              1460
#define TCP_SND_BUF          (4 * TCP_MSS)
#define LWIP_UDP             1
#define LWIP_ARP             1
#define ARP_TABLE_SIZE       4
#define LWIP_ETHERNET        1
#define LWIP_DHCP            0
#define LWIP_AUTOIP          0
#define LWIP_SNMP            0
#define LWIP_IGMP            0
#define LWIP_DNS             0
#define LWIP_SOCKET          0
#define LWIP_NETCONN         0
#define LWIP_NETIF_API       0
#define LWIP_STATS           0
#define LWIP_CHECKSUM_ON_COPY 0

/* Memory model: static heap only */
#define LWIP_RAND()          42u

#endif /* LWIPOPTS_H */
