# SPDX-License-Identifier: GPL-2.0-or-later
# Helpers sourced by build-aosp.sh, after common.sh, which supplies fail().
# They keep every generated property file of a build output naming the build
# number of the build, which Android's build does not do on its own: it reads
# the number without depending on it, so an incremental build over an earlier
# release's output keeps a property file none of whose other inputs changed.
# The step touches only the product's output directory: the product's installed
# tree without obj/, whose packaging intermediates the target-files rule
# rebuilds whole, and Soong's intermediates. Each search root must be a
# physical directory of the output, never a link out of it.

# The bound of one listing of a search root, in seconds.
BUILD_PROPERTY_LIST_LIMIT=1800
# One run's scratch files, in the output directory, named by mktemp.
BUILD_PROPERTY_RAW=
BUILD_PROPERTY_LIST=
BUILD_PROPERTY_COUNT=0
BUILD_PROPERTY_MISMATCHES=

# The properties of a file are read as verify-target-files.py reads them: a line
# splits at its first "=", and blanks and a carriage return around key and value
# go. The file is stale when a build.version.incremental value is not exactly
# the number, compared as strings.
BUILD_PROPERTY_AWK='
{
	split_at = index($0, "=")
	if (split_at == 0)
		next
	key = substr($0, 1, split_at - 1)
	value = substr($0, split_at + 1)
	sub(/^[ \t\r]+/, "", key); sub(/[ \t\r]+$/, "", key)
	sub(/^[ \t\r]+/, "", value); sub(/[ \t\r]+$/, "", value)
	if (key ~ /^ro\.([a-z0-9_]+\.)?build\.version\.incremental$/ && (value "") != (number ""))
		stale = 1
}
END { exit !stale }'

# physical_path DIRECTORY: set BUILD_PROPERTY_PHYSICAL to the physical path of an
# existing directory. The sentinel keeps a name's own trailing newlines, which a
# command substitution would strip; that is also why the path is not printed.
physical_path()
{
	BUILD_PROPERTY_PHYSICAL=$(CDPATH= cd -- "$1" 2>/dev/null && pwd -P && printf x) || {
		BUILD_PROPERTY_PHYSICAL=
		return 1
	}
	BUILD_PROPERTY_PHYSICAL=${BUILD_PROPERTY_PHYSICAL%x}
	BUILD_PROPERTY_PHYSICAL=${BUILD_PROPERTY_PHYSICAL%$'\n'}
}

# check_search_root OUT_DIR OUT_PHYSICAL SUFFIX
#
# Accept OUT_DIR/SUFFIX as a search root: return 1 if it is absent, fail unless
# it is a directory whose physical path is OUT_PHYSICAL/SUFFIX, with no link on
# the way. A root that is a link, or behind one, would be listed, and its files
# removed, outside the output directory.
check_search_root()
{
	local out_dir=$1 out_physical=$2 suffix=$3
	local rest=$3 current=$1 component physical
	while [[ -n "$rest" ]]; do
		component=${rest%%/*}
		if [[ "$rest" == */* ]]; then
			rest=${rest#*/}
		else
			rest=
		fi
		case "$component" in
		'' | . | ..) fail "the search root is not a normalized path below the output directory: $out_dir/$suffix" ;;
		esac
		current=$current/$component
		if [[ -L "$current" ]]; then
			fail "the search root $out_dir/$suffix is behind a symbolic link: $current"
		fi
	done
	[[ -e "$current" ]] || return 1
	[[ -d "$current" ]] || fail "the search root is not a directory: $current"
	physical_path "$current" || BUILD_PROPERTY_PHYSICAL=
	physical=$BUILD_PROPERTY_PHYSICAL
	[[ "$physical" == "$out_physical/$suffix" ]] ||
		fail "the search root $current is not inside the output directory: it resolves to ${physical:-nowhere}"
}

# list_property_files ROOT [PRUNE]
#
# Append to the run's list, NUL-separated and absolute, every build.prop and
# prop.default below the physical directory ROOT. The listing runs inside ROOT
# on relative patterns, so no character of ROOT reaches a pattern, and PRUNE
# (a relative path) is left alone. It is bounded, and a listing that fails or
# times out is an error: a partial scan would pass a stale file.
list_property_files()
{
	local root=$1 relative status
	local -a prune=()
	if [[ -n "${2:-}" ]]; then
		prune=(-path "$2" -prune -o)
	fi
	if (CDPATH= cd -- "$root" && timeout --foreground -k 10 "$BUILD_PROPERTY_LIST_LIMIT" \
		find . ${prune[@]+"${prune[@]}"} -type f \( -name build.prop -o -name prop.default \) -print0) \
		>"$BUILD_PROPERTY_RAW"; then
		status=0
	else
		status=$?
	fi
	if [[ $status -ne 0 ]]; then
		fail "cannot list the property files under $root (exit status $status)"
	fi
	while IFS= read -r -d '' relative; do
		printf '%s\0' "$root/${relative#./}" >>"$BUILD_PROPERTY_LIST"
	done <"$BUILD_PROPERTY_RAW"
}

# each_stale_build_property SCOPE OUT_DIR PRODUCT_DIR NUMBER COMMAND
#
# Run COMMAND with the path of every property file whose build.version.incremental
# names a number other than NUMBER. SCOPE "all" lists the product's installed
# tree and Soong's intermediates; "installed" lists the installed tree alone.
# Both listings finish before COMMAND runs, so a listing that fails leaves every
# file in place. A build.prop or prop.default anywhere under the product output
# that names another number is what the scan after the build refuses, so the
# removal and the scan share this one rule.
#
# The caller runs it in a subshell: it sets that shell's EXIT trap, which removes
# the run's private scratch files when the shell ends, a signal's end included.
each_stale_build_property()
{
	local scope=$1 out_dir=$2 product_dir=$3 number=$4 visit=$5
	local out_physical product_suffix path status product_present soong_present
	case "$product_dir" in
	"$out_dir"/?*) product_suffix=${product_dir#"$out_dir"/} ;;
	*) fail "the product output is not inside the output directory: $product_dir" ;;
	esac
	physical_path "$out_dir" || BUILD_PROPERTY_PHYSICAL=
	out_physical=$BUILD_PROPERTY_PHYSICAL
	[[ -n "$out_physical" ]] || fail "cannot resolve the output directory: $out_dir"
	# The trap is set before either file exists, so no end of the shell leaves one.
	BUILD_PROPERTY_RAW=
	BUILD_PROPERTY_LIST=
	trap 'rm -f -- ${BUILD_PROPERTY_RAW:+"$BUILD_PROPERTY_RAW"} ${BUILD_PROPERTY_LIST:+"$BUILD_PROPERTY_LIST"}' EXIT
	BUILD_PROPERTY_RAW=$(mktemp "$out_physical/.build-number-properties.XXXXXX") ||
		fail "cannot create a scratch file in $out_dir"
	BUILD_PROPERTY_LIST=$(mktemp "$out_physical/.build-number-properties.XXXXXX") ||
		fail "cannot create a scratch file in $out_dir"
	# Every root is checked before any is listed.
	product_present=0
	soong_present=0
	if check_search_root "$out_dir" "$out_physical" "$product_suffix"; then
		product_present=1
	fi
	if [[ "$scope" == all ]] && check_search_root "$out_dir" "$out_physical" soong/.intermediates; then
		soong_present=1
	fi
	if [[ $product_present -eq 1 ]]; then
		list_property_files "$out_physical/$product_suffix" ./obj
	fi
	if [[ $soong_present -eq 1 ]]; then
		list_property_files "$out_physical/soong/.intermediates"
	fi
	while IFS= read -r -d '' path; do
		if awk -v number="$number" "$BUILD_PROPERTY_AWK" "$path" </dev/null; then
			"$visit" "$path"
		else
			status=$?
			if [[ $status -ne 1 ]]; then
				fail "cannot read the property file $path (exit status $status)"
			fi
		fi
	done <"$BUILD_PROPERTY_LIST"
}

remove_stale_build_property()
{
	printf '%s\n' "removing a property file that names another build number: $1 ($(
		grep -E '^[[:space:]]*ro\.([a-z0-9_]+\.)?build\.version\.incremental[[:space:]]*=' "$1" </dev/null |
			tr -d '\r' | tr '\n' ' '))"
	if ! rm -f -- "$1"; then
		fail "cannot remove the property file that names another build number: $1"
	fi
	BUILD_PROPERTY_COUNT=$((BUILD_PROPERTY_COUNT + 1))
}

note_build_property_mismatch()
{
	BUILD_PROPERTY_MISMATCHES="$BUILD_PROPERTY_MISMATCHES
	$1"
}

# remove_stale_build_properties OUT_DIR PRODUCT_DIR NUMBER
#
# Before a build: remove, by the path found, every property file under the
# output directory that names another build number than NUMBER, so that the
# build makes it again. An absent output directory, or none stale, does nothing.
remove_stale_build_properties()
{
	local out_dir=$1 product_dir=$2 number=$3
	[[ -d "$out_dir" ]] || return 0
	(
		BUILD_PROPERTY_COUNT=0
		each_stale_build_property all "$out_dir" "$product_dir" "$number" remove_stale_build_property
		if [[ $BUILD_PROPERTY_COUNT -gt 0 ]]; then
			printf '%s\n' "removed $BUILD_PROPERTY_COUNT property files that name another build number than $number"
		fi
	)
}

# verify_build_number_properties OUT_DIR PRODUCT_DIR NUMBER
#
# After a build: refuse it if an installed property file names another build
# number than NUMBER.
verify_build_number_properties()
{
	local out_dir=$1 product_dir=$2 number=$3
	(
		BUILD_PROPERTY_MISMATCHES=
		each_stale_build_property installed "$out_dir" "$product_dir" "$number" note_build_property_mismatch
		[[ -z "$BUILD_PROPERTY_MISMATCHES" ]] ||
			fail "the build left property files that name another build number than $number:$BUILD_PROPERTY_MISMATCHES"
	)
}
