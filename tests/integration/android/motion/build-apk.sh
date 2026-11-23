#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Builds AndariumMotionProbe.apk from the SDK tools in an Android source tree's prebuilts (aapt2, a JDK, R8's D8,
# zipalign, apksigner) and the AOSP test key. It reads the tree and writes only below OUTPUT_DIR, so it needs no Soong
# run and leaves nothing in a product output. The APK is signed with the AOSP test key, never with a release key, and
# the script checks the package name and the signer of what it built.
#
# Usage: build-apk.sh ANDROID_ROOT OUTPUT_DIR
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
mkdir "$work/classes" "$work/dex"
"$JDK/javac" -source 11 -target 11 -Xlint:all -cp "$ANDROID_JAR" -d "$work/classes" \
	$(find "$HERE/app/src" -name '*.java' | LC_ALL=C sort)
"$JDK/java" -cp "$D8" com.android.tools.r8.D8 --lib "$ANDROID_JAR" --min-api 33 --release --output "$work/dex" \
	$(find "$work/classes" -name '*.class' | LC_ALL=C sort)
# The manifest names no SDK levels, as the Soong module's does; the levels come from here.
"$TOOLS/bin/aapt2" link --manifest "$HERE/app/AndroidManifest.xml" -I "$ANDROID_JAR" \
	--min-sdk-version 33 --target-sdk-version 36 -o "$work/AndariumMotionProbe.unsigned.apk"
(CDPATH= cd -- "$work/dex" && zip -q "$work/AndariumMotionProbe.unsigned.apk" classes.dex)
"$TOOLS/bin/zipalign" -f -p 4 "$work/AndariumMotionProbe.unsigned.apk" "$work/aligned.apk"
"$JDK/java" -jar "$TOOLS/lib/apksigner.jar" sign --key "$KEYDIR/testkey.pk8" --cert "$KEYDIR/testkey.x509.pem" \
	--out "$OUT/AndariumMotionProbe.apk" "$work/aligned.apk"
rm -f "$OUT/AndariumMotionProbe.apk.idsig"

want=$(openssl x509 -in "$KEYDIR/testkey.x509.pem" -outform DER | sha256sum | cut -d' ' -f1)
"$TOOLS/bin/aapt2" dump badging "$OUT/AndariumMotionProbe.apk" | grep -q "^package: name='org.example.andarium.motionprobe'" ||
	{ echo "AndariumMotionProbe.apk is not the package org.example.andarium.motionprobe" >&2; exit 3; }
got=$("$JDK/java" -jar "$TOOLS/lib/apksigner.jar" verify --print-certs "$OUT/AndariumMotionProbe.apk" |
	sed -n 's/^Signer #1 certificate SHA-256 digest: //p')
[ "$got" = "$want" ] || { echo "AndariumMotionProbe.apk is not signed with the AOSP test key ($got, expected $want)" >&2; exit 3; }
echo "AndariumMotionProbe.apk: package org.example.andarium.motionprobe, signed by the AOSP test key $want"
(CDPATH= cd -- "$OUT" && sha256sum AndariumMotionProbe.apk)
