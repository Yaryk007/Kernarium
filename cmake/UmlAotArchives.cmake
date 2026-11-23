# SPDX-License-Identifier: GPL-2.0-or-later

# The one switch for the archive of Andarium's Release configuration and the
# device runtimes it embeds: android/lineage-24/metadata/accepted-aot.env says
# whether the accepted AOT artifact may be in them
# (LINEAGE24_ACCEPTED_AOT_ARCHIVES). uml_aot_accepted_artifact, which
# darwin/cpu/CMakeLists.txt calls, takes the accepted artifact into an iOS
# device runtime only when it says "yes"; an artifact a build names
# (UML_AOT_ARTIFACT) is for measuring on a device, and
# scripts/check-embedded-artifacts.py refuses an archive that carries the
# component while it says "no".

# uml_aot_archives_state(<variable> <descriptor>)
#
# Sets <variable> to "yes" or "no" as the descriptor states, or to "none" when
# there is no descriptor (no artifact is accepted, and none may be archived).
function(uml_aot_archives_state variable descriptor)
    if(NOT EXISTS "${descriptor}")
        set(${variable} none PARENT_SCOPE)
        return()
    endif()
    file(STRINGS "${descriptor}" _line
        REGEX "^LINEAGE24_ACCEPTED_AOT_ARCHIVES='.*'$")
    string(REGEX REPLACE "^[A-Z0-9_]+='(.*)'$" "\\1" _state "${_line}")
    if(NOT _state STREQUAL "yes" AND NOT _state STREQUAL "no")
        message(FATAL_ERROR
            "${descriptor} states no LINEAGE24_ACCEPTED_AOT_ARCHIVES of yes "
            "or no")
    endif()
    set(${variable} ${_state} PARENT_SCOPE)
endfunction()

# uml_aot_accepted_artifact(<source-dir> <apple-platform> <mode>
#                           <source-variable> <directory-variable>
#                           <descriptor-variable>)
#
# The accepted artifact of a product: the descriptor names the release it was
# built from, and it is installed below .build/android/aot beside the accepted
# release. An iOS device runtime (<apple-platform> "ios") takes it only when
# the descriptor says "yes" for archives; macOS and the iOS Simulator take
# every accepted artifact. When one is taken the function sets
# <source-variable> to "artifact", <directory-variable> to its directory and
# <descriptor-variable> to the descriptor; otherwise it leaves the three alone.
# <mode> is the UML_AOT_MODE, which only decides whether a missing install is
# said.
function(uml_aot_accepted_artifact source_dir apple_platform mode
         source_variable directory_variable descriptor_variable)
    set(accepted "${source_dir}/android/lineage-24/metadata/accepted-aot.env")
    set(release "${source_dir}/android/lineage-24/metadata/accepted-release.env")
    if(NOT EXISTS "${accepted}")
        return()
    endif()
    file(STRINGS "${accepted}" accepted_id
        REGEX "^LINEAGE24_ACCEPTED_AOT_RELEASE_ID='.*'$")
    file(STRINGS "${release}" release_id
        REGEX "^LINEAGE24_ACCEPTED_RELEASE_ID='.*'$")
    file(STRINGS "${release}" release_directory
        REGEX "^LINEAGE24_ACCEPTED_DIRECTORY_NAME='.*'$")
    string(REGEX REPLACE "^[A-Z0-9_]+='(.*)'$" "\\1" accepted_id "${accepted_id}")
    string(REGEX REPLACE "^[A-Z0-9_]+='(.*)'$" "\\1" release_id "${release_id}")
    string(REGEX REPLACE "^[A-Z0-9_]+='(.*)'$" "\\1"
        release_directory "${release_directory}")
    if(NOT accepted_id STREQUAL release_id)
        message(FATAL_ERROR
            "accepted-aot.env accepts the artifact of '${accepted_id}', but "
            "the accepted release is '${release_id}'")
    endif()
    uml_aot_archives_state(archives "${accepted}")
    set(archive_product FALSE)
    if(apple_platform STREQUAL "ios")
        set(archive_product TRUE)
    endif()
    set(installed "${source_dir}/.build/android/aot/${release_directory}")
    if(archive_product AND NOT archives STREQUAL "yes")
        message(STATUS
            "The accepted AOT artifact is not accepted for archives yet "
            "(LINEAGE24_ACCEPTED_AOT_ARCHIVES): building without the AOT "
            "component")
    elseif(EXISTS "${installed}/artifact.json")
        set(${source_variable} artifact PARENT_SCOPE)
        set(${directory_variable} "${installed}" PARENT_SCOPE)
        set(${descriptor_variable} "${accepted}" PARENT_SCOPE)
    elseif(mode STREQUAL "AUTO")
        message(STATUS
            "The accepted AOT artifact is not installed at ${installed}: "
            "building without the AOT component")
    endif()
endfunction()
