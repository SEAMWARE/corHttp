//
// FILE            corHttpStream.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Bodies that are not in memory: a file, sent from the file (corHttpResponseFile), and a body written
// over time by the caller, from any thread (corHttpResponseStream).
//
// The loop writes both - after the headers, as the socket takes the bytes (corHttpServer.c, bodyPump).
// What is here is the caller's side of it, and the stream's bookkeeping.
//
// A STREAM'S WRITER NEVER TOUCHES THE CONNECTION. It holds the stream; the stream points at the
// connection only for the loop. A write copies the bytes into the stream under its mutex and, if the
// stream is not queued already, queues it on the loop's stream queue and pokes the loop's eventfd - the
// same wake-up corHttpResume uses, and for the same reason: the socket is the loop's, and two threads
// writing one response is how two answers end up interleaved.
//
#include <stdlib.h>                              // malloc, realloc, free
#include <string.h>                              // memcpy, strcmp
#include <unistd.h>                              // write, close

#include "corHttp/CorHttp.h"                     // CorHttpConn, CorHttpServer
#include "corHttp/corHttpInternal.h"             // Own interface



// -----------------------------------------------------------------------------
//
// corHttpResponseFile -
//
void corHttpResponseFile(CorHttpConn* connP, int fd, int64_t offset, int64_t length)
{
  if (connP->fileFd != -1)
    close(connP->fileFd);

  connP->fileFd      = fd;
  connP->fileOffset  = offset;
  connP->fileLeft    = (length > 0) ? length : 0;
  connP->respBody    = NULL;
  connP->respBodyLen = 0;
}



// -----------------------------------------------------------------------------
//
// corHttpResponseStream -
//
CorHttpStream* corHttpResponseStream(CorHttpConn* connP)
{
  if (connP->streamP != NULL)
    return NULL;                                 // one stream per response

  CorHttpStream* sP = (CorHttpStream*) calloc(1, sizeof(CorHttpStream));

  if (sP == NULL)
    return NULL;

  pthread_mutex_init(&sP->mutex, NULL);
  sP->refs    = 2;                               // the writer and the connection
  sP->serverP = connP->serverP;
  sP->connP   = connP;
  sP->chunked = true;

  connP->streamP     = sP;
  connP->respBody    = NULL;
  connP->respBodyLen = 0;

  return sP;
}



// -----------------------------------------------------------------------------
//
// wake - put the stream on its loop's stream queue (once) and wake the loop; called with sP->mutex held
//
// The queue holds a reference of its own: the writer may end, and the connection let go, before the loop
// gets to it.
//
static bool wakeNeeded(CorHttpStream* sP)
{
  if (sP->queued == true)
    return false;

  sP->queued = true;
  sP->refs  += 1;
  return true;
}

static void wake(CorHttpStream* sP)
{
  CorHttpServer* serverP = sP->serverP;

  pthread_mutex_lock(&serverP->resumeMutex);
  sP->next            = serverP->streamHead;
  serverP->streamHead = sP;
  pthread_mutex_unlock(&serverP->resumeMutex);

  if (serverP->resumeFd != -1)
  {
    uint64_t one     = 1;
    ssize_t  ignored = write(serverP->resumeFd, &one, sizeof(one));
    (void) ignored;                              // a full counter means the loop is already awake
  }
}



// -----------------------------------------------------------------------------
//
// corHttpStreamWrite -
//
bool corHttpStreamWrite(CorHttpStream* sP, const char* data, int len)
{
  if ((sP == NULL) || (len < 0))
    return false;

  pthread_mutex_lock(&sP->mutex);

  if ((sP->gone == true) || (sP->ended == true) || (sP->overflow == true))
  {
    pthread_mutex_unlock(&sP->mutex);
    return false;
  }

  if (len == 0)
  {
    pthread_mutex_unlock(&sP->mutex);
    return true;                                 // nothing to send - and an empty chunk would END the body
  }

  bool ok = true;

  if (sP->len + len > COR_HTTP_STREAM_PENDING_MAX)
  {
    sP->overflow = true;                         // the loop disconnects the client
    ok           = false;
  }
  else
  {
    if (sP->len + len > sP->size)
    {
      int   size = (sP->size == 0) ? 4096 : sP->size;
      while (size < sP->len + len)
        size *= 2;

      char* buf = (char*) realloc(sP->buf, size);
      if (buf == NULL)
      {
        pthread_mutex_unlock(&sP->mutex);
        return false;
      }
      sP->buf  = buf;
      sP->size = size;
    }

    memcpy(&sP->buf[sP->len], data, len);
    sP->len += len;
  }

  bool doWake = wakeNeeded(sP);
  pthread_mutex_unlock(&sP->mutex);

  if (doWake)
    wake(sP);

  return ok;
}



// -----------------------------------------------------------------------------
//
// corHttpStreamEnd - the last chunk; the writer's reference is dropped
//
void corHttpStreamEnd(CorHttpStream* sP)
{
  if (sP == NULL)
    return;

  pthread_mutex_lock(&sP->mutex);
  sP->ended   = true;
  bool doWake = (sP->gone == false) && wakeNeeded(sP);
  pthread_mutex_unlock(&sP->mutex);

  if (doWake)
    wake(sP);

  corHttpStreamUnref(sP);
}



// -----------------------------------------------------------------------------
//
// corHttpStreamTake -
//
char* corHttpStreamTake(CorHttpStream* sP, int* lenP, bool* endedP, bool* overflowP)
{
  pthread_mutex_lock(&sP->mutex);

  char* buf  = sP->buf;
  *lenP      = sP->len;
  *endedP    = sP->ended;
  *overflowP = sP->overflow;

  sP->buf  = NULL;
  sP->len  = 0;
  sP->size = 0;

  pthread_mutex_unlock(&sP->mutex);

  return buf;
}



// -----------------------------------------------------------------------------
//
// corHttpStreamUnref -
//
void corHttpStreamUnref(CorHttpStream* sP)
{
  pthread_mutex_lock(&sP->mutex);
  int refs = --sP->refs;
  pthread_mutex_unlock(&sP->mutex);

  if (refs > 0)
    return;

  pthread_mutex_destroy(&sP->mutex);
  free(sP->buf);
  free(sP);
}



// -----------------------------------------------------------------------------
//
// corHttpBodyBusy -
//
bool corHttpBodyBusy(CorHttpConn* connP)
{
  if (connP->fileFd != -1)
    return true;

  return (connP->streamP != NULL) && (connP->streamP->started == true);
}



// -----------------------------------------------------------------------------
//
// corHttpBodyRelease - the connection lets go of its file and its stream
//
// A stream: refused to its writer from now on (a write says false), and the connection's reference
// dropped. On the loop's thread, or on whichever thread owns a suspended connection.
//
void corHttpBodyRelease(CorHttpConn* connP)
{
  if (connP->fileFd != -1)
  {
    close(connP->fileFd);
    connP->fileFd = -1;
  }

  connP->fileOffset = 0;
  connP->fileLeft   = 0;

  CorHttpStream* sP = connP->streamP;

  if (sP == NULL)
    return;

  connP->streamP = NULL;
  sP->connP      = NULL;

  pthread_mutex_lock(&sP->mutex);
  sP->gone = true;
  pthread_mutex_unlock(&sP->mutex);

  corHttpStreamUnref(sP);
}
