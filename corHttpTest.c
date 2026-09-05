//
// FILE            corHttpTest.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// A server that answers, so the library can be exercised without a broker.
//
// Echoes back what it parsed - method, path, query, a chosen header, the number
// of URI parameters, the body length - because those are the fields a parser
// gets subtly wrong, and a test that only checked "200 OK" would pass with all
// of them empty.
//
// Build:  make corHttpTest
// Run:    ./corHttpTest [port]
//
#include <signal.h>                              // signal
#include <stdio.h>                               // printf, snprintf
#include <stdlib.h>                              // atoi

#include "kalloc/kaAlloc.h"                      // kaAlloc

#include "corHttp/CorHttp.h"                     // the library



static CorHttpServer server;



// -----------------------------------------------------------------------------
//
// onSignal -
//
static void onSignal(int sigNo)
{
  (void) sigNo;
  corHttpStop(&server);
}



// -----------------------------------------------------------------------------
//
// request -
//
static void request(CorHttpConn* connP)
{
  const char* accept = corHttpHeader(connP, "Accept");
  const char* limit  = corHttpUriParam(connP, "limit");

  //
  // From the connection's own allocator, not the stack and not malloc: the
  // response body must outlive this function (the loop writes it after we
  // return) and must not need freeing (the connection resets in bulk).
  //
  char* body = kaAlloc(&connP->alloc, 1024);

  int len = snprintf(body, 1024,
                     "{\"method\":\"%s\",\"path\":\"%s\",\"query\":\"%s\","
                     "\"headers\":%d,\"uriParams\":%d,\"accept\":\"%s\","
                     "\"limit\":\"%s\",\"bodyLen\":%d}",
                     (connP->method.s != NULL) ? connP->method.s : "",
                     (connP->path.s   != NULL) ? connP->path.s   : "",
                     (connP->query.s  != NULL) ? connP->query.s  : "",
                     connP->headers,
                     connP->uriParams,
                     (accept != NULL) ? accept : "",
                     (limit  != NULL) ? limit  : "",
                     connP->body.len);

  corHttpResponseStatus(connP, 200);
  corHttpResponseHeader(connP, "Content-Type", "application/json");
  corHttpResponseBody(connP, body, len);
}



// -----------------------------------------------------------------------------
//
// main -
//
int main(int argC, char* argV[])
{
  unsigned short port = (argC > 1) ? (unsigned short) atoi(argV[1]) : 1041;

  signal(SIGINT,  onSignal);
  signal(SIGTERM, onSignal);
  signal(SIGPIPE, SIG_IGN);                      // a peer that vanishes mid-write is not fatal

  if (corHttpInit(&server, port, 64, request) != CorHttpOk)
  {
    fprintf(stderr, "corHttpInit failed on port %d\n", port);
    return 1;
  }

  printf("corHttpTest listening on %d\n", port);
  fflush(stdout);

  corHttpServe(&server);
  corHttpRelease(&server);

  return 0;
}
