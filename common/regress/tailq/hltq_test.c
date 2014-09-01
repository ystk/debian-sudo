/*
 * Copyright (c) 2013 Todd C. Miller <Todd.Miller@courtesan.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <config.h>

#include <sys/types.h>
#include <stdio.h>
#ifdef STDC_HEADERS
# include <stdlib.h>
# include <stddef.h>
#else
# ifdef HAVE_STDLIB_H
#  include <stdlib.h>
# endif
#endif /* STDC_HEADERS */
#ifdef HAVE_STRING_H
# if defined(HAVE_MEMORY_H) && !defined(STDC_HEADERS)
#  include <memory.h>
# endif
# include <string.h>
#endif /* HAVE_STRING_H */
#ifdef HAVE_STRINGS_H
# include <strings.h>
#endif /* HAVE_STRINGS_H */
#ifdef HAVE_STDBOOL_H
# include <stdbool.h>
#else
# include "compat/stdbool.h"
#endif

#include "missing.h"
#include "fatal.h"
#include "queue.h"

__dso_public int main(int argc, char *argv[]);

/*
 * Note: HLTQ_ENTRY is intentionally in the middle of the struct
 *       to catch bad assumptions in the PREV/NEXT macros.
 */
struct test_data {
    int a;
    HLTQ_ENTRY(test_data) entries;
    char b;
};

TAILQ_HEAD(test_data_list, test_data);

/*
 * Simple tests for headless tail queue macros.
 */
int
main(int argc, char *argv[])
{
    struct test_data d1, d2, d3;
    struct test_data *hltq;
    struct test_data_list tq;
    int errors = 0;

    /*
     * Initialize three data elements and concatenate them in order.
     */
    HLTQ_INIT(&d1, entries);
    d1.a = 1;
    d1.b = 'a';
    if (HLTQ_FIRST(&d1) != &d1) {
	warningx_nodebug("FAIL: HLTQ_FIRST(1 entry) doesn't return first element: got %p, expected %p", HLTQ_FIRST(&d1), &d1);
	errors++;
    }
    if (HLTQ_LAST(&d1, test_data, entries) != &d1) {
	warningx_nodebug("FAIL: HLTQ_LAST(1 entry) doesn't return first element: got %p, expected %p", HLTQ_LAST(&d1, test_data, entries), &d1);
	errors++;
    }
    if (HLTQ_PREV(&d1, test_data, entries) != NULL) {
	warningx_nodebug("FAIL: HLTQ_PREV(1 entry) doesn't return NULL: got %p", HLTQ_PREV(&d1, test_data, entries));
	errors++;
    }

    HLTQ_INIT(&d2, entries);
    d2.a = 2;
    d2.b = 'b';

    HLTQ_INIT(&d3, entries);
    d3.a = 3;
    d3.b = 'c';

    HLTQ_CONCAT(&d1, &d2, entries);
    HLTQ_CONCAT(&d1, &d3, entries);
    hltq = &d1;

    /*
     * Verify that HLTQ_FIRST, HLTQ_LAST, HLTQ_NEXT, HLTQ_PREV
     * work as expected.
     */
    if (HLTQ_FIRST(hltq) != &d1) {
	warningx_nodebug("FAIL: HLTQ_FIRST(3 entries) doesn't return first element: got %p, expected %p", HLTQ_FIRST(hltq), &d1);
	errors++;
    }
    if (HLTQ_LAST(hltq, test_data, entries) != &d3) {
	warningx_nodebug("FAIL: HLTQ_LAST(3 entries) doesn't return third element: got %p, expected %p", HLTQ_LAST(hltq, test_data, entries), &d3);
	errors++;
    }

    if (HLTQ_NEXT(&d1, entries) != &d2) {
	warningx_nodebug("FAIL: HLTQ_NEXT(&d1) doesn't return &d2: got %p, expected %p", HLTQ_NEXT(&d1, entries), &d2);
	errors++;
    }
    if (HLTQ_NEXT(&d2, entries) != &d3) {
	warningx_nodebug("FAIL: HLTQ_NEXT(&d2) doesn't return &d3: got %p, expected %p", HLTQ_NEXT(&d2, entries), &d3);
	errors++;
    }
    if (HLTQ_NEXT(&d3, entries) != NULL) {
	warningx_nodebug("FAIL: HLTQ_NEXT(&d3) doesn't return NULL: got %p", HLTQ_NEXT(&d3, entries));
	errors++;
    }

    if (HLTQ_PREV(&d1, test_data, entries) != NULL) {
	warningx_nodebug("FAIL: HLTQ_PREV(&d1) doesn't return NULL: got %p", HLTQ_PREV(&d1, test_data, entries));
	errors++;
    }
    if (HLTQ_PREV(&d2, test_data, entries) != &d1) {
	warningx_nodebug("FAIL: HLTQ_PREV(&d2) doesn't return &d1: got %p, expected %p", HLTQ_PREV(&d2, test_data, entries), &d1);
	errors++;
    }
    if (HLTQ_PREV(&d3, test_data, entries) != &d2) {
	warningx_nodebug("FAIL: HLTQ_PREV(&d3) doesn't return &d2: got %p, expected %p", HLTQ_PREV(&d3, test_data, entries), &d2);
	errors++;
    }

    /* Test conversion to TAILQ. */
    HLTQ_TO_TAILQ(&tq, hltq, entries);

    if (TAILQ_FIRST(&tq) != &d1) {
	warningx_nodebug("FAIL: TAILQ_FIRST(&tq) doesn't return first element: got %p, expected %p", TAILQ_FIRST(&tq), &d1);
	errors++;
    }
    if (TAILQ_LAST(&tq, test_data_list) != &d3) {
	warningx_nodebug("FAIL: TAILQ_LAST(&tq) doesn't return third element: got %p, expected %p", TAILQ_LAST(&tq, test_data_list), &d3);
	errors++;
    }

    if (TAILQ_NEXT(&d1, entries) != &d2) {
	warningx_nodebug("FAIL: TAILQ_NEXT(&d1) doesn't return &d2: got %p, expected %p", TAILQ_NEXT(&d1, entries), &d2);
	errors++;
    }
    if (TAILQ_NEXT(&d2, entries) != &d3) {
	warningx_nodebug("FAIL: TAILQ_NEXT(&d2) doesn't return &d3: got %p, expected %p", TAILQ_NEXT(&d2, entries), &d3);
	errors++;
    }
    if (TAILQ_NEXT(&d3, entries) != NULL) {
	warningx_nodebug("FAIL: TAILQ_NEXT(&d3) doesn't return NULL: got %p", TAILQ_NEXT(&d3, entries));
	errors++;
    }

    if (TAILQ_PREV(&d1, test_data_list, entries) != NULL) {
	warningx_nodebug("FAIL: TAILQ_PREV(&d1) doesn't return NULL: got %p", TAILQ_PREV(&d1, test_data_list, entries));
	errors++;
    }
    if (TAILQ_PREV(&d2, test_data_list, entries) != &d1) {
	warningx_nodebug("FAIL: TAILQ_PREV(&d2) doesn't return &d1: got %p, expected %p", TAILQ_PREV(&d2, test_data_list, entries), &d1);
	errors++;
    }
    if (TAILQ_PREV(&d3, test_data_list, entries) != &d2) {
	warningx_nodebug("FAIL: TAILQ_PREV(&d3) doesn't return &d2: got %p, expected %p", TAILQ_PREV(&d3, test_data_list, entries), &d2);
	errors++;
    }

    exit(errors);
}
