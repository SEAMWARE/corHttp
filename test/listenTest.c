//
// FILE            listenTest.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Where and how a server listens: corHttpInit / corHttpInitOptions.
//
//   1. port-in-use     a second corHttpInit on a port a first one holds fails (EADDRINUSE)
//   2. reuse-port      two corHttpInitOptions with reusePort on one port both succeed
//   3. reuse-one-side  reusePort on the second server only: it fails
//   4. bind-default    corHttpInit listens on 0.0.0.0
//   5. bind-loopback   bindAddress "127.0.0.1": the listener is on 127.0.0.1
//   6. bind-ipv6       bindAddress "::1": the listener is on ::1 (skipped when the host has no IPv6)
//   7. bind-invalid    bindAddress "localhost" (a name, not an address): corHttpInitOptions fails
//   8. no-listener     a group of two loops, the second with noListener: corHttpAcceptShare succeeds,
//                      the second has no listen socket, and a third server on the port fails
//
// Exit code 0: all pass.
//
#include <stdio.h>                               // printf
#include <string.h>                              // strcmp
#include <errno.h>                               // errno, EADDRINUSE
#include <arpa/inet.h>                           // inet_ntop
#include <netinet/in.h>                          // sockaddr_in, sockaddr_in6
#include <sys/socket.h>                          // getsockname, socket
#include <unistd.h>                              // close

#include "corHttp/CorHttp.h"                     // corHttpInit, corHttpInitOptions, corHttpAcceptShare, corHttpRelease



static int failures = 0;



// -----------------------------------------------------------------------------
//
// request - never called: nothing connects
//
static void request(CorHttpConn* connP)
{
  (void) connP;
}



// -----------------------------------------------------------------------------
//
// verdict -
//
static void verdict(const char* name, int ok, const char* detail)
{
  printf("%-15s %s (%s)\n", name, ok ? "OK" : "FAILED", detail);

  if (!ok)
    ++failures;
}



// -----------------------------------------------------------------------------
//
// listenAddress - the address and port the server's listener is bound to
//
static unsigned short listenAddress(CorHttpServer* serverP, char* address, int size)
{
  struct sockaddr_storage addr;
  socklen_t               len = sizeof(addr);

  getsockname(serverP->listenFd, (struct sockaddr*) &addr, &len);

  if (addr.ss_family == AF_INET6)
  {
    inet_ntop(AF_INET6, &((struct sockaddr_in6*) &addr)->sin6_addr, address, size);
    return ntohs(((struct sockaddr_in6*) &addr)->sin6_port);
  }

  inet_ntop(AF_INET, &((struct sockaddr_in*) &addr)->sin_addr, address, size);
  return ntohs(((struct sockaddr_in*) &addr)->sin_port);
}



// -----------------------------------------------------------------------------
//
// main -
//
int main(void)
{
  CorHttpServer        first;
  CorHttpServer        second;
  CorHttpListenOptions reuse = { NULL, true };
  char                 address[64];
  char                 detail[128];

  // 1. port-in-use - the port: an ephemeral one, picked by the kernel for the first server
  corHttpInit(&first, 0, 16, request);
  unsigned short port = listenAddress(&first, address, sizeof(address));

  errno = 0;
  CorHttpStatus s = corHttpInit(&second, port, 16, request);
  snprintf(detail, sizeof(detail), "second corHttpInit on port %d: %s, errno %d", port, (s == CorHttpOk) ? "Ok" : "Error", errno);
  verdict("port-in-use", (s != CorHttpOk) && (errno == EADDRINUSE), detail);
  if (s == CorHttpOk)
    corHttpRelease(&second);

  // 4. bind-default
  verdict("bind-default", strcmp(address, "0.0.0.0") == 0, address);
  corHttpRelease(&first);

  // 2. reuse-port
  corHttpInitOptions(&first, 0, &reuse, 16, request);
  port = listenAddress(&first, address, sizeof(address));
  s    = corHttpInitOptions(&second, port, &reuse, 16, request);
  snprintf(detail, sizeof(detail), "second corHttpInitOptions(reusePort) on port %d: %s", port, (s == CorHttpOk) ? "Ok" : "Error");
  verdict("reuse-port", s == CorHttpOk, detail);
  if (s == CorHttpOk)
    corHttpRelease(&second);
  corHttpRelease(&first);

  // 3. reuse-one-side
  corHttpInit(&first, 0, 16, request);
  port = listenAddress(&first, address, sizeof(address));
  s    = corHttpInitOptions(&second, port, &reuse, 16, request);
  snprintf(detail, sizeof(detail), "reusePort on the second server only, port %d: %s", port, (s == CorHttpOk) ? "Ok" : "Error");
  verdict("reuse-one-side", s != CorHttpOk, detail);
  if (s == CorHttpOk)
    corHttpRelease(&second);
  corHttpRelease(&first);

  // 5. bind-loopback
  CorHttpListenOptions loopback = { "127.0.0.1", false };
  s = corHttpInitOptions(&first, 0, &loopback, 16, request);
  if (s == CorHttpOk)
  {
    listenAddress(&first, address, sizeof(address));
    corHttpRelease(&first);
  }
  else
    snprintf(address, sizeof(address), "corHttpInitOptions failed");
  verdict("bind-loopback", (s == CorHttpOk) && (strcmp(address, "127.0.0.1") == 0), address);

  // 6. bind-ipv6
  int v6 = socket(AF_INET6, SOCK_STREAM, 0);
  if (v6 < 0)
    printf("%-15s skipped (no IPv6)\n", "bind-ipv6");
  else
  {
    close(v6);

    CorHttpListenOptions ipv6 = { "::1", false };
    s = corHttpInitOptions(&first, 0, &ipv6, 16, request);
    if (s == CorHttpOk)
    {
      listenAddress(&first, address, sizeof(address));
      corHttpRelease(&first);
    }
    else
      snprintf(address, sizeof(address), "corHttpInitOptions failed, errno %d", errno);
    verdict("bind-ipv6", (s == CorHttpOk) && (strcmp(address, "::1") == 0), address);
  }

  // 7. bind-invalid
  CorHttpListenOptions name = { "localhost", false };
  s = corHttpInitOptions(&first, 0, &name, 16, request);
  verdict("bind-invalid", s != CorHttpOk, (s == CorHttpOk) ? "accepted" : "refused");
  if (s == CorHttpOk)
    corHttpRelease(&first);

  // 8. no-listener
  CorHttpServer        groupV[2];
  CorHttpListenOptions handed = { NULL, false, true };
  CorHttpServer        third;

  corHttpInit(&groupV[0], 0, 16, request);
  port = listenAddress(&groupV[0], address, sizeof(address));
  s    = corHttpInitOptions(&groupV[1], port, &handed, 16, request);

  CorHttpStatus share = corHttpAcceptShare(groupV, 2);

  errno = 0;
  CorHttpStatus t = corHttpInit(&third, port, 16, request);
  snprintf(detail, sizeof(detail), "init %s, share %s, second listenFd %d, third server %s errno %d",
           (s == CorHttpOk) ? "Ok" : "Error", (share == CorHttpOk) ? "Ok" : "Error", groupV[1].listenFd,
           (t == CorHttpOk) ? "Ok" : "Error", errno);
  verdict("no-listener", (s == CorHttpOk) && (share == CorHttpOk) && (groupV[1].listenFd == -1) && (t != CorHttpOk) && (errno == EADDRINUSE), detail);
  if (t == CorHttpOk)
    corHttpRelease(&third);
  corHttpRelease(&groupV[1]);
  corHttpRelease(&groupV[0]);

  printf("%s\n", (failures == 0) ? "PASS" : "FAIL");
  return (failures == 0) ? 0 : 1;
}
