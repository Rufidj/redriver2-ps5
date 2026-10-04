/* Heap size for app_heap.c (replaces its weak default of 128 MiB). The game plus Mesa need more. */
#include <stddef.h>
const size_t ps5_opengl_heap_size = 768u * 1024u * 1024u;
