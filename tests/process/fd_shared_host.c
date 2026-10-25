/* Production fd.c under ASan/UBSan. Scheduler/network shims provide a
 * deterministic blocked read; native acceptance establishes real device I/O. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "process/process.h"
#include "process/ksync.h"
#include "net/net.h"
#undef current_thread
#undef current_process
static __thread struct thread *current_thread;
static __thread struct process *current_process;
static pthread_mutex_t tables=PTHREAD_MUTEX_INITIALIZER,scheduler=PTHREAD_MUTEX_INITIALIZER;
static void host_lock(struct mutex *m){(void)m;assert(!pthread_mutex_lock(&tables));}
static void host_unlock(struct mutex *m){(void)m;assert(!pthread_mutex_unlock(&tables));}
static void host_init(struct mutex *m){memset(m,0,sizeof *m);}
static void host_sched_lock(void){assert(!pthread_mutex_lock(&scheduler));}
static void host_sched_unlock(void){assert(!pthread_mutex_unlock(&scheduler));}
#define mutex_lock host_lock
#define mutex_unlock host_unlock
#define mutex_init host_init
#define sched_lock host_sched_lock
#define sched_unlock host_sched_unlock
static int fail_alloc;
void *kmalloc(uint64_t n){return fail_alloc?NULL:malloc(n);}
void kfree(void *p){free(p);}
static int alive[1024],closed[1024],aborted[1024],next_conn;
int net_tcp_connect(uint32_t ip,uint16_t port){(void)ip;(void)port;int n=++next_conn;assert(n<1024);alive[n]=1;return n;}
int net_tcp_connect_start(uint32_t ip,uint16_t port){return net_tcp_connect(ip,port);}
int net_tcp_listen(uint16_t port){return net_tcp_connect(0,port);}
int net_tcp_accept(int conn){assert(alive[conn]);return -1;}
int net_tcp_accept_nb(int conn){assert(alive[conn]);return -2;}
int net_tcp_ready(int conn){return alive[conn]?TCP_RDY_WRITE:-1;}
int net_tcp_send(int conn,const void *data,uint32_t len){(void)data;assert(alive[conn]);return len;}
int net_tcp_recv_nb(int conn,void *buf,uint32_t cap){(void)buf;(void)cap;assert(alive[conn]);return -2;}
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition=PTHREAD_COND_INITIALIZER;
static int started,release_read;
int net_tcp_recv(int conn,void *buf,uint32_t cap){
    assert(cap>=8);pthread_mutex_lock(&gate);started=1;pthread_cond_broadcast(&condition);
    while(!release_read)pthread_cond_wait(&condition,&gate);
    assert(alive[conn]);memcpy(buf,"original",8);pthread_mutex_unlock(&gate);return 8;
}
void net_tcp_close(int conn){assert(alive[conn]);alive[conn]=0;closed[conn]++;}
void net_tcp_abort(int conn){assert(alive[conn]);alive[conn]=0;aborted[conn]++;}
void pty_pin_locked(struct pty *p){(void)p;abort();}
void pty_unpin_locked(struct pty *p){(void)p;abort();}
#include "../../kernel/fs/fd.c"

static struct process owner,child;
static struct thread main_thread,reader_thread;
static int reader_fd;
static void *reader(void *arg){
    (void)arg;current_process=&owner;current_thread=&reader_thread;
    char data[8];size_t n=0;
    assert(vfs_fd_read(reader_fd,data,sizeof data,&n)==0 && n==8 && !memcmp(data,"original",8));return NULL;
}
static void reaper(struct process *p){
    host_sched_lock();
    for(int i=0;i<FD_MAX_OPEN;i++)if(p->fds[i].used){assert(p->fds[i].ops->close_locked);p->fds[i].ops->close_locked(&p->fds[i]);memset(&p->fds[i],0,sizeof p->fds[i]);}
    host_sched_unlock();
}
int main(void){
    current_process=&owner;current_thread=&main_thread;
    fail_alloc=1;assert(fd_alloc_socket(&owner)==-EMBK_ENOMEM && fd_shared_live_count()==0);fail_alloc=0;
    int fd=fd_alloc_socket(&owner);assert(fd==0 && fd_socket_connect(&owner,fd,0,1)==0);
    int conn=owner.fds[fd].shared->conn;
    assert(vfs_fd_set_cloexec(fd,true)==0);
    int alias=vfs_fd_dup(fd,-1,0);assert(alias==1 && vfs_fd_get_cloexec(alias)==0);
    assert(vfs_fd_set_status(alias,O_NONBLOCK|O_APPEND)==0 && vfs_fd_get_flags(fd)==(O_RDWR|O_NONBLOCK|O_APPEND));
    assert(vfs_fd_set_nonblock(fd,false)==0 && vfs_fd_get_flags(alias)==(O_RDWR|O_APPEND));
    assert(fd_snapshot_into(&child)==0);fd_close_exec(&child);
    assert(!child.fds[fd].used && child.fds[alias].used);
    assert(fd_dup_into(&child,alias,48)==0);reaper(&child);
    assert(alive[conn] && !closed[conn] && !aborted[conn]);
    assert(vfs_close(fd)==0 && alive[conn]);assert(vfs_close(alias)==0 && closed[conn]==1 && fd_shared_live_count()==0);
    fd=fd_alloc_socket(&owner);assert(fd_socket_connect(&owner,fd,0,1)==0);conn=owner.fds[fd].shared->conn;
    assert(fd_snapshot_into(&child)==0 && vfs_close(fd)==0 && alive[conn]);reaper(&child);
    assert(aborted[conn]==1 && !closed[conn] && fd_shared_live_count()==0);
    for(int round=0;round<128;round++){
        fd=fd_alloc_socket(&owner);assert(fd_socket_connect(&owner,fd,0,1)==0);conn=owner.fds[fd].shared->conn;
        started=release_read=0;reader_fd=fd;pthread_t worker;assert(!pthread_create(&worker,NULL,reader,NULL));
        pthread_mutex_lock(&gate);while(!started)pthread_cond_wait(&condition,&gate);pthread_mutex_unlock(&gate);
        assert(vfs_close(fd)==0 && alive[conn] && !closed[conn]);
        int reuse=fd_alloc_socket(&owner);assert(reuse==fd && fd_socket_connect(&owner,reuse,0,2)==0);
        int replacement=owner.fds[reuse].shared->conn;assert(replacement!=conn);
        pthread_mutex_lock(&gate);release_read=1;pthread_cond_broadcast(&condition);pthread_mutex_unlock(&gate);
        assert(!pthread_join(worker,NULL));assert(!alive[conn] && closed[conn]==1 && alive[replacement]);
        assert(vfs_close(reuse)==0 && closed[replacement]==1 && fd_shared_live_count()==0);
    }
    puts("PASS: production descriptor allocation rollback, shared flags/CLOEXEC, spawn/reap ownership and 128 barrier-controlled read/close/reuse races");
}
