//
// FILE            streamTest.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Bodies that are not in memory: corHttpResponseFile and corHttpResponseStream, against a server of
// this library on an ephemeral port, its loop on a thread of its own, and a client of plain sockets.
//
//   1. memory          an ordinary response is as before: Content-Length, the body, keep-alive
//   2. file            a 3 MiB file, whole: Content-Length and every byte
//   3. file-part       bytes 1000..1999 of it (offset + length)
//   4. file-head       HEAD: the length, no body - and the connection serves the next request
//   5. stream          100 chunks written by another thread, then the end: chunked framing, every byte,
//                      the terminating chunk - and the connection serves the next request
//   6. stream-now      written and ended inside the callback
//   7. stream-gone     the client hangs up: the writer's next writes say false
//   8. stream-behind   a client that reads nothing: the writes say false once 16 MiB are pending
//   9. stream-http10   an HTTP/1.0 client: no framing, the body, then the connection closes
//  10. stream-quiet    a stream quiet for longer than the keep-alive timeout is not closed by the sweep
//
// Exit code 0: all pass.
//
#include <arpa/inet.h>                           // htons, ntohs
#include <errno.h>                               // errno
#include <fcntl.h>                               // open
#include <netinet/in.h>                          // sockaddr_in
#include <poll.h>                                // poll
#include <pthread.h>                             // pthread_*
#include <signal.h>                              // signal
#include <stdio.h>                               // printf
#include <stdlib.h>                              // malloc, free, strtol
#include <string.h>                              // strstr, memcmp
#include <sys/socket.h>                          // socket, connect
#include <time.h>                                // nanosleep
#include <unistd.h>                              // read, write, close, unlink

#include "corHttp/CorHttp.h"                     // the library



static int            failures = 0;
static CorHttpServer  server;
static char           filePath[] = "/tmp/corHttpStreamTest.XXXXXX";
static char*          fileData;
static const int      fileSize = 3 * 1024 * 1024;

static int            goneWrites  = -1;          // stream-gone: the writes that went through before one said false
static int            behindBytes = -1;          // stream-behind: the bytes written before a write said false



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



static void msleep(int ms)
{
  struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
  nanosleep(&ts, NULL);
}



// -----------------------------------------------------------------------------
//
// Writers - threads that write a stream
//
typedef struct Writer
{
  CorHttpStream*  streamP;
  int             mode;                          // 0: 100 chunks; 1: until a write fails; 2: 64 KiB until false; 3: quiet, then one chunk
} Writer;

static void* writerThread(void* vP)
{
  Writer* wP = (Writer*) vP;
  char    line[64];

  if (wP->mode == 0)
  {
    for (int i = 0; i < 100; i++)
    {
      int n = snprintf(line, sizeof(line), "line %03d\n", i);
      corHttpStreamWrite(wP->streamP, line, n);
      if (i % 10 == 0)
        msleep(2);
    }
  }
  else if (wP->mode == 1)
  {
    int ok = 0;
    for (int i = 0; i < 1000; i++)
    {
      if (corHttpStreamWrite(wP->streamP, "tick\n", 5) == false)
        break;
      ++ok;
      msleep(10);
    }
    __atomic_store_n(&goneWrites, ok, __ATOMIC_SEQ_CST);
  }
  else if (wP->mode == 2)
  {
    static char block[64 * 1024];
    int         total = 0;

    memset(block, 'x', sizeof(block));
    for (int i = 0; i < 1024; i++)                // 64 MiB at most
    {
      if (corHttpStreamWrite(wP->streamP, block, sizeof(block)) == false)
        break;
      total += sizeof(block);
    }
    __atomic_store_n(&behindBytes, total, __ATOMIC_SEQ_CST);
  }
  else if (wP->mode == 3)
  {
    msleep(2600);                                // longer than the 1 s keep-alive timeout of this server, plus the sweep
    corHttpStreamWrite(wP->streamP, "late\n", 5);
  }

  corHttpStreamEnd(wP->streamP);
  free(wP);
  return NULL;
}

static void writerStart(CorHttpStream* streamP, int mode)
{
  Writer*   wP = (Writer*) malloc(sizeof(Writer));
  pthread_t tid;

  wP->streamP = streamP;
  wP->mode    = mode;

  pthread_create(&tid, NULL, writerThread, wP);
  pthread_detach(tid);
}



// -----------------------------------------------------------------------------
//
// request - the server's callback
//
static void request(CorHttpConn* connP)
{
  const char* path = connP->path.s;

  if (strcmp(path, "/memory") == 0)
  {
    corHttpResponseStatus(connP, 200);
    corHttpResponseHeader(connP, "Content-Type", "text/plain");
    corHttpResponseBody(connP, (char*) "hello", 5);
    return;
  }

  if ((strcmp(path, "/file") == 0) || (strcmp(path, "/file-part") == 0))
  {
    int fd = open(filePath, O_RDONLY);
    corHttpResponseStatus(connP, 200);
    corHttpResponseHeader(connP, "Content-Type", "application/octet-stream");
    if (strcmp(path, "/file") == 0)
      corHttpResponseFile(connP, fd, 0, fileSize);
    else
      corHttpResponseFile(connP, fd, 1000, 1000);
    return;
  }

  corHttpResponseStatus(connP, 200);
  corHttpResponseHeader(connP, "Content-Type", "text/plain");

  CorHttpStream* streamP = corHttpResponseStream(connP);

  if      (strcmp(path, "/stream")        == 0)  writerStart(streamP, 0);
  else if (strcmp(path, "/stream-gone")   == 0)  writerStart(streamP, 1);
  else if (strcmp(path, "/stream-behind") == 0)  writerStart(streamP, 2);
  else if (strcmp(path, "/stream-quiet")  == 0)  writerStart(streamP, 3);
  else if (strcmp(path, "/stream-now")    == 0)
  {
    corHttpStreamWrite(streamP, "first\n", 6);
    corHttpStreamWrite(streamP, "second\n", 7);
    corHttpStreamEnd(streamP);
  }
  else
    corHttpStreamEnd(streamP);
}



// -----------------------------------------------------------------------------
//
// Client
//
static unsigned short port;

static int clientConnect(void)
{
  int                fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in sa;

  memset(&sa, 0, sizeof(sa));
  sa.sin_family      = AF_INET;
  sa.sin_port        = htons(port);
  sa.sin_addr.s_addr = htonl(0x7F000001);

  if (connect(fd, (struct sockaddr*) &sa, sizeof(sa)) != 0)
  {
    close(fd);
    return -1;
  }

  return fd;
}

static void sendRequest(int fd, const char* method, const char* path, const char* version)
{
  char req[256];
  int  n = snprintf(req, sizeof(req), "%s %s %s\r\nHost: localhost\r\n\r\n", method, path, version);
  ssize_t ignored = write(fd, req, n);
  (void) ignored;
}

//
// readResponse - one response: the head into 'head', the body (de-chunked) into a malloc'ed buffer
//
// Returns the body's length, -1 on a broken response. *closedP: the server closed the connection at the
// end (read until EOF: a response with neither length nor chunks).
//
typedef struct Reader
{
  int    fd;
  char*  buf;
  int    len;
  int    pos;
} Reader;

static int fill(Reader* rP, int timeoutMs)
{
  struct pollfd pfd = { rP->fd, POLLIN, 0 };

  if (poll(&pfd, 1, timeoutMs) <= 0)
    return -1;

  rP->buf = realloc(rP->buf, rP->len + 65536 + 1);
  int n   = read(rP->fd, rP->buf + rP->len, 65536);

  if (n > 0)
  {
    rP->len += n;
    rP->buf[rP->len] = 0;
  }

  return n;
}

static char* lineGet(Reader* rP)                 // a CRLF-terminated line, terminated in place
{
  while (true)
  {
    char* start = rP->buf + rP->pos;
    char* crlf  = (rP->len > rP->pos) ? strstr(start, "\r\n") : NULL;

    if (crlf != NULL)
    {
      *crlf   = 0;
      rP->pos = (crlf - rP->buf) + 2;
      return start;
    }

    if (fill(rP, 3000) <= 0)
      return NULL;
  }
}

static bool bytesGet(Reader* rP, char* out, int n)
{
  while (rP->len - rP->pos < n)
  {
    if (fill(rP, 3000) <= 0)
      return false;
  }

  memcpy(out, rP->buf + rP->pos, n);
  rP->pos += n;
  return true;
}

static int readResponse(Reader* rP, bool isHead, char* head, int headSize, char** bodyP, bool* closedP)
{
  int   contentLength = -1;
  bool  chunked       = false;
  char* line;

  head[0]  = 0;
  *bodyP   = NULL;
  *closedP = false;

  while ((line = lineGet(rP)) != NULL)
  {
    if (line[0] == 0)
      break;

    strncat(head, line, headSize - strlen(head) - 2);
    strncat(head, "\n", headSize - strlen(head) - 1);

    if (strncasecmp(line, "Content-Length: ", 16) == 0)
      contentLength = atoi(&line[16]);
    if (strcasecmp(line, "Transfer-Encoding: chunked") == 0)
      chunked = true;
  }

  if (line == NULL)
    return -1;

  if (isHead)
    return 0;

  if (contentLength >= 0)
  {
    *bodyP = malloc(contentLength + 1);
    if (bytesGet(rP, *bodyP, contentLength) == false)
      return -1;
    return contentLength;
  }

  if (chunked)
  {
    int total = 0;

    *bodyP = malloc(1);
    while ((line = lineGet(rP)) != NULL)
    {
      int n = (int) strtol(line, NULL, 16);
      if (n == 0)
      {
        lineGet(rP);                             // the blank line after the last chunk
        return total;
      }
      *bodyP = realloc(*bodyP, total + n + 1);
      if (bytesGet(rP, *bodyP + total, n) == false)
        return -1;
      total += n;
      (*bodyP)[total] = 0;
      lineGet(rP);                               // the CRLF after the chunk
    }
    return -1;
  }

  // neither: until the server closes
  while (fill(rP, 3000) > 0)
    ;
  *closedP = true;
  int n    = rP->len - rP->pos;
  *bodyP   = malloc(n + 1);
  memcpy(*bodyP, rP->buf + rP->pos, n);
  (*bodyP)[n] = 0;
  rP->pos = rP->len;
  return n;
}

static bool serverClosed(int fd)                 // what was sent is read and dropped, then the end of it
{
  static char   sink[65536];
  struct pollfd pfd = { fd, POLLIN, 0 };

  while (poll(&pfd, 1, 2000) > 0)
  {
    ssize_t n = read(fd, sink, sizeof(sink));
    if (n == 0)
      return true;
    if (n < 0)
      return errno == ECONNRESET;
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// serveThread -
//
static void* serveThread(void* vP)
{
  (void) vP;
  corHttpServe(&server);
  return NULL;
}



// -----------------------------------------------------------------------------
//
// main -
//
int main(void)
{
  char      head[4096];
  char      detail[256];
  char*     body;
  bool      closed;
  int       n;
  pthread_t tid;

  signal(SIGPIPE, SIG_IGN);

  //
  // The file: 3 MiB of a pattern no two neighbouring bytes share
  //
  int fd = mkstemp(filePath);
  fileData = malloc(fileSize);
  for (int i = 0; i < fileSize; i++)
    fileData[i] = (char) ((i * 7 + i / 251) & 0xFF);
  if (write(fd, fileData, fileSize) != fileSize)
  {
    printf("cannot write %s\n", filePath);
    return 1;
  }
  close(fd);

  if (corHttpInit(&server, 0, 16, request) != CorHttpOk)
  {
    printf("corHttpInit failed\n");
    return 1;
  }
  server.keepAliveTimeout = 1;                   // stream-quiet: the sweep would close an idle connection after 1 s

  struct sockaddr_in sa;
  socklen_t          saLen = sizeof(sa);
  getsockname(server.listenFd, (struct sockaddr*) &sa, &saLen);
  port = ntohs(sa.sin_port);

  pthread_create(&tid, NULL, serveThread, NULL);

  Reader r = { clientConnect(), NULL, 0, 0 };

  // 1. memory
  sendRequest(r.fd, "GET", "/memory", "HTTP/1.1");
  n = readResponse(&r, false, head, sizeof(head), &body, &closed);
  snprintf(detail, sizeof(detail), "%d bytes", n);
  verdict("memory", (n == 5) && (memcmp(body, "hello", 5) == 0) && (strstr(head, "HTTP/1.1 200 OK\n") == head) &&
          (strstr(head, "Content-Length: 5\n") != NULL) && (strstr(head, "Transfer-Encoding") == NULL) && (strstr(head, "Connection: close") == NULL), detail);
  free(body);

  // 2. file
  sendRequest(r.fd, "GET", "/file", "HTTP/1.1");
  n = readResponse(&r, false, head, sizeof(head), &body, &closed);
  snprintf(detail, sizeof(detail), "%d bytes of %d", n, fileSize);
  verdict("file", (n == fileSize) && (memcmp(body, fileData, fileSize) == 0) && (strstr(head, "Content-Length: 3145728\n") != NULL), detail);
  free(body);

  // 3. file-part
  sendRequest(r.fd, "GET", "/file-part", "HTTP/1.1");
  n = readResponse(&r, false, head, sizeof(head), &body, &closed);
  snprintf(detail, sizeof(detail), "%d bytes", n);
  verdict("file-part", (n == 1000) && (memcmp(body, fileData + 1000, 1000) == 0), detail);
  free(body);

  // 4. file-head, then the next request on the same connection
  sendRequest(r.fd, "HEAD", "/file", "HTTP/1.1");
  n = readResponse(&r, true, head, sizeof(head), &body, &closed);
  bool headOk = (n == 0) && (strstr(head, "Content-Length: 3145728\n") != NULL);
  sendRequest(r.fd, "GET", "/memory", "HTTP/1.1");
  n = readResponse(&r, false, head, sizeof(head), &body, &closed);
  verdict("file-head", headOk && (n == 5), headOk ? "the length, no body; the next request answered" : "head wrong");
  free(body);

  // 5. stream - then the next request on the same connection
  sendRequest(r.fd, "GET", "/stream", "HTTP/1.1");
  n = readResponse(&r, false, head, sizeof(head), &body, &closed);
  bool streamOk = (n == 900) && (strstr(head, "Transfer-Encoding: chunked\n") != NULL) && (strstr(head, "Content-Length") == NULL) &&
                  (strncmp(body, "line 000\n", 9) == 0) && (strncmp(body + 891, "line 099\n", 9) == 0);
  free(body);
  sendRequest(r.fd, "GET", "/memory", "HTTP/1.1");
  n = readResponse(&r, false, head, sizeof(head), &body, &closed);
  snprintf(detail, sizeof(detail), "%s; next request %s", streamOk ? "100 lines, chunked" : "body wrong", (n == 5) ? "answered" : "NOT answered");
  verdict("stream", streamOk && (n == 5), detail);
  free(body);

  // 6. stream-now
  sendRequest(r.fd, "GET", "/stream-now", "HTTP/1.1");
  n = readResponse(&r, false, head, sizeof(head), &body, &closed);
  verdict("stream-now", (n == 13) && (strcmp(body, "first\nsecond\n") == 0), (n == 13) ? "first, second, end" : "wrong");
  free(body);
  close(r.fd);
  free(r.buf);

  // 7. stream-gone - the client reads the head and the first tick, then hangs up
  Reader g = { clientConnect(), NULL, 0, 0 };
  sendRequest(g.fd, "GET", "/stream-gone", "HTTP/1.1");
  while ((strstr(g.buf ? g.buf : "", "tick") == NULL) && (fill(&g, 2000) > 0))
    ;
  close(g.fd);
  free(g.buf);
  for (int i = 0; (i < 200) && (__atomic_load_n(&goneWrites, __ATOMIC_SEQ_CST) < 0); i++)
    msleep(10);
  snprintf(detail, sizeof(detail), "the writer stopped after %d writes", goneWrites);
  verdict("stream-gone", (goneWrites >= 1) && (goneWrites < 100), detail);

  // 8. stream-behind - a client that reads nothing
  int b = clientConnect();
  sendRequest(b, "GET", "/stream-behind", "HTTP/1.1");
  for (int i = 0; (i < 500) && (__atomic_load_n(&behindBytes, __ATOMIC_SEQ_CST) < 0); i++)
    msleep(10);
  snprintf(detail, sizeof(detail), "%d MiB written before false", behindBytes / (1024 * 1024));
  verdict("stream-behind", (behindBytes >= COR_HTTP_STREAM_PENDING_MAX - 65536) && (behindBytes < 48 * 1024 * 1024) && serverClosed(b), detail);
  close(b);

  // 9. stream-http10
  Reader h = { clientConnect(), NULL, 0, 0 };
  sendRequest(h.fd, "GET", "/stream", "HTTP/1.0");
  n = readResponse(&h, false, head, sizeof(head), &body, &closed);
  snprintf(detail, sizeof(detail), "%d bytes, %s", n, closed ? "then closed" : "not closed");
  verdict("stream-http10", (n == 900) && closed && (strstr(head, "Connection: close\n") != NULL) && (strstr(head, "Transfer-Encoding") == NULL), detail);
  free(body);
  close(h.fd);
  free(h.buf);

  // 10. stream-quiet
  Reader q = { clientConnect(), NULL, 0, 0 };
  sendRequest(q.fd, "GET", "/stream-quiet", "HTTP/1.1");
  n = readResponse(&q, false, head, sizeof(head), &body, &closed);
  verdict("stream-quiet", (n == 5) && (strcmp(body, "late\n") == 0), (n == 5) ? "the chunk after 2.6 s arrived" : "closed by the sweep");
  free(body);
  close(q.fd);
  free(q.buf);

  corHttpStop(&server);
  pthread_join(tid, NULL);
  corHttpRelease(&server);
  unlink(filePath);
  free(fileData);

  printf("%s\n", (failures == 0) ? "PASS" : "FAIL");
  return (failures == 0) ? 0 : 1;
}
