#include<stdio.h>
#include<stdlib.h>
#include<iostream>
#include <stream.h>

static stream_t stream;

static void tcpsvr_test(stream_t *str) {
    stropen(str, STR_TCPSVR, STR_MODE_RW, ":2000");
    
    Sleep(5000);
    strwrite(str, (unsigned char *)"Hello World\n", 12);

    char buff[1024];
    int len=0;
    for(int i=0;i<5;i++) {
        len=strread(str, (unsigned char *)buff, 1024);
        buff[len]='\0';
        printf("%s\n", buff);
        strwrite(str,(unsigned char *)buff,len);
        Sleep(1000);
    }
    strclose(str);
}

static void tcpcli_test(stream_t *str) {
    stropen(str, STR_TCPCLI, STR_MODE_RW, "localhost:2000");
    
    char buff[1024];
    int len=0;
    for(int i=0;i<2;i++) {
        len=strread(str, (unsigned char *)buff, 1024);
        buff[len]='\0';
        printf("svr (%d)=>%s\n", len, buff);
        strwrite(str, (unsigned char *)"Hello World\n", 12);
        Sleep(1000);
    }
    strclose(str);
}

static void udpsvr_test(stream_t *str) {
    stropen(str, STR_UDPSVR, STR_MODE_RW, "2000");
    char buff[1024];
    int len=0;
    for(int i=0;i<2;i++) {
        len=strread(str, (unsigned char *)buff, 1024);
        buff[len]='\0';
        printf("clt (%d)=>%s\n", len, buff);
        strwrite(str, (unsigned char *)"Hello World\n", 12);
        Sleep(1000);
    }
    strclose(str);
}

static void udpcli_test(stream_t *str) {
    stropen(str, STR_UDPCLI, STR_MODE_RW, "localhost:2000");
    char buff[1024];
    int len=0;
    for(int i=0;i<2;i++) {
        len=strread(str, (unsigned char *)buff, 1024);
        buff[len]='\0';
        printf("svr (%d)=>%s\n", len, buff);
        strwrite(str, (unsigned char *)"Hello World\n", 12);
        Sleep(1000);
    }
    strclose(str);
}

int main(int argc, char **argv) {
    strinitcom();
    strinit(&stream);

    udpcli_test(&stream);
    return EXIT_SUCCESS;
}