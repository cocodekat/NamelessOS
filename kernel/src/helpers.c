#include "helpers.h"
#include <stdint.h>

int strings_equal(const char *a, const char *b)
{
    while (*a && *b)
    {
        if (*a != *b)
            return 0;

        a++;
        b++;
    }

    return *a == *b;
}

int streq(const char *a, const char *b)
{
    while (*a && *b)
    {
        if (*a != *b)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

uint64_t byte_conv(uint64_t value, const char *from, const char *to)
{
    uint64_t from_multiplier;
    uint64_t to_multiplier;

    if (streq(from, "b"))
        from_multiplier = 1;
    else if (streq(from, "kb"))
        from_multiplier = 1000;
    else if (streq(from, "mb"))
        from_multiplier = 1000 * 1000;
    else if (streq(from, "gb"))
        from_multiplier = 1000 * 1000 * 1000;
    else
        return 0;

    if (streq(to, "b"))
        to_multiplier = 1;
    else if (streq(to, "kb"))
        to_multiplier = 1000;
    else if (streq(to, "mb"))
        to_multiplier = 1000 * 1000;
    else if (streq(to, "gb"))
        to_multiplier = 1000 * 1000 * 1000;
    else
        return 0;

    return (value * from_multiplier) / to_multiplier;
}