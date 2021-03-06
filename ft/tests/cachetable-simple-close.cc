/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
/*======
This file is part of PerconaFT.


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.

----------------------------------------

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License, version 3,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.
======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

#include "test.h"
#include "cachetable-test.h"
#include <string>

bool close_called;
bool free_called;

static void close_usr(CACHEFILE UU(cf), int UU(i), void* UU(p), bool UU(b), LSN UU(lsn)) {
    close_called = true;
}
static void free_usr(CACHEFILE UU(cf), void* UU(p)) {
    free_called = true;
}

static void set_cf_userdata(CACHEFILE f1) {
    toku_cachefile_set_userdata(
        f1,
        NULL,
        &dummy_log_fassociate,
        &close_usr,
        &free_usr,
        &dummy_chckpnt_usr,
        &dummy_begin,
        &dummy_end,
        &dummy_note_pin,
        &dummy_note_unpin
        );
}

bool keep_me;
bool write_me;
bool flush_called;
static UU() void
flush (CACHEFILE f __attribute__((__unused__)),
       int UU(fd),
       CACHEKEY k  __attribute__((__unused__)),
       void *v     __attribute__((__unused__)),
       void **dd     __attribute__((__unused__)),
       void *e     __attribute__((__unused__)),
       PAIR_ATTR s      __attribute__((__unused__)),
       PAIR_ATTR* new_size      __attribute__((__unused__)),
       bool w      __attribute__((__unused__)),
       bool keep   __attribute__((__unused__)),
       bool c      __attribute__((__unused__)),
       bool UU(is_clone)
       ) 
{
    flush_called = true;
    if (!keep) keep_me = keep;
    if (w) write_me = w;
}


static void
simple_test(bool unlink_on_close) {
    const int test_limit = 12;
    int r;
    CACHETABLE ct;
    toku_cachetable_create(&ct, test_limit, ZERO_LSN, nullptr);
    const char *fname1 = TOKU_TEST_FILENAME;
    unlink(fname1);
    CACHEFILE f1;
    r = toku_cachetable_openf(&f1, ct, fname1, O_RDWR|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO); assert(r == 0);
    set_cf_userdata(f1);

    // test that if we just open a cachefile and then close it (have no pairs active),
    // then it does not get cached
    close_called = false;
    free_called = false;
    toku_cachefile_close(&f1, false, ZERO_LSN);
    assert(close_called);
    assert(free_called);

    // now reopen the cachefile
    f1 = NULL;
    r = toku_cachetable_openf(&f1, ct, fname1, O_RDWR|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO); assert(r == 0);
    set_cf_userdata(f1);
    void* v1;
    CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);
    wc.flush_callback = flush;
    r = toku_cachetable_get_and_pin(f1, make_blocknum(1), toku_cachetable_hash(f1, make_blocknum(1)), &v1, wc, def_fetch, def_pf_req_callback, def_pf_callback, true, NULL);
    r = toku_test_cachetable_unpin(f1, make_blocknum(1), toku_cachetable_hash(f1, make_blocknum(1)), CACHETABLE_DIRTY, make_pair_attr(8));
    toku_cachetable_verify(ct);
    if (unlink_on_close) {
        toku_cachefile_unlink_on_close(f1);
    }
    close_called = false;
    free_called = false;
    keep_me = true;
    write_me = false;
    flush_called = false;
    // because we ought to have one pair in the cachetable for this cf,
    // close should cache the cf and not free it (unless we unlink on close)
    // also, make sure we wrote dirty pair, but we did NOT free PAIR unless
    // unlink_on_close was set
    toku_cachefile_close(&f1, false, ZERO_LSN);
    CACHETABLE_STATUS_S stats;
    toku_cachetable_get_status(ct, &stats);
    assert(flush_called);
    assert(close_called);
    assert(write_me);
    if (unlink_on_close) {
        assert(free_called);
        assert(!keep_me);
        // pair should NOT still be accounted for
        assert(stats.status[CACHETABLE_STATUS_S::CT_SIZE_CURRENT].value.num == 0);
    }
    else {
        assert(keep_me);
        assert(!free_called);
        // pair should still be accounted for
        assert(stats.status[CACHETABLE_STATUS_S::CT_SIZE_CURRENT].value.num == 8);
    }
    toku_cachetable_close(&ct);
    if (!unlink_on_close) {
        assert(free_called);
        assert(!keep_me);
    }
}

// test to verify that a PAIR stays in cache
// after the cachefile undergoes a close and reopen
static void test_pair_stays_in_cache(enum cachetable_dirty dirty) {
    const int test_limit = 12;
    int r;
    CACHETABLE ct;
    toku_cachetable_create(&ct, test_limit, ZERO_LSN, nullptr);
    const char *fname1 = TOKU_TEST_FILENAME;
    unlink(fname1);
    CACHEFILE f1;

    r = toku_cachetable_openf(&f1, ct, fname1, O_RDWR|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO); assert(r == 0);
    void* v1;
    CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);
    r = toku_cachetable_get_and_pin(f1, make_blocknum(1), toku_cachetable_hash(f1, make_blocknum(1)), &v1, wc, def_fetch, def_pf_req_callback, def_pf_callback, true, NULL);
    r = toku_test_cachetable_unpin(f1, make_blocknum(1), toku_cachetable_hash(f1, make_blocknum(1)), dirty, make_pair_attr(8));
    toku_cachefile_close(&f1, false, ZERO_LSN);
    // now reopen the cachefile
    f1 = NULL;
    r = toku_cachetable_openf(&f1, ct, fname1, O_RDWR|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO); assert(r == 0);
    // do a maybe_get_and_pin and verify that it succeeds
    // therefore proving that the PAIR was cached
    // and could be successfully retrieved
    r = toku_cachetable_maybe_get_and_pin_clean(f1, make_blocknum(1), toku_cachetable_hash(f1, make_blocknum(1)), PL_WRITE_EXPENSIVE, &v1);
    assert(r == 0);
    r = toku_test_cachetable_unpin(f1, make_blocknum(1), toku_cachetable_hash(f1, make_blocknum(1)), CACHETABLE_DIRTY, make_pair_attr(8));
    toku_cachefile_close(&f1, false, ZERO_LSN);

    toku_cachetable_close(&ct);
}

static void test_multiple_cachefiles(bool use_same_hash) {
    for (int iter = 0; iter < 3; iter++) {
        const int test_limit = 1000;
        int r;
        CACHETABLE ct;
        toku_cachetable_create(&ct, test_limit, ZERO_LSN, nullptr);

        std::string fname1s(TOKU_TEST_FILENAME);
        fname1s.append("_1");
        const char *fname1 = fname1s.c_str();
        std::string fname2s(TOKU_TEST_FILENAME);
        fname2s.append("_2");
        const char *fname2 = fname2s.c_str();
        std::string fname3s(TOKU_TEST_FILENAME);
        fname3s.append("_3");
        const char *fname3 = fname3s.c_str();

        unlink(fname1);
        unlink(fname2);
        unlink(fname3);
        CACHEFILE f1;
        CACHEFILE f2;
        CACHEFILE f3;

        r = toku_cachetable_openf(&f1, ct, fname1, O_RDWR|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO); assert(r == 0);
        r = toku_cachetable_openf(&f2, ct, fname2, O_RDWR|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO); assert(r == 0);
        r = toku_cachetable_openf(&f3, ct, fname3, O_RDWR|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO); assert(r == 0);

        void* v1;
        void* v2;
        void* v3;

        CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);
        for (int j = 0; j < 3; j++) {
            uint32_t hash = use_same_hash ? 1 : toku_cachetable_hash(f1, make_blocknum(j));
            r = toku_cachetable_get_and_pin(f1, make_blocknum(j), hash, &v1, wc, def_fetch, def_pf_req_callback, def_pf_callback, true, NULL);
            r = toku_test_cachetable_unpin(f1, make_blocknum(j), hash, CACHETABLE_CLEAN, make_pair_attr(8));
        }

        for (int j = 0; j < 3; j++) {
            uint32_t hash = use_same_hash ? 1 : toku_cachetable_hash(f2, make_blocknum(j));
            r = toku_cachetable_get_and_pin(f2, make_blocknum(j), hash, &v2, wc, def_fetch, def_pf_req_callback, def_pf_callback, true, NULL);
            r = toku_test_cachetable_unpin(f2, make_blocknum(j), hash, CACHETABLE_CLEAN, make_pair_attr(8));
        }

        for (int j = 0; j < 3; j++) {
            uint32_t hash = use_same_hash ? 1 : toku_cachetable_hash(f3, make_blocknum(j));
            r = toku_cachetable_get_and_pin(f3, make_blocknum(j), hash, &v3, wc, def_fetch, def_pf_req_callback, def_pf_callback, true, NULL);
            r = toku_test_cachetable_unpin(f3, make_blocknum(j), hash, CACHETABLE_CLEAN, make_pair_attr(8));
        }


        toku_cachefile_close(&f1, false, ZERO_LSN);
        toku_cachefile_close(&f2, false, ZERO_LSN);
        toku_cachefile_close(&f3, false, ZERO_LSN);

        const char* fname_to_open = NULL;
        if (iter == 0) {
            fname_to_open  = fname1;
        }
        else if (iter == 1) {
            fname_to_open  = fname2;
        }
        else if (iter == 2) {
            fname_to_open = fname3;
        }

        // now reopen the cachefile
        f1 = NULL;
        r = toku_cachetable_openf(&f1, ct, fname_to_open, O_RDWR|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO); assert(r == 0);
        // do a maybe_get_and_pin and verify that it succeeds
        // therefore proving that the PAIR was cached
        // and could be successfully retrieved
        for (int j = 0; j < 3; j++) {
            uint32_t hash = use_same_hash ? 1 : toku_cachetable_hash(f1, make_blocknum(j));
            r = toku_cachetable_maybe_get_and_pin_clean(f1, make_blocknum(j), hash, PL_WRITE_EXPENSIVE, &v1);
            assert(r == 0);
            r = toku_test_cachetable_unpin(f1, make_blocknum(j), hash, CACHETABLE_CLEAN, make_pair_attr(8));
        }
        toku_cachefile_close(&f1, false, ZERO_LSN);

        toku_cachetable_close(&ct);
    }
}

// test that the evictor works properly with closed cachefiles
static void test_evictor(void) {
    const int test_limit = 12;
    int r;
    CACHETABLE ct;
    toku_cachetable_create(&ct, test_limit, ZERO_LSN, nullptr);

    std::string fname1s(TOKU_TEST_FILENAME);
    fname1s.append("_1");
    const char *fname1 = fname1s.c_str();
    std::string fname2s(TOKU_TEST_FILENAME);
    fname2s.append("_2");
    const char *fname2 = fname2s.c_str();

    unlink(fname1);
    unlink(fname2);
    CACHEFILE f1;
    CACHEFILE f2;

    r = toku_cachetable_openf(&f1, ct, fname1, O_RDWR|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO); assert(r == 0);
    set_cf_userdata(f1);
    r = toku_cachetable_openf(&f2, ct, fname2, O_RDWR|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO); assert(r == 0);
    void* v1;
    CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);
    r = toku_cachetable_get_and_pin(f1, make_blocknum(1), toku_cachetable_hash(f1, make_blocknum(1)), &v1, wc, def_fetch, def_pf_req_callback, def_pf_callback, true, NULL);
    r = toku_test_cachetable_unpin(f1, make_blocknum(1), toku_cachetable_hash(f1, make_blocknum(1)), CACHETABLE_CLEAN, make_pair_attr(8));
    close_called = false;
    free_called = false;
    toku_cachefile_close(&f1, false, ZERO_LSN);
    assert(close_called);
    assert(!free_called);

    // at this point, we should f1, along with one PAIR, stale in the cachetable
    // now let's pin another node, and ensure that it causes an eviction and free of f1
    r = toku_cachetable_get_and_pin(f2, make_blocknum(1), toku_cachetable_hash(f2, make_blocknum(1)), &v1, wc, def_fetch, def_pf_req_callback, def_pf_callback, true, NULL);
    r = toku_test_cachetable_unpin(f2, make_blocknum(1), toku_cachetable_hash(f2, make_blocknum(1)), CACHETABLE_CLEAN, make_pair_attr(8));
    // now sleep for 2 seconds, and check to see if f1 has been closed
    sleep(2);
    assert(free_called);

    toku_cachefile_close(&f2, false, ZERO_LSN);

    toku_cachetable_close(&ct);
}

int
test_main(int argc, const char *argv[]) {
    default_parse_args(argc, argv);
    test_evictor();
    test_multiple_cachefiles(false);
    test_multiple_cachefiles(true);
    simple_test(false);
    simple_test(true);
    test_pair_stays_in_cache(CACHETABLE_DIRTY);
    test_pair_stays_in_cache(CACHETABLE_CLEAN);
    return 0;
}
