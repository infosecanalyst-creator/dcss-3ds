/* Minimal <sys/un.h> for the 3DS build: crawl's webtiles framework names
 * sockaddr_un, but the 3DS port never opens a socket (the frontend reads
 * the tile data directly). Used only if libctru has no <sys/un.h>
 * (added with -idirafter, so a system header wins). */
#pragma once
#include <sys/socket.h>
#ifndef AF_UNIX
#define AF_UNIX 1
#endif
struct sockaddr_un
{
    sa_family_t sun_family;
    char sun_path[108];
};
