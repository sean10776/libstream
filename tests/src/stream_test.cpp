#include <string>
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <stream.h>

stream_t stream_i, stream_o;
static const char *test_str = "Hello, World!";

static void sleepms(int ms) {
#ifdef WIN32
    if (ms<5) Sleep(1); else Sleep((DWORD) ms);
#else
    struct timespec ts;
    if (ms<=0) return;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&ts, NULL);
#endif
}

TEST_CASE("Test stream initialization") {
    strinitcom();
    strinit(&stream_i);
    strinit(&stream_o);

    CHECK(stream_i.port==NULL);
    CHECK(stream_i.state==0);
    CHECK(stream_o.port==NULL);
    CHECK(stream_o.state==0);
}

TEST_CASE("Test TCP connection & read write") {    
    char buf_i[1024]={0}, buf_o[1024]={0};
    int len, len_i, len_o;
    len = strlen(test_str);

    stropen(&stream_o, STR_TCPSVR, STR_MODE_RW, ":8080");
    stropen(&stream_i, STR_TCPCLI, STR_MODE_RW, "127.0.0.1:8080");

    CHECK(stream_i.state==1);
    CHECK(stream_o.state==1);

    /* Write test string from stream_i to stream_o */
    while (!strwrite(&stream_i, (unsigned char*) test_str, len)) sleepms(10);
    
    /* Read test string in stream_o */
    len_o = strread(&stream_o, (unsigned char*) buf_o, 1024);
    CHECK(len_o==len);
    CHECK(strncmp(buf_o, test_str, len)==0);
    
    /* Write test string from stream_o to stream_i */
    while (!strwrite(&stream_o, (unsigned char*) test_str, len)) sleepms(10);
    
    /* Read test string in stream_i */
    len_i = strread(&stream_i, (unsigned char*) buf_i, 1024);
    CHECK(len_i==len);
    CHECK(strncmp(buf_i, test_str, len)==0);
    
    /* Close streams */
    strclose(&stream_i);
    strclose(&stream_o);
}

TEST_CASE("Test UDP connection & read write") {
    char buf_i[1024]={0};
    int len, len_i;
    len = strlen(test_str);

    stropen(&stream_o, STR_UDPSVR, STR_MODE_RW, ":2000");
    stropen(&stream_i, STR_UDPCLI, STR_MODE_RW, "127.0.0.1:2000");
    
    CHECK(stream_i.state==1);
    CHECK(stream_o.state==1);
    
    /* Write test string from stream_i to stream_o */
    while (!strwrite(&stream_i, (unsigned char*) test_str, len)) sleepms(10);
    
    /* Read test string in stream_o */
    len_i = strread(&stream_o, (unsigned char*) buf_i, 1024);
    CHECK(len_i==len);
    CHECK(strncmp(buf_i, test_str, len)==0);
    
    /* Close streams */
    strclose(&stream_i);
    strclose(&stream_o);
}

TEST_CASE("Test File I/O") {
    int len, len_i;
    char buf[1024]={0},answer[1024]={0},file[]="./test.txt";
    len = strlen(test_str);
    sprintf(answer,"%s%s",test_str,test_str);

    stropen(&stream_i, STR_FILE, STR_MODE_W, file);
    
    CHECK(stream_i.state==1);

    /* write test message to file */
    strwrite(&stream_i, (unsigned char*) test_str, len);

    /* close stream */
    strclose(&stream_i);

    stropen(&stream_i, STR_FILE, STR_MODE_A, file);

    CHECK(stream_i.state==1);
    
    /* write more test message to file */
    strwrite(&stream_i, (unsigned char*) test_str, len);
    
    /* close stream */
    strclose(&stream_i);
    stropen(&stream_i, STR_FILE, STR_MODE_R, file);
    
    CHECK(stream_i.state==1);
    /* read test message from file */
    len_i = strread(&stream_i, (unsigned char*) buf, 1024);
    CHECK(len_i==len*2);
    CHECK(strncmp(buf, answer, len_i)==0);

    /* close stream */
    strclose(&stream_i);
}