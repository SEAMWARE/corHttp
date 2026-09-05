//
// FILE            corHttpConn.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The connection pool.
//
// Every connection is allocated ONCE, at startup, and reused for the life of
// the process - accepting a connection takes one off a free list and closing it
// puts it back. A server that malloc'd a connection per accept would spend its
// day in the allocator under exactly the load it is meant to survive, and would
// fragment while doing it.
//
// The read buffer is part of that: it starts at COR_HTTP_INITIAL_BUF_SIZE and
// GROWS if a request needs it, but it is never shrunk back. A connection that
// once carried a large entity keeps the room for the next one, and the pool
// converges on the working set instead of oscillating around it.
//
#include <stdlib.h>                              // malloc, free, realloc
#include <string.h>                              // memset
#include <time.h>                                // clock_gettime
#include <unistd.h>                              // close

#include "kalloc/kaBufferInit.h"                 // kaBufferInit
#include "kalloc/kaBufferReset.h"                // kaBufferReset

#include "corHttp/CorHttp.h"                     // CorHttpConn, CorHttpServer
#include "corHttp/corHttpInternal.h"             // Own interface



// -----------------------------------------------------------------------------
//
// corHttpNowMs - CLOCK_MONOTONIC milliseconds
//
// Monotonic and not realtime: the idle sweep measures an INTERVAL, and a
// realtime clock that steps backwards over an NTP correction would make every
// open connection look freshly active, or all of them look expired at once.
//
uint64_t corHttpNowMs(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);

  return ((uint64_t) ts.tv_sec * 1000) + ((uint64_t) ts.tv_nsec / 1000000);
}



// -----------------------------------------------------------------------------
//
// corHttpConnPoolInit -
//
CorHttpStatus corHttpConnPoolInit(CorHttpServer* serverP, int size)
{
  serverP->connPool = (CorHttpConn*) calloc(size, sizeof(CorHttpConn));

  if (serverP->connPool == NULL)
    return CorHttpOutOfMemory;

  serverP->connPoolSize = size;
  serverP->freeConns    = NULL;

  //
  // Threaded back to front so the free list comes out in ascending order. Not
  // required for correctness - it makes a pool dump readable, and a connection
  // index that means something is worth a loop that counts downwards.
  //
  for (int ix = size - 1; ix >= 0; ix--)
  {
    CorHttpConn* connP = &serverP->connPool[ix];

    connP->buf = (char*) malloc(COR_HTTP_INITIAL_BUF_SIZE);
    if (connP->buf == NULL)
    {
      corHttpConnPoolRelease(serverP);
      return CorHttpOutOfMemory;
    }

    connP->bufSize = COR_HTTP_INITIAL_BUF_SIZE;
    connP->fd      = -1;
    connP->state   = COR_HTTP_CONN_FREE;

    connP->next        = serverP->freeConns;
    serverP->freeConns = connP;
  }

  return CorHttpOk;
}



// -----------------------------------------------------------------------------
//
// corHttpConnPoolRelease -
//
void corHttpConnPoolRelease(CorHttpServer* serverP)
{
  if (serverP->connPool == NULL)
    return;

  for (int ix = 0; ix < serverP->connPoolSize; ix++)
  {
    CorHttpConn* connP = &serverP->connPool[ix];

    if (connP->buf != NULL)
    {
      free(connP->buf);
      connP->buf = NULL;
    }

    if (connP->writeBuf != NULL)
    {
      free(connP->writeBuf);
      connP->writeBuf = NULL;
    }

    if (connP->fd != -1)
    {
      close(connP->fd);
      connP->fd = -1;
    }
  }

  free(serverP->connPool);

  serverP->connPool     = NULL;
  serverP->freeConns    = NULL;
  serverP->connPoolSize = 0;
  serverP->activeConns  = 0;
}



// -----------------------------------------------------------------------------
//
// corHttpConnReset - ready this connection for the NEXT request on it
//
// Not the same thing as returning it to the pool: the fd stays open, the read
// buffer keeps its capacity, and only the request and response state goes.
//
// kaBufferReset takes KTRUE here, and the flag is not decoration. With KFALSE
// it frees the blocks and leaves the list pointing at them, which is teardown;
// calling it that way in a loop double-frees on the second pass. This is a
// loop, once per request, for the life of the process.
//
void corHttpConnReset(CorHttpConn* connP)
{
  connP->bufUsed            = 0;

  connP->method.s           = NULL;
  connP->method.len         = 0;
  connP->path.s             = NULL;
  connP->path.len           = 0;
  connP->query.s            = NULL;
  connP->query.len          = 0;
  connP->version.s          = NULL;
  connP->version.len        = 0;
  connP->body.s             = NULL;
  connP->body.len           = 0;

  connP->headers            = 0;
  connP->uriParams          = 0;
  connP->headersTruncated   = false;
  connP->uriParamsTruncated = false;
  connP->contentLength      = -1;
  connP->bodyRefused        = false;
  connP->expectContinue     = false;
  connP->continueSent       = false;

  connP->statusCode         = 0;
  connP->respHeaders        = 0;
  connP->respBody           = NULL;
  connP->respBodyLen        = 0;

  connP->writeLen           = 0;
  connP->writePos           = 0;

  kaBufferReset(&connP->alloc, KTRUE);
}



// -----------------------------------------------------------------------------
//
// corHttpConnGet -
//
CorHttpConn* corHttpConnGet(CorHttpServer* serverP, int fd)
{
  CorHttpConn* connP = serverP->freeConns;

  if (connP == NULL)
    return NULL;                                 // pool exhausted - the caller answers 503

  serverP->freeConns = connP->next;
  connP->next        = NULL;

  connP->fd           = fd;
  connP->serverP      = serverP;
  connP->state        = COR_HTTP_CONN_READING;
  connP->keepAlive    = true;
  connP->requests     = 0;
  connP->userData     = NULL;
  connP->lastActivity = corHttpNowMs();

  kaBufferInit(&connP->alloc, connP->allocBuf, sizeof(connP->allocBuf), 8 * 1024, NULL, "corHttp-conn");

  corHttpConnReset(connP);

  serverP->activeConns++;

  return connP;
}



// -----------------------------------------------------------------------------
//
// corHttpConnPut -
//
void corHttpConnPut(CorHttpServer* serverP, CorHttpConn* connP)
{
  if (connP->state == COR_HTTP_CONN_FREE)
    return;                                      // already returned; a double close would corrupt the free list

  if (connP->fd != -1)
  {
    close(connP->fd);
    connP->fd = -1;
  }

  //
  // KFALSE here and KTRUE in the reset above, and the asymmetry is the point:
  // this IS the teardown of that request's allocations, and the connection will
  // get a fresh kaBufferInit before it is used again.
  //
  kaBufferReset(&connP->alloc, KFALSE);

  if (connP->writeBuf != NULL)
  {
    free(connP->writeBuf);
    connP->writeBuf = NULL;
  }

  connP->writeLen = 0;
  connP->writePos = 0;
  connP->state    = COR_HTTP_CONN_FREE;

  connP->next        = serverP->freeConns;
  serverP->freeConns = connP;

  serverP->activeConns--;
}



// -----------------------------------------------------------------------------
//
// corHttpConnBufGrow - room for 'need' more bytes, plus one for the terminator
//
// Doubling rather than growing by 'need': a body arriving in 1500-byte TCP
// segments would otherwise realloc once per segment, which is O(n^2) copying
// for a large entity.
//
CorHttpStatus corHttpConnBufGrow(CorHttpServer* serverP, CorHttpConn* connP, int need)
{
  int required = connP->bufUsed + need + 1;      // +1: the body's NUL, written past bufUsed

  if (required <= connP->bufSize)
    return CorHttpOk;

  int newSize = connP->bufSize;

  while (newSize < required)
    newSize *= 2;

  if ((serverP->maxRequestSize > 0) && (newSize > serverP->maxRequestSize))
  {
    //
    // One last try at exactly the cap: a request that fits it must not be
    // refused merely because the doubling overshot.
    //
    if (required > serverP->maxRequestSize)
      return CorHttpTooLarge;

    newSize = serverP->maxRequestSize;
  }

  char* newBuf = (char*) realloc(connP->buf, newSize);

  if (newBuf == NULL)
    return CorHttpOutOfMemory;

  connP->buf     = newBuf;
  connP->bufSize = newSize;

  return CorHttpOk;
}
