#include <check.h>
#include "bitmap.h"

START_TEST(test_set_and_get_bitmap)
{
    unsigned short bits_amt = 64;
    unsigned short bytes_needed = bits_amt / (sizeof(char) * 8);
    if (bits_amt % sizeof(char) != 0)
    {
        bytes_needed += 1;
    }

    char bitmap[bytes_needed];
    memset(bitmap, 0, bytes_needed);

    ck_assert_int_eq(sizeof(bitmap), 8);

    unsigned short position = 20;
    set_bitmap(position, bitmap, sizeof(bitmap));

    int result = get_bitmap(position, bitmap, sizeof(bitmap));
    ck_assert_int_eq(result, 1);

}


Suite *arguments_suite(void)
{
    Suite *s;
    TCase *tc_core;
    s = suite_create("bitmap");
    tc_core = tcase_create("Core");
    tcase_add_test(tc_core, test_set_and_get_bitmap);

    suite_add_tcase(s, tc_core);
    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;
    s = arguments_suite();
    sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return (number_failed == 0) ? 0 : 1;
}