// SPDX-License-Identifier: GPL-2.0-or-later
//
// The assertions of the service's host-JDK rows: a failed check is printed and counted, and the row's
// runner exits non-zero when any failed, so that a broken variant can never pass.

package android.kernarium.sms;

final class Checks {
    private static int failures;

    private Checks() {}

    static synchronized void check(boolean condition, String what) {
        if (!condition) {
            ++failures;
            if (failures <= 60) System.out.println("FAIL: " + what);
        }
    }

    static void same(Object expected, Object actual, String what) {
        check(expected.equals(actual), what + ": expected <" + expected + "> but got <" + actual + ">");
    }

    /** Prints PASS and returns 0, or the number of failures and 1. */
    static synchronized int finish() {
        if (failures != 0) {
            System.out.println(failures + " failure(s)");
            return 1;
        }
        System.out.println("PASS");
        return 0;
    }
}
