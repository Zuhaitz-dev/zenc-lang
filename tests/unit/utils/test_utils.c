// SPDX-License-Identifier: MIT
// Unit tests of src/utils/utils.c.
#include "utils/utils.h"
#include "unit_test.h"

UTEST(str_join, several_parts)
{
    const char *parts[] = {"a", "b", "c", NULL};
    char out[32];
    str_join(out, sizeof(out), parts, ", ");
    ASSERT_STREQ("a, b, c", out);
}

UTEST(str_join, one_part_has_no_separator)
{
    const char *parts[] = {"only", NULL};
    char out[32];
    str_join(out, sizeof(out), parts, ", ");
    ASSERT_STREQ("only", out);
}

UTEST(str_join, no_parts_gives_an_empty_string)
{
    char out[8] = "junk";
    str_join(out, sizeof(out), NULL, ", ");
    ASSERT_STREQ("", out);
}

UTEST(str_join, truncates_to_the_buffer)
{
    const char *parts[] = {"abc", "def", NULL};
    char out[6];
    str_join(out, sizeof(out), parts, "-");
    ASSERT_STREQ("abc-d", out);
}
