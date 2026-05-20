#ifndef ZM_SIMPLE_DEFINE_H_
#define ZM_SIMPLE_DEFINE_H_

#include <string>

#define ZM_UNUSED(x)				(void)(x)
#define ZM_ERR_NOERROR				0
#define ZM_MAX(a, b)				(((a) > (b)) ? (a) : (b))
#define ZM_MIN(a, b)				(((a) < (b)) ? (a) : (b))
#define ZM_BETWEEN(v, left, right)	((v)>=(left) && (v)<=(right))
#define ZM_BOOL_STR(b)				((b)? "true" : "false")
#define __ZM_FUNC__					__func__

/**
 * s(秒)、ms(毫秒)、us(微秒)、ns(纳秒), 其中：1s=1000ms, 1ms=1000us, 1us=1000ns
 * Implement usleep in Windows, needs Winsock
 * struct timeval tv;
 * tv.tv_sec  = s;
 * tv.tv_usec = us;
 * ::select(0, NULL, NULL, NULL, &tv);
 */
#define ZmSleepMS(ms)    ::Sleep(ms)
#define ZmSleepUS(us)    do { struct timeval tv = {(us)/1000000L, (us)%1000000}; select(0, NULL, NULL, NULL, &tv); } while(0)

#define zm_memcpy(dst, src, n)      (n>0?   memcpy(dst, src, n) : dst)
#define zm_memset(b, c, len)        (len>0? memset(b, c, len) : b)
#define zm_memmove(dst, src, len)   (len>0? memmove(dst, src, len) : dst)

#define zm_log2(x)  (size_t)std::log2((x))

#define Zm_IsValidHandle(h)         (nullptr!=(h) && INVALID_HANDLE_VALUE!=(h))
#define Zm_IsInvalidHandle(h)       (nullptr==(h) || INVALID_HANDLE_VALUE==(h))


#endif /* ZM_SIMPLE_DEFINE_H */