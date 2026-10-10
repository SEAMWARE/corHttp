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
#include <pthread.h>                             // pthread_mutex_t
#include <stdbool.h>                             // bool
#include <stdint.h>                              // uint64_t

#include "corAlloc/CorAlloc.h"                   // CorAlloc



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
  bool                 bodyRefused;              // Content-Length over the cap: headers delivered, body never read

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

  CorAlloc             alloc;
  char                 allocBuf[8 * 1024];

  //
  // Where the request ends in buf: the bytes after it (bufUsed - requestEnd) arrived behind it - for an
  // upgrade, the first bytes of the new protocol. Set by the parser.
  //
  int                  requestEnd;

  //
  // An upgrade the callback accepted (corHttpUpgrade): who gets the socket once the 101 is written
  //
  void               (*upgradeCb)(int fd, char* extra, int extraLen, void* cls);
  void*                upgradeCls;

  bool                 expectContinue;           // client sent Expect: 100-continue and is waiting
  bool                 continueSent;             // ... and we have already answered it
  bool                 keepAlive;
  bool                 inCallback;               // internal: inside requestCb - see corHttpResumeHere
  int                  requests;                 // served on this connection
  uint64_t             lastActivity;             // CLOCK_MONOTONIC ms, for the idle sweep

  void*                userData;                 // the caller's, untouched here

  //
  // The loop this connection belongs to. corHttpResume runs on a WORKER thread
  // and has to reach the right loop's resume queue and eventfd; the connection
  // is the only thing that worker holds, so this is the path to it. With one
  // loop a file-static would have done - which is what it used to be.
  //
  struct CorHttpServer* serverP;
  struct CorHttpConn*  next;                     // free-list linkage

  //
  // A body that is not in memory - a file (corHttpResponseFile) or one written over time
  // (corHttpResponseStream). Internal; see corHttpStream.c.
  //
  int                  fileFd;                   // -1: no file
  int64_t              fileOffset;
  int64_t              fileLeft;
  struct CorHttpStream* streamP;                 // NULL: no stream
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
// CorHttpDoneCb - the request is over and its bytes are on the wire
//
// Optional, and it exists for the one thing the request callback cannot do: a
// caller that hung per-request state on connP->userData has to free it, and the
// only safe moment is after the response has been WRITTEN - the response
// headers and body are borrowed from that state, not copied.
//
// Called exactly once per request that reached the request callback, on the
// loop thread: after the last byte goes out, or when the connection dies with a
// request still on it. Never for a request the engine answered by itself (a
// parse error, a 413), because those never reached the caller and there is
// nothing of the caller's to free.
//
typedef void (*CorHttpDoneCb)(CorHttpConn* connP);



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
  CorHttpDoneCb        doneCb;                   // optional; see CorHttpDoneCb

  int                  keepAliveTimeout;         // seconds; 0 disables keep-alive
  //
  // maxRequestSize - the largest request this server will hold, in bytes
  //
  // Enforced at the ANNOUNCEMENT where it can be: a Content-Length over this is
  // refused before a byte of the body is read, and the request reaches the
  // callback with bodyRefused set and no body, so the caller answers it in its
  // own words rather than being handed a bare status by the engine. Only a
  // client that lies about its length gets as far as the buffer limit.
  //
  int                  maxRequestSize;           // bytes; 0 = no cap

  //
  // The resume queue: responses a worker has finished, waiting for THIS loop to
  // write them out. Per server rather than per file, because the socket writes
  // are the loop's alone (see corHttpResume) and a second loop must not be
  // handed the first one's connections.
  //
  // resumeFd is an eventfd registered in this server's epoll with data.ptr set
  // to the server itself, which is how the loop tells it apart from a socket.
  //
  pthread_mutex_t      resumeMutex;
  CorHttpConn*         resumeHead;
  int                  resumeFd;

  //
  // Shared accepting (corHttpAcceptShare): ONE loop of a group accepts and deals the connections out,
  // in turn, to every loop of the group - itself included. A connection dealt to another loop goes
  // through that loop's hand queue (handV) and its own eventfd (handFd, data.ptr = &handFd): the
  // socket is then registered by the loop that owns it, as one accepted there would be.
  //
  struct CorHttpServer* acceptV;                 // the group, on the accepting loop only; NULL: none
  int                  acceptN;
  int                  acceptNext;

  pthread_mutex_t      handMutex;
  int*                 handV;
  int                  handCount;
  int                  handSize;
  int                  handFd;

  bool                 running;

  //
  // The stream queue: streams with bytes (or their end) waiting for THIS loop to write them - the
  // writers' side of corHttpStreamWrite / corHttpStreamEnd. Under resumeMutex, woken through resumeFd.
  //
  struct CorHttpStream* streamHead;
} CorHttpServer;



// -----------------------------------------------------------------------------
//
// CorHttpListenOptions - where and how a server listens (corHttpInitOptions)
//
// bindAddress  NULL or "": every IPv4 interface (0.0.0.0), what corHttpInit does. Else ONE numeric
//              IPv4 or IPv6 address ("127.0.0.1", "::1", "::") - not a host name.
// reusePort    SO_REUSEPORT on the listen socket: more than one server may hold the port - every one
//              of them must ask for it - and the kernel hashes the connections between them. It lets
//              a second process of the same user hold the port as well. Without it a second server on
//              a port in use fails (CorHttpError, errno EADDRINUSE). corHttpInit: false.
// noListener   no listen socket at all (port and the other options unused): a loop of a group that is
//              handed its connections by the group's accepting loop (corHttpAcceptShare). The loops
//              of a group then need no SO_REUSEPORT. corHttpInit: false.
//
typedef struct CorHttpListenOptions
{
  const char*          bindAddress;
  bool                 reusePort;
  bool                 noListener;
} CorHttpListenOptions;



// -----------------------------------------------------------------------------
//
// Server lifecycle
//
// corHttpInit is corHttpInitOptions with no options: every IPv4 interface, no SO_REUSEPORT.
//
extern CorHttpStatus  corHttpInit(CorHttpServer* serverP, unsigned short port, int connPoolSize, CorHttpRequestCb cb);
extern CorHttpStatus  corHttpInitOptions(CorHttpServer* serverP, unsigned short port, const CorHttpListenOptions* optionsP, int connPoolSize, CorHttpRequestCb cb);
extern CorHttpStatus  corHttpServe(CorHttpServer* serverP);   // runs until corHttpStop
extern void           corHttpStop(CorHttpServer* serverP);
extern void           corHttpRelease(CorHttpServer* serverP);

//
// corHttpServeEnd - after corHttpServe has returned, on its thread: every connection closed but the
// suspended ones (corHttpSuspend - a request the application still holds). A connection with a request
// on it - a response not yet all written - gets its doneCb first, as on any close, so the application
// frees the request. Call it when the application holds no request any more, before corHttpRelease.
//
extern void           corHttpServeEnd(CorHttpServer* serverP);

//
// corHttpAcceptShare - the n loops of serverV on one port: serverV[0] accepts every connection and deals
// them out in turn, instead of the kernel hashing each to a loop (SO_REUSEPORT), which splits a handful
// of connections unevenly - 10 and 6 of 16 - and the busier loop queues. After corHttpInit of serverV[0]
// and corHttpInitOptions of the others with noListener (or of all n with reusePort), before
// corHttpServe.
//
extern CorHttpStatus  corHttpAcceptShare(CorHttpServer* serverV, int n);



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

//
// corHttpResumeHere - corHttpResume for code that runs ON the loop's thread (a coroutine of the loop):
// the response goes out at once, no queue, no eventfd
//
extern void           corHttpResumeHere(CorHttpConn* connP);



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



// -----------------------------------------------------------------------------
//
// corHttpUpgrade - the request switches this connection to another protocol (WebSocket, ...)
//
// Called by the callback that answers an upgrade request, after it set the 101 and the headers the
// protocol's handshake needs (corHttpResponseStatus(connP, 101), "Upgrade", "Connection: Upgrade", ...).
// Once the 101 is written, the connection leaves this server: its socket is taken out of the loop, the
// connection goes back to the pool WITHOUT closing it, and 'cb' is called on the loop's thread with the
// socket and whatever the client sent behind the request (the new protocol's first bytes - valid only
// during the call: copy them). From then on the socket is the caller's, to read, write and close.
// The libmicrohttpd counterpart is MHD_create_response_for_upgrade - the same contract.
//
typedef void (*CorHttpUpgradeCb)(int fd, char* extra, int extraLen, void* cls);

extern void           corHttpUpgrade(CorHttpConn* connP, CorHttpUpgradeCb cb, void* cls);



// -----------------------------------------------------------------------------
//
// corHttpResponseFile - the body is a file, sent from the file without being read into memory
//
// 'length' bytes of 'fd' from 'offset' (a Range answer sets its 206 and Content-Range itself), sent with
// sendfile as the socket takes them; Content-Length is 'length'. The fd is the library's from here on:
// closed when the bytes are out, or when the connection dies first. A HEAD gets the headers only.
//
extern void           corHttpResponseFile(CorHttpConn* connP, int fd, int64_t offset, int64_t length);



// -----------------------------------------------------------------------------
//
// corHttpResponseStream - the body is written over time, after the callback has returned
//
// Called by the callback (or by the thread a suspended request was handed to, before corHttpResume),
// after the status and the headers. The headers go out with `Transfer-Encoding: chunked` in place of
// Content-Length (an HTTP/1.0 client: no framing, and the connection closes at the end), and the
// connection stays open for what the stream is given:
//
//   corHttpStreamWrite(streamP, data, len)   from ANY thread: the bytes are copied and queued, and the
//                                            loop writes them as one chunk. false: the client is gone
//                                            (or the stream is over) - nothing more will reach it.
//   corHttpStreamEnd(streamP)                from any thread, exactly once - also after a write said
//                                            false: the last chunk, and the handle is released. Not
//                                            used after this.
//
// The writer never waits for the socket: bytes the client has not taken yet pile up in the stream, up
// to COR_HTTP_STREAM_PENDING_MAX - a client that falls that far behind is disconnected, and the writes
// say false. A stream is not closed for being quiet (the idle sweep skips it): a writer with nothing
// to say for a long time sends something small now and then, or the client cannot tell a stream that
// is quiet from one that is dead. Server-sent events are this, with text/event-stream.
//
#define COR_HTTP_STREAM_PENDING_MAX  (16 * 1024 * 1024)

typedef struct CorHttpStream CorHttpStream;

extern CorHttpStream* corHttpResponseStream(CorHttpConn* connP);
extern bool           corHttpStreamWrite(CorHttpStream* streamP, const char* data, int len);
extern void           corHttpStreamEnd(CorHttpStream* streamP);

#endif  // CORHTTP_CORHTTP_H_
