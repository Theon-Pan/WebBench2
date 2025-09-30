#include <check.h>
#include "arguments.h"
#include <stdlib.h>
#include <stdio.h>

START_TEST(test_proxy_server_str_default_hostname)
{
    char *argv[] = {"webbench2", "-t", "10", "-c", "5", "-p", ":7891", "https://www.baidu.com/" };
    int argc = 8;
    Arguments arg = create_default_arguments();
    
    set_arguments_values(argc, argv, &arg);

    printf("bench_time=%d, clients=%d, proxy_host = %s, proxy_port=%d, url=%s\n",
           arg.bench_time, arg.clients, arg.proxy_host, arg.proxy_port, arg.url ? arg.url : "(null)");

    ck_assert_int_eq(arg.bench_time, 10);
    ck_assert_int_eq(arg.clients, 5);
    ck_assert_ptr_nonnull(arg.proxy_host);
    ck_assert_str_eq(arg.proxy_host, "127.0.0.1");
    ck_assert_int_eq(arg.proxy_port, 7891);
    ck_assert_ptr_nonnull(arg.url);
    ck_assert_str_eq(arg.url, "https://www.baidu.com/");

}
END_TEST

START_TEST(test_proxy_server_str_default_port)
{
    char *argv[] = {"webbench2", "-t", "10", "-c", "5", "--proxy", "192.168.1.1", "https://www.baidu.com/"};
    int argc = 8;

    Arguments arg = create_default_arguments();
    set_arguments_values(argc, argv, &arg);

    printf("bench_time=%d, clients=%d, proxy_host=%s, proxy_port=%d, url=%s\n",
        arg.bench_time, arg.clients, arg.proxy_host, arg.proxy_port, arg.url ? arg.url : "(null)");

    ck_assert_int_eq(arg.bench_time, 10);
    ck_assert_int_eq(arg.clients, 5);
    ck_assert_ptr_nonnull(arg.proxy_host);
    ck_assert_str_eq(arg.proxy_host, "192.168.1.1");
    ck_assert_int_eq(arg.proxy_port, 80);
    ck_assert_ptr_nonnull(arg.url);
    ck_assert_str_eq(arg.url, "https://www.baidu.com/");
}
END_TEST

START_TEST(test_proxy_server_str_default_port_1)
{
    char *argv[] = {"webbench2", "-t", "10", "-c", "5", "-p", "192.168.1.1:", "https://www.baidu.com/"};
    int argc = 8;

    Arguments arg = create_default_arguments();
    set_arguments_values(argc, argv, &arg);

    printf("bench_time=%d, clients=%d, proxy_host=%s, proxy_port=%d, url=%s\n",
        arg.bench_time, arg.clients, arg.proxy_host, arg.proxy_port, arg.url ? arg.url : "(null)");

    ck_assert_int_eq(arg.bench_time, 10);
    ck_assert_int_eq(arg.clients, 5);
    ck_assert_ptr_nonnull(arg.proxy_host);
    ck_assert_str_eq(arg.proxy_host, "192.168.1.1");
    ck_assert_int_eq(arg.proxy_port, 80);
    ck_assert_ptr_nonnull(arg.url);
    ck_assert_str_eq(arg.url, "https://www.baidu.com/");
}

START_TEST(test_proxy_server_str_normal_format)
{
    char *argv[] = {"webbench2", "-t", "10", "-c", "5", "-p", "192.168.1.1:7891", "https://www.baidu.com/"};
    int argc = 8;

    Arguments arg = create_default_arguments();
    set_arguments_values(argc, argv, &arg);

    printf("bench_time=%d, clients=%d, proxy_host=%s, proxy_port=%d, url=%s\n",
        arg.bench_time, arg.clients, arg.proxy_host, arg.proxy_port, arg.url ? arg.url : "(null)");

    ck_assert_int_eq(arg.bench_time, 10);
    ck_assert_int_eq(arg.clients, 5);
    ck_assert_ptr_nonnull(arg.proxy_host);
    ck_assert_str_eq(arg.proxy_host, "192.168.1.1");
    ck_assert_int_eq(arg.proxy_port, 7891);
    ck_assert_ptr_nonnull(arg.url);
    ck_assert_str_eq(arg.url, "https://www.baidu.com/");
}

START_TEST(test_legal_http_method)
{
    char *argv[] = {"webbench2", "-t", "10", "-c", "5", "-p", "192.168.1.1:7891", "--options", "https://www.baidu.com/"};
    int argc = 9;

    Arguments args = create_default_arguments();
    set_arguments_values(argc, argv, &args);

    printf("bench_time=%d, clients=%d, proxy_host=%s, proxy_port=%d, http_method=%d, url=%s\n",
        args.bench_time, args.clients, args.proxy_host, args.proxy_port, args.method, args.url ? args.url: "(null)");
    
    ck_assert_int_eq(args.bench_time, 10);
    ck_assert_int_eq(args.clients, 5);
    ck_assert_ptr_nonnull(args.proxy_host);
    ck_assert_str_eq(args.proxy_host, "192.168.1.1");
    ck_assert_int_eq(args.proxy_port, 7891);
    ck_assert_ptr_nonnull(args.url);
    ck_assert_str_eq(args.url, "https://www.baidu.com/");
    ck_assert_int_eq(args.method, METHOD_OPTIONS);
}

START_TEST(test_illegal_http_method)
{
    char *argv[] = {"webbench2", "-t", "10", "-c", "5", "-p", "192.168.1.1:7891", "--post", "https://www.baidu.com/"};
    int argc = 9;
    Arguments args = create_default_arguments();
    set_arguments_values(argc, argv, &args);

    printf("bench_time=%d, clients=%d, proxy_host=%s, proxy_port=%d, http_method=%d, url=%s\n",
        args.bench_time, args.clients, args.proxy_host, args.proxy_port, args.method, args.url ? args.url : "(null)");
    
    ck_assert_int_eq(args.bench_time, 10);
    ck_assert_int_eq(args.clients, 5);
    ck_assert_ptr_nonnull(args.proxy_host);
    ck_assert_str_eq(args.proxy_host, "192.168.1.1");
    ck_assert_int_eq(args.proxy_port, 7891);
    ck_assert_ptr_nonnull(args.url);
    ck_assert_str_eq(args.url, "https://www.baidu.com/");
}



Suite *arguments_suite(void)
{
    Suite *s;
    TCase *tc_core;
    s = suite_create("Arguments");
    tc_core = tcase_create("Core");
    tcase_add_test(tc_core, test_proxy_server_str_default_hostname);
    tcase_add_test(tc_core, test_proxy_server_str_default_port);
    tcase_add_test(tc_core, test_proxy_server_str_default_port_1);
    tcase_add_test(tc_core, test_proxy_server_str_normal_format);
    tcase_add_test(tc_core, test_legal_http_method);
    // tcase_add_test(tc_core, test_illegal_http_method);
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