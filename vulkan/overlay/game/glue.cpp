/* Glue for REDRIVER2 on PS5: pieces of the game's platform layer that do not exist here. */
#include <glob.h>
#include <stdlib.h>
#include <string.h>

struct RENDER_ARGS;

/* crash_handler.cpp is Windows/POSIX-signal only; signal handlers are forbidden on this console. */
void InstallExceptionHandler() {}
void UnInstallExceptionHandler() {}

extern "C" int strcasecmp(const char *a, const char *b)
{
   for (;; a++, b++) {
      int ca = (unsigned char)*a, cb = (unsigned char)*b;
      if (ca >= 'A' && ca <= 'Z') ca += 32;
      if (cb >= 'A' && cb <= 'Z') cb += 32;
      if (ca != cb || !ca) return ca - cb;
   }
}
extern "C" int strncasecmp(const char *a, const char *b, unsigned long n)
{
   for (; n; n--, a++, b++) {
      int ca = (unsigned char)*a, cb = (unsigned char)*b;
      if (ca >= 'A' && ca <= 'Z') ca += 32;
      if (cb >= 'A' && cb <= 'Z') cb += 32;
      if (ca != cb || !ca) return ca - cb;
   }
   return 0;
}

/* chdir("/app0/assets") has no effect here (relative fopen gives EINVAL), so relative paths are
 * resolved against /app0/assets by wrapping fopen (link with APP_WRAP_SYMBOLS="fopen"). */
#include <stdio.h>
extern "C" void *__real_fopen(const char *, const char *);
extern "C" int __real_mkdir(const char *, unsigned short);

/* Relative path -> absolute. "Replays/..." is user data: it goes to the writable save dir. */
static const char *ps5_resolve(const char *path, char *buf, unsigned size)
{
   if (!path || path[0] == '/')
      return path;
   const char *prefix = "/app0/assets/";
   if (path[0] == 'R' && path[1] == 'e' && path[2] == 'p' && path[3] == 'l' && path[4] == 'a' &&
       path[5] == 'y' && path[6] == 's' && (path[7] == '/' || path[7] == 0))
      prefix = "/app0/save/";
   unsigned i = 0;
   for (; prefix[i] && i < size - 1; i++) buf[i] = prefix[i];
   for (unsigned j = 0; path[j] && i < size - 1; j++, i++) buf[i] = path[j];
   buf[i] = 0;
   return buf;
}

/* opendir does not work on this platform, so replays are listed through an index file that
 * fopen("Replays/x.D2RP", "w") maintains (one name per line). */
static const char kReplayIndex[] = "/app0/save/Replays/index.lst";

static bool ps5_index_has(const char *name)
{
   FILE *fp = (FILE *)__real_fopen(kReplayIndex, "rb");
   if (!fp) return false;
   char line[256];
   bool found = false;
   while (fgets(line, sizeof(line), fp)) {
      size_t n = strlen(line);
      while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
      if (!strcmp(line, name)) { found = true; break; }
   }
   fclose(fp);
   return found;
}

extern "C" void *__wrap_fopen(const char *path, const char *mode)
{
   char buf[512];
   if (path && mode && mode[0] == 'w' && !strncmp(path, "Replays/", 8)) {
      size_t n = strlen(path);
      if (n > 14 && !strcmp(path + n - 5, ".D2RP") && !ps5_index_has(path + 8)) {
         FILE *ix = (FILE *)__real_fopen(kReplayIndex, "ab");
         if (ix) { fprintf(ix, "%s\n", path + 8); fclose(ix); }
      }
   }
   return __real_fopen(ps5_resolve(path, buf, sizeof(buf)), mode);
}

/* glob(): only the replay list pattern is supported. */
extern "C" int glob(const char *pattern, int, int (*)(const char *, int), glob_t *g)
{
   memset(g, 0, sizeof(*g));
   if (!pattern || strncmp(pattern, "Replays/", 8) || !strstr(pattern, ".D2RP"))
      return GLOB_NOMATCH;
   FILE *fp = (FILE *)__real_fopen(kReplayIndex, "rb");
   if (!fp) {
      /* No index yet (replays saved before it existed): probe the game's default names Chase1..Chase200. */
      FILE *ix = (FILE *)__real_fopen(kReplayIndex, "ab");
      if (ix) {
         for (int i = 1; i <= 200; i++) {
            char full[128];
            snprintf(full, sizeof(full), "/app0/save/Replays/Chase%d.D2RP", i);
            FILE *t = (FILE *)__real_fopen(full, "rb");
            if (t) { fclose(t); fprintf(ix, "Chase%d.D2RP\n", i); }
         }
         fclose(ix);
      }
      fp = (FILE *)__real_fopen(kReplayIndex, "rb");
      if (!fp) return GLOB_NOMATCH;
   }
   g->gl_pathv = (char **)calloc(256, sizeof(char *));
   char line[256];
   while (g->gl_pathc < 255 && fgets(line, sizeof(line), fp)) {
      size_t n = strlen(line);
      while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
      if (!n) continue;
      char rel[300], full[400];
      snprintf(rel, sizeof(rel), "Replays/%s", line);
      snprintf(full, sizeof(full), "/app0/save/%s", rel);
      FILE *t = (FILE *)__real_fopen(full, "rb");
      if (!t) continue;
      fclose(t);
      g->gl_pathv[g->gl_pathc++] = strdup(rel);
   }
   fclose(fp);
   g->gl_matchc = g->gl_pathc;
   if (!g->gl_pathc) { free(g->gl_pathv); g->gl_pathv = NULL; return GLOB_NOMATCH; }
   return 0;
}

extern "C" void globfree(glob_t *g)
{
   if (!g || !g->gl_pathv) return;
   for (size_t i = 0; i < g->gl_pathc; i++) free(g->gl_pathv[i]);
   free(g->gl_pathv);
   g->gl_pathv = NULL;
   g->gl_pathc = 0;
}

extern "C" int __wrap_mkdir(const char *path, unsigned short mode)
{
   char buf[512];
   __real_mkdir("/app0/save", 0777);
   return __real_mkdir(ps5_resolve(path, buf, sizeof(buf)), mode);
}
