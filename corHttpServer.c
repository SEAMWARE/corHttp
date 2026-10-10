//
// FILE            corHttpServer.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The event loop.
//
// One thread, epoll, edge-triggered. Everything that can block is non-blocking
// and everything that would need a thread per connection needs a state instead.
//
// EDGE-TRIGGERED is the decision the rest of this file answers to. Level
// triggering re-reports a readable socket on every epoll_wait until it is
// drained, which is forgiving; edge triggering reports the TRANSITION once, so
// a handler that reads part of what is available and returns will not be told
// again and the connection stalls until it happens to send more. Hence: every
// accept loops until EAGAIN, every read loops until EAGAIN, and nothing here
// may return early "having done some".
//
// ONE THREAD, and that is not a limitation being apologised for. The broker
// above runs the request off this thread on its own worker pool - see
// corHttpSuspend below - so this loop only ever does I/O, and an I/O loop that
// is single-threaded needs no lock on the connection pool, no atomic on the
// free list, and cannot interleave two responses on one socket.
//
#define _GNU_SOURCE                              // accept4

#include <errno.h>                               // errno, EAGAIN, EWOULDBLOCK, EINTR
#include <fcntl.h>                               // fcntl, O_NONBLOCK
#include <arpa/inet.h>                          // inet_pton
#include <netinet/in.h>                          // sockaddr_in, sockaddr_in6, INADDR_ANY
#include <netinet/tcp.h>                         // TCP_NODELAY
#include <pthread.h>                             // pthread_mutex_*
#include <stdio.h>                               // snprintf
#include <stdlib.h>                              // realloc, free
#include <string.h>                              // memset, strerror
#include <sys/epoll.h>                           // epoll_create1, epoll_ctl, epoll_wait
#include <sys/eventfd.h>                         // eventfd
#include <sys/sendfile.h>                        // sendfile
#include <sys/socket.h>                          // socket, bind, listen, accept4, setsockopt
#include <unistd.h>                              // read, write, close

#include "corBase/corCoLoop.h"                  // corCoLoopEvent, corCoLoopExpire, corCoLoopTimeoutMs
#include "corHttp/CorHttp.h"                     // CorHttpServer, CorHttpConn
#include "corHttp/corHttpInternal.h"             // Own interface



//
// The resume queue - connections a worker thread has finished with.
//
// A worker cannot touch epoll or the connection's socket: it does not own the
// loop. So it puts the connection here and writes to the eventfd, and the loop
// wakes up and does the writing. The mutex covers the queue only, is held for
// the length of a pointer assignment, and is the single piece of cross-thread
// state in the library.
//



// -----------------------------------------------------------------------------
//
// nonBlocking -
//
static int nonBlocking(int fd)
{
  int flags = fcntl(fd, F_GETFL, 0);

  if (flags < 0)
    return -1;

  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}



// -----------------------------------------------------------------------------
//
// listener - a bound, listening, non-blocking socket
//
// On every IPv4 interface without a bind address, else on the one address given (a numeric IPv4 or
// IPv6 address - not a host name, which may resolve to more than one, or to the other family).
//
static int listener(unsigned short port, const CorHttpListenOptions* optionsP)
{
  const char*             bindAddress = (optionsP != NULL) ? optionsP->bindAddress : NULL;
  bool                    reusePort   = (optionsP != NULL) ? optionsP->reusePort   : false;
  struct sockaddr_storage addr;
  socklen_t               addrLen;

  memset(&addr, 0, sizeof(addr));

  if ((bindAddress == NULL) || (bindAddress[0] == 0))
  {
    struct sockaddr_in* in4P = (struct sockaddr_in*) &addr;

    in4P->sin_family      = AF_INET;
    in4P->sin_addr.s_addr = INADDR_ANY;
    in4P->sin_port        = htons(port);
    addrLen               = sizeof(struct sockaddr_in);
  }
  else
  {
    struct sockaddr_in*  in4P = (struct sockaddr_in*)  &addr;
    struct sockaddr_in6* in6P = (struct sockaddr_in6*) &addr;

    if (inet_pton(AF_INET, bindAddress, &in4P->sin_addr) == 1)
    {
      in4P->sin_family = AF_INET;
      in4P->sin_port   = htons(port);
      addrLen          = sizeof(struct sockaddr_in);
    }
    else if (inet_pton(AF_INET6, bindAddress, &in6P->sin6_addr) == 1)
    {
      in6P->sin6_family = AF_INET6;
      in6P->sin6_port   = htons(port);
      addrLen           = sizeof(struct sockaddr_in6);
    }
    else
    {
      errno = EINVAL;
      return -1;
    }
  }

  int fd = socket(addr.ss_family, SOCK_STREAM, 0);

  if (fd < 0)
    return -1;

  //
  // SO_REUSEADDR so a restart does not have to wait out TIME_WAIT on the
  // previous process's sockets. Without it a broker that is restarted inside
  // two minutes fails to bind, which in a test suite is every single run.
  //
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  //
  // SO_REUSEPORT only when asked for (reusePort): it is what lets MORE THAN ONE
  // server hold this port, each with its own listen socket, its own epoll and its
  // own thread - the loops of one process (corHttpAcceptShare). It lets a second
  // PROCESS of the same user hold the port as well, and the kernel then hashes the
  // connections between the two, so without it a second server on a port in use
  // fails to bind (EADDRINUSE) instead of silently getting part of the traffic.
  //
  // A failure to set it is fatal: the caller asked for a shared port and the next
  // server's bind would fail instead.
  //
  if (reusePort == true)
  {
#ifdef SO_REUSEPORT
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) < 0)
    {
      close(fd);
      return -1;
    }
#else
    close(fd);
    errno = ENOPROTOOPT;
    return -1;
#endif
  }

  if (bind(fd, (struct sockaddr*) &addr, addrLen) < 0)
  {
    close(fd);
    return -1;
  }

  //
  // SOMAXCONN rather than a number of our own: the kernel's limit is the one
  // that matters and picking a smaller one here would only cap it.
  //
  if (listen(fd, SOMAXCONN) < 0)
  {
    close(fd);
    return -1;
  }

  if (nonBlocking(fd) < 0)
  {
    close(fd);
    return -1;
  }

  return fd;
}



// -----------------------------------------------------------------------------
//
// epollSet - add or modify a connection's interest set
//
static int epollSet(CorHttpServer* serverP, CorHttpConn* connP, uint32_t events, int op)
{
  struct epoll_event ev;

  memset(&ev, 0, sizeof(ev));
  ev.events   = events | EPOLLET;
  ev.data.ptr = connP;

  return epoll_ctl(serverP->epollFd, op, connP->fd, &ev);
}



// -----------------------------------------------------------------------------
//
// requestDone - tell the caller its request is over
//
// Guarded on userData rather than on a flag of its own: userData is the ONLY
// thing the caller has hung on the connection, so "there is something to free"
// and "userData is set" are the same question. The callback clears it, which is
// also what makes this idempotent - a response that completes and a connection
// that is then closed both come through here, and the second finds nothing.
//
static void requestDone(CorHttpServer* serverP, CorHttpConn* connP)
{
  if ((serverP->doneCb != NULL) && (connP->userData != NULL))
    serverP->doneCb(connP);
}



// -----------------------------------------------------------------------------
//
// connClose -
//
static void connClose(CorHttpServer* serverP, CorHttpConn* connP)
{
  //
  // Before the connection goes back to the pool, not after: the callback may
  // still want to read the request it was given, and every slice of it points
  // into a read buffer this connection is about to hand to the next client.
  //
  requestDone(serverP, connP);

  if (connP->fd != -1)
    epoll_ctl(serverP->epollFd, EPOLL_CTL_DEL, connP->fd, NULL);

  corHttpConnPut(serverP, connP);
}



// -----------------------------------------------------------------------------
//
// connTake - an accepted socket becomes a connection of this loop
//
static void connTake(CorHttpServer* serverP, int fd)
{
  //
  // TCP_NODELAY: responses are small and complete, and Nagle would hold the
  // last partial segment waiting for more that is never coming - up to 40 ms
  // added to a response that was ready.
  //
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  CorHttpConn* connP = corHttpConnGet(serverP, fd);

  if (connP == NULL)
  {
    //
    // Pool exhausted. Closing immediately is the honest answer: accepting it
    // to hold it in a queue would trade a refused connection for a hung one,
    // and the client cannot tell the difference until it times out.
    //
    close(fd);
    return;
  }

  if (epollSet(serverP, connP, EPOLLIN, EPOLL_CTL_ADD) < 0)
    connClose(serverP, connP);
}



// -----------------------------------------------------------------------------
//
// acceptAll - drain the listen backlog
//
// Loops until EAGAIN because the listener is edge-triggered too: two
// connections arriving between wakeups produce ONE event, and an accept loop
// that took a single connection would leave the second waiting for a third to
// arrive before it was noticed.
//
static void acceptAll(CorHttpServer* serverP)
{
  while (true)
  {
    int fd = accept4(serverP->listenFd, NULL, NULL, SOCK_NONBLOCK);

    if (fd < 0)
    {
      if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
        break;
      if (errno == EINTR)
        continue;
      break;
    }

    connTake(serverP, fd);
  }
}



// -----------------------------------------------------------------------------
//
// acceptDeal - the accepting loop of a group: each new connection to the next loop in turn
//
static void acceptDeal(CorHttpServer* serverP)
{
  while (true)
  {
    int fd = accept4(serverP->listenFd, NULL, NULL, SOCK_NONBLOCK);

    if (fd < 0)
    {
      if (errno == EINTR)
        continue;
      break;                                     // EAGAIN: the backlog is drained
    }

    CorHttpServer* toP = &serverP->acceptV[serverP->acceptNext];

    serverP->acceptNext = (serverP->acceptNext + 1) % serverP->acceptN;

    if (toP == serverP)
    {
      connTake(serverP, fd);
      continue;
    }

    pthread_mutex_lock(&toP->handMutex);

    if (toP->handCount == toP->handSize)
    {
      int  size = (toP->handSize == 0) ? 64 : toP->handSize * 2;
      int* v    = realloc(toP->handV, size * sizeof(int));

      if (v == NULL)
      {
        pthread_mutex_unlock(&toP->handMutex);
        close(fd);
        continue;
      }
      toP->handV    = v;
      toP->handSize = size;
    }

    toP->handV[toP->handCount++] = fd;
    pthread_mutex_unlock(&toP->handMutex);

    uint64_t one     = 1;
    ssize_t  ignored = write(toP->handFd, &one, sizeof(one));
    (void) ignored;
  }
}



// -----------------------------------------------------------------------------
//
// handDrain - the connections the accepting loop dealt to this one
//
static void handDrain(CorHttpServer* serverP)
{
  uint64_t counter;
  ssize_t  ignored = read(serverP->handFd, &counter, sizeof(counter));
  (void) ignored;

  while (true)
  {
    int fd = -1;

    pthread_mutex_lock(&serverP->handMutex);
    if (serverP->handCount > 0)
      fd = serverP->handV[--serverP->handCount];
    pthread_mutex_unlock(&serverP->handMutex);

    if (fd < 0)
      return;

    connTake(serverP, fd);
  }
}



// -----------------------------------------------------------------------------
//
// readAll - drain the socket into the connection buffer
//
static CorHttpStatus readAll(CorHttpServer* serverP, CorHttpConn* connP)
{
  while (true)
  {
    int room = connP->bufSize - connP->bufUsed - 1;   // -1: the parser's terminator

    if (room < 1024)
    {
      CorHttpStatus s = corHttpConnBufGrow(serverP, connP, 1024);

      if (s != CorHttpOk)
        return s;

      room = connP->bufSize - connP->bufUsed - 1;
    }

    ssize_t n = read(connP->fd, connP->buf + connP->bufUsed, room);

    if (n < 0)
    {
      if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
        return CorHttpOk;                        // drained; what we have is all there is for now
      if (errno == EINTR)
        continue;
      return CorHttpError;
    }

    if (n == 0)
      return CorHttpClosed;

    connP->bufUsed += n;
  }
}



// -----------------------------------------------------------------------------
//
// writeAll - push out the rendered response, or as much as the socket takes
//
// Returns CorHttpAgain when the socket filled up. The caller then arms EPOLLOUT
// and comes back; writePos is the whole of the state that has to survive.
//
static CorHttpStatus writeAll(CorHttpConn* connP)
{
  while (connP->writePos < connP->writeLen)
  {
    ssize_t n = write(connP->fd, connP->writeBuf + connP->writePos, connP->writeLen - connP->writePos);

    if (n < 0)
    {
      if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
        return CorHttpAgain;
      if (errno == EINTR)
        continue;
      return CorHttpError;
    }

    connP->writePos += n;
  }

  return CorHttpOk;
}



// -----------------------------------------------------------------------------
//
// upgradeHand - the 101 is written: the socket leaves this server, to the caller's upgrade callback
//
// The bytes the client sent behind the request are copied out first - they live in the read buffer,
// which goes back to the pool with the connection. The connection is put back with fd -1, so it is
// not closed: the socket is the callback's now.
//
static void upgradeHand(CorHttpServer* serverP, CorHttpConn* connP)
{
  CorHttpUpgradeCb cb       = connP->upgradeCb;
  void*            cls      = connP->upgradeCls;
  int              fd       = connP->fd;
  int              extraLen = connP->bufUsed - connP->requestEnd;
  char*            extra    = NULL;

  if (extraLen > 0)
  {
    extra = (char*) malloc(extraLen);
    if (extra != NULL)
      memcpy(extra, &connP->buf[connP->requestEnd], extraLen);
    else
      extraLen = 0;
  }
  else
    extraLen = 0;

  requestDone(serverP, connP);
  epoll_ctl(serverP->epollFd, EPOLL_CTL_DEL, fd, NULL);

  connP->fd = -1;
  corHttpConnPut(serverP, connP);

  cb(fd, extra, extraLen, cls);
  free(extra);
}



// -----------------------------------------------------------------------------
//
// corHttpUpgrade -
//
void corHttpUpgrade(CorHttpConn* connP, CorHttpUpgradeCb cb, void* cls)
{
  connP->upgradeCb  = cb;
  connP->upgradeCls = cls;
}



static void responseDone(CorHttpServer* serverP, CorHttpConn* connP);



// -----------------------------------------------------------------------------
//
// chunkFrame - the next bytes of a stream in the write buffer, framed: "<hex length>\r\n<bytes>\r\n",
// and the terminating "0\r\n\r\n" behind them when the writer has ended. Raw for an HTTP/1.0 client.
//
static bool chunkFrame(CorHttpConn* connP, CorHttpStream* sP, char* data, int len, bool last)
{
  int size = len + 32;

  free(connP->writeBuf);
  connP->writeBuf = (char*) malloc(size);
  connP->writeLen = 0;
  connP->writePos = 0;

  if (connP->writeBuf == NULL)
    return false;

  char* p = connP->writeBuf;

  if (len > 0)
  {
    if (sP->chunked == true)
      p += snprintf(p, 16, "%x\r\n", len);

    memcpy(p, data, len);
    p += len;

    if (sP->chunked == true)
    {
      *p++ = '\r';
      *p++ = '\n';
    }
  }

  if ((last == true) && (sP->chunked == true))
  {
    memcpy(p, "0\r\n\r\n", 5);
    p += 5;
  }

  connP->writeLen = (int) (p - connP->writeBuf);
  return true;
}



// -----------------------------------------------------------------------------
//
// bodyPump - a file or a stream body, after its headers: as much as the socket takes
//
// true: the body is complete - the response is over, as an in-memory one is when its buffer is out.
// false: waiting (for the socket - EPOLLOUT is armed - or for the stream's writer, whose next write
// wakes the loop), or the connection was closed.
//
static bool bodyPump(CorHttpServer* serverP, CorHttpConn* connP)
{
  while (true)
  {
    CorHttpStatus s = writeAll(connP);

    if (s == CorHttpAgain)
    {
      epollSet(serverP, connP, EPOLLIN | EPOLLOUT, EPOLL_CTL_MOD);
      return false;
    }

    if (s != CorHttpOk)
    {
      connClose(serverP, connP);
      return false;
    }

    connP->lastActivity = corHttpNowMs();

    if (connP->fileFd != -1)
    {
      while (connP->fileLeft > 0)
      {
        off_t   off  = (off_t) connP->fileOffset;
        size_t  want = (connP->fileLeft > (1 << 30)) ? (1 << 30) : (size_t) connP->fileLeft;
        ssize_t n    = sendfile(connP->fd, connP->fileFd, &off, want);

        if (n < 0)
        {
          if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
          {
            epollSet(serverP, connP, EPOLLIN | EPOLLOUT, EPOLL_CTL_MOD);
            return false;
          }
          if (errno == EINTR)
            continue;

          connClose(serverP, connP);
          return false;
        }

        if (n == 0)
        {
          //
          // The file is shorter than the Content-Length already sent: nothing can make the response
          // right - the client sees the connection close before the length it was promised
          //
          connClose(serverP, connP);
          return false;
        }

        connP->fileOffset  += n;
        connP->fileLeft    -= n;
        connP->lastActivity = corHttpNowMs();
      }

      corHttpBodyRelease(connP);
      return true;
    }

    CorHttpStream* sP = connP->streamP;

    if (sP == NULL)
      return true;                               // no body of its own (HEAD), or done

    if (sP->lastQueued == true)
    {
      corHttpBodyRelease(connP);                 // the terminating chunk is out
      return true;
    }

    int   len;
    bool  ended;
    bool  overflow;
    char* data = corHttpStreamTake(sP, &len, &ended, &overflow);

    if (overflow == true)
    {
      free(data);
      connClose(serverP, connP);                 // the client fell too far behind
      return false;
    }

    if ((len == 0) && (ended == false))
    {
      free(data);
      return false;                              // the writer's next write wakes the loop
    }

    bool ok = chunkFrame(connP, sP, data, len, ended);
    free(data);

    if (ok == false)
    {
      connClose(serverP, connP);
      return false;
    }

    sP->lastQueued = ended;
  }
}



// -----------------------------------------------------------------------------
//
// streamDrain - the streams whose writers wrote or ended: their connections written to
//
static void streamDrain(CorHttpServer* serverP)
{
  pthread_mutex_lock(&serverP->resumeMutex);
  CorHttpStream* sP = serverP->streamHead;
  serverP->streamHead = NULL;
  pthread_mutex_unlock(&serverP->resumeMutex);

  while (sP != NULL)
  {
    CorHttpStream* nextP = sP->next;

    pthread_mutex_lock(&sP->mutex);
    sP->queued = false;
    sP->next   = NULL;
    pthread_mutex_unlock(&sP->mutex);

    //
    // The connection, if it is still this stream's and its headers are rendered (a stream started by a
    // suspended request waits for corHttpResume). The rest of the body follows the response's own path.
    //
    CorHttpConn* connP = sP->connP;

    if ((connP != NULL) && (sP->started == true) && (connP->state == COR_HTTP_CONN_WRITING) && (connP->streamP == sP))
    {
      if (bodyPump(serverP, connP) == true)
        responseDone(serverP, connP);
    }

    corHttpStreamUnref(sP);                      // the queue's reference
    sP = nextP;
  }
}



// -----------------------------------------------------------------------------
//
// responseSend - render and write, and decide what happens to the connection
//
static void responseSend(CorHttpServer* serverP, CorHttpConn* connP)
{
  if (corHttpResponseRender(connP) != CorHttpOk)
  {
    connClose(serverP, connP);
    return;
  }

  CorHttpStatus s = writeAll(connP);

  if (s == CorHttpAgain)
  {
    connP->state = COR_HTTP_CONN_WRITING;
    epollSet(serverP, connP, EPOLLIN | EPOLLOUT, EPOLL_CTL_MOD);
    return;
  }

  if (s != CorHttpOk)
  {
    connClose(serverP, connP);
    return;
  }

  //
  // A file or a stream: the headers are out, the body follows as the socket takes it - or, for a stream,
  // as its writer writes it
  //
  if ((connP->fileFd != -1) || (connP->streamP != NULL))
  {
    connP->state = COR_HTTP_CONN_WRITING;

    if (bodyPump(serverP, connP) == false)
      return;
  }

  responseDone(serverP, connP);
}



// -----------------------------------------------------------------------------
//
// responseDone - the whole response is out: the next request on the connection, or close it
//
static void responseDone(CorHttpServer* serverP, CorHttpConn* connP)
{
  connP->requests++;
  connP->lastActivity = corHttpNowMs();

  if (connP->upgradeCb != NULL)
  {
    upgradeHand(serverP, connP);
    return;
  }

  requestDone(serverP, connP);

  if ((connP->keepAlive == false) || (serverP->keepAliveTimeout == 0))
  {
    connClose(serverP, connP);
    return;
  }

  corHttpConnReset(connP);
  connP->state = COR_HTTP_CONN_READING;
  epollSet(serverP, connP, EPOLLIN, EPOLL_CTL_MOD);
}



// -----------------------------------------------------------------------------
//
// errorSend - answer without troubling the caller
//
// For the failures the engine itself detects: a request it cannot parse, or one
// bigger than the cap. The caller's callback never sees these, because there is
// nothing coherent to hand it.
//
static void errorSend(CorHttpServer* serverP, CorHttpConn* connP, int statusCode)
{
  connP->respHeaders = 0;
  connP->respBody    = NULL;
  connP->respBodyLen = 0;
  connP->keepAlive   = false;                    // the stream is out of sync; do not reuse it

  corHttpResponseStatus(connP, statusCode);
  responseSend(serverP, connP);
}



// -----------------------------------------------------------------------------
//
// requestReady - a whole request has arrived; hand it over
//
static void requestReady(CorHttpServer* serverP, CorHttpConn* connP)
{
  CorHttpStatus s = corHttpParse(connP);

  if (s == CorHttpAgain)
  {
    //
    // Once per request, not once per read: a client that asked for it is told
    // to go ahead, and telling it twice would put a second interim response on
    // the wire for a body already on its way.
    //
    if ((connP->expectContinue == true) && (connP->continueSent == false))
    {
      connP->continueSent = true;
      corHttpContinueSend(connP);
    }

    return;                                      // wait for the rest of it
  }

  if (s == CorHttpTooLarge)
  {
    errorSend(serverP, connP, 413);
    return;
  }

  if (s != CorHttpOk)
  {
    errorSend(serverP, connP, 400);
    return;
  }

  if (connP->headersTruncated)
  {
    errorSend(serverP, connP, 431);
    return;
  }

  connP->statusCode = 0;
  connP->inCallback = true;
  serverP->requestCb(connP);
  connP->inCallback = false;

  //
  // A callback that suspended has taken ownership: a worker thread will answer,
  // and this loop must not touch the connection again until it comes back
  // through the resume queue.
  //
  if (connP->state == COR_HTTP_CONN_IDLE)
    return;

  responseSend(serverP, connP);
}



// -----------------------------------------------------------------------------
//
// corHttpSuspend - the callback is handing this request to another thread
//
// Called from INSIDE the callback. The connection stops being the loop's
// business: no epoll interest, no timeout sweep, no reuse - until
// corHttpResume() puts it back.
//
// "No epoll interest" is a DELETE and not merely a state change. Left armed,
// the fd still reports EPOLLHUP the moment the client hangs up - and this loop
// would then close a connection a worker thread is still holding, returning it
// to the pool to be handed to the next client while the worker writes its
// answer into it. A client that gives up on a slow request is not a rare
// event; it is what a timeout looks like.
//
void corHttpSuspend(CorHttpConn* connP)
{
  CorHttpServer* serverP = connP->serverP;

  if ((serverP != NULL) && (connP->fd != -1))
    epoll_ctl(serverP->epollFd, EPOLL_CTL_DEL, connP->fd, NULL);

  connP->state = COR_HTTP_CONN_IDLE;
}



// -----------------------------------------------------------------------------
//
// corHttpResume - a worker has finished; the response is ready to go out
//
// Safe from any thread, and the only function here that is. It touches the
// queue and the eventfd and nothing else - in particular it does NOT write to
// the socket, because that is the loop's, and two threads writing one response
// is how a broker interleaves two answers on one connection.
//
void corHttpResume(CorHttpConn* connP)
{
  CorHttpServer* serverP = connP->serverP;       // the loop that owns this connection

  pthread_mutex_lock(&serverP->resumeMutex);
  connP->next        = serverP->resumeHead;
  serverP->resumeHead = connP;
  pthread_mutex_unlock(&serverP->resumeMutex);

  if (serverP->resumeFd != -1)
  {
    uint64_t one = 1;
    ssize_t  ignored = write(serverP->resumeFd, &one, sizeof(one));
    (void) ignored;                              // a full counter means the loop is already awake
  }
}



// -----------------------------------------------------------------------------
//
// corHttpResumeHere - the response is ready, and this IS the loop's thread: send it now
//
// What resumeDrain does for a connection a worker handed back, without the queue and the eventfd -
// for a request that ran as a coroutine of this loop.
//
void corHttpResumeHere(CorHttpConn* connP)
{
  CorHttpServer* serverP = connP->serverP;

  connP->state = COR_HTTP_CONN_WRITING;
  epollSet(serverP, connP, EPOLLIN, EPOLL_CTL_ADD);   // ADD: corHttpSuspend removed it

  //
  // Still inside the callback - a coroutine that finished without ever waiting: requestReady sends the
  // response when the callback returns, as for any answer given there. Sent here as well, it went out
  // twice - the second an empty one, which a keep-alive client took for its next request's answer.
  //
  if (connP->inCallback == true)
    return;

  responseSend(serverP, connP);
}



// -----------------------------------------------------------------------------
//
// resumeDrain - send the responses the workers finished with
//
static void resumeDrain(CorHttpServer* serverP)
{
  uint64_t counter;
  ssize_t  ignored = read(serverP->resumeFd, &counter, sizeof(counter));
  (void) ignored;

  pthread_mutex_lock(&serverP->resumeMutex);
  CorHttpConn* connP = serverP->resumeHead;
  serverP->resumeHead = NULL;
  pthread_mutex_unlock(&serverP->resumeMutex);

  while (connP != NULL)
  {
    CorHttpConn* nextP = connP->next;

    connP->next  = NULL;
    connP->state = COR_HTTP_CONN_WRITING;

    //
    // ADD and not MOD: corHttpSuspend removed this fd from the event set, so
    // there is no interest to modify. responseSend may need EPOLLOUT if the
    // answer does not fit the socket buffer, and that MOD needs a registration
    // to modify.
    //
    epollSet(serverP, connP, EPOLLIN, EPOLL_CTL_ADD);

    responseSend(serverP, connP);

    connP = nextP;
  }

  streamDrain(serverP);                          // the same eventfd wakes the loop for the streams
}



// -----------------------------------------------------------------------------
//
// sinkRead - read what the client sent while its response's body is being written, and drop it
//
// Not into the read buffer: the request the response answers is still there, and a caller may read it
// until the request is over (CorHttpDoneCb). false: the client hung up, or the socket failed.
//
static bool sinkRead(CorHttpConn* connP)
{
  char sink[4096];

  while (true)
  {
    ssize_t n = read(connP->fd, sink, sizeof(sink));

    if (n > 0)
      continue;
    if (n == 0)
      return false;
    if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
      return true;
    if (errno == EINTR)
      continue;
    return false;
  }
}



// -----------------------------------------------------------------------------
//
// connEvent -
//
static void connEvent(CorHttpServer* serverP, CorHttpConn* connP, uint32_t events)
{
  if (events & (EPOLLERR | EPOLLHUP))
  {
    connClose(serverP, connP);
    return;
  }

  //
  // A file or a stream body on its way: the client sends nothing now - what it does send is read and
  // dropped (a request behind this one is not served; corHttp does not pipeline), and its hang-up closes
  // the connection, which is how a stream's writer learns that it is gone
  //
  if (corHttpBodyBusy(connP) == true)
  {
    if ((events & EPOLLIN) && (sinkRead(connP) == false))
    {
      connClose(serverP, connP);
      return;
    }

    if ((events & EPOLLOUT) == 0)
      return;
  }

  if (events & EPOLLOUT)
  {
    CorHttpStatus s = writeAll(connP);

    if (s == CorHttpAgain)
      return;

    if (s != CorHttpOk)
    {
      connClose(serverP, connP);
      return;
    }

    if ((connP->fileFd != -1) || (connP->streamP != NULL))
    {
      if (bodyPump(serverP, connP) == false)
        return;
    }

    connP->requests++;
    connP->lastActivity = corHttpNowMs();

    if (connP->upgradeCb != NULL)
    {
      upgradeHand(serverP, connP);
      return;
    }

    requestDone(serverP, connP);

    if (connP->keepAlive == false)
    {
      connClose(serverP, connP);
      return;
    }

    corHttpConnReset(connP);
    connP->state = COR_HTTP_CONN_READING;
    epollSet(serverP, connP, EPOLLIN, EPOLL_CTL_MOD);
    return;
  }

  if (events & EPOLLIN)
  {
    CorHttpStatus s = readAll(serverP, connP);

    if (s == CorHttpClosed)
    {
      //
      // The peer closed. If a request was in flight it is incomplete by
      // definition - nothing to answer, and answering would write to a socket
      // whose other end is gone.
      //
      connClose(serverP, connP);
      return;
    }

    if (s == CorHttpTooLarge)
    {
      errorSend(serverP, connP, 413);
      return;
    }

    if (s != CorHttpOk)
    {
      connClose(serverP, connP);
      return;
    }

    connP->lastActivity = corHttpNowMs();

    if (connP->bufUsed > 0)
      requestReady(serverP, connP);
  }
}



// -----------------------------------------------------------------------------
//
// idleSweep - close connections nobody is using
//
// Walks the whole pool rather than keeping a timer per connection: the pool is
// a flat array of a thousand entries, this runs once a second, and a heap of
// timers would be more code and more state to get wrong for a scan that costs
// microseconds.
//
// A SUSPENDED connection is skipped. Its worker may take as long as a
// distributed operation takes, and closing it would deliver the answer to a
// socket that is gone - or worse, to a connection slot already reused.
//
static void idleSweep(CorHttpServer* serverP)
{
  if (serverP->keepAliveTimeout <= 0)
    return;

  uint64_t now     = corHttpNowMs();
  uint64_t timeout = (uint64_t) serverP->keepAliveTimeout * 1000;

  for (int ix = 0; ix < serverP->connPoolSize; ix++)
  {
    CorHttpConn* connP = &serverP->connPool[ix];

    if ((connP->state == COR_HTTP_CONN_FREE) || (connP->state == COR_HTTP_CONN_IDLE))
      continue;

    if (connP->streamP != NULL)
      continue;                                  // a stream is quiet for as long as its writer is (corHttpResponseStream)

    if ((now - connP->lastActivity) > timeout)
      connClose(serverP, connP);
  }
}



// -----------------------------------------------------------------------------
//
// corHttpInit -
//
CorHttpStatus corHttpInit(CorHttpServer* serverP, unsigned short port, int connPoolSize, CorHttpRequestCb cb)
{
  return corHttpInitOptions(serverP, port, NULL, connPoolSize, cb);
}



// -----------------------------------------------------------------------------
//
// corHttpInitOptions -
//
CorHttpStatus corHttpInitOptions(CorHttpServer* serverP, unsigned short port, const CorHttpListenOptions* optionsP, int connPoolSize, CorHttpRequestCb cb)
{
  memset(serverP, 0, sizeof(*serverP));

  serverP->port             = port;
  serverP->requestCb        = cb;
  serverP->keepAliveTimeout = COR_HTTP_KEEPALIVE_TIMEOUT;
  serverP->maxRequestSize   = 0;
  serverP->listenFd         = -1;
  serverP->epollFd          = -1;
  serverP->resumeFd         = -1;
  serverP->resumeHead       = NULL;
  serverP->handFd           = -1;
  pthread_mutex_init(&serverP->resumeMutex, NULL);   // after the memset, not a static initialiser
  pthread_mutex_init(&serverP->handMutex, NULL);

  if (cb == NULL)
    return CorHttpError;

  CorHttpStatus s = corHttpConnPoolInit(serverP, (connPoolSize > 0) ? connPoolSize : COR_HTTP_CONN_POOL_SIZE);

  if (s != CorHttpOk)
    return s;

  bool noListener = (optionsP != NULL) && (optionsP->noListener == true);

  if ((noListener == false) && ((serverP->listenFd = listener(port, optionsP)) < 0))
  {
    corHttpConnPoolRelease(serverP);
    return CorHttpError;
  }

  if ((serverP->epollFd = epoll_create1(0)) < 0)
  {
    if (serverP->listenFd != -1)
      close(serverP->listenFd);
    corHttpConnPoolRelease(serverP);
    return CorHttpError;
  }

  struct epoll_event ev;

  //
  // The listener is LEVEL-triggered, alone among the fds here. Edge triggering
  // it would mean an accept loop that must drain the backlog perfectly or lose
  // a connection until the next one arrives; level triggering re-reports it,
  // which for the one fd that must never drop anything is worth the extra
  // wakeup. Everything else is edge-triggered.
  //
  memset(&ev, 0, sizeof(ev));
  ev.events   = EPOLLIN;
  ev.data.ptr = NULL;                            // NULL = the listener

  if ((noListener == false) && (epoll_ctl(serverP->epollFd, EPOLL_CTL_ADD, serverP->listenFd, &ev) < 0))
  {
    corHttpRelease(serverP);
    return CorHttpError;
  }

  serverP->resumeFd = eventfd(0, EFD_NONBLOCK);

  if (serverP->resumeFd < 0)
  {
    corHttpRelease(serverP);
    return CorHttpError;
  }

  memset(&ev, 0, sizeof(ev));
  ev.events   = EPOLLIN;
  ev.data.ptr = serverP;                         // the server itself = the resume eventfd

  if (epoll_ctl(serverP->epollFd, EPOLL_CTL_ADD, serverP->resumeFd, &ev) < 0)
  {
    corHttpRelease(serverP);
    return CorHttpError;
  }

  return CorHttpOk;
}



// -----------------------------------------------------------------------------
//
// corHttpServe -
//
CorHttpStatus corHttpServe(CorHttpServer* serverP)
{
  struct epoll_event events[COR_HTTP_MAX_EVENTS];
  uint64_t           lastSweep = corHttpNowMs();

  serverP->running = true;

  while (serverP->running == true)
  {
    //
    // A one-second timeout, which is what makes the sweep and the stop flag
    // work at all: with -1 the loop would sit in epoll_wait until a client did
    // something, and a broker with no traffic would never notice it had been
    // told to stop.
    //
    //
    // ...or less: a coroutine of this loop waiting with a deadline (corBase corCoLoop) wakes it sooner
    //
    int coMs = corCoLoopTimeoutMs();
    int n    = epoll_wait(serverP->epollFd, events, COR_HTTP_MAX_EVENTS, ((coMs >= 0) && (coMs < 1000)) ? coMs : 1000);

    if (n < 0)
    {
      if (errno == EINTR)
        continue;
      return CorHttpError;
    }

    for (int ix = 0; ix < n; ix++)
    {
      void* ptr = events[ix].data.ptr;

      //
      // A socket a coroutine of this loop waits for (its pointer tagged - corCoLoop): resumed there
      //
      if (corCoLoopEvent(ptr, events[ix].events) == true)
        continue;

      if ((ptr == NULL) && (serverP->acceptV != NULL))
        acceptDeal(serverP);
      else if (ptr == NULL)
        acceptAll(serverP);
      else if (ptr == &serverP->handFd)
        handDrain(serverP);
      else if (ptr == serverP)
        resumeDrain(serverP);
      else
        connEvent(serverP, (CorHttpConn*) ptr, events[ix].events);
    }

    corCoLoopExpire();                           // the coroutines whose time is up

    uint64_t now = corHttpNowMs();

    if ((now - lastSweep) >= 1000)
    {
      idleSweep(serverP);
      lastSweep = now;
    }
  }

  return CorHttpOk;
}



// -----------------------------------------------------------------------------
//
// corHttpServeEnd -
//
void corHttpServeEnd(CorHttpServer* serverP)
{
  if (serverP->connPool == NULL)
    return;

  for (int ix = 0; ix < serverP->connPoolSize; ix++)
  {
    CorHttpConn* connP = &serverP->connPool[ix];

    if ((connP->state == COR_HTTP_CONN_FREE) || (connP->state == COR_HTTP_CONN_IDLE))   // IDLE: suspended
      continue;

    connClose(serverP, connP);
  }
}



// -----------------------------------------------------------------------------
//
// corHttpStop -
//
// Sets a flag and returns. Safe from a signal handler, which is where it is
// called from: the loop notices within its one-second timeout, and tearing down
// epoll from a handler while the loop is inside epoll_wait would not be.
//
void corHttpStop(CorHttpServer* serverP)
{
  serverP->running = false;

  //
  // ...and wake the loop now: it notices the flag only when epoll_wait returns, which with nothing to
  // do is up to a second later - every broker stop paid it (in the functests, ~1 s a broker). The
  // eventfd is the one the workers wake it with; an empty resume queue is nothing to drain.
  //
  if (serverP->resumeFd != -1)
  {
    uint64_t one     = 1;
    ssize_t  ignored = write(serverP->resumeFd, &one, sizeof(one));
    (void) ignored;
  }
}



// -----------------------------------------------------------------------------
//
// corHttpRelease -
//
void corHttpRelease(CorHttpServer* serverP)
{
  if (serverP->handFd != -1)
  {
    close(serverP->handFd);
    serverP->handFd = -1;
  }

  for (int ix = 0; ix < serverP->handCount; ix++)
    close(serverP->handV[ix]);                   // dealt, never taken

  free(serverP->handV);
  serverP->handV     = NULL;
  serverP->handCount = 0;
  serverP->handSize  = 0;
  pthread_mutex_destroy(&serverP->handMutex);

  if (serverP->resumeFd != -1)
  {
    close(serverP->resumeFd);
    serverP->resumeFd = -1;
  }

  //
  // Streams still queued: the queue's references dropped (their connections go with the pool below)
  //
  while (serverP->streamHead != NULL)
  {
    CorHttpStream* sP = serverP->streamHead;

    serverP->streamHead = sP->next;
    sP->next            = NULL;
    corHttpStreamUnref(sP);
  }

  pthread_mutex_destroy(&serverP->resumeMutex);

  if (serverP->epollFd != -1)
  {
    close(serverP->epollFd);
    serverP->epollFd = -1;
  }

  if (serverP->listenFd != -1)
  {
    close(serverP->listenFd);
    serverP->listenFd = -1;
  }

  corHttpConnPoolRelease(serverP);
}



// -----------------------------------------------------------------------------
//
// corHttpAcceptShare -
//
// The other loops give up their listeners, if they have one - the kernel would otherwise still hash
// connections to them (SO_REUSEPORT) - and each gets the eventfd its dealt connections arrive on.
//
CorHttpStatus corHttpAcceptShare(CorHttpServer* serverV, int n)
{
  if (n < 2)
    return CorHttpOk;

  for (int ix = 1; ix < n; ix++)
  {
    CorHttpServer*     serverP = &serverV[ix];
    struct epoll_event ev;

    if ((serverP->handFd = eventfd(0, EFD_NONBLOCK)) < 0)
      return CorHttpError;

    memset(&ev, 0, sizeof(ev));
    ev.events   = EPOLLIN;
    ev.data.ptr = &serverP->handFd;              // the hand eventfd

    if (epoll_ctl(serverP->epollFd, EPOLL_CTL_ADD, serverP->handFd, &ev) < 0)
      return CorHttpError;

    if (serverP->listenFd != -1)                 // a loop with no listener of its own (noListener) has none to give up
    {
      epoll_ctl(serverP->epollFd, EPOLL_CTL_DEL, serverP->listenFd, NULL);
      close(serverP->listenFd);
      serverP->listenFd = -1;
    }
  }

  serverV[0].acceptV    = serverV;
  serverV[0].acceptN    = n;
  serverV[0].acceptNext = 0;

  return CorHttpOk;
}
