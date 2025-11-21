#include <check.h>
#include "arguments.h"
#include "request.h"
#include <stdlib.h>
#include <stdio.h>

START_TEST(test_construct_request_first_line_no_proxy_specified)
{
    char *argv[] = {"webbench2", "-t", "10", "-c", "5", "--options", "https://www.baidu.com:12345"};
    char *expected_request_first_line = "OPTIONS / HTTP/1.1\r\nUser-Agent: WebBench 2\r\nHost: www.baidu.com:12345\r\nConnection: close\r\n\r\n";
    int argc = 7;

    Arguments args = create_default_arguments();
    set_arguments_values(argc, argv, &args);

    printf("bench_time=%d, clients=%d, http_method=%d, target_host=%s, target_port=%d.\n",
        args.bench_time, args.clients, args.method, args.target_host, args.target_port);
    
    HTTPRequest request = {0};

    if (build_request(&args, &request) == 0) {
        printf("Request: host=%s, body=\n%s", request.host, request.body);
    }
    ck_assert_int_ne(strlen(request.body), 0);
    ck_assert_int_ne(strlen(request.host), 0);
    ck_assert_str_eq(request.host, "www.baidu.com");
    // for (int i = 0; i < (int) strlen(request.body); i++) {
    //     if (request.body[i] != expected_request_first_line[i]) {
    //         printf("Differecen at position %d:\n",i);
    //         return;
    //     } else {
    //         //printf("Strins are identical.\n");
    //     }

    // }
    ck_assert_str_eq(request.body, expected_request_first_line);

}

START_TEST(test_construct_request_first_line_with_proxy_specified)
{
    char *argv[] = {"webbench2", "-t", "10", "-c", "5", "--options", "--proxy", "localhost:7891", "https://www.baidu.com:12345"};
    char *expected_request_first_line = "OPTIONS https://www.baidu.com:12345/ HTTP/1.1\r\nUser-Agent: WebBench 2\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n";
    int argc = 9;

    Arguments args = create_default_arguments();
    set_arguments_values(argc, argv, &args);

    printf("bench_time=%d, clients=%d, http_method=%d, target_host=%s, target_port=%d, proxy_host=%s, proxy_port=%d.\n",
        args.bench_time, args.clients, args.method, args.target_host, args.target_port, args.proxy_host, args.proxy_port);
    
    HTTPRequest request = {0};
    if (build_request(&args, &request) == 0) {
        printf("Request: host=%s, body=\n%s", request.host, request.body);
    }

    ck_assert_int_ne(strlen(request.host), 0);
    ck_assert_int_ne(strlen(request.body), 0);
    ck_assert_str_eq(request.host, "www.baidu.com");
    ck_assert_str_eq(request.body, expected_request_first_line);
}

Suite *arguments_suite(void)
{
    Suite *s;
    TCase *tc_core;
    s = suite_create("Request");
    tc_core = tcase_create("Core");
    tcase_add_test(tc_core, test_construct_request_first_line_no_proxy_specified);
    tcase_add_test(tc_core, test_construct_request_first_line_with_proxy_specified);
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
    return ((number_failed == 0) ? 0 : 1);
}