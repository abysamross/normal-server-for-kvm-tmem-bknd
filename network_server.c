#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/kthread.h>

#include <linux/errno.h>
#include <linux/types.h>

#include <linux/netdevice.h>
#include <linux/ip.h>
#include <linux/in.h>

#include <linux/unistd.h>
#include <linux/wait.h>

#include <net/sock.h>
#include <net/tcp.h>
#include <net/inet_connection_sock.h>
#include <net/request_sock.h>

#include "network_tcp.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Aby Sam Ross");

#define DEFAULT_PORT 2325
#define MODULE_NAME "tmem_tcp_server"
#define MAX_CONNS 16

static int tcp_listener_stopped = 0;
static int tcp_listener_started = 0;
static int tcp_acceptor_stopped = 0;
static int tcp_acceptor_started = 0;
//struct socket *client_conn_socket = NULL;
int bit_size = 268435456;
//int bit_size = 33554432;
struct bloom_filter *bflt = NULL;

DEFINE_SPINLOCK(tcp_server_lock);
static DECLARE_RWSEM(rs_rwmutex);
LIST_HEAD(rs_head);

struct tcp_conn_handler_data
{
        struct sockaddr_in *address;
        struct socket *accept_socket;
        int thread_id;
        char *ip;
        int port;
        char *in_buf;
};

struct tcp_conn_handler
{
        struct tcp_conn_handler_data *data[MAX_CONNS];
        struct task_struct *thread[MAX_CONNS];
        int tcp_conn_handler_stopped[MAX_CONNS]; 
};

struct tcp_conn_handler *tcp_conn_handler;


struct tcp_server_service
{
      int running;  
      struct socket *listen_socket;
      struct task_struct *thread;
      struct task_struct *accept_thread;
};

struct tcp_server_service *tcp_server;

char *inet_ntoa(struct in_addr *in)
{
        char *str_ip = NULL;
        u_int32_t int_ip = 0;
        
        str_ip = kmalloc(16 * sizeof(char), GFP_KERNEL);

        if(!str_ip)
                return NULL;
        else
                memset(str_ip, 0, 16);

        int_ip = in->s_addr;

        sprintf(str_ip, "%d.%d.%d.%d", (int_ip) & 0xFF, (int_ip >> 8) & 0xFF,
                                 (int_ip >> 16) & 0xFF, (int_ip >> 24) & 0xFF);
        
        return str_ip;
}

int tcp_server_send(struct socket *sock, const char *buf, const size_t length,\
                unsigned long flags)
{
        struct msghdr msg;
        struct kvec vec;
        int len, written = 0, left =length;
        mm_segment_t oldmm;

        msg.msg_name    = 0;
        msg.msg_namelen = 0;
        msg.msg_control = NULL;
        msg.msg_controllen = 0;
        msg.msg_flags = flags;
        msg.msg_flags   = 0;

        oldmm = get_fs(); set_fs(KERNEL_DS);

repeat_send:
        vec.iov_len = left;
        vec.iov_base = (char *)buf + written;

        len = kernel_sendmsg(sock, &msg, &vec, left, left);
        
        if((len == -ERESTARTSYS) || (!(flags & MSG_DONTWAIT) &&\
                                (len == -EAGAIN)))
                goto repeat_send;

        if(len > 0)
        {
                written += len;
                left -= len;
                if(left)
                        goto repeat_send;
        }
        
        set_fs(oldmm);
        return written?written:len;
}

//int tcp_server_receive(struct socket *sock, int id,struct sockaddr_in *address,
//                unsigned char *buf,int size, unsigned long flags)
int tcp_server_receive(struct socket *sock, void *rcv_buf, int size,\
                                        unsigned long flags, int huge)
{
        struct msghdr msg;
        struct kvec vec;
        int len, written = 0, left = size;
        char *buf = NULL;
        unsigned long *bitmap;
        
        if(sock==NULL)
        {
                pr_info(" *** mtp | tcp server receive socket is NULL |"
                        " tcp_server_receive *** \n");
                return -1;
        }

        if(huge)
                bitmap = (unsigned long *)rcv_buf;
        else
                buf = (char *)rcv_buf;


        msg.msg_name = 0;
        msg.msg_namelen = 0;
        msg.msg_control = NULL;
        msg.msg_controllen = 0;
        msg.msg_flags = flags;


read_again:

        vec.iov_len = left;

        if(huge)
                vec.iov_base = bitmap + written;
        else
                vec.iov_base = buf + written;

        if(!skb_queue_empty(&sock->sk->sk_receive_queue))
                pr_info("recv queue empty ? %s \n",
                skb_queue_empty(&sock->sk->sk_receive_queue)?"yes":"no");

        len = kernel_recvmsg(sock, &msg, &vec, size, size, flags);

        if(len == -EAGAIN || len == -ERESTARTSYS)
                goto read_again;
        
        if(huge)
        {
                if(len > 0)
                {
                        written += len;
                        left -= len;
                        if(left)
                                goto read_again;
                }
        }
        //len = msg.msg_iter.kvec->iov_len;
        return len;
}

//struct remote_server *register_rs(struct socket *socket, char* pkt,
//                int id, struct sockaddr_in *address)
struct remote_server *register_rs(struct socket *socket, char* ip, int port)
{
        struct remote_server *rs = NULL;
        
        rs = kmalloc(sizeof(struct remote_server), GFP_KERNEL);

        if(!rs)
                return NULL;

        rs->lcc_socket = socket; 
        rs->rs_ip = ip;
        rs->rs_port = port;
        rs->rs_bitmap = NULL;
        rs->rs_bmap_size = 0;
        
        pr_info(" *** mtp | registered remote server with ip: %s:%d | "
                "register_rs ***\n", rs->rs_ip, rs->rs_port);

        down_write(&rs_rwmutex);
        list_add_tail(&(rs->rs_list), &(rs_head));
        up_write(&rs_rwmutex);

        return rs;
}

/*
int update_bflt(struct remote_server *rs)
{
        struct remote_server *rs_tmp;

        down_read(&rs_rwmutex);
        if(!(list_empty(&rs_head)))
        {
                list_for_each_entry(rs_tmp, &(rs_head), rs_list)
                {
                        up_read(&rs_rwmutex);
                        pr_info("remote server info:\nip-> %s | port-> %d\n", 
                                rs_tmp->rs_ip, rs_tmp->rs_port);
        
                        if(strcmp(rs_tmp->rs_ip, rs->rs_ip) == 0)
                        {
                                //rs_tmp->rs_bitmap = bmap;
                        }
                        //kfree(ip);
                }
        }
        else
        {
              up_read(&rs_rwmutex);
              return -1;
              
        }

        return 0;
}
*/

struct remote_server *create_and_register_rs(char *ip, int port)
{
        int err;
        struct remote_server *rs;
        struct socket *rs_socket;

        err =  sock_create(PF_INET, SOCK_STREAM, IPPROTO_TCP, &rs_socket);

        if(err < 0 || rs_socket == NULL)
        {
              pr_info("*** mtp | error: %d creating "
                      "remote server connection socket for rs: %s |"
                      "create_and_register_rs ***\n", err, ip);

              goto fail;
        }

        rs = register_rs(rs_socket, ip, port);

        if(rs == NULL)
        {
                pr_info("*** mtp | error: registering rs: %s with "
                        " this server | create_and_register_rs ****\n", ip);

                goto fail;
        }

        /*yet to implement this*/
        //err = tcp_client_connect_rs(rs); 

        if(err < 0)
        {
              pr_info("*** mtp | error: %d connecting "
                      "this server to rs:%s |"
                      " create_and_register_rs ***\n", err, ip);

              down_write(&rs_rwmutex);
              list_del_init(&(rs->rs_list));
              up_write(&rs_rwmutex);
              kfree(rs->rs_ip);
              kfree(rs);
              goto fail;
        }

        return rs;
fail:
        return NULL;
}

struct remote_server* look_up_rs(char *ip, int port)
{
        struct remote_server *rs_tmp;

        down_read(&rs_rwmutex);
        if(!(list_empty(&rs_head)))
        {
                list_for_each_entry(rs_tmp, &(rs_head), rs_list)
                {
                        up_read(&rs_rwmutex);
        
                        if(strcmp(rs_tmp->rs_ip, ip) == 0)
                        {
                                pr_info(" *** mtp | found remote server "
                                        "info:\n | ip-> %s | port-> %d "
                                        "look_up_rs ***\n", 
                                        rs_tmp->rs_ip, rs_tmp->rs_port);

                                return rs_tmp;
                        }
                }
        }
        else
              up_read(&rs_rwmutex);

        return NULL;
}

//int receive_bflt(struct socket *accept_socket, char *in_buf, int bmap_size)
int receive_bflt(struct tcp_conn_handler_data *conn)
{
      struct remote_server *rs;
      char *ip, *tmp;
      int port, len = 49;
      char out_buf[len+1];
      unsigned long *bitmap = NULL;
      int bmap_size;
      int i;
      int ret;
      //int bmap_bytes_size = 0;

      ip = kmalloc(16 * sizeof(char), GFP_KERNEL);

      for(i = 0; i < 3; i++)
              tmp = strsep(&(conn->in_buf), ":");

      strcpy(ip, tmp);

      tmp = strsep(&(conn->in_buf), ":");
      kstrtoint(tmp, 10, &port);

      tmp = strsep(&(conn->in_buf), ":");
      kstrtoint(tmp, 10, &bmap_size);

      bitmap = vmalloc(bmap_size);
      memset(bitmap, 0, bmap_size);

      if(!bitmap)
      {
              pr_info("server[%d] failed to allocate memory for bflt of "
                      "rs: %s | receive_bflt ***\n", conn->thread_id, ip);
              kfree(ip);
              goto rcvfail; 
      }

      rs = look_up_rs(ip, port);
              
      if(rs == NULL)
      {
              rs = create_and_register_rs(ip, port);

              if(rs == NULL)
              {
                      pr_info(" *** mtp | server[%d] create_and_register_rs for"
                              " rs: %s failed | receive_bflt ***\n",
                              conn->thread_id, ip);

                      goto rcvfail;
              }
      }
      //receive actual bflt bitmap
      memset(out_buf, 0, len+1);
      strcat(out_buf, "SEND:BFLT");
       
      pr_info(" *** mtp | server[%d] sending response: %s for bflt of rs: %s |"
              " receive_bflt ***\n", conn->thread_id, out_buf, ip);
        
      ret =
      tcp_server_send(conn->accept_socket,(void *)out_buf,strlen(out_buf),\
                      MSG_DONTWAIT);

      ret = 
      tcp_server_receive(conn->accept_socket, (void *)bitmap, bmap_size,\
                         MSG_DONTWAIT, 1);

      pr_info(" *** mtp | server[%d] received bitmap (size: %d) of rs: %s | "
              "receive_bflt ***\n", conn->thread_id, ret, ip);

      /*free the bitmap for an existing server*/
      if(rs->rs_bitmap != NULL)
              vfree(rs->rs_bitmap);

      rs->rs_bitmap = bitmap;

      pr_info(" *** mtp | server[%d] testing received bitmap of rs: %s\n"
              "bitmap[0]: %d, bitmap[10]: %d |\n receive_bflt ***\n", 
              conn->thread_id, ip, test_bit(0, rs->rs_bitmap),
              test_bit(0, rs->rs_bitmap));

      return 0;

rcvfail:
      return -1;
}

int connection_handler(void *data)
{
       int ret;
       int len = 49;
       char in_buf[len+1];
       char out_buf[len+1];

       struct tcp_conn_handler_data *conn_data = 
               (struct tcp_conn_handler_data *)data;

       //struct sockaddr_in *address = conn_data->address;
       struct socket *accept_socket = conn_data->accept_socket;
       char *ip = conn_data->ip;
       int port = conn_data->port;
       int id = conn_data->thread_id;
       DECLARE_WAITQUEUE(recv_wait, current);

       conn_data->in_buf = in_buf;
       allow_signal(SIGKILL|SIGSTOP);

       while(1)
       {
              add_wait_queue(&accept_socket->sk->sk_wq->wait, &recv_wait);  

              while(skb_queue_empty(&accept_socket->sk->sk_receive_queue))
              {
                      __set_current_state(TASK_INTERRUPTIBLE);
                      schedule_timeout(HZ);

                      if(kthread_should_stop())
                      {
                              pr_info(" *** mtp | tcp server handle connection "
                                "thread stopped | connection_handler *** \n");

                              tcp_conn_handler->tcp_conn_handler_stopped[id]= 1;

                              __set_current_state(TASK_RUNNING);
                              remove_wait_queue(&accept_socket->sk->sk_wq->wait,\
                                              &recv_wait);
                              kfree(tcp_conn_handler->data[id]->address);
                              kfree(tcp_conn_handler->data[id]->ip);
                              kfree(tcp_conn_handler->data[id]);
                              //kfree(tmp);
                        sock_release(tcp_conn_handler->data[id]->accept_socket);
                              return 0;
                      }

                      if(signal_pending(current))
                      {
                              __set_current_state(TASK_RUNNING);
                              remove_wait_queue(&accept_socket->sk->sk_wq->wait,\
                                              &recv_wait);
                              goto out;
                      }
              }
              __set_current_state(TASK_RUNNING);
              remove_wait_queue(&accept_socket->sk->sk_wq->wait, &recv_wait);

              pr_info(" *** mtp | server[%d] receiving message | "
                      "connection_handler ***\n", id);

              memset(in_buf, 0, len+1);
              //ret = tcp_server_receive(accept_socket, id, address, in_buf, len,
              //                         MSG_DONTWAIT);
              ret = tcp_server_receive(accept_socket, (void *)in_buf, len,\
                                                        MSG_DONTWAIT, 0);

              pr_info(" *** mtp | server[%d] received: %d bytes from %s:%d, "
                      "says: %s | connection_handler ***\n",
                      id, ret, ip, port, in_buf);

              if(ret > 0)
              {
                      if(memcmp(in_buf, "RECV", 4) == 0)
                      {
                              if(memcmp(in_buf+5, "BFLT", 4) == 0)
                              {

                                      if(receive_bflt(conn_data) < 0)
                                              goto bfltfail;

                                      //update_bflt(rs);
                                      
                                      memset(out_buf, 0, len+1);
                                      strcat(out_buf, "DONE:BFLT");
                                      goto bfltresp;
bfltfail:
                                      memset(out_buf, 0, len+1);
                                      strcat(out_buf, "FAIL:BFLT");
bfltresp:
                                      pr_info(" *** mtp | sending response: %s |"
                                              " connection_handler ***\n",
                                              out_buf);

                                      tcp_server_send(accept_socket, out_buf,\
                                                      strlen(out_buf),\
                                                      MSG_DONTWAIT);
                              }
                              else if(memcmp(in_buf+4, "PAGE", 4) == 0)
                              {
                                      //obtain ip or unique id from in_buf
                                      //do comparison of this page in the tmem
                                      //bknd.
                                      //Give response.
                              }
                      }
                      else if(memcmp(in_buf, "ADIOS", 5) == 0)
                      {
                              memset(out_buf, 0, len+1);
                              strcat(out_buf, "ADIOSAMIGO");
                              pr_info(" *** mtp | sending response: %s"
                                      " | connection_handler ***\n", out_buf);
                              tcp_server_send(accept_socket, out_buf,\
                                              strlen(out_buf), MSG_DONTWAIT);
                              /* here also the local client connection 
                               * with the leader server should be severed.
                               * Not only that, the entire local server 
                               * module should be brought down as there is
                               * no longer a leader server */
                              if(cli_conn_socket)
                              {
                                      pr_info(" *** mtp | 1. Closing client "
                                              "connection | connection_handler "
                                              "**** \n");
                                      tcp_client_exit();
                              }
                              break;
                      }
              }
       }
out:
       tcp_conn_handler->tcp_conn_handler_stopped[id]= 1;
       kfree(tcp_conn_handler->data[id]->address);
       kfree(tcp_conn_handler->data[id]->ip);
       kfree(tcp_conn_handler->data[id]);
       //kfree(tmp);
       sock_release(tcp_conn_handler->data[id]->accept_socket);
       tcp_conn_handler->thread[id] = NULL;
       do_exit(0);
}

int tcp_server_accept(void)
{
        int accept_err = 0;
        struct socket *socket;
        struct socket *accept_socket = NULL;
        struct inet_connection_sock *isock; 
        int id = 0;
        DECLARE_WAITQUEUE(accept_wait, current);

        allow_signal(SIGKILL|SIGSTOP);

        tcp_acceptor_started = 1;
        socket = tcp_server->listen_socket;
        pr_info(" *** mtp | creating the accept socket | tcp_server_accept "
                "*** \n");

        while(1)
        {
                struct tcp_conn_handler_data *data = NULL;
                struct sockaddr_in *client = NULL;
                char *sip;
                int sport;
                int addr_len;

                accept_err =  
                        sock_create(socket->sk->sk_family, socket->type,\
                                    socket->sk->sk_protocol, &accept_socket);

                if(accept_err < 0 || !accept_socket)
                {
                        pr_info(" *** mtp | accept_error: %d while creating "
                                "tcp server accept socket | "
                                "tcp_server_accept *** \n", accept_err);
                        goto err;
                }

                accept_socket->type = socket->type;
                accept_socket->ops  = socket->ops;

                isock = inet_csk(socket->sk);
                
               add_wait_queue(&socket->sk->sk_wq->wait, &accept_wait);
               while(reqsk_queue_empty(&isock->icsk_accept_queue))
               {
                       __set_current_state(TASK_INTERRUPTIBLE);
                       //set_current_state(TASK_INTERRUPTIBLE);

                       //change this HZ to about 5 mins in jiffies
                       schedule_timeout(HZ);


                       if(kthread_should_stop())
                       {
                               pr_info(" *** mtp | 1. tcp server acceptor thread "
                                       "stopped | tcp_server_accept *** \n");
                               tcp_acceptor_stopped = 1;
                               __set_current_state(TASK_RUNNING);
                               remove_wait_queue(&socket->sk->sk_wq->wait,\
                                               &accept_wait);
                               sock_release(accept_socket);
                               return 0;
                       }

                       if(signal_pending(current))
                       {
                               __set_current_state(TASK_RUNNING);
                               remove_wait_queue(&socket->sk->sk_wq->wait,\
                                               &accept_wait);
                               goto release;
                       }

               } 
               __set_current_state(TASK_RUNNING);
               remove_wait_queue(&socket->sk->sk_wq->wait, &accept_wait);

               pr_info(" *** mtp | accept connection | tcp_server_accept ***\n");

               accept_err = 
                       socket->ops->accept(socket, accept_socket, O_NONBLOCK);

               if(accept_err < 0)
               {
                       pr_info(" *** mtp | accept_error: %d while accepting "
                               "tcp server | tcp_server_accept *** \n",
                               accept_err);
                       goto release;
               }

               client = kmalloc(sizeof(struct sockaddr_in), GFP_KERNEL);   
               memset(client, 0, sizeof(struct sockaddr_in));

               addr_len = sizeof(struct sockaddr_in);

               accept_err = 
               accept_socket->ops->getname(accept_socket,\
                               (struct sockaddr *)client,\
                               &addr_len, 2);

               if(accept_err < 0)
               {
                       pr_info(" *** mtp | accept_error: %d in getname "
                               "tcp server | tcp_server_accept *** \n",
                               accept_err);
                       goto release;
               }

               sip = inet_ntoa(&(client->sin_addr));
               sport = ntohs(client->sin_port);

               pr_info("connection from: %s %d \n", sip, sport);

               //kfree(tmp);

               pr_info("handle connection\n");

               /*should I protect this against concurrent access?*/
               for(id = 0; id < MAX_CONNS; id++)
               {
                        if(tcp_conn_handler->thread[id] == NULL)
                                break;
               }

               pr_info("gave free id: %d\n", id);

               if(id == MAX_CONNS)
                       goto release;

               data = kmalloc(sizeof(struct tcp_conn_handler_data), GFP_KERNEL);
               memset(data, 0, sizeof(struct tcp_conn_handler_data));

               data->address = client;
               data->accept_socket = accept_socket;
               data->thread_id = id;
               data->ip = sip;
               data->port = sport;
               data->in_buf = NULL;

               tcp_conn_handler->tcp_conn_handler_stopped[id] = 0;
               tcp_conn_handler->data[id] = data;
               tcp_conn_handler->thread[id] = 
               kthread_run((void *)connection_handler, (void *)data, MODULE_NAME);

               if(kthread_should_stop())
               {
                       pr_info(" *** mtp | 2. tcp server acceptor thread stopped"
                               " | tcp_server_accept *** \n");
                       tcp_acceptor_stopped = 1;
                       //sock_release(accept_socket);
                       return 0;
               }
                        
               if(signal_pending(current))
               {
                       break;
               }
        }

        tcp_acceptor_stopped = 1;
        do_exit(0);
release: 
       sock_release(accept_socket);
err:
       tcp_acceptor_stopped = 1;
       do_exit(0);
}

int tcp_server_listen(void)
{
        int server_err;
        struct socket *conn_socket;
        struct sockaddr_in server;

        DECLARE_WAIT_QUEUE_HEAD(wq);

        allow_signal(SIGKILL|SIGTERM);         

        tcp_listener_started = 1;
        server_err = sock_create(PF_INET, SOCK_STREAM, IPPROTO_TCP,\
                                &tcp_server->listen_socket);
        if(server_err < 0)
        {
                pr_info(" *** mtp | Error: %d while creating tcp server "
                        "listen socket | tcp_server_listen *** \n", server_err);
                goto err;
        }

        conn_socket = tcp_server->listen_socket;
        tcp_server->listen_socket->sk->sk_reuse = 1;

        server.sin_addr.s_addr = htonl(INADDR_ANY);
        server.sin_family = AF_INET;
        server.sin_port = htons(DEFAULT_PORT);

        server_err = 
        conn_socket->ops->bind(conn_socket, (struct sockaddr*)&server,\
                        sizeof(server));

        if(server_err < 0) {
                pr_info(" *** mtp | Error: %d while binding tcp server "
                        "listen socket | tcp_server_listen *** \n", server_err);
                goto release;
        }

        server_err = conn_socket->ops->listen(conn_socket, 16);

        if(server_err < 0)
        {
                pr_info(" *** mtp | Error: %d while listening in tcp "
                        "server listen socket | tcp_server_listen "
                        "*** \n", server_err);
                        goto release;
        }

        tcp_server->accept_thread = 
                kthread_run((void*)tcp_server_accept, NULL, MODULE_NAME);

        while(1)
        {
                wait_event_timeout(wq, 0, 3*HZ);

                if(kthread_should_stop())
                {
                        pr_info(" *** mtp | tcp server listening thread"
                                " stopped | tcp_server_listen *** \n");
                        tcp_listener_stopped = 1;
                        return 0;
                }

                if(signal_pending(current))
                        goto release;
        }

        sock_release(conn_socket);
        tcp_listener_stopped = 1;
        do_exit(0);
release:
        sock_release(conn_socket);
err:
        tcp_listener_stopped = 1;
        do_exit(0);
}

int tcp_server_start(void)
{
        tcp_server->running = 1;
        tcp_server->thread = kthread_run((void *)tcp_server_listen, NULL,\
                                        MODULE_NAME);
        if(tcp_server->thread == NULL)
                return -1;
        
        if(tcp_listener_stopped)
                return -1;

        return 0;
}

static int __init network_server_init(void)
{
        struct bloom_filter *bflt = NULL;
        unsigned long bitmap_bytes_size; 

        pr_info(" *** mtp | initiating network_server | "
                "network_server_init ***\n");

        tcp_server = kmalloc(sizeof(struct tcp_server_service), GFP_KERNEL);
        memset(tcp_server, 0, sizeof(struct tcp_server_service));

        tcp_conn_handler = kmalloc(sizeof(struct tcp_conn_handler), GFP_KERNEL);
        memset(tcp_conn_handler, 0, sizeof(struct tcp_conn_handler));

        bitmap_bytes_size = 
                BITS_TO_LONGS(bit_size)*sizeof(unsigned long);
        
        bflt = vmalloc(sizeof(*bflt)+bitmap_bytes_size);
        memset(bflt, 0, sizeof(*bflt)+bitmap_bytes_size);

        if(!bflt)
        {
                pr_info(" *** mtp | failed to allocate memory for bflt | "
                        "network_server_init *** \n");
        }
        else
        {
                bflt->bitmap_size = bit_size;

                /* randomly set two bits in the bitmap */
                set_bit(0, bflt->bitmap);
                set_bit(10, bflt->bitmap);
                //set_bit(1024, bflt->bitmap);
                //set_bit(4095, bflt->bitmap);
                //set_bit(1024, bflt->bitmap);
                //set_bit(20160, bflt->bitmap);
        }

        if(tcp_server_start() != 0)
        {
                pr_info(" *** mtp | could not start network server "
                        "network_server_init *** \n");
                return -1;
        }

        if(tcp_listener_stopped)
        {
                pr_info(" *** mtp | could not start network server "
                        "network_server_init *** \n");
                return -1;
        }

        if(tcp_client_init() != 0) 
        {
                int ret;
                
                if(tcp_acceptor_started && !tcp_acceptor_stopped)
                {
                        ret = kthread_stop(tcp_server->accept_thread);
                        if(!ret)
                                pr_info(" *** mtp | stopping tcp server accept "
                                        "thread as local client could not setup "
                                        "a connection with leader server | "
                                        "network_server_init *** \n");
                }

                if(tcp_listener_started && !tcp_listener_stopped)
                {
                        ret = kthread_stop(tcp_server->thread);
                        if(!ret)
                                pr_info(" *** mtp | stopping tcp server listening"
                                        " thread as local client could not setup"
                                        " a connection with leader server |"
                                        " network_server_init *** \n");

                        if(tcp_server->listen_socket != NULL)
                        {
                                sock_release(tcp_server->listen_socket);
                                tcp_server->listen_socket = NULL;
                        }
                }
                

                kfree(tcp_conn_handler);
                kfree(tcp_server);
                tcp_server = NULL;
                return -1;
        }

        if(bflt)
        {
                if(tcp_client_fwd_filter(bflt) < 0)
                {
                        /*
                        char out[50];
                        memset(out, 0, 50);
                        strcat(out, "FAIL");
                        msleep(5000);
                        tcp_client_passon(out);
                        */
                        /*if you want try resending the bflt here*/
                        pr_info(" *** mtp | tcp_client_fwd_filter attmept failed | "
                                "network_server_init ***\n");
                }
        }
        else
                pr_info(" *** mtp | network server unable to call "
                        "tcp_client_fwd_filter (bflt) as bflt not created | "
                        "network_server_init ***\n");

        return 0;
}

static void __exit network_server_exit(void)
{
        int ret;
        int id;

        if(tcp_server->thread == NULL)
                pr_info(" *** mtp | No kernel thread to kill | "
                        "network_server_exit *** \n");
        else
        {
                for(id = 0; id < MAX_CONNS; id++)
                {
                        if(tcp_conn_handler->thread[id] != NULL)
                        {

                        if(!tcp_conn_handler->tcp_conn_handler_stopped[id])
                                {
                                        ret = 
                                kthread_stop(tcp_conn_handler->thread[id]);

                                        if(!ret)
                                                pr_info(" *** mtp | tcp server "
                                                "connection handler thread: %d "
                                                "stopped | network_server_exit "
                                                "*** \n", id);
                                }
                       }
                }

                if(tcp_acceptor_started && !tcp_acceptor_stopped)
                {
                        ret = kthread_stop(tcp_server->accept_thread);
                        if(!ret)
                                pr_info(" *** mtp | tcp server acceptor thread"
                                        " stopped | network_server_exit *** \n");
                }

                if(tcp_listener_started && !tcp_listener_stopped)
                {
                        ret = kthread_stop(tcp_server->thread);
                        if(!ret)
                                pr_info(" *** mtp | tcp server listening thread"
                                        " stopped | network_server_exit *** \n");

                        if(tcp_server->listen_socket != NULL)
                        {
                                sock_release(tcp_server->listen_socket);
                                tcp_server->listen_socket = NULL;
                        }

                }

                kfree(tcp_conn_handler);
                kfree(tcp_server);
                tcp_server = NULL;
                if(cli_conn_socket)
                {
                        pr_info(" *** mtp | 2. Closing client connection | "
                                "network_server_exit ***\n");
                        tcp_client_exit();
                }
        }

        pr_info(" *** mtp | network server module unloaded | "
                "network_server_exit *** \n");
}
module_init(network_server_init)
module_exit(network_server_exit)
