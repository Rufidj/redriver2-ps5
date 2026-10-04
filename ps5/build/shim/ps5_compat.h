#ifndef PS5_COMPAT_H
#define PS5_COMPAT_H
#ifdef __cplusplus
extern "C" {
#endif
int strcasecmp(const char*, const char*);
int strncasecmp(const char*, const char*, unsigned long);
#ifdef __cplusplus
}
#endif
#endif
