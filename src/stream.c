#ifndef WIN32
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#define __USE_MISC
#ifndef CRTSCTS
#define CRTSCTS  020000000000
#endif
#include <errno.h>
#include <termios.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif
#include <time.h>
#include <math.h>
#include <string.h>
#include "stream.h"

/* constants -----------------------------------------------------------------*/

#define TINTACT             200         /* period for stream active (ms) */
#define SERIBUFFSIZE        4096        /* serial buffer size (bytes) */
#define TIMETAGH_LEN        64          /* time tag file header length */
#define MAXCLI              32          /* max client connection for tcp svr */
#define MAXSTATMSG          32          /* max length of status message */
#define DEFAULT_MEMBUF_SIZE 4096        /* default memory buffer size (bytes) */

#define NTRIP_CLI_PORT      2101        /* default ntrip-client connection port */
#define NTRIP_SVR_PORT      80          /* default ntrip-server connection port */
#define NTRIP_MAXRSP        32768       /* max size of ntrip response */
#define NTRIP_MAXSTR        256         /* max length of mountpoint string */
#define NTRIP_RSP_OK_CLI    "ICY 200 OK\r\n" /* ntrip response: client */
#define NTRIP_RSP_OK_SVR    "OK\r\n"    /* ntrip response: server */
#define NTRIP_RSP_SRCTBL    "SOURCETABLE 200 OK\r\n" /* ntrip response: source table */
#define NTRIP_RSP_TBLEND    "ENDSOURCETABLE"
#define NTRIP_RSP_HTTP      "HTTP/"     /* ntrip response: http */
#define NTRIP_RSP_ERROR     "ERROR"     /* ntrip response: error */
#define NTRIP_RSP_UNAUTH    "HTTP/1.0 401 Unauthorized\r\n"
#define NTRIP_RSP_ERR_PWD   "ERROR - Bad Pasword\r\n"
#define NTRIP_RSP_ERR_MNTP  "ERROR - Bad Mountpoint\r\n"

/* global options ------------------------------------------------------------*/

static int toinact  =10000; /* inactive timeout (ms) */
static int ticonnect=10000; /* interval to re-connect (ms) */
static int tirate   =1000;  /* averaging time for data rate (ms) */
static int buffsize =32768; /* receive/send buffer size (bytes) */
static char proxyaddr[256]=""; /* http/ntrip/ftp proxy address */
static int fswapmargin=30;  /* file swap margin (s) */

/* macros --------------------------------------------------------------------*/

#ifdef WIN32
#define dev_t               HANDLE
#define socket_t            SOCKET
typedef int socklen_t;
#else
#define dev_t               int
#define socket_t            int
#define closesocket         close
#endif

/* common function ----------------------------------------------------------*/
static void trace(int level, const char *format, ...) {
#ifndef NDEBUG
    if (level != 2) return;
    va_list ap;
    fprintf(stderr, "%d - ", level);
    va_start(ap, format); vfprintf(stderr, format, ap); va_end(ap);
#endif
}

static uint32_t tickget(void)
{
#ifdef WIN32
    return (uint32_t)timeGetTime();
#else
    struct timespec tp={0};
    struct timeval  tv={0};
    
#ifdef CLOCK_MONOTONIC_RAW
    /* linux kernel > 2.6.28 */
    if (!clock_gettime(CLOCK_MONOTONIC_RAW,&tp)) {
        return tp.tv_sec*1000u+tp.tv_nsec/1000000u;
    }
    else {
        gettimeofday(&tv,NULL);
        return tv.tv_sec*1000u+tv.tv_usec/1000u;
    }
#else
    gettimeofday(&tv,NULL);
    return tv.tv_sec*1000u+tv.tv_usec/1000u;
#endif
#endif /* WIN32 */
}

/* TCP ----------------------------------------------------------------------*/
typedef struct {            /* tcp control type */
    int state;              /* state (0:close,1:wait,2:connect) */
    char saddr[256];        /* address string */
    int port;               /* port */
    struct sockaddr_in addr; /* address resolved */
    socket_t sock;          /* socket descriptor */
    int tcon;               /* reconnect time (ms) (-1:never,0:now) */
    uint32_t tact;          /* data active tick */
    uint32_t tdis;          /* disconnect tick */
} tcp_t;

typedef struct tcpsvr_tag { /* tcp server type */
    tcp_t svr;              /* tcp server control */
    tcp_t cli[MAXCLI];      /* tcp client controls */
} tcpsvr_t;

typedef struct {            /* tcp cilent type */
    tcp_t svr;              /* tcp server control */
    int toinact;            /* inactive timeout (ms) (0:no timeout) */
    int tirecon;            /* reconnect interval (ms) (0:no reconnect) */
} tcpcli_t;

/* decode tcp/ntrip path (path=[user[:passwd]@]addr[:port][/mntpnt[:str]]) ---*/
static void decodetcppath(const char *path, char *addr, char *port, char *user,
                          char *passwd, char *mntpnt, char *str)
{
    char buff[MAXSTRPATH],*p,*q;
    
    if (port) *port='\0';
    if (user) *user='\0';
    if (passwd) *passwd='\0';
    if (mntpnt) *mntpnt='\0';
    if (str) *str='\0';
    
    strcpy(buff,path);
    
    if ((p=strrchr(buff,'@'))==NULL) p=buff;
    
    if ((p=strchr(p,'/'))!=NULL) {
        if ((q=strchr(p+1,':'))!=NULL) {
            *q='\0'; if (str) sprintf(str,"%.*s",NTRIP_MAXSTR-1,q+1);
        }
        *p='\0'; if (mntpnt) sprintf(mntpnt,"%.255s",p+1);
    }
    if ((p=strrchr(buff,'@'))!=NULL) {
        *p++='\0';
        if ((q=strchr(buff,':'))!=NULL) {
            *q='\0'; if (passwd) sprintf(passwd,"%.255s",q+1);
        }
        if (user) sprintf(user,"%.255s",buff);
    }
    else p=buff;
    
    if ((q=strchr(p,':'))!=NULL) {
        *q='\0'; if (port) sprintf(port,"%.255s",q+1);
    }
    if (addr) sprintf(addr,"%.255s",p);
}
/* get socket error ----------------------------------------------------------*/
#ifdef WIN32
static int errsock(void) {return WSAGetLastError();}
#else
static int errsock(void) {return errno;}
#endif

/* set socket option ---------------------------------------------------------*/
static int setsock(socket_t sock, char *msg)
{
    int bs=buffsize,mode=1;
#ifdef WIN32
    int tv=0;
#else
    struct timeval tv={0};
#endif
    
    if (setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,(const char *)&tv,sizeof(tv))==-1||
        setsockopt(sock,SOL_SOCKET,SO_SNDTIMEO,(const char *)&tv,sizeof(tv))==-1) {
        sprintf(msg,"sockopt error: notimeo");
        trace(1,"setsock: setsockopt error 1 sock=%d err=%d\n",sock,errsock());
        closesocket(sock);
        return 0;
    }
    if (setsockopt(sock,SOL_SOCKET,SO_RCVBUF,(const char *)&bs,sizeof(bs))==-1||
        setsockopt(sock,SOL_SOCKET,SO_SNDBUF,(const char *)&bs,sizeof(bs))==-1) {
        trace(1,"setsock: setsockopt error 2 sock=%d err=%d bs=%d\n",sock,errsock(),bs);
        sprintf(msg,"sockopt error: bufsiz");
    }
    if (setsockopt(sock,IPPROTO_TCP,TCP_NODELAY,(const char *)&mode,sizeof(mode))==-1) {
        trace(1,"setsock: setsockopt error 3 sock=%d err=%d\n",sock,errsock());
        sprintf(msg,"sockopt error: nodelay");
    }
    return 1;
}
/* non-block accept ----------------------------------------------------------*/
static socket_t accept_nb(socket_t sock, struct sockaddr *addr, socklen_t *len)
{
    struct timeval tv={0};
    fd_set rs;
    int ret;
    
    FD_ZERO(&rs); FD_SET(sock,&rs);
    ret=select(sock+1,&rs,NULL,NULL,&tv);
    if (ret<=0) return (socket_t)ret;
    return accept(sock,addr,len);
}
/* non-block connect ---------------------------------------------------------*/
static int connect_nb(socket_t sock, struct sockaddr *addr, socklen_t len)
{
#ifdef WIN32
    u_long mode=1; 
    int err;
    
    ioctlsocket(sock,FIONBIO,&mode);
    if (connect(sock,addr,len)==-1) {
        err=errsock();
        if (err==WSAEWOULDBLOCK||err==WSAEINPROGRESS||
            err==WSAEALREADY   ||err==WSAEINVAL) return 0;
        if (err!=WSAEISCONN) return -1;
    }
#else
    struct timeval tv={0};
    fd_set rs,ws;
    int err,flag;
    
    flag=fcntl(sock,F_GETFL,0);
    fcntl(sock,F_SETFL,flag|O_NONBLOCK);
    if (connect(sock,addr,len)==-1) {
        err=errsock();
        if (err!=EISCONN&&err!=EINPROGRESS&&err!=EALREADY) return -1;
        FD_ZERO(&rs); FD_SET(sock,&rs); ws=rs;
        if (select(sock+1,&rs,&ws,NULL,&tv)==0) return 0;
    }
#endif
    return 1;
}
/* non-block receive ---------------------------------------------------------*/
static int recv_nb(socket_t sock, unsigned char *buff, int n)
{
    struct timeval tv={0};
    fd_set rs;
    int ret,nr;
    
    FD_ZERO(&rs); FD_SET(sock,&rs);
    ret=select(sock+1,&rs,NULL,NULL,&tv);
    if (ret<=0) return ret;
    nr=recv(sock,(char *)buff,n,0);
    return nr<=0?-1:nr;
}
/* non-block send ------------------------------------------------------------*/
static int send_nb(socket_t sock, unsigned char *buff, int n)
{
    struct timeval tv={0};
    fd_set ws;
    int ret,ns;
    
    FD_ZERO(&ws); FD_SET(sock,&ws);
    ret=select(sock+1,NULL,&ws,NULL,&tv);
    if (ret<=0) return ret;
    ns=send(sock,(char *)buff,n,0);
    return ns<n?-1:ns;
}
/* generate tcp socket -------------------------------------------------------*/
static int gentcp(tcp_t *tcp, int type, char *msg)
{    
    /* generate socket */
    if ((tcp->sock=socket(AF_INET,SOCK_STREAM,0))==(socket_t)-1) {
        sprintf(msg,"socket error (%d)",errsock());
        tcp->state=-1;
        return 0;
    }
    if (!setsock(tcp->sock,msg)) {
        tcp->state=-1;
        return 0;
    }
    memset(&tcp->addr,0,sizeof(tcp->addr));
    tcp->addr.sin_family=AF_INET;
    tcp->addr.sin_port=htons((uint16_t)tcp->port);
    
    if (type==0) { /* server socket */
        if (bind(tcp->sock,(struct sockaddr *)&tcp->addr,sizeof(tcp->addr))==-1) {
            sprintf(msg,"bind error (%d) : %d",errsock(),tcp->port);
            closesocket(tcp->sock);
            tcp->state=-1;
            return 0;
        }
        listen(tcp->sock,5);
    }
    else { /* client socket */
        int ret=1;
        struct addrinfo hints = {0}, *addrs;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        char port[256] = "";
        sprintf(port, "%d", tcp->port);
        if ((ret=getaddrinfo(tcp->saddr,port,&hints,&addrs))!=0) {
            sprintf(msg,"address error (%s)",tcp->saddr);
            freeaddrinfo(addrs);
            closesocket(tcp->sock);
            tcp->state=0;
            tcp->tcon=ticonnect;
            tcp->tdis=tickget();
            return 0;
        }
        memcpy(&tcp->addr, addrs->ai_addr, sizeof(tcp->addr));
        freeaddrinfo(addrs);
    }
    tcp->state=1;
    tcp->tact=tickget();
    return 1;
}
/* disconnect tcp ------------------------------------------------------------*/
static void discontcp(tcp_t *tcp, int tcon)
{
    closesocket(tcp->sock);
    tcp->state=0;
    tcp->tcon=tcon;
    tcp->tdis=tickget();
}

static tcpsvr_t *opentcpsvr(const char *path, char *msg)
{
    tcpsvr_t *tcpsvr, tcpsvr0 = {{0}, {{0}}};
    char port[256]="";

    if ((tcpsvr=(tcpsvr_t *)malloc(sizeof(tcpsvr_t)))==NULL) return NULL;
    *tcpsvr=tcpsvr0;
    decodetcppath(path,tcpsvr->svr.saddr,port,NULL,NULL,NULL,NULL);
    if (sscanf(port,"%d",&tcpsvr->svr.port)<1) {
        sprintf(msg,"port error: %s",port);
        free(tcpsvr);
        return NULL;
    }
    if (!gentcp(&tcpsvr->svr,0,msg)) {
        free(tcpsvr);
        return NULL;
    }
    tcpsvr->svr.tcon=0;
    return tcpsvr;
}

static void closetcpsvr(tcpsvr_t *tcpsvr)
{
    int i;
    for (i=0;i<MAXCLI;i++) {
        if (tcpsvr->cli[i].state) closesocket(tcpsvr->cli[i].sock);
    }
    closesocket(tcpsvr->svr.sock);
    free(tcpsvr);
}

/* update tcp server ---------------------------------------------------------*/
static void updatetcpsvr(tcpsvr_t *tcpsvr, char *msg)
{
    char saddr[256]="";
    int i,n=0;
    
    if (tcpsvr->svr.state==0) return;
    
    for (i=0;i<MAXCLI;i++) {
        if (!tcpsvr->cli[i].state) continue;
        strcpy(saddr,tcpsvr->cli[i].saddr);
        n++;
    }
    if (n==0) {
        tcpsvr->svr.state=1;
        sprintf(msg,"waiting...");
        return;
    }
    tcpsvr->svr.state=2;
    if (n==1) sprintf(msg,"%s",saddr); else sprintf(msg,"%d clients",n);
}
/* accept client connection --------------------------------------------------*/
static int accsock(tcpsvr_t *tcpsvr, char *msg)
{
    struct sockaddr_in addr;
    socket_t sock;
    socklen_t len=sizeof(addr);
    int i,err;
    
    trace(4,"accsock: sock=%d\n",tcpsvr->svr.sock);
    
    for (i=0;i<MAXCLI;i++) {
        if (tcpsvr->cli[i].state==0) break;
    }
    if (i>=MAXCLI) {
        trace(2,"accsock: too many clients sock=%d\n",tcpsvr->svr.sock);
        return 0;
    }
    if ((sock=accept_nb(tcpsvr->svr.sock,(struct sockaddr *)&addr,&len))==(socket_t)-1) {
        err=errsock();
        sprintf(msg,"accept error (%d)",err);
        trace(2,"accsock: accept error sock=%d err=%d\n",tcpsvr->svr.sock,err);
        closesocket(tcpsvr->svr.sock);
        tcpsvr->svr.state=0;
        return 0;
    }
    if (sock==0) return 0;
    if (!setsock(sock,msg)) return 0;
    
    tcpsvr->cli[i].sock=sock;
    memcpy(&tcpsvr->cli[i].addr,&addr,sizeof(addr));
    strcpy(tcpsvr->cli[i].saddr,inet_ntoa(addr.sin_addr));
    sprintf(msg,"%s",tcpsvr->cli[i].saddr);
    tcpsvr->cli[i].state=2;
    tcpsvr->cli[i].tact=tickget();
    return 1;
}
/* wait socket accept --------------------------------------------------------*/
static int waittcpsvr(tcpsvr_t *tcpsvr, char *msg)
{
    if (tcpsvr->svr.state<=0) return 0;
    
    while (accsock(tcpsvr,msg)) ;
    
    updatetcpsvr(tcpsvr,msg);
    return tcpsvr->svr.state==2;
}
/* read tcp server -----------------------------------------------------------*/
static int readtcpsvr(tcpsvr_t *tcpsvr, unsigned char *buff, int n, char *msg)
{
    int i,nr,err;

    trace(4,"readtcpsvr: n=%d\n",n);

    if (!waittcpsvr(tcpsvr,msg)) return 0;
    
    for (i=0;i<MAXCLI;i++) {
        if (tcpsvr->cli[i].state!=2) continue;
        
        if ((nr=recv_nb(tcpsvr->cli[i].sock,buff,n))==-1) {
            if ((err=errsock())!=0) {
                trace(2,"readtcpsvr: recv error sock=%d err=%d\n",
                        tcpsvr->cli[i].sock,err);
            }
            discontcp(&tcpsvr->cli[i],ticonnect);
            updatetcpsvr(tcpsvr,msg);
        }
        if (nr>0) {
            tcpsvr->cli[i].tact=tickget();
            return nr;
        }
    }
    return 0;
}
/* write tcp server -----------------------------------------------------------*/
static int writetcpsvr(tcpsvr_t *tcpsvr, unsigned char *buff, int n, char *msg)
{
    int i,ns=0,nmax=0,err;
    
    trace(4,"writetcpsvr: n=%d\n",n);
    
    if (!waittcpsvr(tcpsvr,msg)) return 0;
    
    for (i=0;i<MAXCLI;i++) {
        if (tcpsvr->cli[i].state!=2) continue;
        
        if ((ns=send_nb(tcpsvr->cli[i].sock,buff,n))==-1) {
            if ((err=errsock())!=0) {
                trace(2,"writetcpsvr: send error sock=%d err=%d\n",
                        tcpsvr->cli[i].sock,err);
            }
            discontcp(&tcpsvr->cli[i],ticonnect);
            updatetcpsvr(tcpsvr,msg);
        } else {
            if (ns>nmax) nmax=ns;
            if (ns>0) tcpsvr->cli[i].tact=tickget();
        }
    }
    return nmax;
}
/* get state tcp server -------------------------------------------------------*/
static int statetcpsvr(tcpsvr_t *tcpsvr) { return tcpsvr->svr.state; }
static int statextcp(tcp_t *tcp, char *msg)
{
    char *p=msg;

    p+=sprintf(p,"    state = %d\n",tcp->state);
    p+=sprintf(p,"    saddr = %s\n",tcp->saddr);
    p+=sprintf(p,"    port  = %d\n",tcp->port);
    p+=sprintf(p,"    sock  = %d\n",(int)tcp->sock);
    return (int)(p-msg);
}
/* get extended state tcp server ---------------------------------------------*/
static int statextcpsvr(tcpsvr_t *tcpsvr, char *msg)
{
    char *p=msg;
    int i,state=tcpsvr->svr.state;
    
    p+=sprintf(p,"tcpsvr:\n");
    p+=sprintf(p,"  state   = %d\n",state);
    if (!state) return 0;
    p+=sprintf(p,"  svr:\n");
    p+=statextcp(&tcpsvr->svr,p);
    for (i=0;i<MAXCLI;i++) {
        if (!tcpsvr->cli[i].state) continue;
        p+=sprintf(p,"  cli#%d:\n",i);
        p+=statextcp(tcpsvr->cli+i,p);
    }
    return state;
}
/* connect server ------------------------------------------------------------*/
static int consock(tcpcli_t *tcpcli, char *msg)
{
    int stat,err;
    
    trace(4,"consock: sock=%d\n",tcpcli->svr.sock);
    
    /* wait re-connect */
    if (tcpcli->svr.tcon<0||(tcpcli->svr.tcon>0&&
        (int)(tickget()-tcpcli->svr.tdis)<tcpcli->svr.tcon)) {
        return 0;
    }
    /* non-block connect */
    if ((stat=connect_nb(tcpcli->svr.sock,(struct sockaddr *)&tcpcli->svr.addr,
                            sizeof(tcpcli->svr.addr)))==-1) {
        err=errsock();
        sprintf(msg,"connect error (%d)",err);
        trace(2,"consock: connect error sock=%d err=%d\n",tcpcli->svr.sock,err);
        closesocket(tcpcli->svr.sock);
        tcpcli->svr.state=0;
        return 0;
    }
    if (!stat) { /* not connect */
        sprintf(msg,"connecting...");
        return 0;
    }
    sprintf(msg,"%s",tcpcli->svr.saddr);
    trace(3,"consock: connected sock=%d addr=%s\n",tcpcli->svr.sock,tcpcli->svr.saddr);
    tcpcli->svr.state=2;
    tcpcli->svr.tact=tickget();
    return 1;
}
/* open tcp client -----------------------------------------------------------*/
static tcpcli_t *opentcpcli(const char *path, char *msg)
{
    tcpcli_t *tcpcli,tcpcli0={{0},0,0};
    char port[256]="";
    
    trace(3,"opentcpcli: path=%s\n",path);
    
    if ((tcpcli=(tcpcli_t *)malloc(sizeof(tcpcli_t)))==NULL) return NULL;
    *tcpcli=tcpcli0;
    decodetcppath(path,tcpcli->svr.saddr,port,NULL,NULL,NULL,NULL);
    if (sscanf(port,"%d",&tcpcli->svr.port)<1) {
        sprintf(msg,"port error: %s",port);
        trace(2,"opentcp: port error port=%s\n",port);
        free(tcpcli);
        return NULL;
    }
    tcpcli->svr.tcon=0;
    tcpcli->toinact=toinact;
    tcpcli->tirecon=ticonnect;
    return tcpcli;
}
/* close tcp client ----------------------------------------------------------*/
static void closetcpcli(tcpcli_t *tcpcli)
{
    trace(3,"closetcpcli: sock=%d\n",tcpcli->svr.sock);
    
    closesocket(tcpcli->svr.sock);
    free(tcpcli);
}
static int waittcpcli(tcpcli_t *tcpcli, char *msg)
{
    trace(4,"waittcpcli: sock=%d state=%d\n",tcpcli->svr.sock,tcpcli->svr.state);
    
    if (tcpcli->svr.state<0) return 0;
    
    if (tcpcli->svr.state==0) { /* close */
        if (!gentcp(&tcpcli->svr,1,msg)) return 0;
    }
    if (tcpcli->svr.state==1) { /* wait */
        if (!consock(tcpcli,msg)) return 0;
    }
    if (tcpcli->svr.state==2) { /* connect */
        if (tcpcli->toinact>0&&
            (int)(tickget()-tcpcli->svr.tact)>tcpcli->toinact) { /* timeout */
            sprintf(msg,"timeout");
            trace(2,"waittcpcli: inactive timeout sock=%d\n",tcpcli->svr.sock);
            discontcp(&tcpcli->svr,tcpcli->tirecon);
            return 0;
        }
    }
    return 1;
}
/* read tcp client -----------------------------------------------------------*/
static int readtcpcli(tcpcli_t *tcpcli, unsigned char *buff, int n, char *msg)
{
    int nr,err;
    
    trace(4,"readtcpcli: sock=%d\n",tcpcli->svr.sock);
    
    if (!waittcpcli(tcpcli,msg)) return 0;
    
    if ((nr=recv_nb(tcpcli->svr.sock,buff,n))==-1) {
        if ((err=errsock())!=0) {
            trace(2,"readtcpcli: recv error sock=%d err=%d\n",tcpcli->svr.sock,err);
            sprintf(msg,"recv error (%d)",err);
        }
        else {
            sprintf(msg,"disconnected");
        }
        discontcp(&tcpcli->svr,tcpcli->tirecon);
        return 0;
    }
    if (nr>0) tcpcli->svr.tact=tickget();
    trace(5,"readtcpcli: exit sock=%d nr=%d\n",tcpcli->svr.sock,nr);
    return nr;
}
/* write tcp client ----------------------------------------------------------*/
static int writetcpcli(tcpcli_t *tcpcli, unsigned char *buff, int n, char *msg)
{
    int ns,err;
    
    trace(3,"writetcpcli: sock=%d state=%d n=%d\n",tcpcli->svr.sock,tcpcli->svr.state,n);
    
    if (!waittcpcli(tcpcli,msg)) return 0;
    
    if ((ns=send_nb(tcpcli->svr.sock,buff,n))==-1) {
        if ((err=errsock())!=0) {
            trace(2,"writetcp: send error sock=%d err=%d\n",tcpcli->svr.sock,err);
            sprintf(msg,"send error (%d)",err);
        }
        discontcp(&tcpcli->svr,tcpcli->tirecon);
        return 0;
    }
    if (ns>0) tcpcli->svr.tact=tickget();
    trace(5,"writetcpcli: exit sock=%d ns=%d\n",tcpcli->svr.sock,ns);
    return ns;
}
/* get state tcp client ------------------------------------------------------*/
static int statetcpcli(tcpcli_t *tcpcli) { return tcpcli->svr.state; }
/* get extended state tcp client ---------------------------------------------*/
static int statextcpcli(tcpcli_t *tcpcli, char *msg)
{   
    int state=tcpcli->svr.state;
    char *p=msg;

    p+=sprintf(p,"tcpcli:\n");
    p+=sprintf(p,"  state   = %d\n",state);
    p+=sprintf(p,"  toinact = %d\n",tcpcli->toinact);
    p+=sprintf(p,"  tirecon = %d\n",tcpcli->tirecon);
    if (!state) return 0;
    p+=sprintf(p,"  svr:\n");
    p+=statextcp(&tcpcli->svr,p);
    return state;
}
/* NTRIP --------------------------------------------------------------------*/
typedef struct {
    int state;              /* state (0:close,1:wait,2:connect) */
    int type;               /* type (0:server,1:client) */
    int nb;                 /* response buffer size */
    char url[MAXSTRPATH];   /* url for proxy */
    char mntpnt[256];       /* mountpoint */
    char user[256];         /* user */
    char passwd[256];       /* password */
    char str[NTRIP_MAXSTR]; /* mountpoint string for server */
    unsigned char buff[NTRIP_MAXRSP]; /* response buffer */
    tcpcli_t *tcp;          /* tcp client */
}ntrip_t;
/* base64 encoder ------------------------------------------------------------*/
static int encbase64(char *str, const unsigned char *byte, int n)
{
    const char table[]=
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int i,j,k,b;
    
    for (i=j=0;i/8<n;) {
        for (k=b=0;k<6;k++,i++) {
            b<<=1; if (i/8<n) b|=(byte[i/8]>>(7-i%8))&0x1;
        }
        str[j++]=table[b];
    }
    while (j&0x3) str[j++]='=';
    str[j]='\0';
    return j;
}
/* send ntrip server request -------------------------------------------------*/
static int reqntrip_s(ntrip_t *ntrip, char *msg)
{
    char buff[1024+NTRIP_MAXSTR],*p=buff;
    trace(3,"reqntrip_s: state=%d\n",ntrip->state);
    
    p+=sprintf(p,"SOURCE %s %s\r\n",ntrip->passwd,ntrip->mntpnt);
    p+=sprintf(p,"Source-Agent: NTRIP %s\r\n",NTRIP_AGENT);
    p+=sprintf(p,"STR: %s\r\n",ntrip->str);
    p+=sprintf(p,"\r\n");
    
    int n=(int)(p-buff);
    if (writetcpcli(ntrip->tcp,(unsigned char *)buff,n,msg)!=n) return 0;
    
    trace(3,"reqntrip_s: send request state=%d ns=%d\n",ntrip->state,n);
    ntrip->state=1;
    return 1;
}
/* send ntrip client request -------------------------------------------------*/
static int reqntrip_c(ntrip_t *ntrip, char *msg)
{
    char buff[MAXSTRPATH+1024],user[514],*p=buff;
    
    trace(3,"reqntrip_c: state=%d\n",ntrip->state);

    p+=sprintf(p,"GET %s/%s HTTP/1.0\r\n",ntrip->url,ntrip->mntpnt);
    p+=sprintf(p,"User-Agent: NTRIP %s\r\n",NTRIP_AGENT);

    if (!*ntrip->user) {
        p+=sprintf(p,"Accept: */*\r\n");
        p+=sprintf(p,"Connection: close\r\n");
    }
    else {
        sprintf(user,"%s:%s",ntrip->user,ntrip->passwd);
        p+=sprintf(p,"Authorization: Basic ");
        p+=encbase64(p,(unsigned char *)user,(int)strlen(user));
        p+=sprintf(p,"\r\n");
    }
    p+=sprintf(p,"\r\n");

    int n=(int)(p-buff);
    if (writetcpcli(ntrip->tcp,(unsigned char *)buff,n,msg)!=n) return 0;

    ntrip->state=1;
    return 1;
}
/* test ntrip server response ------------------------------------------------*/
static int rspntrip_s(ntrip_t *ntrip, char *msg)
{
    int i,nb;
    char *p,*q;
    
    trace(3,"rspntrip_s: state=%d nb=%d\n",ntrip->state,ntrip->nb);
    ntrip->buff[ntrip->nb]='\0'; // null terminate the buffer?

    if ((p=strstr((char *)ntrip->buff,NTRIP_RSP_OK_SVR))!=NULL) { /* ok */
        q=(char *)ntrip->buff;
        p+=strlen(NTRIP_RSP_OK_SVR);
        ntrip->nb-=(int)(p-q);
        for (i=0;i<ntrip->nb;i++) *q++=*p++;
        ntrip->state=2;
        sprintf(msg,"%s/%s",ntrip->tcp->svr.saddr,ntrip->mntpnt);
        trace(3,"rspntrip_s: response ok nb=%d\n",ntrip->nb);
        return 1;
    }
    else if ((p=strstr((char *)ntrip->buff,NTRIP_RSP_ERROR))!=NULL) { /* error */
        nb=ntrip->nb<MAXSTATMSG?ntrip->nb:MAXSTATMSG;
        sprintf(msg,"%.*s",nb,(char *)ntrip->buff);
        if ((p=strchr(msg,'\r'))!=NULL) *p='\0';
        trace(3,"rspntrip_s: %s nb=%d\n",msg,ntrip->nb);
        ntrip->nb=0;
        ntrip->buff[0]='\0';
        ntrip->state=0;
        discontcp(&ntrip->tcp->svr,ntrip->tcp->tirecon);
    }
    else if (ntrip->nb>=NTRIP_MAXRSP) { /* buffer overflow */
        sprintf(msg,"response overflow");
        trace(3,"rspntrip_s: response overflow nb=%d\n",ntrip->nb);
        ntrip->nb=0;
        ntrip->buff[0]='\0';
        ntrip->state=0;
        discontcp(&ntrip->tcp->svr,ntrip->tcp->tirecon);
    }
    return 0;
}
/* test ntrip client response ------------------------------------------------*/
static int rspntrip_c(ntrip_t *ntrip, char *msg)
{
    int i;
    char *p,*q;

    trace(3,"rspntrip_c: state=%d nb=%d\n",ntrip->state,ntrip->nb);
    ntrip->buff[ntrip->nb]='\0'; // null terminate the buffer?

    if ((p=strstr((char *)ntrip->buff,NTRIP_RSP_OK_CLI))!=NULL) { /* ok */
        q=(char *)ntrip->buff;
        p+=strlen(NTRIP_RSP_OK_CLI);
        ntrip->nb-=(int)(p-q);
        for (i=0;i<ntrip->nb;i++) *q++=*p++;
        ntrip->state=2;
        sprintf(msg,"%s/%s",ntrip->tcp->svr.saddr,ntrip->mntpnt);
        trace(3,"rspntrip_c: response ok nb=%d\n",ntrip->nb);
		ntrip->tcp->tirecon=ticonnect;
        return 1;
    }
    if ((p=strstr((char *)ntrip->buff,NTRIP_RSP_SRCTBL))!=NULL) { /* source table */
        if (!*ntrip->mntpnt) { /* source table request */
            ntrip->state=2;
            sprintf(msg,"source table received");
            trace(3,"rspntrip_c: receive source table nb=%d\n",ntrip->nb);
            return 1;
        }
        sprintf(msg,"no mountp. reconnect...");
        trace(2,"rspntrip_c: no mount point nb=%d\n",ntrip->nb);
        ntrip->nb=0;
        ntrip->buff[0]='\0';
        ntrip->state=0;
		/* increase subsequent disconnect time to avoid too many reconnect requests */
        if (ntrip->tcp->tirecon>300000) ntrip->tcp->tirecon=ntrip->tcp->tirecon*5/4;

        discontcp(&ntrip->tcp->svr,ntrip->tcp->tirecon);
    }
    else if ((p=strstr((char *)ntrip->buff,NTRIP_RSP_HTTP))!=NULL) { /* http response */
        if ((q=strchr(p,'\r'))!=NULL) *q='\0'; else ntrip->buff[128]='\0';
        strcpy(msg,p);
        trace(3,"rspntrip_s: %s nb=%d\n",msg,ntrip->nb);
        ntrip->nb=0;
        ntrip->buff[0]='\0';
        ntrip->state=0;
        discontcp(&ntrip->tcp->svr,ntrip->tcp->tirecon);
    }
    else if (ntrip->nb>=NTRIP_MAXRSP) { /* buffer overflow */
        sprintf(msg,"response overflow");
        trace(2,"rspntrip_s: response overflow nb=%d\n",ntrip->nb);
        ntrip->nb=0;
        ntrip->buff[0]='\0';
        ntrip->state=0;
        discontcp(&ntrip->tcp->svr,ntrip->tcp->tirecon);
    }
    return 0;
}
/* wait ntrip request/response -----------------------------------------------*/
static int waitntrip(ntrip_t *ntrip, char *msg)
{
    int n;
    char *p;
    
    trace(4,"waitntrip: state=%d nb=%d\n",ntrip->state,ntrip->nb);
    
    if (ntrip->state<0) return 0; /* error */
    
    if (ntrip->tcp->svr.state<2) ntrip->state=0; /* tcp disconnected */
    
    if (ntrip->state==0) { /* send request */
        if (!(ntrip->type==0?reqntrip_s(ntrip,msg):reqntrip_c(ntrip,msg))) {
            return 0;
        }
        trace(3,"waitntrip: state=%d nb=%d\n",ntrip->state,ntrip->nb);
    }
    if (ntrip->state==1) { /* read response */
        p=(char *)ntrip->buff+ntrip->nb;
        if ((n=readtcpcli(ntrip->tcp,(unsigned char *)p,NTRIP_MAXRSP-ntrip->nb-1,msg))==0) {
            return 0;
        }
        ntrip->nb+=n; ntrip->buff[ntrip->nb]='\0';
        
        /* wait response */
        return ntrip->type==0?rspntrip_s(ntrip,msg):rspntrip_c(ntrip,msg);
    }
    return 1;
}
/* open ntrip ----------------------------------------------------------------*/
static ntrip_t *openntrip(const char *path, int type, char *msg)
{
    ntrip_t *ntrip;
    int i;
    char addr[256]="",port[256]="",tpath[MAXSTRPATH];
    
    trace(3,"openntrip: path=%s type=%d\n",path,type);
    
    if ((ntrip=(ntrip_t *)malloc(sizeof(ntrip_t)))==NULL) return NULL;
    
    ntrip->state=0;
    ntrip->type=type; /* 0:server,1:client */
    ntrip->nb=0;
    ntrip->url[0]='\0';
    ntrip->mntpnt[0]=ntrip->user[0]=ntrip->passwd[0]=ntrip->str[0]='\0';
    for (i=0;i<NTRIP_MAXRSP;i++) ntrip->buff[i]=0;
    
    /* decode tcp/ntrip path */
    decodetcppath(path,addr,port,ntrip->user,ntrip->passwd,ntrip->mntpnt,
                    ntrip->str);
    
    /* use default port if no port specified */
    if (!*port) {
        sprintf(port,"%d",type?NTRIP_CLI_PORT:NTRIP_SVR_PORT);
    }
    sprintf(tpath,"%s:%s",addr,port);
    
    /* ntrip access via proxy server */
    if (*proxyaddr) {
        sprintf(ntrip->url,"http://%.*s",MAXSTRPATH-8,tpath);
        sprintf(tpath,"%.*s",MAXSTRPATH-1,proxyaddr);
    }
    /* open tcp client stream */
    if ((ntrip->tcp=opentcpcli(tpath,msg))==NULL) {
        trace(2,"openntrip: opentcp error\n");
        free(ntrip);
        return NULL;
    }
    return ntrip;
}
/* close ntrip ---------------------------------------------------------------*/
static void closentrip(ntrip_t *ntrip)
{
    trace(3,"closentrip: state=%d\n",ntrip->state);

    closetcpcli(ntrip->tcp);
    free(ntrip);
}
/* read ntrip ----------------------------------------------------------------*/
static int readntrip(ntrip_t *ntrip, unsigned char *buff, int n, char *msg)
{
    int nb;
    
    trace(4,"readntrip:\n");
    
    if (!waitntrip(ntrip,msg)) return 0;
    
    if (ntrip->nb>0) { /* read response buffer first */
        nb=ntrip->nb<=n?ntrip->nb:n;
        memcpy(buff,ntrip->buff+ntrip->nb-nb,(size_t)(nb));
        ntrip->nb=0;
        return nb;
    }
    return readtcpcli(ntrip->tcp,buff,n,msg);
}
/* write ntrip ---------------------------------------------------------------*/
static int writentrip(ntrip_t *ntrip, unsigned char *buff, int n, char *msg)
{
    trace(3,"writentrip: n=%d\n",n);
    
    if (!waitntrip(ntrip,msg)) return 0;
    
    return writetcpcli(ntrip->tcp,buff,n,msg);
}
/* get state ntrip -----------------------------------------------------------*/
static int statentrip(ntrip_t *ntrip) { 
    return ntrip->state?ntrip->state:ntrip->tcp->svr.state;
}
/* get extended state ntrip --------------------------------------------------*/
static int statexntrip(ntrip_t *ntrip, char *msg)
{
    char *p=msg;
    int state=!ntrip?0:(ntrip->state==0?ntrip->tcp->svr.state:ntrip->state);
    
    p+=sprintf(p,"ntrip:\n");
    p+=sprintf(p,"  state   = %d\n",state);
    if (!state) return 0;
    p+=sprintf(p,"  state   = %d\n",state);
    p+=sprintf(p,"  type    = %d\n",ntrip->type);
    p+=sprintf(p,"  nb      = %d\n",ntrip->nb);
    p+=sprintf(p,"  url     = %s\n",ntrip->url);
    p+=sprintf(p,"  mntpnt  = %s\n",ntrip->mntpnt);
    p+=sprintf(p,"  user    = %s\n",ntrip->user);
    p+=sprintf(p,"  passwd  = %s\n",ntrip->passwd);
    p+=sprintf(p,"  str     = %s\n",ntrip->str);
    p+=sprintf(p,"  svr:\n");
    p+=statextcp(&ntrip->tcp->svr,p);
    return state;
}
/* Serial -------------------------------------------------------------------*/
typedef struct {            /* serial control type */
    dev_t dev;              /* serial device */
    int error;              /* error state */
#ifdef WIN32
    int state,wp,rp;        /* state,write/read pointer */
    int buffsize;           /* write buffer size (bytes) */
    HANDLE thread;          /* write thread */
    lock_t lock;            /* lock flag */
    unsigned char *buff;    /* write buffer */
#endif
    tcpsvr_t *tcpsvr;       /* tcp server for received stream */
} serial_t;

#ifdef WIN32
#define SERIAL_CYCLE        10          /* serial cycle (ms) */

static int readseribuff(serial_t *serial, unsigned char *buff, int nmax)
{
    int ns;
        
    lock(&serial->lock);
    for (ns=0;serial->rp!=serial->wp&&ns<nmax;ns++) {
        buff[ns]=serial->buff[serial->rp];
        if (++serial->rp>=serial->buffsize) serial->rp=0;
    }
    unlock(&serial->lock);
    return ns;
}

static int writeseribuff(serial_t *serial, unsigned char *buff, int n)
{
    int ns,wp;
        
    lock(&serial->lock);
    for (ns=0;ns<n;ns++) {
        serial->buff[wp=serial->wp]=buff[ns];
        if (++wp>=serial->buffsize) wp=0;
        if (wp!=serial->rp) serial->wp=wp;
        else {
            break;
        }
    }
    unlock(&serial->lock);
    return ns;
}

static DWORD WINAPI serialthread(void *arg)
{
    serial_t *ser=(serial_t *)arg;
    unsigned char buff[128];
    uint32_t tick;
    DWORD ns;
    int n;

    for (;;) {
        tick=tickget();
        while ((n=readseribuff(ser,buff,sizeof(buff)))>0) {
            if(!WriteFile(ser->dev,buff,(DWORD)n,&ns,NULL)) ser->error=1;
        }
        if (ser->state<=0) break;
        
        Sleep((DWORD)(SERIAL_CYCLE-(int)(tickget()-tick)));
    }
    free(ser->buff); ser->buff=NULL;
    return 0;
}

static dev_t _openserial(const char *port, stream_mode mode, char *msg, int brate,
    char parity, int bsize, int stopb, const char *fctr){

    char dev_path[128];
    dev_t dev;

    DWORD error,rw=0,siz=sizeof(COMMCONFIG);
    COMMCONFIG cc={0};
    COMMTIMEOUTS co={MAXDWORD,0,0,0,0}; /* non-block-read */
    char dcb[64]="";
    sprintf(dev_path,"\\\\.\\%s",port);
    if (mode&STR_MODE_R) rw|=GENERIC_READ;
    if (mode&STR_MODE_W) rw|=GENERIC_WRITE;

    dev=CreateFile(dev_path,rw,0,NULL,OPEN_EXISTING,0,NULL);
    if (dev==INVALID_HANDLE_VALUE) {
        sprintf(msg,"%s open error (%d)",port,(int)GetLastError());
        return NULL;
    }
    if (!GetCommConfig(dev,&cc,&siz)) {
        sprintf(msg,"%s get comm config error (%d)",port,(int)GetLastError());
        CloseHandle(dev);
        return NULL;
    }
    sprintf(dcb,"baud=%d parity=%c data=%d stop=%d",brate,parity,bsize,stopb);
    if (!BuildCommDCB(dcb,&cc.dcb)) {
        sprintf(msg,"%s buiddcb error (%d)",port,(int)GetLastError());
        CloseHandle(dev);
        return NULL;
    }
    if (!strcmp(fctr,"rts")) {
        cc.dcb.fRtsControl=RTS_CONTROL_HANDSHAKE;
    }
    SetCommConfig(dev,&cc,siz); /* ignore error to support novatel */
    SetCommTimeouts(dev,&co);
    ClearCommError(dev,&error,NULL);
    PurgeComm(dev,PURGE_TXABORT|PURGE_RXABORT|PURGE_TXCLEAR|PURGE_RXCLEAR);
    return dev;
}

#else /* LINUX/UNIX */
static dev_t _openserial(const char *port, stream_mode mode, char *msg, int brate,
    char parity, int bsize, int stopb, const char *fctr){

    char dev_path[128];
    dev_t dev;
    const int br[] = { /* baudrate table {code,rate} */
        300,600,1200,2400,4800,9600,19200,38400,57600,115200,230400,460800,
        921600,3000000
    };

#ifdef __APPLE__
    /* MacOS doesn't support higher baudrates (>230400B) */
    const speed_t bs[]={
        B300,B600,B1200,B2400,B4800,B9600,B19200,B38400,B57600,B115200,B230400
    };
#else /* regular Linux with higher baudrates */
    const speed_t bs[]={
        B300,B600,B1200,B2400,B4800,B9600,B19200,B38400,B57600,B115200,B230400,
        B460800,B921600,B3000000
    };
#endif /* ifdef __APPLE__ */
    struct termios ios={0};
    int rw=0,i;
    for(i=0;i<sizeof(br)/sizeof(*br);i++) if (br[i]==brate) break;
    if (i>=sizeof(br)/sizeof(*br)) {
        sprintf(msg,"baud rate error (%d)",brate);
        return 0;
    }

    sprintf(dev_path,"/dev/%.*s",(int)sizeof(port)-6,port);
    if ((mode&STR_MODE_R)&&(mode&STR_MODE_W)) rw=O_RDWR;
    else if (mode&STR_MODE_R) rw=O_RDONLY;
    else if (mode&STR_MODE_W) rw=O_WRONLY;
    
    if ((dev=open(dev_path,rw|O_NOCTTY|O_NONBLOCK))<0) {
        sprintf(msg,"%s open error (%d)",dev_path,errno);
        return 0;
    }
    tcgetattr(dev,&ios);
    ios.c_iflag=0;
    ios.c_oflag=0;
    ios.c_lflag=0;     /* non-canonical */
    ios.c_cc[VMIN ]=0; /* non-block-mode */
    ios.c_cc[VTIME]=0;
    cfsetospeed(&ios,bs[i]);
    cfsetispeed(&ios,bs[i]);
    ios.c_cflag|=bsize==7?CS7:CS8;
    ios.c_cflag|=parity=='O'?(PARENB|PARODD):(parity=='E'?PARENB:0);
    ios.c_cflag|=stopb==2?CSTOPB:0;
    ios.c_cflag|=!strcmp(fctr,"rts")?CRTSCTS:0;
    tcsetattr(dev,TCSANOW,&ios);
    tcflush(dev,TCIOFLUSH);
    sprintf(msg,"%s",dev);
    return dev;
}
#endif
static serial_t* openserial(const char *path, stream_mode mode, char *msg)
{
    serial_t *serial;
    char *p,buff[1024],port[128],fctr[64]="",parity='N',path_tcp[32],msg_tcp[128];
    int brate=115200,bsize=8,stopb=0,tcp_port=0;
    
    if((serial=(serial_t *)malloc(sizeof(serial_t)))==NULL) return NULL;

    /* decode serial path */
    if((p=strchr(path,':'))!=NULL) {
        size_t len=(size_t)(p-path);
        strncpy(port,path,len); port[len]='\0'; // get port
        sscanf(p,":%d:%d:%c:%d:%s",&brate,&bsize,&parity,&stopb,fctr);
        parity=(char)toupper((int)parity);
    }
    else strcpy(buff,path);

    /* open serial */
    if ((serial->dev=_openserial(
            port,mode,msg,brate,parity,bsize,stopb,fctr))==NULL) {
        trace(2, "openserial: serial open error path=%s\n",path);
        free(serial);
        return NULL;
    }

#ifdef WIN32
    initlock(&serial->lock);
    serial->state=serial->wp=serial->rp=serial->error=0;
    serial->buffsize=buffsize;
    if ((serial->buff=(unsigned char *)malloc((size_t)buffsize))==NULL) {
        CloseHandle(serial->dev);
        free(serial);
        return NULL;
    }
    serial->state=1;
    if ((serial->thread=CreateThread(NULL,0,serialthread,serial,0,NULL))==NULL) {
        sprintf(msg,"%s serial thread error (%d)",port,(int)GetLastError());
        CloseHandle(serial->dev);
        serial->state=0;
        free(serial);
        return NULL;
    }
    sprintf(msg,"%s",port);
#endif /* WIN32 */

    /* decode tcp path if exist */
    if ((p=strchr(buff,'#'))!=NULL) sscanf(p,"#%d",&tcp_port);
    
    serial->tcpsvr=NULL;
    if (tcp_port) {
        sprintf(path_tcp, ":%d", tcp_port);
        serial->tcpsvr=opentcpsvr(path_tcp,msg_tcp);
    }
    return serial;
}
/* close serial --------------------------------------------------------------*/
static void closeserial(serial_t *serial)
{
    trace(3,"closeserial:\n");

#ifdef WIN32
    serial->state=0;
    WaitForSingleObject(serial->thread,10000);
    CloseHandle(serial->thread);
    CloseHandle(serial->dev);
#else
    close(serial->dev);
#endif
    if (serial->tcpsvr) closetcpsvr(serial->tcpsvr);
    free(serial);
}
/* read serial ---------------------------------------------------------------*/
static int readserial(serial_t *serial, unsigned char *buff, int n, char *msg)
{
    char msg_tcp[128];
#ifdef WIN32
    DWORD nr;
#else
    int nr;
#endif
    trace(4,"readserial: dev=%d n=%d\n",serial->dev,n);
    if (!serial) return 0;
#ifdef WIN32
    if (!ReadFile(serial->dev,buff,(DWORD)n,&nr,NULL)) return 0;
#else
    if ((nr=read(serial->dev,buff,n))<0) return 0;
#endif
    trace(5,"readserial: exit dev=%d nr=%d\n",serial->dev,nr);
    
    /* write received stream to tcp server port */
    if (serial->tcpsvr&&nr>0) {
        writetcpsvr(serial->tcpsvr,buff,(int)nr,msg_tcp);
    }
    return (int)nr;
}
/* write serial --------------------------------------------------------------*/
static int writeserial(serial_t *serial, unsigned char *buff, int n, char *msg)
{
    int ns=0;
    
    trace(3,"writeserial: dev=%d n=%d\n",serial->dev,n);
    
    if (!serial) return 0;
#ifdef WIN32
    if ((ns=writeseribuff(serial,buff,n))<n) serial->error=1;
#else
    if (write(serial->dev,buff,n)<n) {
        serial->error=1;
    }
#endif
    trace(5,"writeserial: exit dev=%d ns=%d\n",serial->dev,ns);
    return ns;
}
/* get state serial ----------------------------------------------------------*/
static int stateserial(serial_t *serial) { return serial->error?-1:2; }
/* get extended state serial -------------------------------------------------*/
static int statexserial(serial_t *serial, char *msg)
{
    char *p=msg;
    int state=serial->error?-1:2;
    
    p+=sprintf(p,"serial:\n");
    p+=sprintf(p,"  state   = %d\n",state);
    if (!state) return 0;
    p+=sprintf(p,"  dev     = %d\n",(int)serial->dev);
    p+=sprintf(p,"  error   = %d\n",serial->error);
#ifdef WIN32
    p+=sprintf(p,"  buffsize= %d\n",serial->buffsize);
    p+=sprintf(p,"  wp      = %d\n",serial->wp);
    p+=sprintf(p,"  rp      = %d\n",serial->rp);
#endif
    return state;
}
/* UDP -----------------------------------------------------------------------*/
typedef struct {            /* udp type */
    int state;              /* state (0:close,1:open) */
    int type;               /* type  (0:server,1:client) */
    int port;               /* port */
    char saddr[256];        /* address (server:filter,client:server) */
    struct sockaddr_in addr; /* address resolved */
    socket_t sock;           /* socket descriptor */
} udp_t;
/* generate udp socket -------------------------------------------------------*/
static udp_t *genudp(int type, int port, const char *saddr, char *msg)
{
    udp_t *udp;
    #ifdef WIN32
        int tv=10;
    #else
        struct timeval tv={.tv_sec=0,.tv_usec=10};
    #endif
    int bs=buffsize, opt=1;
    
    if ((udp=(udp_t *)malloc(sizeof(udp_t)))==NULL) return NULL;
    udp->state=2;
    udp->type=type;
    udp->port=port;
    strcpy(udp->saddr,saddr);

    if ((udp->sock=socket(AF_INET,SOCK_DGRAM,0))==(socket_t)-1) {
        sprintf(msg,"socket error (%d)",errsock());
        free(udp);
        return NULL;
    }

    if (setsockopt(udp->sock,SOL_SOCKET,SO_RCVBUF,(const char *)&bs,sizeof(bs))==-1||
        setsockopt(udp->sock,SOL_SOCKET,SO_SNDBUF,(const char *)&bs,sizeof(bs))==-1) {
        trace(2,"genudp: setsockopt error sock=%d err=%d bs=%d\n",udp->sock,errsock(),bs);
        sprintf(msg,"sockopt error: bufsiz");
    }
    /* set udp receive timeout with 10 us*/
    if (setsockopt(udp->sock,SOL_SOCKET,SO_RCVTIMEO,(const char *)&tv,sizeof(tv))==-1||
        setsockopt(udp->sock,SOL_SOCKET,SO_SNDTIMEO,(const char *)&tv,sizeof(tv))==-1) {
        trace(2,"genudp: setsockopt error sock=%d err=%d\n",udp->sock,errsock());
        sprintf(msg,"sockopt error");
    }

    memset(&udp->addr,0,sizeof(udp->addr));
    udp->addr.sin_family=AF_INET;
    udp->addr.sin_port=htons((unsigned short) port);
    
    if (!udp->type) { /* udp server */
        udp->addr.sin_addr.s_addr=htonl(INADDR_ANY);
        if (bind(udp->sock,(struct sockaddr *)&udp->addr,sizeof(udp->addr))==-1) {
            sprintf(msg,"bind error (%d)",errsock());
            closesocket(udp->sock);
            free(udp);
            return NULL;
        }
    } else { /* udp client */
        if(!strcmp(saddr,"255.255.255.255")&&
            setsockopt(udp->sock,SOL_SOCKET,SO_BROADCAST,(const char *)&opt,
            sizeof(opt))==-1) {
            sprintf(msg,"sockopt error: broadcast");
        }
        
        int ret=1;
        struct addrinfo hints = {0}, *addrs;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_UDP;
        char port[256] = "";
        sprintf(port, "%d", udp->port);
        if ((ret=getaddrinfo(udp->saddr,port,&hints,&addrs))!=0) {
            sprintf(msg,"address error (%s)",udp->saddr);
            freeaddrinfo(addrs);
            closesocket(udp->sock);
            free(udp);
            return 0;
        }
        memcpy(&udp->addr, addrs->ai_addr, sizeof(udp->addr));
        freeaddrinfo(addrs);
    }
    return udp;
}
/* open udp server/client ----------------------------------------------------*/
static udp_t *openudp(const char *path, int type, char *msg)
{
    char sport[256]="",saddr[256]="";
    int port=0;

    decodetcppath(path,saddr,sport,NULL,NULL,NULL,NULL);

    if (sscanf(sport,"%d",&port)<1) {
        sprintf(msg,"port error: %s",sport);
        return NULL;
    }
    return genudp(type,port,saddr,msg);
}
/* close udp -----------------------------------------------------------------*/
static void closeudp(udp_t *udp)
{
    closesocket(udp->sock);
    free(udp);
}
/* read udp ------------------------------------------------------------------*/
static int readudp(udp_t *udp, unsigned char *buff, int n, char *msg)
{
    struct sockaddr_in addr;
    socklen_t len=sizeof(addr);
    int nr;
    
    if ((nr=(int)recvfrom(udp->sock,(char *)buff,n,0,
            (struct sockaddr *)&addr,&len))==-1) {
        sprintf(msg,"recvfrom error (%d)",errsock());
        return 0;
    }
    return nr;
}
/* write udp -----------------------------------------------------------------*/
static int writeudp(udp_t *udp, unsigned char *buff, int n, char *msg)
{
    if (udp->type==0) {
        sprintf(msg, "udp server read only. use udp client to send data.");
        return 0;
    }
    int ns;
    if ((ns=(int)sendto(udp->sock,(char *)buff,n,0,
            (struct sockaddr *)&udp->addr,sizeof(udp->addr)))==-1) {
        sprintf(msg,"sendto error (%d)",errsock());
        return 0;
    }
    return ns;
}
/* get state udp -------------------------------------------------------------*/
static int stateudp(udp_t *udp) { return udp->state; }
/* get extended state udp ----------------------------------------------------*/
static int statexudp(udp_t *udp, char *msg)
{
    static const char *type[]={"server","client"};
    char *p=msg;
    int state=udp->state;
    
    p+=sprintf(p,"udp:\n");
    p+=sprintf(p,"  state   = %d\n",state);
    if (!state) return 0;
    p+=sprintf(p,"  type    = %s\n",type[udp->type]);
    p+=sprintf(p,"  sock    = %d\n",(int)udp->sock);
    p+=sprintf(p,"  addr    = %s\n",udp->saddr);
    p+=sprintf(p,"  port    = %d\n",udp->port);
    return state;
}
/* File ----------------------------------------------------------------------*/
typedef struct {
    FILE *fp;               /* file pointer */
    FILE *fp_tmp;           /* temporary file pointer for swap */
    char path[MAXSTRPATH];  /* file path */
    char openpath[MAXSTRPATH]; /* open file path */
    int mode;               /* file mode */
    int repmode;            /* replay mode (0:master,1:slave) */
    int offset;             /* time offset (ms) for slave */
    int size_fpos;          /* file position size (bytes) */
    time_t time;            /* start time */
    time_t wtime;           /* write time */
    uint32_t tick;          /* start tick */
    uint32_t tick_f;        /* start tick in file */
    long fpos_n;            /* next file position */
    uint32_t tick_n;        /* next tick */
    double start;           /* start offset (s) */
    double speed;           /* replay speed (time factor) */
    double swapintv;        /* swap interval (hr) (0: no swap) */
    lock_t lock;            /* lock flag */
} file_t;

static time_t epoch2time(const double *ep)
{
    const int mday[]={ /* # of days in a month */
        31,28,31,30,31,30,31,31,30,31,30,31,31,28,31,30,31,30,31,31,30,31,30,31,
        31,29,31,30,31,30,31,31,30,31,30,31,31,28,31,30,31,30,31,31,30,31,30,31
    };
    int days,sec;
    
    days=(int)((ep[0]-1970)/4)*1461+((int)ep[1]-1)*59+((int)ep[2]-1);
    sec=((int)ep[3]*60+(int)ep[4])*60+(int)ep[5];
    if ((int)ep[1]<=2) days+=(int)((ep[0]-1969)/4)-1;
    else days+=(int)((ep[0]-1970)/4);
    for (int i=0;i<((int)ep[1]-1)%12;i++) days+=mday[i];
    return (time_t)days*86400+sec;
}
static void time2epoch(time_t t, double *ep)
{
    const int mday[]={ /* # of days in a month */
        31,28,31,30,31,30,31,31,30,31,30,31,31,28,31,30,31,30,31,31,30,31,30,31,
        31,29,31,30,31,30,31,31,30,31,30,31,31,28,31,30,31,30,31,31,30,31,30,31
    };
    int days,sec,mon,day;
    
    /* leap year if year%4==0 in 1901-2099 */
    days=(int)(t/86400);
    sec=(int)(t-(time_t)days*86400);
    for (day=days%1461,mon=0;mon<48;mon++) {
        if (day>=mday[mon]) day-=mday[mon]; else break;
    }
    ep[0]=1970+days/1461*4+mon/12; ep[1]=mon%12+1; ep[2]=day+1;
    ep[3]=sec/3600; ep[4]=sec%3600/60; ep[5]=sec%60;
}
/* time to string -------------------------------------------------------------*/
static void time2str(time_t t, char *s, int n)
{
    double ep[6];
    
    if (n<0) n=0; else if (n>12) n=12;
    time2epoch(t,ep);
    sprintf(s,"%04.0f/%02.0f/%02.0f %02.0f:%02.0f:%0*.*f",ep[0],ep[1],ep[2],
            ep[3],ep[4],n<=0?2:n+3,n<=0?0:n,ep[5]);
}
/* replace string ------------------------------------------------------------*/
static int repstr(char *str, const char *pat, const char *rep)
{
    int len=(int)strlen(pat);
    char buff[1024]={'\0'},*p,*q,*r;
    
    for (p=str,r=buff;*p;p=q+len) {
        if ((q=strstr(p,pat))==NULL) break;
        strncpy(r,p,(size_t)(q-p));
        r+=q-p;
        r+=sprintf(r,"%s",rep);
    }
    if (p<=str) return 0;
    strcpy(r,p);
    strcpy(str,buff);
    return 1;
}
/* replace keywords in file path -----------------------------------------------
* replace keywords in file path with date, time, rover and base station id
* args   : char   *path     I   file path (see below)
*          char   *rpath    O   file path in which keywords replaced (see below)
*          gtime_t time     I   time (gpst)  (time.time==0: not replaced)
*          char   *rov      I   rover id string        ("": not replaced)
*          char   *base     I   base station id string ("": not replaced)
* return : status (1:keywords replaced, 0:no valid keyword in the path,
*                  -1:no valid time)
* notes  : the following keywords in path are replaced by date, time and name
*              %Y -> yyyy : year (4 digits) (1900-2099)
*              %y -> yy   : year (2 digits) (00-99)
*              %m -> mm   : month           (01-12)
*              %d -> dd   : day of month    (01-31)
*              %h -> hh   : hours           (00-23)
*              %M -> mm   : minutes         (00-59)
*              %S -> ss   : seconds         (00-59)
*              %n -> ddd  : day of year     (001-366)
*              %H -> h    : hour code       (a=0,b=1,c=2,...,x=23)
*              %ha-> hh   : 3 hours         (00,03,06,...,21)
*              %hb-> hh   : 6 hours         (00,06,12,18)
*              %hc-> hh   : 12 hours        (00,12)
*              %t -> mm   : 15 minutes      (00,15,30,45)
*              %r -> rrrr : rover id
*              %b -> bbbb : base station id
*-----------------------------------------------------------------------------*/
static int reppath(const char *path, char *rpath, time_t timer, const char *rov,
                   const char *base)
{
    double ep[6],ep0[6]={2000,1,1,0,0,0};
    int doy,stat=0;
    char rep[64];
    
    strcpy(rpath,path);
    
    if (!strstr(rpath,"%")) return 0;
    if (*rov ) stat|=repstr(rpath,"%r",rov );
    if (*base) stat|=repstr(rpath,"%b",base);
    if (timer!=0) {
        time2epoch(timer,ep);
        ep0[0]=ep[0];
        doy=(int)floor(difftime(timer,epoch2time(ep0))/86400.0)+1;
        sprintf(rep,"%02d",  ((int)ep[3]/3)*3);   stat|=repstr(rpath,"%ha",rep);
        sprintf(rep,"%02d",  ((int)ep[3]/6)*6);   stat|=repstr(rpath,"%hb",rep);
        sprintf(rep,"%02d",  ((int)ep[3]/12)*12); stat|=repstr(rpath,"%hc",rep);
        sprintf(rep,"%04.0f",ep[0]);              stat|=repstr(rpath,"%Y",rep);
        sprintf(rep,"%02.0f",fmod(ep[0],100.0));  stat|=repstr(rpath,"%y",rep);
        sprintf(rep,"%02.0f",ep[1]);              stat|=repstr(rpath,"%m",rep);
        sprintf(rep,"%02.0f",ep[2]);              stat|=repstr(rpath,"%d",rep);
        sprintf(rep,"%02.0f",ep[3]);              stat|=repstr(rpath,"%h",rep);
        sprintf(rep,"%02.0f",ep[4]);              stat|=repstr(rpath,"%M",rep);
        sprintf(rep,"%02.0f",floor(ep[5]));       stat|=repstr(rpath,"%S",rep);
        sprintf(rep,"%03d",  doy);                stat|=repstr(rpath,"%n",rep);
        sprintf(rep,"%c",    'a'+(int)ep[3]);     stat|=repstr(rpath,"%H",rep);
        sprintf(rep,"%02d",  ((int)ep[4]/15)*15); stat|=repstr(rpath,"%t",rep);
    }
    else if (strstr(rpath,"%ha")||strstr(rpath,"%hb")||strstr(rpath,"%hc")||
            strstr(rpath,"%Y" )||strstr(rpath,"%y" )||strstr(rpath,"%m" )||
            strstr(rpath,"%d" )||strstr(rpath,"%h" )||strstr(rpath,"%M" )||
            strstr(rpath,"%S" )||strstr(rpath,"%n" )||strstr(rpath,"%W" )||
            strstr(rpath,"%D" )||strstr(rpath,"%H" )||strstr(rpath,"%t" )) {
        return -1; /* no valid time */
    }
    return stat;
}
/* generate local directory recursively --------------------------------------*/
static int mkdir_r(const char *dir)
{
    char pdir[1024],*p;

#ifdef WIN32
    HANDLE h;
    WIN32_FIND_DATA data;
    
    if (!*dir||!strcmp(dir+1,":\\")) return 1;
    
    sprintf(pdir,"%.1023s",dir);
    if ((p=strrchr(pdir,FILEPATHSEP))!=NULL) {
        *p='\0';
        h=FindFirstFile(pdir,&data);
        if (h==INVALID_HANDLE_VALUE) {
            if (!mkdir_r(pdir)) return 0;
        }
        else FindClose(h);
    }
    if (CreateDirectory(dir,NULL)||GetLastError()==ERROR_ALREADY_EXISTS) {
        return 1;
    }
#else
    FILE *fp;
    
    if (!*dir) return 1;
    
    sprintf(pdir,"%.1023s",dir);
    if ((p=strrchr(pdir,FILEPATHSEP))) {
        *p='\0';
        if (!(fp=fopen(pdir,"r"))) {
            if (!mkdir_r(pdir)) return 0;
        }
        else fclose(fp);
    }
    if (!mkdir(dir,0777)||errno==EEXIST) return 1;
#endif
    trace(2,"directory generation error: dir=%s\n",dir);
    return 0;
}
static void createdir(const char *path)
{
    char dir[1024],*p;
    strcpy(dir,path);
    if ((p=strrchr(dir,FILEPATHSEP))==NULL) return;
    *p='\0';

    mkdir_r(dir);
}
/* open file -----------------------------------------------------------------*/
static int openfile_(file_t *file, time_t timer, char *msg)
{
    char *rw;
    
    file->time=time(NULL);
    file->tick=file->tick_f=tickget();
    file->fpos_n=0;
    file->tick_n=(uint32_t)0;

    if (!*file->path) {
        sprintf(msg,"file path error");
        return 0;
    }
    reppath(file->path,file->openpath,timer,"","");

    /* create directory */
    if ((file->mode&(STR_MODE_W|STR_MODE_A))&&!(file->mode&STR_MODE_R)) {
        createdir(file->openpath);
    }
    if (file->mode&STR_MODE_R) rw="rb";
    else rw=(file->mode&STR_MODE_W)?"wb":"ab";
    
    /* open new file */
    if ((file->fp=fopen(file->openpath,rw))==NULL) {
        sprintf(msg,"file open error: %s",file->openpath);
        trace(2,"openfile: %s\n",msg);
        return 0;
    }
    
    return 1;
}
/* close file ----------------------------------------------------------------*/
static void closefile_(file_t *file)
{
    trace(3,"closefile_: path=%s\n",file->path);
    
    if (file->fp) fclose(file->fp);
    if (file->fp_tmp) fclose(file->fp_tmp);
    file->fp=file->fp_tmp=NULL;
}
/* open file (path=filepath[::T[::+<off>][::x<speed>]][::S=swapintv][::P={4|8}] */
static file_t *openfile(const char *path, int mode, char *msg)
{
    file_t *file;
    time_t timer,time0={0};
    double speed=1.0,start=0.0,swapintv=0.0;
    char *p;
    int size_fpos=4; /* default 4B */
    
    trace(3,"openfile: path=%s mode=%d\n",path,mode);
    
    if (!(mode&(STR_MODE_R|STR_MODE_W|STR_MODE_A))) return NULL;
    
    /* file options */
    for (p=(char *)path;(p=strstr(p,"::"))!=NULL;p+=2) { /* file options */
        if (*(p+2)=='+') sscanf(p+2,"+%lf",&start);
        else if (*(p+2)=='x') sscanf(p+2,"x%lf",&speed);
        else if (*(p+2)=='S') sscanf(p+2,"S=%lf",&swapintv);
        else if (*(p+2)=='P') sscanf(p+2,"P=%d",&size_fpos);
    }
    if (start<=0.0) start=0.0;
    if (swapintv<=0.0) swapintv=0.0;
    
    if ((file=(file_t *)malloc(sizeof(file_t)))==NULL) return NULL;
    
    file->fp=file->fp_tmp=NULL;
    strcpy(file->path,path);
    if ((p=strstr(file->path,"::"))!=NULL) *p='\0';
    file->openpath[0]='\0';
    file->mode=mode;
    file->repmode=0;
    file->offset=0;
    file->fpos_n=0;
    file->size_fpos=size_fpos;
    file->time=file->wtime=time0;
    file->tick=file->tick_f=file->tick_n=(uint32_t)0;
    file->start=start;
    file->speed=speed;
    file->swapintv=swapintv;
    initlock(&file->lock);
    
    timer=time(NULL);

    /* open new file */
    if (!openfile_(file,timer,msg)) {
        free(file);
        return NULL;
    }
    return file;
}
/* close file ----------------------------------------------------------------*/
static void closefile(file_t *file)
{
    trace(3,"closefile: fp=%d\n",file->fp);
    
    closefile_(file);
    free(file);
}
/* open new swap file --------------------------------------------------------*/
static void swapfile(file_t *file, time_t timer, char *msg)
{
    char openpath[MAXSTRPATH];
    
    /* return if old swap file open */
    if (file->fp_tmp) return;
    
    /* check path of new swap file */
    reppath(file->path,openpath,timer,"","");
    
    if (!strcmp(openpath,file->openpath)) {
        trace(2,"swapfile: no need to swap %s\n",openpath);
        return;
    }
    /* save file pointer to temporary pointer */
    file->fp_tmp=file->fp;
    
    /* open new swap file */
    openfile_(file,timer,msg);
}
/* close old swap file -------------------------------------------------------*/
static void swapclose(file_t *file)
{
    trace(3,"swapclose: fp_tmp=%d\n",file->fp_tmp);
    
    if (file->fp_tmp) fclose(file->fp_tmp);
    file->fp_tmp=NULL;
}
/* read file -----------------------------------------------------------------*/
static int readfile(file_t *file, unsigned char *buff, int nmax, char *msg)
{
    int nr=0;
    
    trace(4,"readfile: fp=%d nmax=%d\n",file->fp,nmax);
    
    if (!file) return 0;
    
    if (file->fp==stdin) {
#ifndef WIN32
        fd_set rs;
        struct timeval tv={0};
        /* input from stdin */
        FD_ZERO(&rs); FD_SET(0,&rs);
        if (!select(1,&rs,NULL,NULL,&tv)) return 0;
        if ((nr=(int)read(0,buff,nmax))<0) return 0;
        return nr;
#else
        return 0;
#endif
    }
    if (nmax>0) {
        nr=(int)fread(buff,1,(size_t)nmax,file->fp);
    }
    if (feof(file->fp)) {
        sprintf(msg,"end");
    }
    trace(5,"readfile: fp=%d nr=%d\n",file->fp,nr);
    return nr;
}
/* write file ----------------------------------------------------------------*/
static int writefile(file_t *file, unsigned char *buff, int n, char *msg)
{
    time_t wtime;
    int ns;
    double pt,ct,intv;
    long fpos;
    
    trace(4,"writefile: fp=%d n=%d\n",file->fp,n);
    
    if (!file) return 0;
    
    wtime=time(NULL); /* write time in gpst */
    
    /* swap writing file */
    if (file->swapintv>0.0&&file->time!=0) {
        intv=file->swapintv*3600.0;
        pt = (double)file->wtime;
        ct = (double)wtime;
        /* open new swap file */
        if (floor((pt+fswapmargin)/intv)<floor((ct+fswapmargin)/intv)) {
            swapfile(file,wtime+fswapmargin,msg);
        }
        /* close old swap file */
        if (floor((pt-fswapmargin)/intv)<floor((ct-fswapmargin)/intv)) {
            swapclose(file);
        }
    }
    if (!file->fp) return 0;
    
    ns=(int)fwrite(buff,1,(size_t)n,file->fp);
    fpos=ftell(file->fp);
    fflush(file->fp);
    file->wtime=wtime;
    
    if (file->fp_tmp) {
        fwrite(buff,1,(size_t)n,file->fp_tmp);
        fflush(file->fp_tmp);
    }
    
    return ns;
}
/* get state file ------------------------------------------------------------*/
static int statefile(file_t *file) { return file?2:0; }
/* get extended state file ---------------------------------------------------*/
static int statexfile(file_t *file, char *msg)
{
    char *p=msg,tstr1[32],tstr2[32];
    int state=2;
    
    p+=sprintf(p,"file:\n");
    p+=sprintf(p,"  state   = %d\n",state);
    if (!state) return 0;
    time2str(file->time ,tstr1,3);
    time2str(file->wtime,tstr2,3);
    p+=sprintf(p,"  path    = %s\n",file->path);
    p+=sprintf(p,"  openpath= %s\n",file->openpath);
    p+=sprintf(p,"  mode    = %d\n",file->mode);
    p+=sprintf(p,"  repmode = %d\n",file->repmode);
    p+=sprintf(p,"  offsete = %d\n",file->offset);
    p+=sprintf(p,"  time    = %s\n",tstr1);
    p+=sprintf(p,"  wtime   = %s\n",tstr2);
    p+=sprintf(p,"  tick    = %u\n",file->tick);
    p+=sprintf(p,"  tick_f  = %u\n",file->tick_f);
    p+=sprintf(p,"  start   = %.3f\n",file->start);
    p+=sprintf(p,"  speed   = %.3f\n",file->speed);
    p+=sprintf(p,"  swapintv= %.3f\n",file->swapintv);
    return state;
}
/* TCP with TLS --------------------------------------------------------------*/
/* TODO */
static void *opentcpclissl(const char *path, char *msg)
{
    trace(2,"opentcpclissl: no supported\n");
    return NULL;
}
static void closetcpclissl(void *tcp)
{
    trace(3,"closetcpclissl: no supported\n");
}
static int readtcpclissl(void *tcp, unsigned char *buff, int n, char *msg)
{
    trace(3,"readtcpclissl: no supported\n");
    return 0;
}
static int writetcpclissl(void *tcp, unsigned char *buff, int n, char *msg)
{
    trace(3,"writetcpclissl: no supported\n");
    return 0;
}
static int statetcpclissl(void *tcp)
{
    trace(3,"statetcpclissl: no supported\n");
    return 0;
}
static int statextcpclissl(void *tcp, char *msg)
{
    trace(3,"statextcpclissl: no supported\n");
    return 0;
}
/* extern function -----------------------------------------------------------*/
/* initialize stream environment -----------------------------------------------
* initialize stream environment for windows socket
* args   : none
* return : none
*-----------------------------------------------------------------------------*/
extern void strinitcom(void)
{
#ifdef WIN32
    WSADATA data;
    WSAStartup(MAKEWORD(2,0),&data);
#endif
}
/* initialize stream -----------------------------------------------------------
* initialize stream struct
* args   : stream_t *stream IO  stream
* return : none
*-----------------------------------------------------------------------------*/
extern void strinit(stream_t *stream)
{
    stream->type=0;
    stream->mode=0;
    stream->state=0;
    stream->inb=stream->inr=stream->outb=stream->outr=0;
    stream->tick_i=stream->tick_o=stream->tact=stream->inbt=stream->outbt=0;
    initlock(&stream->lock);
    stream->port=NULL;
    stream->path[0]='\0';
    stream->msg [0]='\0';
}
/* open stream -----------------------------------------------------------------
*
* open stream to read or write data from or to virtual devices.
*
* args   : stream_t *stream IO  stream
*          int type         I   stream type
*                                 STR_SERIAL   = serial device
*                                 STR_FILE     = file (record and playback)
*                                 STR_TCPSVR   = TCP server
*                                 STR_TCPCLI   = TCP client
*                                 STR_TCPCLISSL= TCP client with SSL
*                                 STR_UDPSVR   = UDP server (readonly)
*                                 STR_UDPCLI   = UDP client
*                                 STR_NTRIPSVR = NTRIP server
*                                 STR_NTRIPCLI = NTRIP client
*          int mode         I   stream mode (STR_MODE_???)
*                                 STR_MODE_R   = read only
*                                 STR_MODE_W   = write only
*                                 STR_MODE_RW  = read and write
*                                 STR_MODE_A   = append for file (output only)
*          char *path       I   stream path (see below)
*
* return : status (0:error,1:ok)
*
* notes  : see reference [1] for NTRIP
*
* stream path ([] options):
*
*   STR_SERIAL   port[:brate[:bsize[:parity[:stopb[:fctr[#port]]]]]]
*                    port  = COM??  (windows)
*                            tty??? (linuex, omit /dev/)
*                    brate = bit rate     (bps)
*                    bsize = bit size     (7|8)
*                    parity= parity       (n|o|e)
*                    stopb = stop bits    (1|2)
*                    fctr  = flow control (off|rts)
*                    port  = tcp server port to output received stream
*
*   STR_FILE     path[::T][::+start][::xseppd][::S=swap][::P={4|8}]
*                    path  = file path
*                            (can include keywords defined by )
*                    ::T   = enable time tag
*                    start = replay start offset (s)
*                    speed = replay speed factor
*                    swap  = output swap interval (hr) (0: no swap)
*                    ::P={4|8} = file pointer size (4:32bit,8:64bit)
*
*   STR_TCPSVR   :port
*                    port  = TCP server port to accept
*
*   STR_TCPCLI   addr:port
*                    addr  = TCP server address to connect
*                    port  = TCP server port to connect
*
*   STR_NTRIPSVR [:passwd@]addr[:port]/mponit[:string]
*                    addr  = NTRIP caster address to connect
*                    port  = NTRIP caster server port to connect
*                    passwd= NTRIP caster server password to connect
*                    mpoint= NTRIP mountpoint
*                    string= NTRIP server string
*
*   STR_NTRIPCLI [user[:passwd]@]addr[:port]/mpoint
*                    addr  = NTRIP caster address to connect
*                    port  = NTRIP caster client port to connect
*                    user  = NTRIP caster client user to connect
*                    passwd= NTRIP caster client password to connect
*                    mpoint= NTRIP mountpoint
*
*-----------------------------------------------------------------------------*/
extern int stropen(stream_t *stream, stream_type type, stream_mode mode, const char *path)
{
    trace(3,"stropen: type=%d mode=%d path=%s\n",type,mode,path);
    
    stream->type=type;
    stream->mode=mode;
    strcpy(stream->path,path);
    stream->inb=stream->inr=stream->outb=stream->outr=0;
    stream->tick_i=stream->tick_o=tickget();
    stream->inbt=stream->outbt=0;
    stream->msg[0]='\0';
    stream->port=NULL;
    if (type != STR_FILE && mode == STR_MODE_A) mode=STR_MODE_W;
    switch (type) {
        case STR_SERIAL  : stream->port=openserial(path,mode,stream->msg); break;
        case STR_FILE    : stream->port=openfile  (path,mode,stream->msg); break;
        case STR_TCPSVR  : stream->port=opentcpsvr(path,     stream->msg); break;
        case STR_TCPCLI  : stream->port=opentcpcli(path,     stream->msg); break;
        case STR_TCPCLI_SSL: stream->port=opentcpclissl(path, stream->msg); break;
        case STR_UDPSVR  : stream->port=openudp   (path,0,   stream->msg); break;
        case STR_UDPCLI  : stream->port=openudp   (path,1,   stream->msg); break;
        case STR_NTRIPSVR: stream->port=openntrip (path,0,   stream->msg); break;
        case STR_NTRIPCLI: stream->port=openntrip (path,1,   stream->msg); break;
        default: stream->state=0; return 1;
    }
    stream->state=!stream->port?-1:1;
    return stream->port!=NULL;
}
/* close stream ----------------------------------------------------------------
* close stream and free stream memory
* args   : stream_t *stream IO  stream
* return : none
*-----------------------------------------------------------------------------*/
extern void strclose(stream_t *stream)
{
    trace(3,"strclose: type=%d\n",stream->type);
    
    strlock(stream);
    if (!stream->state) return;
    switch (stream->type) {
        case STR_SERIAL  : closeserial((serial_t *)stream->port); break;
        case STR_FILE    : closefile  ((file_t   *)stream->port); break;
        case STR_TCPSVR  : closetcpsvr((tcpsvr_t *)stream->port); break;
        case STR_TCPCLI  : closetcpcli((tcpcli_t *)stream->port); break;
        case STR_TCPCLI_SSL: closetcpclissl((tcpcli_t *)stream->port); break;
        case STR_UDPSVR  :
        case STR_UDPCLI  : closeudp   ((udp_t    *)stream->port); break;
        case STR_NTRIPSVR:
        case STR_NTRIPCLI: closentrip ((ntrip_t  *)stream->port); break;
    }
    stream->state=0;
    stream->port=NULL;
    strunlock(stream);
}

/* read stream -----------------------------------------------------------------
* read data from stream
* args   : stream_t *stream IO  stream
*          unsinged char *buff O   receive data buffer
*          int    n         I   buffer size (bytes)
* return : read data size (bytes)
*-----------------------------------------------------------------------------*/
extern int strread(stream_t *stream, unsigned char *buff, int n)
{
    if (!stream->port || !(stream->mode&STR_MODE_R)) return 0;

    uint32_t tick=tickget();
    char *msg=stream->msg;
    int nr=0,tt;

    strlock(stream);

    switch (stream->type) {
        case STR_SERIAL  : nr=readserial((serial_t *)stream->port,buff,n,msg); break;
        case STR_FILE    : nr=readfile  ((file_t   *)stream->port,buff,n,msg); break;
        case STR_TCPSVR  : nr=readtcpsvr((tcpsvr_t *)stream->port,buff,n,msg); break;
        case STR_TCPCLI  : nr=readtcpcli((tcpcli_t *)stream->port,buff,n,msg); break;
        case STR_TCPCLI_SSL: nr=readtcpclissl((tcpcli_t *)stream->port,buff,n,msg); break;
        case STR_UDPSVR  :
        case STR_UDPCLI  : nr=readudp   ((udp_t *)stream->port,buff,n,msg); break;
        case STR_NTRIPSVR:
        case STR_NTRIPCLI: nr=readntrip ((ntrip_t  *)stream->port,buff,n,msg); break;
        default: strunlock(stream); return 0;
    }

    if (nr){
        stream->inb+=(uint32_t)nr; stream->inr+=(uint32_t)nr;
        stream->tick_i=tickget();
    }

    tt=(int)(tick-stream->tick_i);
    if (tt>=tirate) {
        stream->inr=
            (uint32_t)((double)((stream->inb-stream->inbt)*8)/(tt*0.001));
        stream->tick_i=tick;
        stream->inbt=stream->inb;
    }
    strunlock(stream);
    return nr;
}
/* write stream ----------------------------------------------------------------
* write data to stream
* args   : stream_t *stream IO  stream
*          unsinged char *buff I   send data buffer
*          int    n         I   write data size (bytes)
* return : write data size (bytes)
*-----------------------------------------------------------------------------*/
extern int strwrite(stream_t *stream, unsigned char *buff, int n)
{
    if (!stream->port || !(stream->mode&(STR_MODE_W|STR_MODE_A))) return 0;
    
    uint32_t tick=tickget();
    char *msg=stream->msg;
    int ns=0,tt;

    strlock(stream);

    switch (stream->type) {
        case STR_SERIAL  : ns=writeserial((serial_t *)stream->port,buff,n,msg); break;
        case STR_FILE    : ns=writefile  ((file_t   *)stream->port,buff,n,msg); break;
        case STR_TCPSVR  : ns=writetcpsvr((tcpsvr_t *)stream->port,buff,n,msg); break;
        case STR_TCPCLI  : ns=writetcpcli((tcpcli_t *)stream->port,buff,n,msg); break;
        case STR_TCPCLI_SSL: ns=writetcpclissl((tcpcli_t *)stream->port,buff,n,msg); break;
        case STR_UDPSVR  :
        case STR_UDPCLI  : ns=writeudp   ((udp_t *)stream->port,buff,n,msg); break;
        case STR_NTRIPSVR:
        case STR_NTRIPCLI: ns=writentrip ((ntrip_t  *)stream->port,buff,n,msg); break;
        default: strunlock(stream); return 0;
    }

    if (ns) {
        stream->outb+=(uint32_t)ns; stream->outr+=(uint32_t)ns;
        stream->tick_o=tickget();
    }
    tt=(int)(tick-stream->tick_o);
    if (tt>=tirate) {
        stream->outr=
            (uint32_t)((double)((stream->outb-stream->outbt)*8)/(tt*0.001));
        stream->tick_o=tick;
        stream->outbt=stream->outb;
    }
    strunlock(stream);
    return ns;
}
/* get stream status ----------------------------------------------------------
* get stream status
* args   : stream_t *stream I   stream
*          char     *msg    IO  status message (NULL: no output)
* return : status (-1:error,0:close,1:wait,2:connect,3:active)
*-----------------------------------------------------------------------------*/
extern int strstat(stream_t *stream, char *msg)
{
    int state=0;
    if (!stream->port) return stream->state;

    if (msg) {
        strncpy(msg,stream->msg,MAXSTRMSG-1);
        msg[MAXSTRMSG-1]='\0';
    }

    strlock(stream);

    switch (stream->type) {
        case STR_SERIAL  : state=stateserial((serial_t *)stream->port); break;
        case STR_FILE    : state=statefile  ((file_t   *)stream->port); break;
        case STR_TCPSVR  : state=statetcpsvr((tcpsvr_t *)stream->port); break;
        case STR_TCPCLI  : state=statetcpcli((tcpcli_t *)stream->port); break;
        case STR_TCPCLI_SSL: state=statetcpclissl((tcpcli_t *)stream->port); break;
        case STR_UDPSVR  :
        case STR_UDPCLI  : state=stateudp   ((udp_t    *)stream->port); break;
        case STR_NTRIPSVR:
        case STR_NTRIPCLI: state=statentrip ((ntrip_t  *)stream->port); break;
    }
    if (state==2&&(int)(tickget()-stream->tact)<=TINTACT) state=3;
    strunlock(stream);
    return state;
}
/* get stream status with extended status message -----------------------------
* get stream status with extended status message
* args   : stream_t *stream I   stream
*          char     *msg    IO  status message (NULL: no output)
* return : status (-1:error,0:close,1:wait,2:connect,3:active)
*-----------------------------------------------------------------------------*/
extern int strstatx(stream_t *stream, char *msg)
{
    int state=0;
    if (!stream->port) return stream->state;

    if (msg) {
        strncpy(msg,stream->msg,MAXSTRMSG-1);
        msg[MAXSTRMSG-1]='\0';
    }

    strlock(stream);

    switch (stream->type) {
        case STR_SERIAL  : state=statexserial((serial_t *)stream->port,msg); break;
        case STR_FILE    : state=statexfile  ((file_t   *)stream->port,msg); break;
        case STR_TCPSVR  : state=statextcpsvr((tcpsvr_t *)stream->port,msg); break;
        case STR_TCPCLI  : state=statextcpcli((tcpcli_t *)stream->port,msg); break;
        case STR_TCPCLI_SSL: state=statextcpclissl((tcpcli_t *)stream->port,msg); break;
        case STR_UDPSVR  :
        case STR_UDPCLI  : state=statexudp   ((udp_t    *)stream->port,msg); break;
        case STR_NTRIPSVR:
        case STR_NTRIPCLI: state=statexntrip ((ntrip_t  *)stream->port,msg); break;
    }
    if (state==2&&(int)(tickget()-stream->tact)<=TINTACT) state=3;
    strunlock(stream);
    return state;
}