/* Minimal <sys/un.h> for the 3DS build: crawl's webtiles framework names
 * sockaddr_un and a few socket constants, but the 3DS port never opens a
 * socket (the frontend reads the tile data directly). libctru has no
 * <sys/un.h> and lacks some of the constants; this is added with
 * -idirafter, so real system headers always win. */
#pragma once
#include <sys/socket.h>
#ifndef AF_UNIX
#define AF_UNIX 1
#endif
#ifndef PF_UNIX
#define PF_UNIX AF_UNIX
#endif
#ifndef SOCK_DGRAM
#define SOCK_DGRAM 2
#endif
#ifndef SOL_SOCKET
#define SOL_SOCKET 0xFFFF
#endif
#ifndef SO_SNDBUF
#define SO_SNDBUF 0x1001
#endif
#ifndef SO_SNDTIMEO
#define SO_SNDTIMEO 0x1005
#endif
struct sockaddr_un
{
    sa_family_t sun_family;
    char sun_path[108];
};
