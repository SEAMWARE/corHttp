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
// ⚠️ THE BYTE ORDER HERE IS NOT A CHOICE. This server is a drop-in replacement
// for another one, and several hundred functional tests compare captured HTTP
// responses line by line - so the header SET, their ORDER and their FORMAT are
// a specification, not a preference. Measured from the responses being
// replaced:
//
//     HTTP/1.1 200 OK
//     Date: Sat, 05 Sep 2026 15:54:13 GMT      <- always, immediately after
//     Connection: close                        <- ONLY when closing
//     Content-Type: application/json           <- the caller's, in order added
//     Link: <...>
//     Content-Length: 765                      <- always LAST
//
// and the two rules that are easy to get wrong because they are absences:
//
//   - NO `Connection: keep-alive`. HTTP/1.1 is persistent by default and
//     saying so explicitly is redundant; the header appears only to announce
//     the opposite. Emitting it on every response would fail every test.
//   - NO Content-Length on a 204. Every other status gets one, INCLUDING 0
//     (a 201 says `Content-Length: 0`), so "omit when the body is empty" is
//     the wrong rule - it is the status that decides.
//
#include <errno.h>                               // errno, EAGAIN, EINTR
#include <stdio.h>                               // snprintf
#include <stdlib.h>                              // malloc, realloc
#include <string.h>                              // memcpy, strcmp, strlen
#include <time.h>                                // time, gmtime_r
#include <unistd.h>                              // write

#include "corHttp/CorHttp.h"                     // CorHttpConn
#include "corHttp/corHttpInternal.h"             // Own interface



// -----------------------------------------------------------------------------
//
// reasonPhrase - the text after the status code
//
// Only the codes this broker actually sends, and the spelling matters: the
// phrase is compared verbatim by the test suite, so "Content Too Large" and
// "Unprocessable Content" are the RFC 9110 names rather than the older
// "Request Entity Too Large" / "Unprocessable Entity". RFC 9112 § 4 lets a
// recipient ignore the phrase, but the tests are not a recipient.
//
static const char* reasonPhrase(int code)
{
  switch (code)
  {
  case 100: return "Continue";
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
  case 502: return "Bad Gateway";
  case 501: return "Not Implemented";
  case 503: return "Service Unavailable";
  case 504: return "Gateway Timeout";
  case 508: return "Loop Detected";
  }

  return "Unknown";
}



// -----------------------------------------------------------------------------
//
// httpDate - "Sat, 05 Sep 2026 15:54:13 GMT"
//
// Hand-rolled rather than strftime("%a, %d %b %Y %H:%M:%S GMT"): %a and %b are
// LOCALE-DEPENDENT, so on a broker started under a non-English locale strftime
// would produce a date field no HTTP client is required to parse - and one the
// test suite would not recognise. RFC 9110 § 5.6.7 fixes these spellings.
//
static void httpDate(char* buf, int bufSize)
{
  static const char* day[]   = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
  static const char* month[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
  struct tm  tm;
  time_t     now = time(NULL);

  gmtime_r(&now, &tm);

  snprintf(buf, bufSize, "%s, %02d %s %04d %02d:%02d:%02d GMT",
           day[tm.tm_wday], tm.tm_mday, month[tm.tm_mon], tm.tm_year + 1900,
           tm.tm_hour, tm.tm_min, tm.tm_sec);
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
  // 204 carries no representation at all, and no Content-Length to describe the
  // one it does not have. 1xx likewise. Every other status gets one, zero
  // included.
  //
  bool withLength = (connP->statusCode != 204) && (connP->statusCode >= 200);

  //
  // HEAD: the headers of the GET, and none of its body - Content-Length still
  // describes what a GET would have returned (RFC 9110 § 9.3.2). So the length
  // is rendered and the bytes are not.
  //
  bool bodyOut = (connP->method.s != NULL) && (strcmp(connP->method.s, "HEAD") != 0);

  //
  // Size the buffer before writing a byte of it. Every part is known, so
  // guessing and growing would mean a realloc in the middle of rendering, on
  // the one path that runs for every single response.
  //
  int size = 64;                                 // status line, comfortably

  size += 40;                                    // Date
  size += 21;                                    // Connection: close
  size += 32;                                    // Content-Length

  for (int ix = 0; ix < connP->respHeaders; ix++)
    size += connP->respHeader[ix].key.len + connP->respHeader[ix].value.len + 4;

  size += 2;                                     // the blank line

  if (bodyOut == true)
    size += connP->respBodyLen;

  if (connP->writeBuf != NULL)
    free(connP->writeBuf);

  connP->writeBuf = (char*) malloc(size);

  if (connP->writeBuf == NULL)
    return CorHttpOutOfMemory;

  char* p   = connP->writeBuf;
  char* end = connP->writeBuf + size;

  p += snprintf(p, end - p, "HTTP/1.1 %d %s\r\n", connP->statusCode, reasonPhrase(connP->statusCode));

  char dateBuf[40];

  httpDate(dateBuf, sizeof(dateBuf));
  p += snprintf(p, end - p, "Date: %s\r\n", dateBuf);

  if (connP->keepAlive == false)
    p += snprintf(p, end - p, "Connection: close\r\n");

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

  if (withLength == true)
    p += snprintf(p, end - p, "Content-Length: %d\r\n", connP->respBodyLen);

  *p++ = '\r';
  *p++ = '\n';

  if ((bodyOut == true) && (connP->respBodyLen > 0))
  {
    memcpy(p, connP->respBody, connP->respBodyLen);
    p += connP->respBodyLen;
  }

  connP->writeLen = (int) (p - connP->writeBuf);
  connP->writePos = 0;

  return CorHttpOk;
}



// -----------------------------------------------------------------------------
//
// corHttpContinueSend - the interim 100 response, before the body is read
//
// A client that sent `Expect: 100-continue` is WAITING and will not send the
// body until it hears this (or times out, which curl does after a second - the
// delay is the visible symptom of forgetting it). It is a complete response of
// its own on the wire, ahead of the real one.
//
CorHttpStatus corHttpContinueSend(CorHttpConn* connP)
{
  static const char  continue100[] = "HTTP/1.1 100 Continue\r\n\r\n";
  const int          len           = sizeof(continue100) - 1;
  int                written       = 0;

  while (written < len)
  {
    ssize_t n = write(connP->fd, continue100 + written, len - written);

    if (n < 0)
    {
      if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
        continue;                                // 25 bytes; the socket buffer will take them
      if (errno == EINTR)
        continue;
      return CorHttpError;
    }

    written += n;
  }

  return CorHttpOk;
}
