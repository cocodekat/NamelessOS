#ifndef HELPERS_H
#define HELPERS_H
#include <stdint.h>

int strings_equal(const char *a, const char *b);
int streq(const char *a, const char *b);
uint64_t byte_conv(uint64_t value, const char *from, const char *to);

#endif