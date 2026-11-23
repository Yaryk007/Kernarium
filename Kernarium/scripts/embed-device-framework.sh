#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Embed a framework that exists for the iOS device alone: the Run Script
# phase of an application target that also builds for the iOS Simulator calls
# it in place of an Embed Frameworks entry.
#
# usage: embed-device-framework.sh [--optional] XCFRAMEWORK
#
# KosmicKrisp is the Vulkan driver of the device's GPU stack and requires
# Metal 4, which the Simulator does not have, so its XCFramework has a device
# slice alone. Xcode resolves a slice of every XCFramework in an Embed
# Frameworks phase for the SDK it builds, and a platform filter cannot name
# the device apart from the Simulator, so a device-only framework cannot sit
# in that phase of a target the Simulator also builds. This script embeds it
# as the phase does for a device build, with no headers or modules and the
# target's signing identity, and for any other SDK embeds nothing: the
# Simulator's GPU stack has no Vulkan driver to carry.
#
# Xcode runs it with the target's build settings in the environment, on every
# build. It touches the embedded framework only when the framework, or how it
# is signed, changed: a copy and a signature made again on an unchanged build
# alter a file the application's seal covers, and Xcode, which then skips
# signing the application, leaves that seal invalid. A stamp in the target's
# intermediates records the framework, and the signing, it embedded and what
# the embedded framework held afterwards; an embedded framework that matches
# is left as it is.
#
# --optional embeds a framework that may not be packaged at all, for the
# device and the Simulator alike (UmlArm64Aot: the packaging step makes it
# only when the Android runtime links it). An XCFramework that is not there,
# or has no slice for the SDK being built, embeds nothing and removes a copy
# an earlier build left, so an application never carries a framework its
# runtime does not load. It also fails when an embedded framework's load
# commands name the missing one: dyld refuses to launch an application without
# a framework its runtime links, so the build stops where the slice is missing
# rather than at the first launch.
set -eu

fail()
{
	printf '%s\n' "embed-device-framework.sh: $*" >&2
	exit 1
}

OPTIONAL=0
if [ "${1:-}" = --optional ]; then
	OPTIONAL=1
	shift
fi
[ "$#" -eq 1 ] || fail "usage: embed-device-framework.sh [--optional] XCFRAMEWORK"
XCFRAMEWORK=$1
NAME=$(basename -- "$XCFRAMEWORK" .xcframework)

case "${PLATFORM_NAME:-}" in
iphoneos) SLICE_NAME=ios-arm64 ;;
iphonesimulator)
	if [ "$OPTIONAL" -eq 0 ]; then
		printf '%s\n' "$NAME is a device-only framework: not embedded for $PLATFORM_NAME"
		exit 0
	fi
	SLICE_NAME=ios-arm64-simulator
	;;
*)
	printf '%s\n' "$NAME is a device-only framework: not embedded for ${PLATFORM_NAME:-an unknown SDK}"
	exit 0
	;;
esac

[ -n "${TARGET_BUILD_DIR:-}" ] && [ -n "${FRAMEWORKS_FOLDER_PATH:-}" ] &&
	[ -n "${TARGET_TEMP_DIR:-}" ] || fail "run it from an Xcode build phase"

DESTINATION="$TARGET_BUILD_DIR/$FRAMEWORKS_FOLDER_PATH/$NAME.framework"
STAMP="$TARGET_TEMP_DIR/embed-device-framework-$NAME.stamp"

# linking_framework: the first embedded framework, other than this one, whose
# load commands name it. It is empty when none does.
linking_framework()
{
	for candidate in "$TARGET_BUILD_DIR/$FRAMEWORKS_FOLDER_PATH"/*.framework; do
		[ -d "$candidate" ] || continue
		candidate_name=$(basename -- "$candidate" .framework)
		[ "$candidate_name" != "$NAME" ] && [ -f "$candidate/$candidate_name" ] || continue
		if /usr/bin/otool -L "$candidate/$candidate_name" 2>/dev/null |
			LC_ALL=C grep -Fq "@rpath/$NAME.framework/$NAME"; then
			printf '%s\n' "$candidate_name"
			return 0
		fi
	done
	return 0
}

SLICE="$XCFRAMEWORK/$SLICE_NAME/$NAME.framework"
if [ ! -d "$SLICE" ]; then
	if [ "$OPTIONAL" -eq 1 ]; then
		LINKER=$(linking_framework)
		[ -z "$LINKER" ] ||
			fail "$LINKER links $NAME, and $XCFRAMEWORK has no $SLICE_NAME slice: package it with Kernarium/scripts/package-ios-frameworks.sh"
		rm -rf -- "$DESTINATION" "$STAMP"
		printf '%s\n' "$NAME is not packaged for ${PLATFORM_NAME}: not embedded"
		exit 0
	fi
	fail "$XCFRAMEWORK has no device slice; package it with Kernarium/scripts/package-ios-frameworks.sh"
fi

# tree_digest DIRECTORY: one digest of the mode, content and path of every
# file and link below DIRECTORY that is embedded, in a fixed order. The names
# are those the copy below leaves out.
tree_digest()
{
	(
		CDPATH= cd -- "$1"
		find . \( -name .DS_Store -o -name Headers -o -name PrivateHeaders \
			-o -name Modules -o -name '*.tbd' \) -prune \
			-o \( -type f -o -type l \) -print | LC_ALL=C sort |
			while IFS= read -r entry; do
				if [ -L "$entry" ]; then
					printf 'link %s %s\n' "$entry" "$(readlink -- "$entry")"
				else
					printf '%s %s %s\n' "$(stat -f %Lp -- "$entry")" \
						"$(/usr/bin/shasum -a 256 <"$entry" | cut -c1-64)" "$entry"
				fi
			done
	) | /usr/bin/shasum -a 256 | cut -c1-64
}

# The framework as embedded: its content, the signing it gets, and this
# script, which decides both.
embed_request()
{
	printf '%s\n%s\n%s\n%s\n%s\n' "$(tree_digest "$SLICE")" \
		"${CODE_SIGNING_ALLOWED:-YES}" "${EXPANDED_CODE_SIGN_IDENTITY:-}" \
		"${OTHER_CODE_SIGN_FLAGS:-}" "$(/usr/bin/shasum -a 256 <"$0" | cut -c1-64)" |
		/usr/bin/shasum -a 256 | cut -c1-64
}

# embedded_up_to_date REQUEST: the stamp names this request and the embedded
# framework still holds what that embedding left.
embedded_up_to_date()
{
	[ -f "$STAMP" ] && [ -d "$DESTINATION" ] || return 1
	[ "$(cat -- "$STAMP")" = "$1 $(tree_digest "$DESTINATION")" ]
}

# This framework bypasses Xcode's Embed Frameworks phase, so its symbols must
# follow the same path explicitly. Xcode gathers that directory into dSYMs in
# the archive; debug symbols never belong inside the application bundle.
if [ "${ACTION:-}" = install ]; then
	SYMBOLS="$XCFRAMEWORK/$SLICE_NAME/dSYMs/$NAME.framework.dSYM"
	[ -d "$SYMBOLS" ] || fail "missing debug symbols: $SYMBOLS; repackage the framework"
	[ -n "${DWARF_DSYM_FOLDER_PATH:-}" ] || fail "the archive has no debug symbols directory"
	mkdir -p -- "$DWARF_DSYM_FOLDER_PATH"
	/usr/bin/rsync -a --delete "$SYMBOLS/" \
		"$DWARF_DSYM_FOLDER_PATH/$NAME.framework.dSYM/"
fi

REQUEST=$(embed_request)
if embedded_up_to_date "$REQUEST"; then
	printf '%s\n' "$NAME is already embedded for $PLATFORM_NAME"
	exit 0
fi

rm -f -- "$STAMP"
mkdir -p -- "$TARGET_BUILD_DIR/$FRAMEWORKS_FOLDER_PATH" "$TARGET_TEMP_DIR"
rm -rf -- "$DESTINATION"
/usr/bin/rsync -a \
	--exclude .DS_Store --exclude Headers --exclude PrivateHeaders \
	--exclude Modules --exclude '*.tbd' \
	"$SLICE/" "$DESTINATION/"

# Sign it on copy, as the Embed Frameworks phase's CodeSignOnCopy does.
if [ "${CODE_SIGNING_ALLOWED:-YES}" = YES ] &&
	[ -n "${EXPANDED_CODE_SIGN_IDENTITY:-}" ]; then
	# shellcheck disable=SC2086
	/usr/bin/codesign --force --sign "$EXPANDED_CODE_SIGN_IDENTITY" \
		${OTHER_CODE_SIGN_FLAGS:-} \
		--preserve-metadata=identifier,entitlements,flags \
		--generate-entitlement-der "$DESTINATION"
fi
printf '%s %s\n' "$REQUEST" "$(tree_digest "$DESTINATION")" >"$STAMP"
printf '%s\n' "embedded $NAME for $PLATFORM_NAME"
