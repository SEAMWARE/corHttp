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
#include <netinet/in.h>                          // sockaddr_in, INADDR_ANY
#include <netinet/tcp.h>                         // TCP_NODELAY
#include <pthread.h>                             // pthread_mutex_*
#include <string.h>                              // memset, strerror
#include <sys/epoll.h>                           // epoll_create1, epoll_ctl, epoll_wait
#include <sys/eventfd.h>                         // eventfd
#include <sys/socket.h>                          // socket, bind, listen, accept4, setsockopt
#include <unistd.h>                              // read, write, close

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
static int listener(unsigned short port)
{
  int fd = socket(AF_INET, SOCK_STREAM, 0);

  if (fd < 0)
    return -1;

  //
  // SO_REUSEADDR so a restart does not have to wait out TIME_WAIT on the
  // previous process's sockets. Without it a broker that is restarted inside
  // two minutes fails to bind, which in a test suite is every single run.
  //
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  struct sockaddr_in addr;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port        = htons(port);

  if (bind(fd, (struct sockaddr*) &addr, sizeof(addr)) < 0)
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
      continue;
    }

    if (epollSet(serverP, connP, EPOLLIN, EPOLL_CTL_ADD) < 0)
      connClose(serverP, connP);
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

  connP->requests++;
  connP->lastActivity = corHttpNowMs();

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
  serverP->requestCb(connP);

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

    connP->requests++;
    connP->lastActivity = corHttpNowMs();

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
  memset(serverP, 0, sizeof(*serverP));

  serverP->port             = port;
  serverP->requestCb        = cb;
  serverP->keepAliveTimeout = COR_HTTP_KEEPALIVE_TIMEOUT;
  serverP->maxRequestSize   = 0;
  serverP->listenFd         = -1;
  serverP->epollFd          = -1;
  serverP->resumeFd         = -1;
  serverP->resumeHead       = NULL;
  pthread_mutex_init(&serverP->resumeMutex, NULL);   // after the memset, not a static initialiser

  if (cb == NULL)
    return CorHttpError;

  CorHttpStatus s = corHttpConnPoolInit(serverP, (connPoolSize > 0) ? connPoolSize : COR_HTTP_CONN_POOL_SIZE);

  if (s != CorHttpOk)
    return s;

  if ((serverP->listenFd = listener(port)) < 0)
  {
    corHttpConnPoolRelease(serverP);
    return CorHttpError;
  }

  if ((serverP->epollFd = epoll_create1(0)) < 0)
  {
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

  if (epoll_ctl(serverP->epollFd, EPOLL_CTL_ADD, serverP->listenFd, &ev) < 0)
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
    int n = epoll_wait(serverP->epollFd, events, COR_HTTP_MAX_EVENTS, 1000);

    if (n < 0)
    {
      if (errno == EINTR)
        continue;
      return CorHttpError;
    }

    for (int ix = 0; ix < n; ix++)
    {
      void* ptr = events[ix].data.ptr;

      if (ptr == NULL)
        acceptAll(serverP);
      else if (ptr == serverP)
        resumeDrain(serverP);
      else
        connEvent(serverP, (CorHttpConn*) ptr, events[ix].events);
    }

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
// corHttpStop -
//
// Sets a flag and returns. Safe from a signal handler, which is where it is
// called from: the loop notices within its one-second timeout, and tearing down
// epoll from a handler while the loop is inside epoll_wait would not be.
//
void corHttpStop(CorHttpServer* serverP)
{
  serverP->running = false;
}



// -----------------------------------------------------------------------------
//
// corHttpRelease -
//
void corHttpRelease(CorHttpServer* serverP)
{
  if (serverP->resumeFd != -1)
  {
    close(serverP->resumeFd);
    serverP->resumeFd = -1;
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
