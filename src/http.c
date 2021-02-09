#include "arch.h"
#include "base64.h"
#include "http.h"
#include "log.h"
#include "net.h"
#include "private.h"
#include "ssi.h"
#include "util.h"
#include "version.h"
#include "ws.h"

struct http_data {
  void *old_pfn_data;  // Previous pfn_data
  FILE *fp;            // For static file serving
};

// Multipart POST example:
// https://gist.github.com/cpq/b8dd247571e6ee9c54ef7e8dfcfecf48

void mg_http_bauth(struct mg_connection *c, const char *user,
                   const char *pass) {
  struct mg_str u = mg_str(user), p = mg_str(pass);
  size_t need = c->send.len + 36 + (u.len + p.len) * 2;
  if (c->send.size < need) mg_iobuf_resize(&c->send, need);
  if (c->send.size >= need) {
    size_t i, n = 0;
    char *buf = (char *) &c->send.buf[c->send.len + 21];
    memcpy(&buf[-21], "Authorization: Basic ", 21);  // DON'T use mg_send!
    for (i = 0; i < u.len; i++) n = mg_base64_update(u.ptr[i], buf, n);
    if (p.len > 0) {
      n = mg_base64_update(':', buf, n);
      for (i = 0; i < p.len; i++) n = mg_base64_update(p.ptr[i], buf, n);
    }
    n = mg_base64_final(buf, n);
    c->send.len += 21 + n + 2;
    memcpy(&c->send.buf[c->send.len - 2], "\r\n", 2);
  } else {
    LOG(LL_ERROR, ("%lu %s cannot resize iobuf %d->%d ", c->id, c->label,
                   (int) c->send.size, (int) need));
  }
}

int mg_http_get_var(const struct mg_str *buf, const char *name, char *dst,
                    int dst_len) {
  const char *p, *e, *s;
  size_t name_len;
  int len;

  if (dst == NULL || dst_len == 0) {
    len = -2;  // Bad destination
  } else if (buf->ptr == NULL || name == NULL || buf->len == 0) {
    len = -1;  // Bad source
    dst[0] = '\0';
  } else {
    name_len = strlen(name);
    e = buf->ptr + buf->len;
    len = -4;  // Name does not exist
    dst[0] = '\0';
    for (p = buf->ptr; p + name_len < e; p++) {
      if ((p == buf->ptr || p[-1] == '&') && p[name_len] == '=' &&
          !mg_ncasecmp(name, p, name_len)) {
        p += name_len + 1;
        s = (const char *) memchr(p, '&', (size_t)(e - p));
        if (s == NULL) s = e;
        len = mg_url_decode(p, (size_t)(s - p), dst, dst_len, 1);
        if (len < 0) len = -3;  // Failed to decode
        break;
      }
    }
  }
  return len;
}

int mg_url_decode(const char *src, size_t src_len, char *dst, size_t dst_len,
                  int is_form_url_encoded) {
  size_t i, j;
  for (i = j = 0; i < src_len && j + 1 < dst_len; i++, j++) {
    if (src[i] == '%') {
      // Use `i + 2 < src_len`, not `i < src_len - 2`, note small src_len
      if (i + 2 < src_len && isxdigit(*(const unsigned char *) (src + i + 1)) &&
          isxdigit(*(const unsigned char *) (src + i + 2))) {
        mg_unhex(src + i + 1, 2, (uint8_t *) &dst[j]);
        i += 2;
      } else {
        return -1;
      }
    } else if (is_form_url_encoded && src[i] == '+') {
      dst[j] = ' ';
    } else {
      dst[j] = src[i];
    }
  }
  if (j < dst_len) dst[j] = '\0';  // Null-terminate the destination
  return i >= src_len && j < dst_len ? (int) j : -1;
}

int mg_http_get_request_len(const unsigned char *buf, size_t buf_len) {
  size_t i;
  for (i = 0; i < buf_len; i++) {
    if (!isprint(buf[i]) && buf[i] != '\r' && buf[i] != '\n' && buf[i] < 128)
      return -1;
    if ((i > 0 && buf[i] == '\n' && buf[i - 1] == '\n') ||
        (i > 3 && buf[i] == '\n' && buf[i - 1] == '\r' && buf[i - 2] == '\n'))
      return (int) i + 1;
  }
  return 0;
}

static const char *skip(const char *s, const char *end, const char *delims,
                        struct mg_str *v) {
  v->ptr = s;
  while (s < end && strchr(delims, *(unsigned char *) s) == NULL) s++;
  v->len = s - v->ptr;
  while (s < end && strchr(delims, *(unsigned char *) s) != NULL) s++;
  return s;
}

struct mg_str *mg_http_get_header(struct mg_http_message *h, const char *name) {
  size_t i, n = strlen(name), max = sizeof(h->headers) / sizeof(h->headers[0]);
  for (i = 0; i < max && h->headers[i].name.len > 0; i++) {
    struct mg_str *k = &h->headers[i].name, *v = &h->headers[i].value;
    if (n == k->len && mg_ncasecmp(k->ptr, name, n) == 0) return v;
  }
  return NULL;
}

void mg_http_parse_headers(const char *s, const char *end,
                           struct mg_http_header *h, int max_headers) {
  int i;
  for (i = 0; i < max_headers; i++) {
    struct mg_str k, v, tmp;
    const char *he = skip(s, end, "\n", &tmp);
    s = skip(s, he, ": \r\n", &k);
    s = skip(s, he, "\r\n", &v);
    if (k.len == tmp.len) continue;
    while (v.len > 0 && v.ptr[v.len - 1] == ' ') v.len--;  // Trim spaces
    if (k.len == 0) break;
    // LOG(LL_INFO, ("--HH [%.*s] [%.*s] [%.*s]", (int) tmp.len - 1, tmp.ptr,
    //(int) k.len, k.ptr, (int) v.len, v.ptr));
    h[i].name = k;
    h[i].value = v;
  }
}

int mg_http_parse(const char *s, size_t len, struct mg_http_message *hm) {
  int is_response, req_len = mg_http_get_request_len((unsigned char *) s, len);
  const char *end = s + req_len, *qs;
  struct mg_str *cl;

  memset(hm, 0, sizeof(*hm));
  if (req_len <= 0) return req_len;

  hm->message.ptr = hm->head.ptr = s;
  hm->body.ptr = end;
  hm->head.len = req_len;
  hm->message.len = hm->body.len = (size_t) ~0;  // Set body length to infinite

  // Parse request line
  s = skip(s, end, " ", &hm->method);
  s = skip(s, end, " ", &hm->uri);
  s = skip(s, end, "\r\n", &hm->proto);
  if (hm->uri.ptr <= hm->method.ptr || hm->proto.ptr <= hm->uri.ptr) return -1;

  // If URI contains '?' character, setup query string
  if ((qs = (const char *) memchr(hm->uri.ptr, '?', hm->uri.len)) != NULL) {
    hm->query.ptr = qs + 1;
    hm->query.len = &hm->uri.ptr[hm->uri.len] - (qs + 1);
    hm->uri.len = qs - hm->uri.ptr;
  }

  mg_http_parse_headers(s, end, hm->headers,
                        sizeof(hm->headers) / sizeof(hm->headers[0]));
  if ((cl = mg_http_get_header(hm, "Content-Length")) != NULL) {
    hm->body.len = (size_t) mg_to64(*cl);
    hm->message.len = req_len + hm->body.len;
  }

  // mg_http_parse() is used to parse both HTTP requests and HTTP
  // responses. If HTTP response does not have Content-Length set, then
  // body is read until socket is closed, i.e. body.len is infinite (~0).
  //
  // For HTTP requests though, according to
  // http://tools.ietf.org/html/rfc7231#section-8.1.3,
  // only POST and PUT methods have defined body semantics.
  // Therefore, if Content-Length is not specified and methods are
  // not one of PUT or POST, set body length to 0.
  //
  // So, if it is HTTP request, and Content-Length is not set,
  // and method is not (PUT or POST) then reset body length to zero.
  is_response = mg_ncasecmp(hm->method.ptr, "HTTP/", 5) == 0;
  if (hm->body.len == (size_t) ~0 && !is_response &&
      mg_vcasecmp(&hm->method, "PUT") != 0 &&
      mg_vcasecmp(&hm->method, "POST") != 0) {
    hm->body.len = 0;
    hm->message.len = req_len;
  }

  // The 204 (No content) responses also have 0 body length
  if (hm->body.len == (size_t) ~0 && is_response &&
      mg_vcasecmp(&hm->uri, "204") == 0) {
    hm->body.len = 0;
    hm->message.len = req_len;
  }

  return req_len;
}

static void mg_http_vprintf_chunk(struct mg_connection *c, const char *fmt,
                                  va_list ap) {
  char mem[256], *buf = mem;
  int len = mg_vasprintf(&buf, sizeof(mem), fmt, ap);
  mg_printf(c, "%X\r\n", len);
  mg_send(c, buf, len);
  mg_send(c, "\r\n", 2);
  if (buf != mem) free(buf);
}

void mg_http_printf_chunk(struct mg_connection *c, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  mg_http_vprintf_chunk(c, fmt, ap);
  va_end(ap);
}

void mg_http_write_chunk(struct mg_connection *c, const char *buf, size_t len) {
  mg_printf(c, "%X\r\n", len);
  mg_send(c, buf, len);
  mg_send(c, "\r\n", 2);
}

void mg_http_reply(struct mg_connection *c, int code, const char *headers,
                   const char *fmt, ...) {
  char mem[100], *buf = mem;
  va_list ap;
  int len;
  va_start(ap, fmt);
  len = mg_vasprintf(&buf, sizeof(mem), fmt, ap);
  va_end(ap);
  mg_printf(c, "HTTP/1.1 %d OK\r\n%sContent-Length: %d\r\n\r\n", code,
            headers == NULL ? "" : headers, len);
  mg_send(c, buf, len);
  if (buf != mem) free(buf);
}

#if MG_ENABLE_FS
static void http_cb(struct mg_connection *, int, void *, void *);
static void restore_http_cb(struct mg_connection *c) {
  struct http_data *d = (struct http_data *) c->pfn_data;
  if (d->fp != NULL) fclose(d->fp);
  c->pfn_data = d->old_pfn_data;
  c->pfn = http_cb;
  free(d);
}

char *mg_http_etag(char *buf, size_t len, mg_stat_t *st) {
  snprintf(buf, len, "\"%lx." MG_INT64_FMT "\"", (unsigned long) st->st_mtime,
           (int64_t) st->st_size);
  return buf;
}

int mg_http_upload(struct mg_connection *c, struct mg_http_message *hm,
                   const char *dir) {
  char offset[40] = "", name[200] = "", path[256];
  mg_http_get_var(&hm->query, "offset", offset, sizeof(offset));
  mg_http_get_var(&hm->query, "name", name, sizeof(name));
  if (name[0] == '\0') {
    mg_http_reply(c, 400, "", "%s", "name required");
    return -1;
  } else {
    FILE *fp;
    size_t oft = strtoul(offset, NULL, 0);
    snprintf(path, sizeof(path), "%s%c%s", dir, MG_DIRSEP, name);
    LOG(LL_DEBUG,
        ("%p %d bytes @ %d [%s]", c->fd, (int) hm->body.len, (int) oft, name));
    if ((fp = mg_fopen(path, oft == 0 ? "wb" : "ab")) == NULL) {
      mg_http_reply(c, 400, "", "fopen(%s): %d", name, errno);
      return -2;
    } else {
      fwrite(hm->body.ptr, 1, hm->body.len, fp);
      fclose(fp);
      mg_http_reply(c, 200, "", "");
      return hm->body.len;
    }
  }
}

static void static_cb(struct mg_connection *c, int ev, void *ev_data,
                      void *fn_data) {
  if (ev == MG_EV_WRITE || ev == MG_EV_POLL) {
    struct http_data *d = (struct http_data *) fn_data;
    // Read to send IO buffer directly, avoid extra on-stack buffer
    size_t n, max = 2 * MG_IO_SIZE;
    if (c->send.size < max) mg_iobuf_resize(&c->send, max);
    if (c->send.len >= c->send.size) return;  // Rate limit
    n = fread(c->send.buf + c->send.len, 1, c->send.size - c->send.len, d->fp);
    if (n > 0) c->send.len += n;
    if (c->send.len < c->send.size) restore_http_cb(c);
  } else if (ev == MG_EV_CLOSE) {
    restore_http_cb(c);
  }
  (void) ev_data;
}

static const char *guess_content_type(const char *filename) {
  size_t n = strlen(filename);
#define MIME_ENTRY(_ext, _type) \
  { _ext, sizeof(_ext) - 1, _type }
  const struct {
    const char *ext;
    size_t ext_len;
    const char *type;
  } * t, types[] = {
             MIME_ENTRY("html", "text/html; charset=utf-8"),
             MIME_ENTRY("htm", "text/html; charset=utf-8"),
             MIME_ENTRY("shtml", "text/html; charset=utf-8"),
             MIME_ENTRY("css", "text/css"),
             MIME_ENTRY("js", "text/javascript"),
             MIME_ENTRY("mjs", "text/javascript"),
             MIME_ENTRY("json", "application/json"),
             MIME_ENTRY("ico", "image/x-icon"),
             MIME_ENTRY("gif", "image/gif"),
             MIME_ENTRY("jpg", "image/jpeg"),
             MIME_ENTRY("jpeg", "image/jpeg"),
             MIME_ENTRY("png", "image/png"),
             MIME_ENTRY("svg", "image/svg+xml"),
             MIME_ENTRY("txt", "text/plain; charset=utf-8"),
             MIME_ENTRY("wav", "audio/wav"),
             MIME_ENTRY("mp3", "audio/mpeg"),
             MIME_ENTRY("mid", "audio/mid"),
             MIME_ENTRY("ogg", "application/ogg"),
             MIME_ENTRY("xml", "application/xml"),
             MIME_ENTRY("ttf", "font/ttf"),
             MIME_ENTRY("json", "application/json"),
             MIME_ENTRY("xsl", "application/xml"),
             MIME_ENTRY("doc", "application/msword"),
             MIME_ENTRY("exe", "application/octet-stream"),
             MIME_ENTRY("zip", "application/zip"),
             MIME_ENTRY("xls", "application/excel"),
             MIME_ENTRY("tgz", "application/tar-gz"),
             MIME_ENTRY("tar", "application/tar"),
             MIME_ENTRY("gz", "application/gzip"),
             MIME_ENTRY("rar", "application/rar"),
             MIME_ENTRY("rtf", "application/rtf"),
             MIME_ENTRY("pdf", "application/pdf"),
             MIME_ENTRY("mpg", "video/mpeg"),
             MIME_ENTRY("webm", "video/webm"),
             MIME_ENTRY("mpeg", "video/mpeg"),
             MIME_ENTRY("mov", "video/quicktime"),
             MIME_ENTRY("mp4", "video/mp4"),
             MIME_ENTRY("avi", "video/x-msvideo"),
             MIME_ENTRY("csv", "text/csv"),
             MIME_ENTRY("bmp", "image/bmp"),
             MIME_ENTRY("bin", "application/octet-stream"),
             MIME_ENTRY("wasm", "application/wasm"),
             {NULL, 0, NULL},
         };

  for (t = types; t->ext != NULL; t++) {
    if (n < t->ext_len + 2) continue;
    if (mg_ncasecmp(t->ext, &filename[n - t->ext_len], t->ext_len)) continue;
    return t->type;
  }
  return "text/plain; charset=utf-8";
}

void mg_http_serve_file(struct mg_connection *c, struct mg_http_message *hm,
                        const char *path, const char *mime, const char *hdrs) {
  struct mg_str *inm = mg_http_get_header(hm, "If-None-Match");
  mg_stat_t st;
  char etag[64];
  FILE *fp = mg_fopen(path, "rb");
  if (fp == NULL || mg_stat(path, &st) != 0 ||
      mg_http_etag(etag, sizeof(etag), &st) != etag) {
    LOG(LL_DEBUG,
        ("404 [%.*s] [%s] %p", (int) hm->uri.len, hm->uri.ptr, path, fp));
    mg_http_reply(c, 404, "", "%s", "Not found\n");
    if (fp != NULL) fclose(fp);
  } else if (inm != NULL && mg_vcasecmp(inm, etag) == 0) {
    fclose(fp);
    mg_printf(c, "HTTP/1.1 304 Not Modified\r\nContent-Length: 0\r\n\r\n");
  } else {
    mg_printf(c,
              "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
              "Etag: %s\r\nContent-Length: " MG_INT64_FMT "\r\n%s\r\n",
              mime, etag, (int64_t) st.st_size, hdrs ? hdrs : "");
    if (mg_vcasecmp(&hm->method, "HEAD") == 0) {
      fclose(fp);
    } else {
      struct http_data *d = (struct http_data *) calloc(1, sizeof(*d));
      if (d == NULL) {
        mg_error(c, "static HTTP OOM");
      } else {
        d->fp = fp;
        d->old_pfn_data = c->pfn_data;
        c->pfn = static_cb;
        c->pfn_data = d;
      }
    }
  }
}

#if MG_ARCH == MG_ARCH_ESP32 || MG_ARCH == MG_ARCH_ESP8266 || \
    MG_ARCH == MG_ARCH_FREERTOS
char *realpath(const char *src, char *dst) {
  int len = strlen(src);
  if (len > MG_PATH_MAX - 1) len = MG_PATH_MAX - 1;
  strncpy(dst, src, len);
  dst[len] = '\0';
  LOG(LL_DEBUG, ("[%s] -> [%s]", src, dst));
  return dst;
}
#endif

// Allow user to override this function
bool mg_is_dir(const char *path) WEAK;
bool mg_is_dir(const char *path) {
#if MG_ARCH == MG_ARCH_FREERTOS
  struct FF_STAT st;
  return (ff_stat(path, &st) == 0) && (st.st_mode & FF_IFDIR);
#else
  mg_stat_t st;
  return mg_stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

#if MG_ENABLE_DIRECTORY_LISTING

#ifdef _WIN32
struct dirent {
  char d_name[MAX_PATH];
};

typedef struct win32_dir {
  HANDLE handle;
  WIN32_FIND_DATAW info;
  struct dirent result;
} DIR;

int gettimeofday(struct timeval *tv, void *tz) {
  FILETIME ft;
  unsigned __int64 tmpres = 0;

  if (tv != NULL) {
    GetSystemTimeAsFileTime(&ft);
    tmpres |= ft.dwHighDateTime;
    tmpres <<= 32;
    tmpres |= ft.dwLowDateTime;
    tmpres /= 10;  // convert into microseconds
    tmpres -= (int64_t) 11644473600000000;
    tv->tv_sec = (long) (tmpres / 1000000UL);
    tv->tv_usec = (long) (tmpres % 1000000UL);
  }
  (void) tz;
  return 0;
}

static int to_wchar(const char *path, wchar_t *wbuf, size_t wbuf_len) {
  int ret;
  char buf[MAX_PATH * 2], buf2[MAX_PATH * 2], *p;
  strncpy(buf, path, sizeof(buf));
  buf[sizeof(buf) - 1] = '\0';
  // Trim trailing slashes. Leave backslash for paths like "X:\"
  p = buf + strlen(buf) - 1;
  while (p > buf && p[-1] != ':' && (p[0] == '\\' || p[0] == '/')) *p-- = '\0';
  memset(wbuf, 0, wbuf_len * sizeof(wchar_t));
  ret = MultiByteToWideChar(CP_UTF8, 0, buf, -1, wbuf, (int) wbuf_len);
  // Convert back to Unicode. If doubly-converted string does not match the
  // original, something is fishy, reject.
  WideCharToMultiByte(CP_UTF8, 0, wbuf, (int) wbuf_len, buf2, sizeof(buf2),
                      NULL, NULL);
  if (strcmp(buf, buf2) != 0) {
    wbuf[0] = L'\0';
    ret = 0;
  }
  return ret;
}

DIR *opendir(const char *name) {
  DIR *d = NULL;
  wchar_t wpath[MAX_PATH];
  DWORD attrs;

  if (name == NULL) {
    SetLastError(ERROR_BAD_ARGUMENTS);
  } else if ((d = (DIR *) malloc(sizeof(*d))) == NULL) {
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
  } else {
    to_wchar(name, wpath, sizeof(wpath) / sizeof(wpath[0]));
    attrs = GetFileAttributesW(wpath);
    if (attrs != 0Xffffffff && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
      (void) wcscat(wpath, L"\\*");
      d->handle = FindFirstFileW(wpath, &d->info);
      d->result.d_name[0] = '\0';
    } else {
      free(d);
      d = NULL;
    }
  }
  return d;
}

int closedir(DIR *d) {
  int result = 0;
  if (d != NULL) {
    if (d->handle != INVALID_HANDLE_VALUE)
      result = FindClose(d->handle) ? 0 : -1;
    free(d);
  } else {
    result = -1;
    SetLastError(ERROR_BAD_ARGUMENTS);
  }
  return result;
}

struct dirent *readdir(DIR *d) {
  struct dirent *result = NULL;
  if (d != NULL) {
    memset(&d->result, 0, sizeof(d->result));
    if (d->handle != INVALID_HANDLE_VALUE) {
      result = &d->result;
      WideCharToMultiByte(CP_UTF8, 0, d->info.cFileName, -1, result->d_name,
                          sizeof(result->d_name), NULL, NULL);
      if (!FindNextFileW(d->handle, &d->info)) {
        FindClose(d->handle);
        d->handle = INVALID_HANDLE_VALUE;
      }
    } else {
      SetLastError(ERROR_FILE_NOT_FOUND);
    }
  } else {
    SetLastError(ERROR_BAD_ARGUMENTS);
  }
  return result;
}
#endif

static bool mg_is_safe(int c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
         (c >= 'A' && c <= 'Z') || strchr("._-$,;~()/", c) != NULL;
}

static int mg_url_encode(const char *s, char *buf, size_t len) {
  int i, n = 0;
  for (i = 0; s[i] != '\0'; i++) {
    int c = *((const unsigned char *) (s + i));
    if ((size_t) n + 4 >= len) return 0;
    if (mg_is_safe(c)) {
      buf[n++] = s[i];
    } else {
      buf[n++] = '%';
      mg_hex(&s[i], 1, &buf[n]);
      n += 2;
    }
  }
  return n;
}

static void printdirentry(struct mg_connection *c, const char *name,
                          mg_stat_t *stp) {
  char size[64], mod[64], path[MG_PATH_MAX];
  int is_dir = S_ISDIR(stp->st_mode);
  const char *slash = is_dir ? "/" : "";
  // const char *es = hm->uri.ptr[hm->uri.len - 1] != '/' ? "/" : "";
  int n = 0;

  if (is_dir) {
    snprintf(size, sizeof(size), "%s", "[DIR]");
  } else {
    if (stp->st_size < 1024) {
      snprintf(size, sizeof(size), "%d", (int) stp->st_size);
    } else if (stp->st_size < 0x100000) {
      snprintf(size, sizeof(size), "%.1fk", (double) stp->st_size / 1024.0);
    } else if (stp->st_size < 0x40000000) {
      snprintf(size, sizeof(size), "%.1fM", (double) stp->st_size / 1048576);
    } else {
      snprintf(size, sizeof(size), "%.1fG", (double) stp->st_size / 1073741824);
    }
  }
  strftime(mod, sizeof(mod), "%d-%b-%Y %H:%M", localtime(&stp->st_mtime));
  n = mg_url_encode(name, path, sizeof(path));
  mg_printf(c,
            "  <tr><td><a href=\"%.*s%s\">%s%s</a></td>"
            "<td>%s</td><td>%s</td></tr>\n",
            n, path, slash, name, slash, mod, size);
}

static void listdir(struct mg_connection *c, struct mg_http_message *hm,
                    char *dir) {
  char path[MG_PATH_MAX], *p = &dir[strlen(dir) - 1], tmp[10];
  struct dirent *dp;
  DIR *dirp;
  static const char *sort_js_code =
      "<script>function srt(tb, sc, so, d) {"
      "var tr = Array.prototype.slice.call(tb.rows, 0),"
      "tr = tr.sort(function (a, b) { var c1 = a.cells[sc], c2 = b.cells[sc],"
      "n1 = c1.getAttribute('name'), n2 = c2.getAttribute('name'), "
      "t1 = a.cells[2].getAttribute('name'), "
      "t2 = b.cells[2].getAttribute('name'); "
      "return so * (t1 < 0 && t2 >= 0 ? -1 : t2 < 0 && t1 >= 0 ? 1 : "
      "n1 ? parseInt(n2) - parseInt(n1) : "
      "c1.textContent.trim().localeCompare(c2.textContent.trim())); });";
  static const char *sort_js_code2 =
      "for (var i = 0; i < tr.length; i++) tb.appendChild(tr[i]); "
      "if (!d) window.location.hash = ('sc=' + sc + '&so=' + so); "
      "};"
      "window.onload = function() {"
      "var tb = document.getElementById('tb');"
      "var m = /sc=([012]).so=(1|-1)/.exec(window.location.hash) || [0, 2, 1];"
      "var sc = m[1], so = m[2]; document.onclick = function(ev) { "
      "var c = ev.target.rel; if (c) {if (c == sc) so *= -1; srt(tb, c, so); "
      "sc = c; ev.preventDefault();}};"
      "srt(tb, sc, so, true);"
      "}"
      "</script>";

  while (p > dir && *p != '/') *p-- = '\0';
  if ((dirp = (opendir(dir))) != NULL) {
    size_t off, n;
    mg_printf(c, "%s\r\n",
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: text/html; charset=utf-8\r\n"
              "Content-Length:         \r\n");
    off = c->send.len;  // Start of body
    mg_printf(c,
              "<!DOCTYPE html><html><head><title>Index of %.*s</title>%s%s"
              "<style>th,td {text-align: left; padding-right: 1em; "
              "font-family: monospace; }</style></head>"
              "<body><h1>Index of %.*s</h1><table cellpadding=\"0\"><thead>"
              "<tr><th><a href=\"#\" rel=\"0\">Name</a></th><th>"
              "<a href=\"#\" rel=\"1\">Modified</a></th>"
              "<th><a href=\"#\" rel=\"2\">Size</a></th></tr>"
              "<tr><td colspan=\"3\"><hr></td></tr>"
              "</thead>"
              "<tbody id=\"tb\">\n",
              (int) hm->uri.len, hm->uri.ptr, sort_js_code, sort_js_code2,
              (int) hm->uri.len, hm->uri.ptr);

    while ((dp = readdir(dirp)) != NULL) {
      mg_stat_t st;
      const char *sep = dp->d_name[0] == MG_DIRSEP ? "/" : "";
      // Do not show current dir and hidden files
      if (!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, "..")) continue;
      // SPIFFS can report "/foo.txt" in the dp->d_name
      if (snprintf(path, sizeof(path), "%s%s%s", dir, sep, dp->d_name) < 0) {
        LOG(LL_ERROR, ("%s truncated", dp->d_name));
      } else if (mg_stat(path, &st) != 0) {
        LOG(LL_ERROR, ("%lu stat(%s): %d", c->id, path, errno));
      } else {
        printdirentry(c, dp->d_name, &st);
      }
    }
    closedir(dirp);
    mg_printf(c,
              "</tbody><tfoot><tr><td colspan=\"3\"><hr></td></tr></tfoot>"
              "</table><address>Mongoose v.%s</address></body></html>\n",
              MG_VERSION);
    n = snprintf(tmp, sizeof(tmp), "%lu", (unsigned long) (c->send.len - off));
    memcpy(c->send.buf + off - 10, tmp, n);  // Set content length
  } else {
    mg_http_reply(c, 400, "", "Cannot open dir");
    LOG(LL_ERROR, ("%lu opendir(%s) -> %d", c->id, dir, errno));
  }
}
#endif

void mg_http_serve_dir(struct mg_connection *c, struct mg_http_message *hm,
                       struct mg_http_serve_opts *opts) {
  char t1[MG_PATH_MAX], t2[sizeof(t1)];
  t1[0] = t2[0] = '\0';

  if (realpath(opts->root_dir, t1) == NULL)
    LOG(LL_ERROR, ("realpath(%s): %d", opts->root_dir, errno));

  if (!mg_is_dir(t1)) {
    mg_http_reply(c, 400, "", "Bad web root [%s]\n", t1);
  } else {
    // NOTE(lsm): Xilinx snprintf does not 0-terminate the detination for
    // the %.*s specifier, if the length is zero. Make sure hm->uri.len > 0
    bool is_index = false;
    size_t n1 = strlen(t1), n2;

    mg_url_decode(hm->uri.ptr, hm->uri.len, t1 + n1, sizeof(t1) - n1, 0);
    t1[sizeof(t1) - 1] = '\0';
    n2 = strlen(t1);
    while (n2 > 0 && t1[n2 - 1] == '/') t1[--n2] = 0;

    if (realpath(t1, t2) == NULL)
      LOG(LL_ERROR, ("realpath(%s): %d", t1, errno));

    if (mg_is_dir(t2)) {
      strncat(t2, "/index.html", sizeof(t2) - strlen(t2) - 1);
      t2[sizeof(t2) - 1] = '\0';
      is_index = true;
    }

    if (strlen(t2) < n1 || memcmp(t1, t2, n1) != 0) {
      // Requested file is located outside root directory, fail
      mg_http_reply(c, 404, "", "Not found %.*s\n", (int) hm->uri.len,
                    hm->uri.ptr);
    } else {
      FILE *fp = mg_fopen(t2, "r");
#if MG_ENABLE_SSI
      if (is_index && fp == NULL) {
        char *p = t2 + strlen(t2);
        while (p > t2 && p[-1] != '/') p--;
        strncpy(p, "index.shtml", &t2[sizeof(t2)] - p - 2);
        t2[sizeof(t2) - 1] = '\0';
        fp = mg_fopen(t2, "r");
      }
#endif
#if MG_ENABLE_HTTP_DEBUG_ENDPOINT
      snprintf(c->label, sizeof(c->label) - 1, "<-F %s", t2);
#endif
      if (is_index && fp == NULL) {
#if MG_ENABLE_DIRECTORY_LISTING
        listdir(c, hm, t2);
#else
        mg_http_reply(c, 403, "", "%s", "Directory listing not supported");
#endif
#if MG_ENABLE_SSI
      } else if (opts->ssi_pattern != NULL &&
                 mg_globmatch(opts->ssi_pattern, strlen(opts->ssi_pattern), t2,
                              strlen(t2))) {
        t1[n1] = '\0';
        mg_http_serve_ssi(c, t1, t2);
#endif
      } else {
        mg_http_serve_file(c, hm, t2, guess_content_type(t2), NULL);
      }
      if (fp != NULL) fclose(fp);
    }
  }
}
#endif

void mg_http_creds(struct mg_http_message *hm, char *user, int userlen,
                   char *pass, int passlen) {
  struct mg_str *v = mg_http_get_header(hm, "Authorization");
  user[0] = pass[0] = '\0';
  if (v != NULL && v->len > 6 && memcmp(v->ptr, "Basic ", 6) == 0) {
    char buf[256];
    int n = mg_base64_decode(v->ptr + 6, (int) v->len - 6, buf);
    const char *p = (const char *) memchr(buf, ':', n);
    if (p != NULL) {
      snprintf(user, userlen, "%.*s", (int) (p - buf), buf);
      snprintf(pass, passlen, "%.*s", n - (int) (p - buf) - 1, p + 1);
    }
  } else if (v != NULL && v->len > 7 && memcmp(v->ptr, "Bearer ", 7) == 0) {
    snprintf(pass, passlen, "%.*s", (int) v->len - 7, v->ptr + 7);
  } else if ((v = mg_http_get_header(hm, "Cookie")) != NULL) {
    size_t i;
    for (i = 0; i < v->len - 13; i++) {
      if (memcmp(&v->ptr[i], "access_token=", 13) == 0) {
        const char *p2 = v->ptr + i + 13, *p3 = p2;
        while (p2 < &v->ptr[v->len] && p2[0] != ';' && p2[0] != ' ') p2++;
        snprintf(pass, passlen, "%.*s", (int) (p2 - p3), p3);
        break;
      }
    }
  } else {
    mg_http_get_var(&hm->query, "access_token", pass, passlen);
  }
}

bool mg_http_match_uri(const struct mg_http_message *hm, const char *glob) {
  return mg_globmatch(glob, strlen(glob), hm->uri.ptr, hm->uri.len);
}

static void http_cb(struct mg_connection *c, int ev, void *ev_data,
                    void *fn_data) {
  if (ev == MG_EV_READ || ev == MG_EV_CLOSE) {
    struct mg_http_message hm;
    for (;;) {
      int n = mg_http_parse((char *) c->recv.buf, c->recv.len, &hm);
      if (ev == MG_EV_CLOSE) {
        hm.message.len = c->recv.len;
        hm.body.len = hm.message.len - (hm.body.ptr - hm.message.ptr);
      }
      if (n < 0 && ev == MG_EV_READ) {
        LOG(LL_ERROR, ("%lu HTTP parse error", c->id));
        c->is_closing = 1;
        break;
      } else if (n > 0 && (size_t) c->recv.len >= hm.message.len) {
#if MG_ENABLE_HTTP_DEBUG_ENDPOINT
        snprintf(c->label, sizeof(c->label) - 1, "<-[%.*s]", (int) hm.uri.len,
                 hm.uri.ptr);
        if (mg_http_match_uri(&hm, "/debug/info")) {
          struct mg_mgr *mgr = (struct mg_mgr *) fn_data;
          struct mg_connection *x;
          mg_printf(c, "%s\r\n",
                    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n");
          for (x = mgr->conns; x != NULL; x = x->next) {
            // LOG(LL_INFO, ("%p %s", x->fd, x->label));
            mg_http_printf_chunk(
                c,
                "%-4p %-12s %04d.%04d/%04d.%04d"
                " %d%d%d%d%d%d%d%d%d%d%d%d%d%d\n",
                x->fd, x->label, x->recv.len, x->recv.size, x->send.len,
                x->send.size, c->is_listening, c->is_client, c->is_accepted,
                c->is_resolving, c->is_connecting, c->is_tls, c->is_tls_hs,
                c->is_udp, c->is_websocket, c->is_hexdumping, c->is_draining,
                c->is_closing, c->is_readable, c->is_writable);
          }
          mg_http_write_chunk(c, "", 0);
          mg_iobuf_delete(&c->recv, hm.message.len);
          continue;
        }
#endif
        mg_call(c, MG_EV_HTTP_MSG, &hm);
        mg_iobuf_delete(&c->recv, hm.message.len);
      } else {
        break;
      }
    }
  }
  (void) fn_data;
  (void) ev_data;
}

struct mg_connection *mg_http_connect(struct mg_mgr *mgr, const char *url,
                                      mg_event_handler_t fn, void *fn_data) {
  struct mg_connection *c = mg_connect(mgr, url, fn, fn_data);
  if (c != NULL) c->pfn = http_cb, c->pfn_data = mgr;
#if MG_ENABLE_HTTP_DEBUG_ENDPOINT
  snprintf(c->label, sizeof(c->label) - 1, "->%s", url);
#endif
  return c;
}

struct mg_connection *mg_http_listen(struct mg_mgr *mgr, const char *url,
                                     mg_event_handler_t fn, void *fn_data) {
  struct mg_connection *c = mg_listen(mgr, url, fn, fn_data);
  if (c != NULL) c->pfn = http_cb, c->pfn_data = mgr;
#if MG_ENABLE_HTTP_DEBUG_ENDPOINT
  if (c != NULL) snprintf(c->label, sizeof(c->label) - 1, "<-LSN");
#endif
  return c;
}
