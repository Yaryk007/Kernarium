#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Builds the three APKs of the microG signature spoofing probe from the SDK tools in an Android source tree's
# prebuilts (aapt2, a JDK, R8's D8, zipalign, apksigner) and the AOSP test key. It reads the tree and writes only
# below OUTPUT_DIR, so it needs no Soong run and leaves nothing in a product output.
#
#   AndariumMicrogTests.apk          the check (package com.android.frameworks.andariummicrog.tests)
#   AndariumMicrogDecoyServices.apk  a code-less package named com.google.android.gms
#   AndariumMicrogDecoyStore.apk     a code-less package named com.android.vending
#
# All three are signed with the AOSP test key, never with a release key, and the script checks the signer and the
# package names of what it built. Usage: build-apks.sh ANDROID_ROOT OUTPUT_DIR
set -euo pipefail
export LC_ALL=C
HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
ROOT=${1:?the Android source tree (its prebuilts are the SDK)}
OUT=${2:?the output directory (outside the source tree)}
ROOT=$(CDPATH= cd -- "$ROOT" && pwd)
mkdir -p "$OUT"
OUT=$(CDPATH= cd -- "$OUT" && pwd)
case "$OUT/" in "$ROOT"/*) echo "the output directory must be outside the source tree" >&2; exit 2 ;; esac

TOOLS=$ROOT/prebuilts/sdk/tools/linux
JDK=$ROOT/prebuilts/jdk/jdk21/linux-x86/bin
ANDROID_JAR=$ROOT/prebuilts/sdk/current/public/android.jar
D8=$ROOT/prebuilts/r8/r8.jar
KEYDIR=$ROOT/build/make/target/product/security
for f in "$TOOLS/bin/aapt2" "$TOOLS/bin/zipalign" "$TOOLS/lib/apksigner.jar" "$JDK/javac" "$JDK/java" "$ANDROID_JAR" "$D8" \
	"$KEYDIR/testkey.pk8" "$KEYDIR/testkey.x509.pem"; do
	[ -e "$f" ] || { echo "missing $f" >&2; exit 2; }
done

work=$(mktemp -d "$OUT/.work.XXXXXX")
trap 'rm -rf "$work"' EXIT
sign() {  # unsigned apk, output apk
	"$TOOLS/bin/zipalign" -f -p 4 "$1" "$work/aligned.apk"
	"$JDK/java" -jar "$TOOLS/lib/apksigner.jar" sign --key "$KEYDIR/testkey.pk8" --cert "$KEYDIR/testkey.x509.pem" \
		--out "$2" "$work/aligned.apk"
	rm -f "$work/aligned.apk" "$2.idsig"
}

# The decoys: a manifest and nothing else.
for pair in "decoy-services:AndariumMicrogDecoyServices" "decoy-store:AndariumMicrogDecoyStore"; do
	dir=${pair%%:*}; name=${pair#*:}
	"$TOOLS/bin/aapt2" link --manifest "$HERE/$dir/AndroidManifest.xml" -I "$ANDROID_JAR" -o "$work/$name.unsigned.apk"
	sign "$work/$name.unsigned.apk" "$OUT/$name.apk"
done

# The check: the instrumentation, compiled against the platform API and converted to dex.
mkdir "$work/classes" "$work/dex"
"$JDK/javac" -source 11 -target 11 -Xlint:all -cp "$ANDROID_JAR" -d "$work/classes" \
	$(find "$HERE/src" -name '*.java' | LC_ALL=C sort)
"$JDK/java" -cp "$D8" com.android.tools.r8.D8 --lib "$ANDROID_JAR" --min-api 24 --release --output "$work/dex" \
	$(find "$work/classes" -name '*.class' | LC_ALL=C sort)
"$TOOLS/bin/aapt2" link --manifest "$HERE/AndroidManifest.xml" -I "$ANDROID_JAR" -o "$work/AndariumMicrogTests.unsigned.apk"
(CDPATH= cd -- "$work/dex" && zip -q "$work/AndariumMicrogTests.unsigned.apk" classes.dex)
sign "$work/AndariumMicrogTests.unsigned.apk" "$OUT/AndariumMicrogTests.apk"

# What was built: the package names and the signer (the AOSP test key's certificate).
want=$(openssl x509 -in "$KEYDIR/testkey.x509.pem" -outform DER | sha256sum | cut -d' ' -f1)
for pair in "AndariumMicrogTests:com.android.frameworks.andariummicrog.tests" "AndariumMicrogDecoyServices:com.google.android.gms" \
	"AndariumMicrogDecoyStore:com.android.vending"; do
	name=${pair%%:*}; package=${pair#*:}
	"$TOOLS/bin/aapt2" dump badging "$OUT/$name.apk" | grep -q "^package: name='$package'" ||
		{ echo "$name.apk is not the package $package" >&2; exit 3; }
	got=$("$JDK/java" -jar "$TOOLS/lib/apksigner.jar" verify --print-certs "$OUT/$name.apk" |
		sed -n 's/^Signer #1 certificate SHA-256 digest: //p')
	[ "$got" = "$want" ] || { echo "$name.apk is not signed with the AOSP test key ($got, expected $want)" >&2; exit 3; }
	echo "$name.apk: package $package, signed by the AOSP test key $want"
done
(CDPATH= cd -- "$OUT" && sha256sum AndariumMicrogTests.apk AndariumMicrogDecoyServices.apk AndariumMicrogDecoyStore.apk)
