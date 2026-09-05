#ifndef CORHTTP_CORHTTP_H_
#define CORHTTP_CORHTTP_H_

//
// FILE            CorHttp.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// corHttp - an HTTP/1.1 server, and nothing else.
//
// It accepts connections, parses requests, and writes responses. It does not
// know what NGSI-LD is, it does not route, and it does not parse JSON: a
// request arrives at one callback with its method, path, query, headers and
// body, and the caller decides everything from there. Routing belongs to the
// layer that owns the service table, and putting it here would mean two of
// them.
//
// ZERO COPY is the property to preserve when changing anything below. The
// method, path, query, header keys and values and the body are all POINTERS
// INTO THE CONNECTION'S READ BUFFER, NUL-terminated in place by the parser.
// Nothing is duplicated, and nothing survives the callback returning - a caller
// that needs a value afterwards has to copy it.
//
#include <stdbool.h>                             // bool
#include <stdint.h>                              // uint64_t

#include "kalloc/KAlloc.h"                       // KAlloc



// -----------------------------------------------------------------------------
//
// Limits
//
// Fixed arrays rather than growth: a request that needs more than this is not a
// request this broker wants to serve, and the alternative is an allocation on
// the hot path for a case that does not occur. Exceeding them is a 431/413, not
// a realloc.
//
#define COR_HTTP_MAX_HEADERS        64
#define COR_HTTP_MAX_URI_PARAMS     32
#define COR_HTTP_INITIAL_BUF_SIZE   8192
#define COR_HTTP_CONN_POOL_SIZE     1024
#define COR_HTTP_KEEPALIVE_TIMEOUT  30           // seconds



// -----------------------------------------------------------------------------
//
// CorHttpStatus - what an internal operation did
//
// Not HTTP status codes. CorHttpAgain is the one that carries the design: a
// partially-arrived request is the normal case on a non-blocking socket, not an
// error, and the parser says so rather than blocking or failing.
//
typedef enum CorHttpStatus
{
  CorHttpOk = 0,
  CorHttpAgain,                                  // incomplete - read more and re-parse
  CorHttpError,
  CorHttpClosed,
  CorHttpParseError,
  CorHttpTooLarge,
  CorHttpOutOfMemory
} CorHttpStatus;



// -----------------------------------------------------------------------------
//
// CorHttpSlice - a piece of the read buffer
//
// Both a pointer and a length, and the pointer is NUL-terminated too. The
// length is there so a caller can compare without strlen; the terminator is
// there so a caller can pass it straight to something that expects a C string.
//
typedef struct CorHttpSlice
{
  char*  s;
  int    len;
} CorHttpSlice;



// -----------------------------------------------------------------------------
//
// CorHttpKeyValue - a header or a URI parameter
//
typedef struct CorHttpKeyValue
{
  CorHttpSlice  key;
  CorHttpSlice  value;
} CorHttpKeyValue;



// -----------------------------------------------------------------------------
//
// CorHttpConn - one connection, and the request currently on it
//
// Pre-allocated in a pool and REUSED, so everything here is reset per request
// rather than freed. 'alloc' is a per-request pool with an inline buffer: it is
// bulk-reset when the response goes out, which is why nothing in the request
// path has to remember what it allocated.
//
typedef struct CorHttpConn
{
  int                  fd;
  int                  state;                    // internal; see corHttpInternal.h

  // Read buffer. Grows on demand up to the server's maxRequestSize.
  char*                buf;
  int                  bufSize;
  int                  bufUsed;

  // The parsed request - all of it pointing into buf.
  CorHttpSlice         method;
  CorHttpSlice         path;
  CorHttpSlice         query;
  CorHttpSlice         version;
  CorHttpSlice         body;

  CorHttpKeyValue      header[COR_HTTP_MAX_HEADERS];
  int                  headers;
  bool                 headersTruncated;         // more than COR_HTTP_MAX_HEADERS arrived

  CorHttpKeyValue      uriParam[COR_HTTP_MAX_URI_PARAMS];
  int                  uriParams;
  bool                 uriParamsTruncated;

  int                  contentLength;            // -1 when the header was absent

  // The response the callback fills in, via corHttpResponse*().
  int                  statusCode;
  CorHttpKeyValue      respHeader[COR_HTTP_MAX_HEADERS];
  int                  respHeaders;
  char*                respBody;
  int                  respBodyLen;

  // Outgoing bytes, for the partial writes a non-blocking socket will hand us.
  char*                writeBuf;
  int                  writeLen;
  int                  writePos;

  KAlloc               alloc;
  char                 allocBuf[8 * 1024];

  bool                 expectContinue;           // client sent Expect: 100-continue and is waiting
  bool                 continueSent;             // ... and we have already answered it
  bool                 keepAlive;
  int                  requests;                 // served on this connection
  uint64_t             lastActivity;             // CLOCK_MONOTONIC ms, for the idle sweep

  void*                userData;                 // the caller's, untouched here

  struct CorHttpConn*  next;                     // free-list linkage
} CorHttpConn;



// -----------------------------------------------------------------------------
//
// CorHttpRequestCb - the one callback
//
// Called once per complete request, on the thread that runs corHttpServe(). The
// callback fills the response through corHttpResponse*() and returns; returning
// without setting a status is a 500, because a request that produced no answer
// is a bug in the caller and not something to hide behind an empty 200.
//
typedef void (*CorHttpRequestCb)(CorHttpConn* connP);



// -----------------------------------------------------------------------------
//
// CorHttpServer
//
typedef struct CorHttpServer
{
  int                  listenFd;
  int                  epollFd;
  unsigned short       port;

  CorHttpConn*         connPool;                 // the whole pool, one allocation
  CorHttpConn*         freeConns;                // free list head
  int                  connPoolSize;
  int                  activeConns;

  CorHttpRequestCb     requestCb;

  int                  keepAliveTimeout;         // seconds; 0 disables keep-alive
  int                  maxRequestSize;           // bytes; 0 = no cap

  bool                 running;
} CorHttpServer;



// -----------------------------------------------------------------------------
//
// Server lifecycle
//
extern CorHttpStatus  corHttpInit(CorHttpServer* serverP, unsigned short port, int connPoolSize, CorHttpRequestCb cb);
extern CorHttpStatus  corHttpServe(CorHttpServer* serverP);   // runs until corHttpStop
extern void           corHttpStop(CorHttpServer* serverP);
extern void           corHttpRelease(CorHttpServer* serverP);



// -----------------------------------------------------------------------------
//
// Request accessors - convenience over the arrays above, not a second source
//
extern const char*    corHttpHeader(CorHttpConn* connP, const char* key);      // case-insensitive
extern const char*    corHttpUriParam(CorHttpConn* connP, const char* key);



// -----------------------------------------------------------------------------
//
// corHttpSuspend / corHttpResume - hand a request to another thread and back
//
// The pair that lets the caller answer off this library's event-loop thread,
// which is what a broker that does I/O of its own during a request needs: a
// database round-trip or a forward to another context source cannot run on the
// loop without stopping every other connection for its duration.
//
// corHttpSuspend is called from INSIDE the callback and means "I am not
// answering now". The connection leaves the loop's care entirely - no epoll
// interest, no idle timeout, not reused - until it comes back.
//
// corHttpResume is the ONLY function here that may be called from another
// thread. It queues the connection and pokes the loop; it does not write to the
// socket, because the socket belongs to the loop and two threads writing one
// response is how two answers end up interleaved on one connection.
//
extern void           corHttpSuspend(CorHttpConn* connP);
extern void           corHttpResume(CorHttpConn* connP);



// -----------------------------------------------------------------------------
//
// Response
//
// corHttpResponseHeader and corHttpResponseBody do NOT copy: what is passed
// must outlive the callback, and the natural way to guarantee that is to
// allocate it from connP->alloc, which lives exactly that long.
//
extern void           corHttpResponseStatus(CorHttpConn* connP, int statusCode);
extern void           corHttpResponseHeader(CorHttpConn* connP, const char* key, const char* value);
extern void           corHttpResponseBody(CorHttpConn* connP, char* body, int bodyLen);

#endif  // CORHTTP_CORHTTP_H_
