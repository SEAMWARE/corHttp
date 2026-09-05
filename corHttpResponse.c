//
// FILE            corHttpResponse.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Building the response, and turning it into bytes.
//
// The callback sets a status, adds headers and hands over a body; this renders
// the lot into one contiguous buffer. ONE buffer and one write() rather than a
// writev of the pieces: the status line and headers are a few hundred bytes
// that have to be built somewhere anyway, and a single buffer is also what
// makes a partial write trivial to resume - there is one offset to remember.
//
#include <stdio.h>                               // snprintf
#include <stdlib.h>                              // malloc, realloc
#include <string.h>                              // memcpy, strlen

#include "corHttp/CorHttp.h"                     // CorHttpConn
#include "corHttp/corHttpInternal.h"             // Own interface



// -----------------------------------------------------------------------------
//
// reasonPhrase - the text after the status code
//
// Only the codes this broker actually sends. RFC 9112 § 4 lets a recipient
// ignore the phrase entirely, and inventing one for a code we never emit would
// be a table nobody maintains - the default covers anything new.
//
static const char* reasonPhrase(int code)
{
  switch (code)
  {
  case 200: return "OK";
  case 201: return "Created";
  case 204: return "No Content";
  case 207: return "Multi-Status";
  case 400: return "Bad Request";
  case 403: return "Forbidden";
  case 404: return "Not Found";
  case 405: return "Method Not Allowed";
  case 406: return "Not Acceptable";
  case 409: return "Conflict";
  case 411: return "Length Required";
  case 413: return "Content Too Large";
  case 415: return "Unsupported Media Type";
  case 422: return "Unprocessable Content";
  case 431: return "Request Header Fields Too Large";
  case 500: return "Internal Server Error";
  case 501: return "Not Implemented";
  case 503: return "Service Unavailable";
  case 504: return "Gateway Timeout";
  case 508: return "Loop Detected";
  }

  return "Unknown";
}



// -----------------------------------------------------------------------------
//
// corHttpResponseStatus -
//
void corHttpResponseStatus(CorHttpConn* connP, int statusCode)
{
  connP->statusCode = statusCode;
}



// -----------------------------------------------------------------------------
//
// corHttpResponseHeader -
//
// Neither key nor value is copied. Both must outlive the callback, which means
// a literal or something from connP->alloc - the same rule as the body, and for
// the same reason: this library does not own a second copy of anything.
//
// Over the limit the header is DROPPED rather than growing the array. A
// response that needs more than COR_HTTP_MAX_HEADERS headers is a bug in the
// caller, and dropping the last one is a far smaller lie than refusing to
// answer at all.
//
void corHttpResponseHeader(CorHttpConn* connP, const char* key, const char* value)
{
  if (connP->respHeaders >= COR_HTTP_MAX_HEADERS)
    return;

  CorHttpKeyValue* kvP = &connP->respHeader[connP->respHeaders];

  kvP->key.s     = (char*) key;
  kvP->key.len   = strlen(key);
  kvP->value.s   = (char*) value;
  kvP->value.len = strlen(value);

  connP->respHeaders++;
}



// -----------------------------------------------------------------------------
//
// corHttpResponseBody -
//
void corHttpResponseBody(CorHttpConn* connP, char* body, int bodyLen)
{
  connP->respBody    = body;
  connP->respBodyLen = bodyLen;
}



// -----------------------------------------------------------------------------
//
// corHttpResponseRender -
//
CorHttpStatus corHttpResponseRender(CorHttpConn* connP)
{
  //
  // A callback that set no status produced no answer, and that is a bug in the
  // caller rather than something to paper over with an empty 200. 500 says so.
  //
  if (connP->statusCode == 0)
    connP->statusCode = 500;

  //
  // Size the buffer before writing a byte of it. Every part is known: the
  // status line, each header as "key: value\r\n", the blank line, the body.
  // Guessing and growing would mean a realloc in the middle of rendering, on
  // the one path that runs for every single response.
  //
  int size = 64;                                 // status line, comfortably

  for (int ix = 0; ix < connP->respHeaders; ix++)
    size += connP->respHeader[ix].key.len + connP->respHeader[ix].value.len + 4;

  size += 32;                                    // Content-Length
  size += 24;                                    // Connection
  size += 2;                                     // the blank line
  size += connP->respBodyLen;

  if (connP->writeBuf != NULL)
    free(connP->writeBuf);

  connP->writeBuf = (char*) malloc(size);

  if (connP->writeBuf == NULL)
    return CorHttpOutOfMemory;

  char* p    = connP->writeBuf;
  char* end  = connP->writeBuf + size;

  p += snprintf(p, end - p, "HTTP/1.1 %d %s\r\n", connP->statusCode, reasonPhrase(connP->statusCode));

  for (int ix = 0; ix < connP->respHeaders; ix++)
  {
    memcpy(p, connP->respHeader[ix].key.s, connP->respHeader[ix].key.len);
    p += connP->respHeader[ix].key.len;
    *p++ = ':';
    *p++ = ' ';
    memcpy(p, connP->respHeader[ix].value.s, connP->respHeader[ix].value.len);
    p += connP->respHeader[ix].value.len;
    *p++ = '\r';
    *p++ = '\n';
  }

  //
  // Content-Length ALWAYS, including zero. Without it a client has to read
  // until the connection closes to know the body ended, which defeats
  // keep-alive - and a 204 with no length and no close is a client that hangs.
  //
  p += snprintf(p, end - p, "Content-Length: %d\r\n", connP->respBodyLen);

  //
  // And the keep-alive decision, stated rather than implied. HTTP/1.1 defaults
  // to persistent, but a client that asked to close and is not told "close"
  // has to guess whether we honoured it.
  //
  p += snprintf(p, end - p, "Connection: %s\r\n", connP->keepAlive ? "keep-alive" : "close");

  *p++ = '\r';
  *p++ = '\n';

  if (connP->respBodyLen > 0)
  {
    memcpy(p, connP->respBody, connP->respBodyLen);
    p += connP->respBodyLen;
  }

  connP->writeLen = (int) (p - connP->writeBuf);
  connP->writePos = 0;

  return CorHttpOk;
}
