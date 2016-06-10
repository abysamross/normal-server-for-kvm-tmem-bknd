#ifndef _LTCP_H_
#define _LTCP_H_
extern struct socket *cli_conn_socket;
extern int tcp_client_fwd_filter(void);
extern int tcp_client_init(void);
extern void tcp_client_exit(void);
#endif
