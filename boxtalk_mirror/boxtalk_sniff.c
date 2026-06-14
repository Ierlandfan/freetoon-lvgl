/* boxtalk_sniff — connect to a Toon's boxtalk_proxy, subscribe to a set of
 * services, force a notify republish, and dump every frame. Used to RE what the
 * master actually publishes (energy flow notifies, boiler pressure, the
 * statusUsage dataset, etc.) so the mirror can synthesize them.
 *
 *   boxtalk_sniff <host> <port> <user> <pass> [seconds]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static const char *g_host, *g_user, *g_pass;
static int g_port;

static int b64enc(const unsigned char *in, int n, char *out, int outsz) {
    static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;
    for (int i = 0; i < n; i += 3) {
        int b0=in[i], b1=i+1<n?in[i+1]:0, b2=i+2<n?in[i+2]:0;
        if (o+4 >= outsz) break;
        out[o++]=T[b0>>2]; out[o++]=T[((b0&3)<<4)|(b1>>4)];
        out[o++]=i+1<n?T[((b1&15)<<2)|(b2>>6)]:'=';
        out[o++]=i+2<n?T[b2&63]:'=';
    }
    out[o]=0; return o;
}
static int tcp_connect(const char *host, int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0); if (s<0) return -1;
    struct sockaddr_in a; memset(&a,0,sizeof a);
    a.sin_family=AF_INET; a.sin_port=htons(port);
    if (!inet_aton(host,&a.sin_addr)) { close(s); return -1; }
    if (connect(s,(struct sockaddr*)&a,sizeof a)!=0){ close(s); return -1; }
    return s;
}
static int proxy_open(void) {
    int s = tcp_connect(g_host, g_port); if (s<0) return -1;
    char up[160], cred[256], req[512];
    snprintf(up,sizeof up,"%s:%s",g_user,g_pass);
    b64enc((unsigned char*)up,strlen(up),cred,sizeof cred);
    int n=snprintf(req,sizeof req,"GET /boxtalk HTTP/1.0\r\nAuthorization: Basic %s\r\n\r\n",cred);
    if (send(s,req,n,0)!=n){close(s);return -1;}
    char hdr[512]; int h=0;
    while (h<(int)sizeof(hdr)-1){ int k=recv(s,hdr+h,1,0); if(k<=0){close(s);return -1;} h+=k; hdr[h]=0; if(h>=4&&!strcmp(hdr+h-4,"\r\n\r\n"))break; }
    if(!strstr(hdr," 200 ")){ fprintf(stderr,"auth failed: %.60s\n",hdr); close(s); return -1; }
    return s;
}
static int send_frame(int fd,const char*xml){ size_t n=strlen(xml); char z=0; if(send(fd,xml,n,0)!=(ssize_t)n)return -1; if(send(fd,&z,1,0)!=1)return -1; return 0; }
static int read_frame(int fd,char*buf,int cap){ int n=0; for(;;){ char c; int k=recv(fd,&c,1,0); if(k<=0)return -1; if(c==0){buf[n]=0;return n;} if(n<cap-1)buf[n++]=c; } }

/* services p1bridge / happ_thermstat / sensors publish */
static const char *SERVICES[] = {
    "ThermostatInfo","BoilerInfo","ElectricityFlowMeter","GasFlowMeter",
    "WaterFlowMeter","ElectricityQuantityMeter","GasQuantityMeter",
    "WaterQuantityMeter","HumiditySensor","TemperatureSensor","PowerUsage",
};
#define NSVC ((int)(sizeof SERVICES/sizeof SERVICES[0]))

int main(int argc,char**argv){
    if(argc<5){ fprintf(stderr,"usage: %s host port user pass [secs]\n",argv[0]); return 2; }
    g_host=argv[1]; g_port=atoi(argv[2]); g_user=argv[3]; g_pass=argv[4];
    int secs = argc>5?atoi(argv[5]):12;
    int fd=proxy_open(); if(fd<0){ fprintf(stderr,"connect failed\n"); return 1; }
    fprintf(stderr,"connected to %s:%d\n",g_host,g_port);
    int pid=(int)getpid(); long now=(long)time(NULL); char b[1024];
    snprintf(b,sizeof b,"<discovery nts=\"ssdp:connect\" uuid=\"sniff-%d\" type=\"urn:schemas-hcb-hae-com:device:toonui\" version=\"v\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" sessionKey=\"%d-%ld\"></discovery>",pid,pid,now);
    send_frame(fd,b);
    for(int i=0;i<NSVC;i++){
        snprintf(b,sizeof b,"<subscribe uuid=\"sniff-%d\" destuuid=\"\"><target uuid=\"\" serviceid=\"urn:hcb-hae-com:serviceId:%s\"></target></subscribe>",pid,SERVICES[i]);
        send_frame(fd,b);
    }
    snprintf(b,sizeof b,"<action class=\"invoke\" uuid=\"sniff-%d\" destuuid=\"\" serviceid=\"urn:hcb-hae-com:serviceId:specific1\"><u:reSendAllNotifies xmlns:u=\"urn:hcb-hae-com:service:specific1:1\"></u:reSendAllNotifies></action>",pid);
    send_frame(fd,b);
    /* optional query: argv 6=GUID 7=varName -> QueryStateVariable on ThermostatInfo */
    if (argc>7) {
        snprintf(b,sizeof b,"<query class=\"invoke\" uuid=\"sniff-%d\" destuuid=\"%s\" serviceid=\"urn:hcb-hae-com:serviceId:ThermostatInfo\" requestid=\"%d-9\"><u:QueryStateVariable xmlns:u=\"urn:hcb-hae-com:service:ThermostatInfo:1\"><varName>%s</varName><requestId>%d-9</requestId><timeout>30</timeout></u:QueryStateVariable></query>",pid,argv[6],pid,argv[7],pid);
        send_frame(fd,b);
        fprintf(stderr,"sent query %s on %s\n",argv[7],argv[6]);
    }
    /* dump frames until time runs out */
    time_t t0=time(NULL); static char fr[65536];
    while(time(NULL)-t0 < secs){
        int n=read_frame(fd,fr,sizeof fr); if(n<0) break;
        /* skip the noisy watchdog/lighttpd/heartbeat control chatter */
        if(strstr(fr,"hcb_watchdog")||strstr(fr,"lighttpd")||strstr(fr,"<heartbeat")) continue;
        printf("--- %dB ---\n%s\n", n, fr);
        fflush(stdout);
    }
    return 0;
}
