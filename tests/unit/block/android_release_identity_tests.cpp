// SPDX-License-Identifier: GPL-2.0-or-later

/* What a release says it is, as a launch states it to the emulator: the
 * manifest's ID (the top-level one, whatever the nested objects hold), the
 * SHA-256 of the manifest's bytes and the layout's digest of the system
 * partition. Every value here is synthetic. */

#include "android_boot.hpp"
#include "android_release_identity.hpp"
#include "require.hpp"
#include "scratch_directory.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace {

namespace android = uml::android;
namespace fs = std::filesystem;
using uml::testing::require;

constexpr std::string_view manifestText =
	"{\"release_id\": \"r-test.1\", \"producer\": {\"release_id\": "
	"\"decoy\", \"n\": [1, 2.5e3, null]}, \"ok\": true}\n";
/* sha256sum of manifestText. */
constexpr std::string_view manifestDigest =
	"36e2bad6e3d7b5ee6890f1c539fb31f9db3ae5c1ea6be3e600a93f7ddfde9f1f";

void write(const fs::path &path, std::string_view text)
{
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	file.write(text.data(), static_cast<std::streamsize>(text.size()));
}

void testTopLevelString()
{
	const auto member = [](std::string_view text, std::string_view key) {
		return android::topLevelString(text, key);
	};
	require(member(manifestText, "release_id") == "r-test.1",
		"the top-level member was not the one read");
	require(member("{\"a\": {\"k\": \"inner\"}, \"k\": \"outer\"}", "k") ==
			"outer",
		"a nested member was read for the top-level one");
	require(member("{\"a\": [{\"k\": \"inner\"}]}", "k") == std::nullopt,
		"a member of an array was read");
	require(member("{\"k\": 7}", "k") == std::nullopt,
		"a number was read as a string");
	require(member("{\"k\": \"a\\\"b\"}", "k") == std::nullopt,
		"a string with an escape was read");
	require(member("{\"k\": \"a\", \"k\": \"a\"}", "k") == std::nullopt,
		"a repeated member was read");
	require(member("{\"k\": \"a\"} x", "k") == std::nullopt,
		"text after the document was read past");
	require(member("{\"k\": \"a\"", "k") == std::nullopt,
		"a document that ends early was read");
	require(member("{\"k\": \"a\", \"b\": [1, {\"c\": true}", "k") ==
			std::nullopt,
		"an unclosed nested value was read past");
	require(member("[\"k\"]", "k") == std::nullopt,
		"an array was read as an object");
	require(member("{}", "k") == std::nullopt, "an empty object had a member");
	require(member("{\"a\": \"x\\\\\", \"k\": \"b\"}", "k") == "b",
		"an escaped backslash ended a string early");
	require(member("{\"k\" : \"v\" , \"z\" : false }", "k") == "v",
		"spaces around the members were not skipped");
}

void testIdentity(const fs::path &directory, const fs::path &fixtureRoot)
{
	const auto layout = android::readLayout(
		fixtureRoot / "release" / android::releaseLayout);
	const auto &system = layout.systemPartition("system");

	write(directory / "release-manifest.json", manifestText);
	const auto identity = android::readReleaseIdentity(directory, layout);
	require(identity.has_value(), "the identity of a release was not read");
	require(identity->id == "r-test.1", "the release's ID was not read");
	require(identity->manifestSha256 == manifestDigest,
		"the manifest's digest was not the SHA-256 of its bytes");
	require(identity->systemSha256 == system.sha256,
		"the system partition's digest was not the layout's");

	/* A manifest with no valid ID states no identity. */
	for (const std::string_view bad : {
		     "{\"release_id\": \"UPPER\"}",
		     "{\"release_id\": \"\"}",
		     "{\"release_id\": \"-lead\"}",
		     "{\"release_id\": \"a b\"}",
		     "{\"id\": \"r-test.1\"}",
		     "{\"release_id\": 7}",
		     "not json"}) {
		write(directory / "release-manifest.json", bad);
		require(!android::readReleaseIdentity(directory, layout),
			"a manifest with no valid ID gave an identity");
	}
	fs::remove(directory / "release-manifest.json");
	require(!android::readReleaseIdentity(directory, layout),
		"a release with no manifest gave an identity");
	fs::create_directory(directory / "release-manifest.json");
	require(!android::readReleaseIdentity(directory, layout),
		"a directory gave an identity");
	fs::remove(directory / "release-manifest.json");
	write(directory / "release-manifest.json", "");
	require(!android::readReleaseIdentity(directory, layout),
		"an empty manifest gave an identity");
}

} // namespace

int main(int argc, char *argv[])
{
	if (argc != 2) {
		std::fprintf(stderr, "usage: %s FIXTURE_DIRECTORY\n", argv[0]);
		return 2;
	}
	const uml::testing::ScratchSubdirectory scratch{
		"uml-android-release-identity.XXXXXX"};
	testTopLevelString();
	testIdentity(scratch.path(), fs::path{argv[1]});
	return 0;
}
