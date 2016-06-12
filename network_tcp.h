#ifndef _LTCP_H_
#define _LTCP_H_
struct bloom_filter
{
        unsigned int bitmap_bits_size;
        unsigned long bitmap[0];
};
extern struct socket *cli_conn_socket;
extern int tcp_client_fwd_filter(struct bloom_filter *);
extern int tcp_client_init(void);
extern void tcp_client_exit(void);
//extern struct bloom_filter *bflt;
extern int bit_size;
#endif
