#pragma once
#include <string>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
using namespace std;
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t IO_BUFFER_SIZE = 64 * 1024;
const int LISTEN_BACKLOG = 16;
const int CLIENT_TIMEOUT_SEC = 30;       
const char STATUS_OK = 0;                 
const char STATUS_ERROR = 1;              
const int CRC_SIZE = 4;                   
const int64_t TTDB_HEADER_SIZE = 20;     
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin;
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField;
    string targetFuncName;
};
struct Variable
{
    std::string name;
    int32_t value;
};
struct Frame
{
    std::string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader
{
    char magic[4]; 
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
class Crc32
{
    uint32_t crc;

public:
    Crc32() { 
        crc = 0xFFFFFFFFu;
    }
    void update(const char* buf, size_t n)
    {
        for (size_t i = 0; i < n; i++)
        {
            crc ^= (unsigned char)buf[i];
            for (int b = 0; b < 8; b++)
                crc = (crc >> 1)^(0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    uint32_t value() const {
        return ~crc;
    }
};
inline void putU32(uint32_t v, char out[CRC_SIZE])
{
    out[0] = (char)((v >> 24) & 0xFF);
    out[1] = (char)((v >> 16) & 0xFF);
    out[2] = (char)((v >> 8) & 0xFF);
    out[3] = (char)(v & 0xFF);
}
inline uint32_t getU32(const char in[CRC_SIZE])
{
    return ((uint32_t)(unsigned char)in[0] << 24) | ((uint32_t)(unsigned char)in[1] << 16) |
        ((uint32_t)(unsigned char)in[2]<<8) | (uint32_t)(unsigned char)in[3];
}
class TrailerChecker
{
    Crc32 crc;
    char tail[CRC_SIZE];
    int held;
    public:
    TrailerChecker() { held = 0; }
    size_t feed(const char* in, size_t n, char* out)
    {
        size_t outCount = 0;
        for (size_t i = 0; i < n; i++)
        {
            if (held == CRC_SIZE)
            {
                out[outCount++] = tail[0];
                memmove(tail, tail+1, CRC_SIZE - 1);
                tail[CRC_SIZE - 1] = in[i];
            }
            else
                tail[held++] = in[i];
        }
        crc.update(out, outCount);
        return outCount;
    }
       bool verify() const
    {
        if (held<CRC_SIZE)
            return false;
        return crc.value()==getU32(tail);
    }
    void getTrailer(char out[CRC_SIZE]) const { 
        memcpy(out, tail, CRC_SIZE);
    }
};
inline bool sendAll(int sock, const char* buf, size_t n)
{
    size_t sent = 0;
    while (sent < n)
    {
        ssize_t r=send(sock,buf+sent,n-sent, MSG_NOSIGNAL);
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (r == 0)
            return false;
        sent += (size_t)r;
    }
    return true;
}
inline bool recvAll(int sock, char* buf, size_t n)
{
    size_t got=0;
    while (got < n)
    {
        ssize_t r = recv(sock, buf + got, n - got, 0);
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (r == 0)
            return false;
        got += (size_t)r;
    }
    return true;
}
inline bool sendError(int sock, const std::string& message)
{
    char status = STATUS_ERROR;
    Crc32 c;
    c.update(&status, 1);
    c.update(message.c_str(), message.size());
    char trailer[CRC_SIZE];
    putU32(c.value(), trailer);
    return sendAll(sock, &status, 1) && sendAll(sock, message.c_str(), message.size()) &&
        sendAll(sock, trailer, CRC_SIZE);
}
inline void setSocketTimeout(int sock, int seconds)
{
    timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}
inline int createListenSocket(int port)
{
     int s=socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s<0)
    {
        perror("socket");
        exit(1);
    }
    int yes=1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)); 
    sockaddr_in addr;
    memset(&addr,0,sizeof(addr));
    addr.sin_family=AF_INET;
    addr.sin_addr.s_addr=htonl(INADDR_ANY);
    addr.sin_port=htons((uint16_t)port);
    if (bind(s,(sockaddr*)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        exit(1);
    }
    if (listen(s, LISTEN_BACKLOG)<0)
    {
        perror("listen");
        exit(1);
    }
    return s;
}
inline int connectSocket(const char* host, int port)
{
    int s = socket(AF_INET,SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0)
        return -1;
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1||connect(s, (sockaddr*)&addr, sizeof(addr)) < 0)
    {
        close(s);
        return -1;
    }
    return s;
}