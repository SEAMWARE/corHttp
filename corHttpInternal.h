#ifndef CORHTTP_CORHTTPINTERNAL_H_
#define CORHTTP_CORHTTPINTERNAL_H_

//
// FILE            corHttpInternal.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Internal to corHttp. Nothing outside the library includes this.
//
#include "corHttp/CorHttp.h"                     // CorHttpConn, CorHttpServer, CorHttpStatus



// -----------------------------------------------------------------------------
//
// Connection state
//
// Three states and not more: a connection is reading a request, writing the
// response, or idle between the two waiting for the next one on a keep-alive.
// Everything else that could be a state - "parsing", "handling" - happens
// inside one epoll wakeup and never outlives it, so it would be a state the
// event loop could never observe.
//
#define COR_HTTP_CONN_FREE     0
#define COR_HTTP_CONN_READING  1
#define COR_HTTP_CONN_WRITING  2
#define COR_HTTP_CONN_IDLE     3

#define COR_HTTP_MAX_EVENTS    256



// corHttpConnPoolInit  - allocate the pool and thread the free list
extern CorHttpStatus  corHttpConnPoolInit(CorHttpServer* serverP, int size);
extern void           corHttpConnPoolRelease(CorHttpServer* serverP);

// corHttpConnGet/Put   - take one from the free list / give it back (and close the fd)
extern CorHttpConn*   corHttpConnGet(CorHttpServer* serverP, int fd);
extern void           corHttpConnPut(CorHttpServer* serverP, CorHttpConn* connP);

// corHttpConnReset     - between two requests on the SAME connection
extern void           corHttpConnReset(CorHttpConn* connP);

// corHttpConnBufGrow   - make room for at least 'need' more bytes
extern CorHttpStatus  corHttpConnBufGrow(CorHttpServer* serverP, CorHttpConn* connP, int need);

// corHttpParse         - CorHttpAgain until the whole request has arrived
extern CorHttpStatus  corHttpParse(CorHttpConn* connP);

// corHttpResponseRender - build the wire bytes into connP->writeBuf
extern CorHttpStatus  corHttpResponseRender(CorHttpConn* connP);

// corHttpContinueSend  - the interim "100 Continue", ahead of the real response
extern CorHttpStatus  corHttpContinueSend(CorHttpConn* connP);

extern uint64_t       corHttpNowMs(void);



// -----------------------------------------------------------------------------
//
// CorHttpStream - a body written over time (corHttpResponseStream)
//
// Held by up to three: the writer (until corHttpStreamEnd), the connection (until the stream is over or
// the connection dies) and the loop's stream queue (while it is in it). 'refs' counts them and the last
// one out frees it - so a writer on another thread never holds a pointer to a connection, only to this,
// and a connection reused for the next client cannot be written to by the previous one's writer.
//
// Under 'mutex': the pending bytes, ended, gone, overflow, queued, refs. The rest is the loop's alone.
//
typedef struct CorHttpStream
{
  pthread_mutex_t        mutex;
  int                    refs;

  char*                  buf;                    // bytes written, not yet taken by the loop
  int                    len;
  int                    size;

  bool                   ended;                  // the writer called corHttpStreamEnd
  bool                   gone;                   // the connection is over: writes are refused
  bool                   overflow;               // the client fell COR_HTTP_STREAM_PENDING_MAX behind
  bool                   queued;                 // in the server's stream queue

  // The loop's
  struct CorHttpServer*  serverP;
  CorHttpConn*           connP;                  // NULL once the connection is done with it
  bool                   started;                // its headers are rendered: the loop may write its body
  bool                   chunked;                // HTTP/1.1: chunk framing; HTTP/1.0: raw, close at the end
  bool                   lastQueued;             // the terminating chunk is in the write buffer

  struct CorHttpStream*  next;                   // stream queue linkage
} CorHttpStream;

// corHttpBodyBusy      - a file or a stream body after its headers: the loop is writing it
extern bool           corHttpBodyBusy(CorHttpConn* connP);

// corHttpBodyRelease   - the connection is done with its file / stream (finished, reset or closed)
extern void           corHttpBodyRelease(CorHttpConn* connP);

// corHttpStreamTake    - the pending bytes (malloc'ed, the caller frees), and whether the writer ended
extern char*          corHttpStreamTake(CorHttpStream* streamP, int* lenP, bool* endedP, bool* overflowP);

// corHttpStreamUnref   - one holder less; the last one frees it
extern void           corHttpStreamUnref(CorHttpStream* streamP);

#endif  // CORHTTP_CORHTTPINTERNAL_H_
