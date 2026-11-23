// SPDX-License-Identifier: Apache-2.0
package com.android.frameworks.andariummicrog;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.content.pm.SigningInfo;
import android.os.Bundle;
import android.util.Log;

import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;

/**
 * Reads the signing certificates that PackageManager reports for the two packages LineageOS's
 * restricted signature spoofing covers, through both `signatures` and `signingInfo`, and checks
 * them against an expectation given with `-e expect <mode>`:
 *
 * <ul>
 *   <li>`report` (default): print only.
 *   <li>`none`: neither package is installed.
 *   <li>`own`: every installed one carries Google's certificate as its fake-signature and reports
 *       the signer of this probe (the AOSP test key that signs the decoys too) and nothing else:
 *       a package that is not signed by microG's key is not spoofed whatever its name and
 *       meta-data say.
 *   <li>`google`: every installed one reports Google's certificate, and only it, in both lists
 *       (microG's own packages on a build that spoofs).
 *   <li>`notgoogle`: no installed one reports Google's certificate in either list (microG's own
 *       packages on a build that does not spoof).
 * </ul>
 *
 * A mode other than `report` and `none` needs both packages installed. The result is
 * `INSTRUMENTATION_RESULT: verdict=PASS` or `verdict=FAIL` with the reasons; each package also
 * gets one status block of `package`, `installed`, `signatures`, `signing_info`, `history`
 * (comma separated SHA-256 digests, each named by who owns it) and `fake_signature` (the digest of
 * the certificate in its `fake-signature` meta-data).
 */
public final class MicrogSpoofingProbe extends Instrumentation {
    private static final String TAG = "AndariumMicrogProbe";
    private static final String[] PACKAGES = {"com.google.android.gms", "com.android.vending"};
    // SHA-256 of the certificate that ComputerEngine presents for microG's packages (the
    // certificate of Google's Android apps), and of microG's own release certificate.
    private static final String GOOGLE = "f0fd6c5b410f25cb25c3b53346c8972fae30f8ee7411df910480ad6b2d60db83";
    private static final String MICROG = "9bd06727e62796c0130eb6dab39b73157451582cbd138e86c468acc395d14165";

    private String mExpect = "report";

    @Override
    public void onCreate(Bundle arguments) {
        super.onCreate(arguments);
        if (arguments != null && arguments.getString("expect") != null) {
            mExpect = arguments.getString("expect");
        }
        start();
    }

    @Override
    public void onStart() {
        super.onStart();
        List<String> failures = new ArrayList<>();
        try {
            List<String> own = read(getContext().getPackageName(), failures).signers;
            if (own.size() != 1) {
                failures.add("the probe's own signer is not exactly one certificate: " + own);
            }
            for (String name : PACKAGES) {
                Certs found = read(name, null);
                boolean installed = found != null;
                Bundle status = new Bundle();
                status.putString("package", name);
                status.putString("installed", Boolean.toString(installed));
                if (installed) {
                    status.putString("signatures", describe(found.signatures));
                    status.putString("signing_info", describe(found.signers));
                    status.putString("history", describe(found.history));
                    status.putString("fake_signature", found.fake);
                }
                sendStatus(0, status);
                Log.i(TAG, name + " " + (installed ? "signatures=" + describe(found.signatures)
                        + " signingInfo=" + describe(found.signers) + " history="
                        + describe(found.history) + " fakeSignature=" + found.fake : "not installed"));
                judge(name, found, own, failures);
            }
        } catch (RuntimeException e) {
            failures.add("unexpected " + e);
        }
        Bundle result = new Bundle();
        result.putString("expect", mExpect);
        result.putString("verdict", failures.isEmpty() ? "PASS" : "FAIL");
        if (!failures.isEmpty()) {
            result.putString("failures", String.join("; ", failures));
        }
        finish(Activity.RESULT_OK, result);
    }

    private void judge(String name, Certs found, List<String> own, List<String> failures) {
        switch (mExpect) {
            case "report":
                return;
            case "none":
                if (found != null) {
                    failures.add(name + " is installed");
                }
                return;
            case "own":
            case "google":
            case "notgoogle":
                break;
            default:
                failures.add("unknown expect mode " + mExpect);
                return;
        }
        if (found == null) {
            failures.add(name + " is not installed");
            return;
        }
        List<String> everything = new ArrayList<>(found.signatures);
        everything.addAll(found.signers);
        everything.addAll(found.history);
        if (found.signatures.isEmpty() || found.signers.isEmpty()) {
            failures.add(name + " reports no certificate in signatures or in signingInfo");
        }
        switch (mExpect) {
            case "own":
                // The check means something only if the decoy would be spoofed but for its signer:
                // it must carry Google's certificate as a string in its fake-signature meta-data.
                if (!GOOGLE.equals(found.fake)) {
                    failures.add(name + " does not carry Google's certificate as fake-signature ("
                            + found.fake + ")");
                }
                // The decoys are signed by the AOSP test key, like this probe.
                for (List<String> list : found.all()) {
                    if (!list.equals(own)) {
                        failures.add(name + " reports " + describe(list) + ", not its own signer "
                                + describe(own));
                    }
                }
                break;
            case "google":
                for (List<String> list : found.all()) {
                    if (list.size() != 1 || !list.contains(GOOGLE)) {
                        failures.add(name + " reports " + describe(list) + ", not Google's alone");
                    }
                }
                break;
            default:
                if (everything.contains(GOOGLE)) {
                    failures.add(name + " reports Google's certificate");
                }
                break;
        }
    }

    /** The certificates PackageManager reports for a package, as SHA-256 digests. */
    private static final class Certs {
        final List<String> signatures = new ArrayList<>();
        final List<String> signers = new ArrayList<>();
        final List<String> history = new ArrayList<>();
        // SHA-256 of the DER certificate in the `fake-signature` meta-data, or "none" or "invalid".
        String fake = "none";

        List<List<String>> all() {
            return Arrays.asList(signatures, signers, history);
        }
    }

    /** The package's certificates, or null when it is not installed (recorded in `failures` if given). */
    @SuppressWarnings("deprecation")
    private Certs read(String name, List<String> failures) {
        PackageManager pm = getContext().getPackageManager();
        PackageInfo info;
        try {
            info = pm.getPackageInfo(name, PackageManager.GET_SIGNATURES
                    | PackageManager.GET_SIGNING_CERTIFICATES | PackageManager.GET_META_DATA);
        } catch (PackageManager.NameNotFoundException e) {
            if (failures != null) {
                failures.add(name + " is not installed");
            }
            return null;
        }
        Certs result = new Certs();
        if (info.applicationInfo != null && info.applicationInfo.metaData != null) {
            result.fake = fakeDigest(info.applicationInfo.metaData.getString("fake-signature"));
        }
        if (info.signatures != null) {
            for (Signature signature : info.signatures) {
                result.signatures.add(sha256(signature.toByteArray()));
            }
        }
        SigningInfo signing = info.signingInfo;
        if (signing != null) {
            Signature[] signers = signing.getApkContentsSigners();
            if (signers != null) {
                for (Signature signature : signers) {
                    result.signers.add(sha256(signature.toByteArray()));
                }
            }
            Signature[] history = signing.getSigningCertificateHistory();
            if (history != null) {
                for (Signature signature : history) {
                    result.history.add(sha256(signature.toByteArray()));
                }
            }
        }
        return result;
    }

    private static String fakeDigest(String hex) {
        if (hex == null || hex.isEmpty()) {
            return "none";
        }
        if (hex.length() % 2 != 0 || !hex.matches("[0-9a-f]+")) {
            return "invalid";
        }
        byte[] der = new byte[hex.length() / 2];
        for (int i = 0; i < der.length; i++) {
            der[i] = (byte) Integer.parseInt(hex.substring(2 * i, 2 * i + 2), 16);
        }
        return sha256(der);
    }

    private static String describe(List<String> digests) {
        List<String> named = new ArrayList<>();
        for (String digest : digests) {
            named.add(digest + (GOOGLE.equals(digest) ? "(google)" : MICROG.equals(digest) ? "(microg)" : ""));
        }
        return String.join(",", named);
    }

    private static String sha256(byte[] bytes) {
        try {
            StringBuilder hex = new StringBuilder();
            for (byte b : MessageDigest.getInstance("SHA-256").digest(bytes)) {
                hex.append(String.format(Locale.ROOT, "%02x", b));
            }
            return hex.toString();
        } catch (java.security.NoSuchAlgorithmException e) {
            throw new IllegalStateException(e);
        }
    }
}
