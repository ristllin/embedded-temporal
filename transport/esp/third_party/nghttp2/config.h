/* mwf-transport vendored nghttp2 — minimal config.h (replaces autotools/cmake
 * detection). Compile every lib .c file with -DHAVE_CONFIG_H and this directory on
 * the include path. The full macro set nghttp2's lib/ actually consults is tiny
 * (grep HAVE_ over lib/): ARPA_INET_H, NETINET_IN_H, CLOCK_GETTIME,
 * GETTICKCOUNT64 (win32), WINDOWS_H (win32).
 *
 * Both targets we build for provide the POSIX trio:
 *   - macOS host twin: system headers, clock_gettime since 10.12.
 *   - ESP32-S3 (ESP-IDF/newlib + lwIP): lwIP ships arpa/inet.h + netinet/in.h,
 *     newlib ships clock_gettime.
 */
#ifndef MWF_NGHTTP2_CONFIG_H
#define MWF_NGHTTP2_CONFIG_H

#define HAVE_ARPA_INET_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_CLOCK_GETTIME 1

#endif /* MWF_NGHTTP2_CONFIG_H */
