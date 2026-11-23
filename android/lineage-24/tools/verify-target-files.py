#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import functools
import hashlib
import io
import json
import re
import stat
import struct
import sys
import zipfile
import zlib
import xml.etree.ElementTree as ET
from pathlib import Path, PurePosixPath
from collections.abc import Callable
from typing import NamedTuple

sys.path.insert(0, str(Path(__file__).resolve().parent))

import android_image  # noqa: E402
import camera_layouts  # noqa: E402
import default_wallpaper  # noqa: E402
from release_common import (  # noqa: E402
    BUILD_VARIANTS,
    OTA_CERTIFICATE_ARCHIVES,
    configured_build_variant,
    ota_certificate_member,
    sha256_path,
)


ET_EXEC = 2
ET_DYN = 3
MAX_PROGRAM_HEADERS_SIZE = 1 << 20
APEX_PAYLOAD_MAGIC_SIZE = android_image.EROFS_SUPERBLOCK_OFFSET + android_image.EROFS_SUPERBLOCK_SIZE
EXT4_MAGIC_OFFSET = 1024 + 0x38
EXT4_MAGIC = b"\x53\xef"
# The one APEX whose payload stays ext4: Soong installs the checked-in presigned CTS shim
# (system/apex/shim/prebuilts), which no product setting recompresses.
APEX_EXT4_PAYLOADS = frozenset({"SYSTEM/apex/com.android.apex.cts.shim.apex"})
# The Mesa Virgl GLES stack: the EGL and GLES libraries the platform loader
# finds through ro.hardware.egl, and the gallium driver they link. The boot
# command line's androidboot.hardware.egl must name the same suffix.
MESA_EGL_SUFFIX = "kernarium"
MESA_GALLIUM_DRIVER = "venus_gallium_dri.so"
# Both userspace ABIs load the stack: a 64-bit application finds it in lib64,
# and an armeabi-v7a one in lib. The ABI directory decides the ELF class
# check_mesa_gles_stack requires.
MESA_ABI_DIRECTORIES = {"lib64": android_image.EM_AARCH64, "lib": android_image.EM_ARM}
# The hardware video codecs: the vendor Codec2 service of external/v4l2_codec2
# and the components it serves, two encoders and three decoders. The codec list
# names exactly those, beside the updates to the software decoders' limits. The
# properties enable those components, admit one instance per stream the host
# serves, and state the pixel rate the host sustains, which the service reports
# as its capacity and which no published performance point may exceed.
VIDEO_ENCODER_SERVICE = "android.hardware.media.c2-service-v4l2"
VIDEO_ENCODER_CODECS = {
    "c2.v4l2.avc.encoder": "video/avc",
    "c2.v4l2.hevc.encoder": "video/hevc",
}
VIDEO_ENCODER_PROPERTIES = {
    "ro.vendor.v4l2_codec2.encoder.supported.h264": "true",
    "ro.vendor.v4l2_codec2.encoder.supported.hevc": "true",
    "ro.vendor.v4l2_codec2.encode_concurrent_instances": "8",
    "ro.vendor.v4l2_codec2.video_pixel_rate": "108000000",
}
VIDEO_DECODER_CODECS = {
    "c2.v4l2.avc.decoder": "video/avc",
    "c2.v4l2.hevc.decoder": "video/hevc",
    "c2.v4l2.vp9.decoder": "video/x-vnd.on2.vp9",
}
VIDEO_DECODER_PROPERTY_PREFIX = "ro.vendor.v4l2_codec2.decoder.supported."
VIDEO_DECODER_PROPERTIES = {
    "ro.vendor.v4l2_codec2.decoder.supported.h264": "true",
    "ro.vendor.v4l2_codec2.decoder.supported.hevc": "true",
    "ro.vendor.v4l2_codec2.decoder.supported.vp9": "true",
    "ro.vendor.v4l2_codec2.decode_concurrent_instances": "4",
    "ro.vendor.v4l2_codec2.video_decode_pixel_rate": "456192000",
}
# CTS demands that the use of the largest published performance point be at least this share of
# the capacity (VideoEncoderAvailabilityTest and VideoDecoderAvailabilityTest, resource
# consumption for performance points), so a capacity cannot be far above what the codec publishes.
VIDEO_MIN_CAPACITY_SHARE = 0.6
# The smallest picture and the lowest bit rate of the encoders: the floors that the host lists
# (the platform's kVideoEncodeMinimumSide and the bit rate control's minimum), below which its
# streams fail CTS's quality bar. The codec list states exactly them.
VIDEO_ENCODER_MINIMUM_SIZE = "64x64"
VIDEO_ENCODER_MINIMUM_BITRATE = "20000"
VIDEO_ENCODER_UEVENTD = "/dev/video* 0660 system camera"
# The service reads this policy beside its default one. The poller of the V4L2
# device wakes its thread through an eventfd, which the default lacks.
VIDEO_ENCODER_SECCOMP_CALLS = ("eventfd2",)
# The crash handler of debuggerd runs in the service when it faults.
VIDEO_CRASH_HANDLER_SECCOMP_CALLS = ("getppid", "recvfrom", "setsockopt", "uname")
VIDEO_ENCODER_SECCOMP_DEFAULT = "android.hardware.media.c2-default-seccomp_policy"
VIDEO_ENCODER_SECCOMP_EXTENDED = "android.hardware.media.c2-extended-seccomp_policy"
MESA_EGL_ENTRIES = {
    f"VENDOR/{abi}/egl/lib{name}_{MESA_EGL_SUFFIX}.so"
    for abi in MESA_ABI_DIRECTORIES
    for name in ("EGL", "GLESv1_CM", "GLESv2")
}
MESA_ELF_ENTRIES = {
    *(f"VENDOR/{abi}/{MESA_GALLIUM_DRIVER}" for abi in MESA_ABI_DIRECTORIES),
    *MESA_EGL_ENTRIES,
}
# The Vulkan HAL and the buffer allocator and mapper Mesa queries through
# gralloc: same-process HALs of each ABI.
MESA_HAL_ENTRIES = {
    f"VENDOR/{abi}/hw/{module}.so"
    for abi in MESA_ABI_DIRECTORIES
    for module in ("vulkan.virtio", "gralloc.minigbm", "mapper.minigbm")
}
# A 32-bit library of the stack must be built with -D_FILE_OFFSET_BITS=64, or
# its mmap of a DRM offset above 4 GiB (the Venus renderer's, the buffer
# allocator's) passes a 32-bit off_t and fails with EINVAL. Bionic's headers
# then rename these calls to their 64 suffix. The rule covers this stack alone:
# the platform's own 32-bit libraries import plain mmap and lseek for
# offsets that never leave 32 bits.
LARGE_FILE_FUNCTIONS = ("mmap", "lseek")
# The graphics HAL selections. The product init script copies each
# androidboot.hardware.<name> into ro.hardware.<name> at early-init, so the
# boot command line is each property's one writer and must name these values;
# a vendor property would make that setprop fail.
BOOT_HAL_SELECTIONS = {
    "egl": MESA_EGL_SUFFIX,
    "vulkan": "virtio",
    "gralloc": "minigbm",
}
# The OpenGL ES claim. The product init script copies the boot command line's
# androidboot.opengles.version into ro.opengles.version at early-init, the
# version the framework reports and the CTS gates its dEQP lists on; 0x30002
# is OpenGL ES 3.2. Every ES 3.1 or later context of the driver lists
# GL_ANDROID_extension_pack_es31a, and the CTS requires the
# android.hardware.opengles.aep feature exactly when a device that reports 3.1
# or later lists it. A declared dEQP level makes the CTS run every EGL and GLES
# list dated up to it, so the level is pinned to the one whose lists passed
# whole: a platform update that moves the "latest" file fails here instead of
# raising the claim unqualified.
BOOT_OPENGLES_VERSION = 0x30002
OPENGLES_DEQP_LEVEL_FEATURE = "android.software.opengles.deqp.level"
OPENGLES_DEQP_LEVEL = 132776705  # 2026-03-01
UML_INIT_OPENGLES_COPY = "setprop ro.opengles.version ${ro.boot.opengles.version}"
# The Venus HAL makes vkAllocateMemory wait for the host's reply, so a refusal of the host's GPU
# budget reaches the application as VK_ERROR_OUT_OF_DEVICE_MEMORY instead of failing the ring and
# aborting the process. Mesa reads its VN_PERF option as this property; vendor init sets it at
# early-init, before any process that loads the driver starts, and only there. The driver loads in
# applications, SurfaceFlinger and programs run from the shell (the CTS among them), and each must
# be able to read the property, or Mesa reads no option and allocates asynchronously.
VENUS_PERF_PROPERTY = "debug.mesa.vn.perf"
VENUS_PERF_COPY = f"setprop {VENUS_PERF_PROPERTY} no_async_mem_alloc"
VENUS_DRIVER_DOMAINS = ("untrusted_app", "surfaceflinger", "shell")
# The retired AOSP external/mesa3d stack: its EGL and GLES libraries, and the
# directory it loaded its driver from.
RETIRED_EGL_ENTRIES = {
    f"VENDOR/{abi}/egl/{name}.so"
    for abi in MESA_ABI_DIRECTORIES
    for name in ("libGLES_mesa", "libEGL_mesa", "libGLESv1_CM_mesa", "libGLESv2_mesa")
}
RETIRED_DRI_DIRECTORIES = tuple(f"VENDOR/{abi}/dri/" for abi in MESA_ABI_DIRECTORIES)
REQUIRED_SDK_APEX_ENTRIES = {
    "SYSTEM/apex/com.android.bt.apex",
    "SYSTEM/apex/com.android.devicelock.apex",
    "SYSTEM/apex/com.android.nfcservices.apex",
    "SYSTEM/apex/com.android.rkpd.apex",
    "SYSTEM/apex/com.android.telephonycore.apex",
    "SYSTEM/apex/com.android.tethering.apex",
    "SYSTEM/apex/com.android.uwb.apex",
    "SYSTEM/apex/com.android.virt.apex",
    "SYSTEM/apex/com.android.wifi.apex",
}

# The product runs 64-bit applications and 32-bit armeabi-v7a or legacy
# armeabi applications. The build writes each ABI list per partition
# (ro.<partition>.product.cpu.abilist*) and the runtime derives the unprefixed
# ones from them.
ABI_PROPERTY = re.compile(
    r"ro\.(?:(?:system|vendor|odm)\.product|product)\.cpu\.(abi|abi2|abilist|abilist32|abilist64)"
)
EXPECTED_ABIS = {
    "abi": "arm64-v8a",
    "abi2": "",
    "abilist": "arm64-v8a,armeabi-v7a,armeabi",
    "abilist32": "armeabi-v7a,armeabi",
    "abilist64": "arm64-v8a",
}
# Partitions whose build properties list the ABIs, with the properties a
# multilib product must carry in them.
ABI_LIST_PROPERTY_FILES = {
    "SYSTEM/build.prop": "system",
    "VENDOR/build.prop": "vendor",
    "ODM/etc/build.prop": "odm",
}
# What only the vendor properties describe: the second zygote that starts the
# 32-bit applications, the 64-bit compiler ART Service runs for both ISAs, and
# the second architecture's variant for dex2oat and bionic.
VENDOR_MULTILIB_PROPERTIES = {
    "ro.zygote": "zygote64_32",
    "dalvik.vm.dex2oat64.enabled": "true",
    "ro.bionic.2nd_arch": "arm",
    "ro.bionic.2nd_cpu_variant": "cortex-a53",
    "dalvik.vm.isa.arm.variant": "cortex-a53",
}


REQUIRED_ENTRIES = {
    "VENDOR/bin/hw/android.hardware.sensors-service.multihal",
    "VENDOR/etc/sensors/hals.conf",
    "VENDOR/etc/vintf/manifest/android.hardware.sensors-multihal.xml",
    "VENDOR/lib64/hw/android.hardware.sensors@2.1-impl.andarium.so",
    "VENDOR/etc/init/android.hardware.sensors-service-multihal.rc",
    "VENDOR/bin/hw/android.hardware.vibrator-service.andarium",
    "VENDOR/etc/init/android.hardware.vibrator-service.andarium.rc",
    "ODM/etc/vintf/manifest_haptics.xml",
    "VENDOR/bin/hw/android.hardware.gnss-service.andarium",
    "VENDOR/etc/init/android.hardware.gnss-service.andarium.rc",
    "VENDOR/etc/vintf/manifest/android.hardware.gnss-service.andarium.xml",
    "VENDOR/bin/hw/android.hardware.thermal-service.andarium",
    "VENDOR/etc/init/android.hardware.thermal-service.andarium.rc",
    "VENDOR/etc/vintf/manifest/android.hardware.thermal-service.andarium.xml",
    "VENDOR/etc/displayconfig/default.xml",
    "VENDOR/bin/kernarium_gamepad",
    "VENDOR/etc/init/kernarium_gamepad.rc",
    "VENDOR/bin/hw/android.hardware.camera.provider@2.7-service-google-lazy",
    "VENDOR/etc/init/android.hardware.camera.provider@2.7-service-google-lazy.rc",
    "VENDOR/lib64/libgooglecamerahwl_impl.so",
    "VENDOR/etc/config/emu_camera_v4l2.json",
    "PRODUCT/app/Aperture/Aperture.apk",

    "BOOT/kernel",
    "INIT_BOOT/RAMDISK/init",
    "IMAGES/init_boot.img",
    "IMAGES/odm.img",
    "IMAGES/product.img",
    "IMAGES/recovery.img",
    "IMAGES/system.img",
    "IMAGES/system_ext.img",
    "IMAGES/userdata.img",
    "IMAGES/vendor.img",
    "META/init_boot_filesystem_config.txt",
    "META/kernel_configs.txt",
    "META/kernel_version.txt",
    "META/misc_info.txt",
    "META/ramdisk_node_list",
    "PRODUCT/app/Camelot/Camelot.apk",
    "PRODUCT/app/Etar/Etar.apk",
    "PRODUCT/app/ExactCalculator/ExactCalculator.apk",
    "PRODUCT/app/Glimpse/Glimpse.apk",
    "PRODUCT/app/LatinIME/LatinIME.apk",
    "PRODUCT/app/Twelve/Twelve.apk",
    "PRODUCT/app/webview/webview.apk",
    "PRODUCT/etc/build.prop",
    "SYSTEM/build.prop",
    "RECOVERY/RAMDISK/system/bin/hw/android.hardware.health-service.example_recovery",
    "RECOVERY/RAMDISK/system/bin/recovery",
    "SYSTEM/bin/e2fsck",
    "SYSTEM/bin/resize2fs",
    "SYSTEM_EXT/priv-app/Launcher3QuickStep/Launcher3QuickStep.apk",
    "SYSTEM_EXT/priv-app/LineageParts/LineageParts.apk",
    "SYSTEM_EXT/priv-app/LineageSetupWizard/LineageSetupWizard.apk",
    "SYSTEM_EXT/priv-app/LineageSettingsProvider/LineageSettingsProvider.apk",
    "SYSTEM_EXT/priv-app/Seedvault/Seedvault.apk",
    "SYSTEM_EXT/bin/kernarium_wifi_network.sh",
    "SYSTEM_EXT/etc/init/kernarium_wifi_network.rc",
    "SYSTEM_EXT/bin/kernarium_reclaim_boot_apps.sh",
    "SYSTEM_EXT/etc/init/kernarium_reclaim_boot_apps.rc",
    "SYSTEM_EXT/bin/kernarium_time_zone.sh",
    "SYSTEM_EXT/etc/init/kernarium_time_zone.rc",
    "SYSTEM_EXT/bin/kernarium_keyboard_default.sh",
    "SYSTEM_EXT/etc/init/kernarium_keyboard_default.rc",
    "SYSTEM_EXT/bin/kernarium_oldapp",
    "SYSTEM_EXT/framework/kernarium_oldapp.jar",
    "SYSTEM_EXT/bin/kernarium_apps",
    "SYSTEM_EXT/framework/kernarium_apps.jar",
    "VENDOR/usr/keylayout/Vendor_1af4_Product_0002.kl",
    "VENDOR/usr/idc/Vendor_1af4_Product_0003.idc",
    "VENDOR/usr/idc/Vendor_1af4_Product_0004.idc",
    "VENDOR/usr/idc/Vendor_1af4_Product_0005.idc",
    "SYSTEM_EXT/etc/selinux/system_ext_file_contexts",
    "SYSTEM_EXT/etc/selinux/system_ext_property_contexts",
    "SYSTEM_EXT/etc/selinux/system_ext_sepolicy.cil",
    "SYSTEM_EXT/priv-app/ThemePicker/ThemePicker.apk",
    "SYSTEM/etc/selinux/plat_property_contexts",
    "SYSTEM/etc/selinux/plat_sepolicy.cil",
    "VENDOR/bin/hw/android.hardware.health-service.example",
    "VENDOR/bin/kernarium_net_setup",
    "VENDOR/bin/suspend_blocker",
    "VENDOR/etc/audio_effects_config.xml",
    "VENDOR/etc/audio_policy_configuration.xml",
    "VENDOR/etc/audio_policy_volumes.xml",
    "VENDOR/etc/default_volume_tables.xml",
    "VENDOR/etc/fstab.uml",
    "VENDOR/etc/init/init.kernarium.rc",
    f"VENDOR/bin/hw/{VIDEO_ENCODER_SERVICE}",
    f"VENDOR/etc/init/{VIDEO_ENCODER_SERVICE}.rc",
    "VENDOR/etc/media_codecs_c2.xml",
    f"VENDOR/etc/seccomp_policy/{VIDEO_ENCODER_SECCOMP_DEFAULT}",
    f"VENDOR/etc/seccomp_policy/{VIDEO_ENCODER_SECCOMP_EXTENDED}",
    f"VENDOR/etc/vintf/manifest/{VIDEO_ENCODER_SERVICE}.xml",
    "VENDOR/lib64/libc2plugin_store.so",
    "VENDOR/etc/permissions/bluetooth_excluded_hardware.xml",
    "VENDOR/etc/permissions/handheld_core_hardware.xml",
    "VENDOR/etc/primary_audio_policy_configuration.xml",
    "VENDOR/etc/r_submix_audio_policy_configuration.xml",
    "VENDOR/etc/selinux/vendor_file_contexts",
    "VENDOR/etc/selinux/vendor_property_contexts",
    "VENDOR/etc/selinux/vendor_sepolicy.cil",
    "VENDOR/etc/ueventd.rc",
    "VENDOR/etc/vintf/compatibility_matrix.xml",
    "VENDOR/etc/vintf/manifest.xml",
    "VENDOR_BOOT/RAMDISK/first_stage_ramdisk/fstab.uml",
    "VENDOR/build.prop",
    # The network setup tool's netlink library (Cuttlefish's).
    "VENDOR/lib64/cuttlefish_net.so",
    # The Wi-Fi module's overlay: virt_wifi cannot randomize its MAC.
    "VENDOR/overlay/KernariumWifiOverlay.apk",
    # The overlay that releases PinnerService's home app pin.
    "VENDOR/overlay/KernariumPinnerOverlay.apk",
    # The service that delivers the host's text messages (overlay/sms), the holder of its vsock port, and the
    # script that starts the service when a connection waits (overlay/sms/listener).
    "SYSTEM_EXT/app/KernariumSms/KernariumSms.apk",
    "SYSTEM_EXT/bin/kernarium_sms_listener",
    "SYSTEM_EXT/bin/kernarium_sms_wake.sh",
    "SYSTEM_EXT/etc/init/kernarium_sms_listener.rc",
    # The application that draws the launcher applications' icons for kernarium_apps (overlay/app_icons).
    "SYSTEM_EXT/app/KernariumIcons/KernariumIcons.apk",
    *ABI_LIST_PROPERTY_FILES,
    *MESA_ELF_ENTRIES,
    *MESA_HAL_ENTRIES,
    *REQUIRED_SDK_APEX_ENTRIES,
    # vold executes sgdisk to scan and partition any disk it manages; without
    # it every such operation fails with ENOENT.
    "SYSTEM/bin/sgdisk",
    # The 32-bit runtime the product adds beside the 64-bit one: the second
    # zygote's process, the ART boot image of its ISA, and the bootstrap
    # linker that starts it.
    "SYSTEM/bin/app_process32",
    "SYSTEM/bin/bootstrap/linker",
    "SYSTEM/bin/boringssl_self_test32",
    "SYSTEM/lib/libkernarium_oldapp.so",
    "SYSTEM/framework/arm/boot.oat",
    "SYSTEM/framework/arm/boot-framework.oat",
    "SYSTEM/etc/init/hw/init.zygote64_32.rc",
}
REQUIRED_PREOPT_APKS = {
    "PRODUCT/app/LatinIME/LatinIME.apk",
    "SYSTEM/app/CaptivePortalLogin/CaptivePortalLogin.apk",
    "SYSTEM/app/PartnerBookmarksProvider/PartnerBookmarksProvider.apk",
    "SYSTEM/app/PrintRecommendationService/PrintRecommendationService.apk",
    "SYSTEM/priv-app/BlockedNumberProvider/BlockedNumberProvider.apk",
    "SYSTEM/priv-app/ContactsProvider/ContactsProvider.apk",
    "SYSTEM/priv-app/CredentialManager/CredentialManager.apk",
    "SYSTEM/priv-app/E2eeContactKeysProvider/E2eeContactKeysProvider.apk",
    "SYSTEM/priv-app/IntentResolver/IntentResolver.apk",
    "SYSTEM/priv-app/LiveWallpapersPicker/LiveWallpapersPicker.apk",
    "SYSTEM/priv-app/LocalTransport/LocalTransport.apk",
    "SYSTEM/priv-app/ManagedProvisioning/ManagedProvisioning.apk",
    "SYSTEM/priv-app/PackageInstaller/PackageInstaller.apk",
    "SYSTEM/priv-app/ProxyHandler/ProxyHandler.apk",
    "SYSTEM/priv-app/Shell/Shell.apk",
    "SYSTEM/priv-app/SoundPicker/SoundPicker.apk",
    "SYSTEM/priv-app/Tag/Tag.apk",
    "SYSTEM/priv-app/UserDictionaryProvider/UserDictionaryProvider.apk",
    "SYSTEM/priv-app/VpnDialogs/VpnDialogs.apk",
    "SYSTEM_EXT/priv-app/Launcher3QuickStep/Launcher3QuickStep.apk",
    "SYSTEM_EXT/priv-app/LineageParts/LineageParts.apk",
    "SYSTEM_EXT/priv-app/LineageSetupWizard/LineageSetupWizard.apk",
    "SYSTEM_EXT/priv-app/Seedvault/Seedvault.apk",
    "SYSTEM_EXT/priv-app/Settings/Settings.apk",
    "SYSTEM_EXT/priv-app/StorageManager/StorageManager.apk",
    "SYSTEM_EXT/priv-app/SystemUI/SystemUI.apk",
    "SYSTEM_EXT/priv-app/ThemePicker/ThemePicker.apk",
}
# The one application whose WebView renderer and in-process library run under
# either zygote: it carries libraries for both ABIs and declares multiArch.
REQUIRED_ARM_PREOPT_APKS = {
    "PRODUCT/app/webview/webview.apk",
}
APP_ISAS = {"arm64", "arm"}
DLKM_COMPONENTS = {"system_dlkm", "vendor_dlkm", "odm_dlkm"}
DLKM_COMPATIBILITY_ENTRIES = {
    "RECOVERY/RAMDISK/odm_dlkm",
    "RECOVERY/RAMDISK/odm_dlkm/etc",
    "RECOVERY/RAMDISK/system_dlkm",
    "RECOVERY/RAMDISK/vendor_dlkm",
    "RECOVERY/RAMDISK/vendor_dlkm/etc",
    "ROOT/odm_dlkm",
    "ROOT/odm_dlkm/etc",
    "ROOT/system_dlkm",
    "ROOT/vendor_dlkm",
    "ROOT/vendor_dlkm/etc",
    "SYSTEM/system_dlkm",
    "SYSTEM/system_dlkm/etc",
    "SYSTEM/system_dlkm/etc/build.prop",
    "VENDOR/odm_dlkm",
    "VENDOR/odm_dlkm/etc",
    "VENDOR/odm_dlkm/etc/build.prop",
    "VENDOR/vendor_dlkm",
    "VENDOR/vendor_dlkm/etc",
    "VENDOR/vendor_dlkm/etc/build.prop",
    "VENDOR_BOOT/RAMDISK/odm_dlkm",
    "VENDOR_BOOT/RAMDISK/odm_dlkm/etc",
    "VENDOR_BOOT/RAMDISK/system_dlkm",
    "VENDOR_BOOT/RAMDISK/vendor_dlkm",
    "VENDOR_BOOT/RAMDISK/vendor_dlkm/etc",
}
TARGET_PARTITIONS = {"SYSTEM", "SYSTEM_EXT", "PRODUCT", "VENDOR", "ODM"}
# The one 32-bit executable that does not carry the 32 stem suffix of a binary
# built for both ABIs: the bootstrap linker that starts the first processes.
ELF32_BIN_EXCEPTIONS = {"SYSTEM/bin/bootstrap/linker"}
FORBIDDEN_STATIC_ENTRIES = {
    "IMAGES/super.img",
    "IMAGES/super_empty.img",
    "IMAGES/system_other.img",
    "IMAGES/vbmeta.img",
    "IMAGES/vbmeta_system.img",
    "IMAGES/vbmeta_vendor.img",
    "META/ab_partitions.txt",
    "PRODUCT/app/QuickSearchBox/QuickSearchBox.apk",
    "PRODUCT/etc/init/init.rc",
    "PRODUCT/etc/permissions/privapp-permissions-org.fdroid.fdroid.privileged.xml",
    "PRODUCT/overlay/cuttlefish_overlay_uwb.apk",
    "SYSTEM_EXT/etc/permissions/privapp-permissions-com.android.threadnetwork.demoapp.xml",
    "SYSTEM_EXT/priv-app/ThreadNetworkDemoApp/ThreadNetworkDemoApp.apk",
    "SYSTEM/bin/wificond",
    "VENDOR/apex/com.android.hardware.boot.apex",
    "VENDOR/apex/com.android.hardware.contexthub.apex",
    "VENDOR/apex/com.android.hardware.gnss.apex",
    "VENDOR/apex/com.android.hardware.neuralnetworks.apex",
    "VENDOR/apex/com.android.hardware.thermal.apex",
    "VENDOR/apex/com.android.hardware.threadnetwork.apex",
    "VENDOR/apex/com.android.hardware.uwb.apex",
    "VENDOR/apex/com.android.hardware.vibrator.apex",
    "VENDOR/apex/com.google.cf.bt.apex",
    "VENDOR/apex/com.google.cf.health.apex",
    "VENDOR/apex/com.google.cf.light.apex",
    "VENDOR/apex/com.google.cf.nfc.apex",
    "VENDOR/apex/com.google.cf.oemlock.apex",
    "VENDOR/apex/com.google.cf.rild.apex",
    "VENDOR/apex/com.google.cf.wifi.apex",
    "VENDOR/etc/bluetooth_with_le_audio_policy_configuration_7_0.xml",
    "VENDOR/etc/init/init_graphics.vendor.rc",
    "VENDOR/etc/init/seriallogging.rc",
    "VENDOR/etc/permissions/android.hardware.ethernet.xml",
    "VENDOR/etc/permissions/android.hardware.uwb.xml",
    "VENDOR/etc/recovery-resource.dat",
    "VENDOR/lib64/hw/android.hardware.sensors@2.1-impl.cuttlefish.so",
    "VENDOR/overlay/CuttlefishTetheringOverlay.apk",
    "VENDOR/overlay/CuttlefishWifiOverlay.apk",
    "VENDOR/overlay/cuttlefish_overlay_uwb_gsi.apk",
    "VENDOR/bin/install-recovery.sh",
    "VENDOR/etc/recovery.img",
    "VENDOR/recovery-from-boot.p",
    "VENDOR/ueventd.rc",
    "SYSTEM/vendor/bin/install-recovery.sh",
    "SYSTEM/vendor/etc/recovery.img",
    "SYSTEM/vendor/etc/recovery-resource.dat",
    "SYSTEM/vendor/recovery-from-boot.p",
}
FORBIDDEN_APEX_FILENAMES = {
    "com.android.hardware.authsecret.apex",
    "com.android.hardware.graphics.composer.ranchu.apex",
    "com.android.hardware.input.processor.apex",
    "com.android.hardware.net.nlinterceptor.apex",
    "com.android.hardware.rebootescrow.apex",
    "com.android.hardware.security.authgraph.apex",
    "com.android.hardware.security.secretkeeper.apex",
    "com.android.hardware.tetheroffload.apex",
    "com.android.hardware.usb.apex",
    "com.android.vndk.v31.apex",
    "com.android.vndk.v32.apex",
    "com.android.vndk.v33.apex",
    "com.android.vndk.v34.apex",
    "com.google.cf.confirmationui.apex",
    "com.google.cf.identity.apex",
    "com.google.cf.ir.apex",
}
FORBIDDEN_EXECUTABLE_FILENAMES = {
    "fastbootd",
    "gsi_tool",
    "gsid",
    "pmgd",
    "snapshotctl",
    "snapuserd",
    "socket_vsock_proxy",
    "tombstone_transmit",
    "update_engine",
    "update_engine_client",
    "update_verifier",
}
FORBIDDEN_APP_DIRECTORIES = {
    # The camera application stays; its Google Lens launcher does not (route p0031).
    "ApertureLensLauncher",
    "AudioFX",
    "BasicDreams",
    "BluetoothMidiService",
    "Calendar",
    "Camera2",
    "CameraExtensionsProxy",
    "Canvas",
    "Contacts",
    "CuttlefishService",
    "DeskClock",
    "DeviceAsWebcam",
    "DeviceDiagnostics",
    "Dialer",
    "DynamicSystemInstallationService",
    "EmergencyInfo",
    "EyesFreeVidService",
    "F-Droid",
    "F-DroidPrivilegedExtension",
    "Gallery2",
    "Jelly",
    # In-tree test applications (tests/integration/android/{gamepad,keymouse,location,motion,sms,webgl,storage}):
    # `m <module>` leaves them in the shared product output, and none belongs
    # in an image, whichever partition or DATA/app path it would reach.
    "AndariumGamepadProbe",
    "AndariumKeyMouseProbe",
    "AndariumLocationProbe",
    "AndariumMotionProbe",
    "AndariumSmsProbe",
    "AndariumStorageTests",
    "AndariumWebGLProbe",
    "Profiles",
    # Setup Wizard owns provisioning; the AOSP auto-provisioner would bypass OOBE.
    "Provision",
    "QuickAccessWallet",
    "QuickSearchBox",
    "Recorder",
    "TeleService",
    "Telecom",
    "Updater",
}
FORBIDDEN_FEATURE_DECLARATIONS = {
    "android.hardware.bluetooth.xml",
    "android.hardware.bluetooth_le.xml",
    "android.hardware.camera.concurrent.xml",
    "android.hardware.camera.flash-autofocus.xml",
    "android.hardware.camera.front.xml",
    "android.hardware.camera.full.xml",
    "android.hardware.camera.raw.xml",
    "android.hardware.ethernet.xml",
    "android.hardware.fingerprint.xml",
    "android.hardware.telephony.xml",
    "android.hardware.usb.accessory.xml",
    "android.hardware.usb.host.xml",
    "android.hardware.uwb.xml",
    "android.hardware.wifi.aware.xml",
    "android.hardware.wifi.direct.xml",
    "android.hardware.wifi.passpoint.xml",
    "android.hardware.wifi.rtt.xml",
    "android.software.verified_boot.xml",
}
# The host's position is the guest's GPS on every device (overlay/gnss), so the product
# declares android.hardware.location and .gps permanently. The GNSS measurement tests of CTS
# fail by design: the host gives positions and no raw measurements.
# The Virtio sound capture stream is the product's microphone. A build
# fingerprint cannot report it only on some boots, so the declaration is
# permanent (CDD 7.8.1 C-0-7) and the audio policy carries its input.
# system_server starts the CompanionDeviceManager service only with
# android.software.companion_device_setup, which the handheld declaration the
# product replaces used to carry; the service runs without Bluetooth.
# The kernel's virt_wifi is a station, which is android.hardware.wifi and no more:
# there is no peer-to-peer, Aware or RTT, and Passpoint waits for the first Wi-Fi
# CTS run. android.hardware.ethernet is not declared, because eth0 is the management
# NIC and the framework must not manage it.
# system_server starts MidiService only with android.software.midi, which no
# handheld declaration of the platform carries, and applications call
# MidiManager without a null check. Virtual MIDI devices (MidiDeviceService)
# need no hardware transport, so the declaration adds no USB or Bluetooth MIDI.
REQUIRED_FEATURE_NAMES = {
    "android.hardware.location",
    "android.hardware.location.gps",
    "android.hardware.touchscreen",
    "android.hardware.touchscreen.multitouch",
    "android.hardware.touchscreen.multitouch.distinct",
    "android.hardware.touchscreen.multitouch.jazzhand",
    "android.hardware.sensor.accelerometer",
    "android.hardware.sensor.gyroscope",
    "android.hardware.microphone",
    "android.hardware.wifi",
    "android.hardware.telephony.messaging",
    "android.software.companion_device_setup",
    "android.software.midi",
    "android.hardware.opengles.aep",
}
# The camera features are the host's to decide (overlay/camera): only the permission file of a
# camera layout may declare one, and check_camera compares those files with the generator's.
CAMERA_FEATURE_PREFIX = "android.hardware.camera"


@functools.cache
def camera_layout_files() -> frozenset[str]:
    """The files of the camera layouts, by their path in the target-files archive."""
    return frozenset(camera_layouts.installed())

FORBIDDEN_FEATURE_NAMES = {
    "android.hardware.bluetooth",
    "android.hardware.bluetooth_le",
    "android.hardware.context_hub",
    "android.hardware.ethernet",
    "android.hardware.fingerprint",
    "android.hardware.telephony",
    "android.hardware.usb.accessory",
    "android.hardware.usb.host",
    "android.hardware.uwb",
    "android.hardware.wifi.aware",
    "android.hardware.wifi.direct",
    "android.hardware.wifi.passpoint",
    "android.hardware.wifi.rtt",
}
UML_FSTAB_ENTRIES = (
    "VENDOR/etc/fstab.uml",
    "VENDOR_BOOT/RAMDISK/first_stage_ramdisk/fstab.uml",
)
READ_ONLY_FILESYSTEMS = {
    "system_fs_type": "erofs",
    "system_ext_fs_type": "erofs",
    "product_fs_type": "erofs",
    "vendor_fs_type": "erofs",
    "odm_fs_type": "erofs",
}
READ_ONLY_PARTITIONS = ("system", "system_ext", "product", "vendor", "odm")
# The images use mkfs.erofs's defaults apart from the product's compressor and
# cluster size: no other EROFS policy may be set, for a partition or for all.
EROFS_FORBIDDEN_POLICY = (
    "erofs_blocksize",
    "erofs_enable_dedupe",
    "erofs_share_dup_blocks",
    "erofs_use_legacy_compression",
)
EROFS_PARTITION_POLICY = re.compile(
    "^(?:" + "|".join(READ_ONLY_PARTITIONS) + ")_erofs_[a-z_]+$"
)
EROFS_COMPRESS_HINTS_ENTRY = "META/erofs_default_compress_hints.txt"
VENDOR_SPL_APP_READ = re.compile(
    rb"^\(allow appdomain(?:_[0-9]+)? vendor_security_patch_level_prop(?:_[0-9]+)? "
    rb"\(file \([^)]*\bread\b[^)]*\)\)\)$",
    re.MULTILINE,
)
# The image's inode count, which the first-boot resize keeps per block group
# (BOARD_USERDATAIMAGE_EXTFS_INODE_COUNT in overlay/BoardConfig.mk).
USERDATA_EXTFS_INODE_COUNT = "8192"
# 3840x2160 at 30 frames per second, in the blocks that the codec limits count: HEVC counts
# 8x8 blocks (480 x 270 = 129,600, 3,888,000 per second), VP9 16x16 blocks (240 x 135 = 32,400).
MEDIA_C2_HEVC_BLOCKS_PER_SECOND = 129_600 * 30
MEDIA_C2_VP9_BLOCK_COUNT = 32_400
MEDIA_C2_SIZE = (3840, 2160)
# The ClearKey DRM HAL maps the shared buffers of applications, which are memfd files labelled with
# the application's domain on a kernel without ashmem (overlay/sepolicy/vendor/hal_drm_clearkey.te).
# The compiled rule names the application domains less the isolated ones through a generated
# attribute, a set expression of the form (and (appdomain) (not (isolated_app_all))).
CLEARKEY_MEMFD_RULE = re.compile(
    rb"^\(allow hal_drm_clearkey_aidl(?:_[0-9]+)? (\S+) \(memfd_file \(([^)]*)\)\)\)$",
    re.MULTILINE,
)
CLEARKEY_MEMFD_PERMISSIONS = frozenset({"getattr", "map", "read", "write"})
# The two example audio HAL processes share one domain and call each other's binder
# (overlay/sepolicy/vendor/hal_audio_default.te).
AUDIO_HAL_SELF_BINDER = re.compile(
    rb"^\(allow hal_audio_default(?:_[0-9]+)? hal_audio_default(?:_[0-9]+)? "
    rb"\(binder \([^)]*\bcall\b[^)]*\)\)\)$",
    re.MULTILINE,
)
# The Wi-Fi HAL reads the platform's persist.vendor.debug.wifi.* properties at start-up
# (overlay/sepolicy/vendor/hal_wifi_default.te).
WIFI_HAL_DEBUG_PROPERTY_READ = re.compile(
    rb"^\(allow hal_wifi_default(?:_[0-9]+)? persist_vendor_debug_wifi_prop(?:_[0-9]+)? "
    rb"\(file \([^)]*\bread\b[^)]*\)\)\)$",
    re.MULTILINE,
)
# The camera HAL uses the display composer's file descriptors: the present fence that a preview buffer
# comes back with once an activity covers its surface (overlay/sepolicy/vendor/hal_camera_default.te).
CAMERA_HAL_COMPOSER_FENCE = re.compile(
    rb"^\(allow hal_camera_default(?:_[0-9]{6})? hal_graphics_composer(?:_[0-9]{6})? "
    rb"\(fd \([^)]*\buse\b[^)]*\)\)\)$",
    re.MULTILINE,
)
# adbd reports its host in this property and init holds a userspace wakelock while
# it is 1 (adbd's p0011 topic, overlay/config/init.vendor.rc,
# overlay/sepolicy/system_ext/private). The property is system-owned: adbd is a core
# domain, which the platform neverallow keeps from setting a vendor-owned property.
ADB_HOST_PROPERTY = "sys.kernarium.adb.host_connected"
ADB_HOST_PROPERTY_TYPE = "kernarium_adb_host_prop"
# The host application is the adb host of its guest and reaches adbd over virtio-vsock. adbd's p0011
# topics read two launch properties themselves: ro.boot.kernarium.adb_key, the public key the
# application authorizes for the boot, and ro.boot.kernarium.adb_tcp, 0 when the launch has no
# host forward, which keeps adbd off guest TCP (service.adb.listen_addrs would not do, as it is a
# system_prop that vendor_init may not set). The TCP property is a bootloader_prop, which every
# domain may read. The key is the installation's own, and a bootloader_prop is readable by every
# application, so the key has a type of its own (overlay/sepolicy/system_ext/private/
# adb_key_prop.te) that no application may read: adbd reads it, init sets it and dumpstate can
# read it, so a bug report that the user shares can carry the key. The adbd binary sits in its
# APEX, whose payload hints keep it uncompressed so that the verifier reads these names in the
# payload's bytes.
ADBD_APEX = "SYSTEM/apex/com.android.adbd.apex"
ADBD_BINARY = "bin/adbd"
ADB_KEY_PROPERTY = "ro.boot.kernarium.adb_key"
ADB_KEY_PROPERTY_TYPE = "kernarium_adb_key_prop"
ADBD_LAUNCH_PROPERTIES = (ADB_KEY_PROPERTY, "ro.boot.kernarium.adb_tcp")
# No application may read the key. The platform lists the concrete types that run applications in
# the attributes below (untrusted_app_29, untrusted_app_34, runas_app, sdk_sandbox_34, ...), and
# a rule is written against a concrete type or a generated attribute, so a rule for
# untrusted_app_34 does not reach the name untrusted_app. The check reads every concrete member,
# after the names that a rule is most likely to use.
ADB_KEY_APP_DOMAINS = (
    "appdomain", "untrusted_app", "isolated_app", "ephemeral_app", "platform_app", "priv_app", "system_app",
)
ADB_KEY_APP_ATTRIBUTES = ("appdomain", "untrusted_app_all", "isolated_app_all", "sdk_sandbox_all")
# The policies that may add application domains: the platform's, the vendor's and system_ext's, and
# the product's and the odm's when the image has them.
APP_POLICY_CIL = ("PRODUCT/etc/selinux/product_sepolicy.cil", "ODM/etc/selinux/odm_sepolicy.cil")
# The key rule of p0011 reads the guest's debugging setting: AdbService records USB debugging in
# persist.sys.usb.config (route p0007) and wireless debugging is persist.adb.tls_server.enable. adbd
# offers any key but the host's only while one is on, and logs the refusal line otherwise; the line
# is what only the guard topic adds, as stock adbd already holds the property names.
ADBD_DEBUGGING_PROPERTIES = ("persist.sys.usb.config", "persist.adb.tls_server.enable")
ADBD_REFUSAL_MARKER = b"kernarium: key refused, debugging is off"
# A guest that boots without ADB authorization is an unattended test host, and the p0011 bypass
# topic starts LineageOS's adb_root service with rooted debugging on for it. Only a debuggable
# build can act on that: a user build has no init trigger that restarts adbd as root, so adbd would
# announce a restart that never comes and `adb root` would hang. The topic leaves the switch off on
# a build that is not debuggable and logs this line; it is what only the guarded topic adds.
ADB_ROOT_BINARY = "SYSTEM_EXT/bin/adb_root"
ADB_ROOT_GUARD_MARKER = b"kernarium: rooted debugging stays off, this build is not debuggable"
CIL_PLATFORM_VERSION_SUFFIX = re.compile(r"_[0-9]{6}$")
CIL_PROPERTY_SET_RULE = re.compile(
    rb"^\(allow (\S+) (\S+) \(property_service \(([^)]*)\)\)\)$", re.MULTILINE
)
CIL_FILE_ALLOW_RULE = re.compile(
    rb"^\(allow (\S+) (\S+) \(file \(([^)]*)\)\)\)$", re.MULTILINE
)
# An attribute's set: a list of names, or a set expression such as
# (and (domain) (not (appdomain coredomain))).
CIL_TYPE_ATTRIBUTE_SET = re.compile(rb"^\(typeattributeset (\S+) (\(.*\))\)$", re.MULTILINE)
CIL_SET_OPERATORS = frozenset({"all", "and", "not", "or", "xor"})
INIT_PROPERTY_TRIGGER = re.compile(r"property:([^=\s&]+)=")
# The property_contexts of every partition, in the order libselinux merges them.
PROPERTY_CONTEXT_FILES = (
    "SYSTEM/etc/selinux/plat_property_contexts",
    "SYSTEM_EXT/etc/selinux/system_ext_property_contexts",
    "PRODUCT/etc/selinux/product_property_contexts",
    "VENDOR/etc/selinux/vendor_property_contexts",
    "ODM/etc/selinux/odm_property_contexts",
)
CIL_DONTAUDIT_RULE = re.compile(rb"^\(dontaudit (\S+) (\S+) \((\w+) \(([^)]*)\)\)\)$", re.MULTILINE)
CIL_ALLOW_RULE = re.compile(rb"^\(allow (\S+) (\S+) \((\w+) \(([^)]*)\)\)\)$", re.MULTILINE)
CIL_NEVERALLOW_RULE = re.compile(rb"^\(neverallow (\S+) (\S+) \((\w+) \(([^)]*)\)\)\)$", re.MULTILINE)
CIL_PERMISSIVE_TYPE = re.compile(rb"^\(typepermissive (\S+)\)$", re.MULTILINE)
# The only domain that may be permissive, and only in the diagnostics build: the userdebug
# root shell. A user build has none.
PERMISSIVE_DOMAINS = {"user": frozenset(), "userdebug": frozenset({"su"})}
# What only a debuggable build installs (PRODUCT_PACKAGES_DEBUG, adbd's debuggable block): the
# root shell, `remount` and `procmem`. A user build must carry none of them; a userdebug build
# must carry the root shell the diagnostics need.
DEBUG_ONLY_ENTRIES = ("SYSTEM/xbin/su", "SYSTEM/bin/remount", "SYSTEM/bin/procmem")
DEBUG_REQUIRED_ENTRIES = ("SYSTEM/xbin/su",)
SUSPEND_BLOCKER_SERVICE = ("suspend_blocker", "/vendor/bin/suspend_blocker")
# eth0 is the management NIC adb is reached on and the framework sees only the
# virtual Wi-Fi built on it. The tool that sets this up runs at post-fs-data, after
# init has limited every interface to answering ARP for its own addresses
# (overlay/config/init.vendor.rc, tools/kernarium_net_setup.cpp).
NET_SETUP_SERVICE = ("kernarium_net_setup", "/vendor/bin/kernarium_net_setup")
NET_SETUP_TRIGGER = "post-fs-data"
# The tool turns IPv6 off on eth0 and the macvlan through the kernel's sysctl files, so that
# neither carries a link-local address built from its MAC.
NET_SETUP_DOMAIN = "kernarium_net_setup"
NET_SETUP_SYSCTL_LABEL = "proc_net"
NET_SETUP_SYSCTLS = (
    "write /proc/sys/net/ipv4/conf/all/arp_ignore 1",
    "write /proc/sys/net/ipv4/conf/all/arp_announce 2",
)
# The Wi-Fi HAL and the supplicant keep their sockets in these directories, which Cuttlefish's
# own init script creates and this product's replaces.
WIFI_DATA_DIRECTORIES = (
    "mkdir /data/vendor/wifi 0770 wifi wifi",
    "mkdir /data/vendor/wifi/wpa 0770 wifi wifi",
    "mkdir /data/vendor/wifi/wpa/sockets 0770 wifi wifi",
)
NET_SETUP_TRANSITION = re.compile(
    rb"^\(typetransition init(?:_[0-9]+)? kernarium_net_setup_exec process kernarium_net_setup\)$",
    re.MULTILINE,
)
# After boot completes a script saves the virtual Wi-Fi network (VirtWifi, open) once, in a
# core domain of its own on system_ext (overlay/tools/kernarium_wifi_network.sh,
# overlay/sepolicy/system_ext/private).
# Until a boot has found the network stored and joined (the marker then reads 2) the saved list
# decides on every boot: a boot saves the network when the list lacks it, and saves it again when
# it finds Wi-Fi on and nothing joined after a settle time, which enables a network an earlier
# release left disabled for lack of internet, unless the dump of the Wi-Fi service shows that the
# user turned Auto-connect off for it, which is a choice and is left alone. The Wi-Fi service
# writes its store some time after a save or a join, so a boot that saved leaves the marker at 1. The script never turns Wi-Fi on
# or connects: the user's switch decides whether the saved network is joined. What it decides is
# tested on the tracked file (tests/tooling/android/test_wifi_network_script.py); the checks here
# are presence checks of the image's copy.
WIFI_NETWORK_SERVICE = ("kernarium_wifi_network", "/system_ext/bin/kernarium_wifi_network.sh")
WIFI_NETWORK_TRIGGER = "property:sys.boot_completed=1"
WIFI_NETWORK_MARKER = "persist.sys.kernarium.virtwifi_saved"
WIFI_NETWORK_MARKER_TYPE = "kernarium_wifi_network_prop"
# The marker holds 1 (saved, no join seen) and 2 (joined), so its property type must accept both.
WIFI_NETWORK_MARKER_VALUES = ("1", "2")
WIFI_NETWORK_SCRIPT_REQUIRED = (
    "cmd wifi add-network VirtWifi open",
    "cmd wifi list-networks",
    "cmd wifi status",
    WIFI_NETWORK_MARKER,
    'setprop "$marker" 1',
    'setprop "$marker" 2',
)
# The module's overlay raises both no-internet disable thresholds to the largest value: the
# platform disables a network when its failure count, which is at least one from the first
# failure, reaches the threshold, so 0 and 1 disable at once and only a value the count never
# reaches keeps the guest's one network enabled. A disabled network is never joined again.
WIFI_OVERLAY = "VENDOR/overlay/KernariumWifiOverlay.apk"
WIFI_NO_INTERNET_THRESHOLDS = (
    "config_wifiDisableReasonNoInternetPermanentThreshold",
    "config_wifiDisableReasonNoInternetTemporaryThreshold",
)
WIFI_NEVER_DISABLED_THRESHOLD = (1 << 31) - 1
WIFI_NETWORK_SCRIPT_FORBIDDEN = (
    "set-wifi-enabled",
    "connect-network",
    "settings put",
    "wifi_on",
    "svc wifi",
)
WIFI_NETWORK_TRANSITION = re.compile(
    rb"^\(typetransition init(?:_[0-9]+)? kernarium_wifi_network_exec process "
    rb"kernarium_wifi_network\)$",
    re.MULTILINE,
)
# Once boot has settled a script kills the cached processes of a reviewed list of apps the
# system starts at boot, or that the first listing of the launcher applications starts
# (overlay/tools/kernarium_reclaim_boot_apps.sh), in a core domain of its
# own on system_ext (overlay/sepolicy/system_ext/private). Their receivers and bindings stay;
# only `cmd activity kill` runs on them, which kills nothing the system would not have chosen,
# and never `kill-all` or `force-stop`, which cancel alarms and jobs. Each entry is a package
# and the names of its processes.
RECLAIM_SERVICE = ("kernarium_reclaim_boot_apps", "/system_ext/bin/kernarium_reclaim_boot_apps.sh")
RECLAIM_TRIGGER = "property:sys.boot_completed=1"
RECLAIM_SETTLE = "settle=120"
RECLAIM_APPS = (
    ("android.ext.services", (".adservices", "android.ext.services")),
    ("com.android.federatedcompute.services", ("com.android.federatedcompute.services",)),
    ("com.android.ondevicepersonalization.services",
     ("com.android.ondevicepersonalization.services",)),
    ("com.stevesoltys.seedvault", ("com.stevesoltys.seedvault",)),
    ("org.calyxos.backup.contacts", ("org.calyxos.backup.contacts",)),
    ("com.android.localtransport", ("com.android.localtransport",)),
    ("com.android.rkpdapp", ("com.android.rkpdapp",)),
    ("com.android.statementservice", ("com.android.statementservice",)),
    ("com.android.healthconnect.controller", ("com.android.healthconnect.controller",)),
    ("com.android.messaging", ("com.android.messaging",)),
    # The SMS provider's process is named com.android.phone.
    ("com.android.providers.telephony", ("com.android.phone",)),
    ("com.kernarium.icons", ("com.kernarium.icons",)),
)
RECLAIM_KILL = 'cmd activity kill --user 0 "$pkg"'
RECLAIM_SCRIPT_REQUIRED = (
    "dumpsys activity lru",
    RECLAIM_KILL,
    RECLAIM_SETTLE,
)
RECLAIM_SCRIPT_FORBIDDEN = (
    "kill-all",
    "force-stop",
    "kill -9",
    "kill -KILL",
    "kill -s",
    "pm disable",
    "pm clear",
    "pm uninstall",
    "set-inactive",
    "stop-app",
)
# Outside the one reviewed kill, the script runs nothing that ends a process (a command word, not part
# of a dotted name), and it assigns the list once.
RECLAIM_OTHER_KILL = re.compile(r"(?<![\w.])(?:kill|pkill|killall|am)(?![\w.])|force-stop")
RECLAIM_LIST_ASSIGNMENT = re.compile(r"\breclaim_list\s*\+?=")
RECLAIM_LIST = re.compile(r"^reclaim_list='\n(.*?)^'$", re.MULTILINE | re.DOTALL)
RECLAIM_TRANSITION = re.compile(
    rb"^\(typetransition init(?:_[0-9]+)? kernarium_reclaim_boot_apps_exec process "
    rb"kernarium_reclaim_boot_apps\)$",
    re.MULTILINE,
)
# Once boot completes a script applies the host's time zone, which the host application passes
# at every start (ro.boot.kernarium.timezone), through the platform's own setter, in a core domain
# of its own on system_ext (overlay/tools/kernarium_time_zone.sh, overlay/sepolicy/system_ext/
# private). The guest follows the host only while nobody in the guest has chosen a zone: the
# user's "Set time zone automatically" (Settings.Global.auto_time_zone, which the script reads and
# never writes) is not off, and the zone is still the one the script applied last, which it
# records in a marker property of its own. The platform has no automatic time zone here, so
# Settings has no switch and the zone pickers (Settings', the setup wizard's) are the only
# choices, neither of which turns the setting off. The domain sets no time zone property:
# system_server writes persist.sys.timezone.
TIME_ZONE_SERVICE = ("kernarium_time_zone", "/system_ext/bin/kernarium_time_zone.sh")
TIME_ZONE_TRIGGER = "property:sys.boot_completed=1"
TIME_ZONE_MARKER = "persist.sys.kernarium.timezone_applied"
TIME_ZONE_MARKER_TYPE = "kernarium_time_zone_prop"
TIME_ZONE_SCRIPT_REQUIRED = (
    "ro.boot.kernarium.timezone",
    "cmd settings get global auto_time_zone",
    'cmd alarm set-timezone "$zone"',
    TIME_ZONE_MARKER,
    'setprop "$marker" "$zone"',
)
TIME_ZONE_SCRIPT_FORBIDDEN = (
    "settings put",
    "setprop persist.sys.timezone",
    "time_zone_detector",
    "set_auto_detection",
    "svc ",
)
TIME_ZONE_TRANSITION = re.compile(
    rb"^\(typetransition init(?:_[0-9]+)? kernarium_time_zone_exec process "
    rb"kernarium_time_zone\)$",
    re.MULTILINE,
)
# Android hides its on-screen keyboard while a hardware keyboard is attached unless the user's
# "Show on-screen keyboard" (Settings.Secure.show_ime_with_hard_keyboard) is on. The product's
# default is off, which SettingsProvider writes only when it first creates a guest's settings, so
# once boot completes a script changes a stored 1 to 0 in a guest that started with an older release,
# once (overlay/tools/kernarium_keyboard_default.sh, in a core domain of its own on system_ext in
# overlay/sepolicy/system_ext/private). A marker property records that it has looked, so a person who
# turns the setting on afterwards keeps it on.
KEYBOARD_DEFAULT_SERVICE = (
    "kernarium_keyboard_default", "/system_ext/bin/kernarium_keyboard_default.sh"
)
KEYBOARD_DEFAULT_TRIGGER = "property:sys.boot_completed=1"
KEYBOARD_DEFAULT_MARKER = "persist.sys.kernarium.keyboard_default"
KEYBOARD_DEFAULT_MARKER_TYPE = "kernarium_keyboard_default_prop"
KEYBOARD_DEFAULT_SCRIPT_REQUIRED = (
    "cmd settings get secure show_ime_with_hard_keyboard",
    "cmd settings put secure show_ime_with_hard_keyboard 0",
    KEYBOARD_DEFAULT_MARKER,
    'setprop "$marker" 1',
)
KEYBOARD_DEFAULT_SCRIPT_FORBIDDEN = (
    "settings put secure show_ime_with_hard_keyboard 1",
    "settings put global",
    "settings put system",
    "settings delete",
    "settings reset",
    "--user",
    "svc ",
)
KEYBOARD_DEFAULT_TRANSITION = re.compile(
    rb"^\(typetransition init(?:_[0-9]+)? kernarium_keyboard_default_exec process "
    rb"kernarium_keyboard_default\)$",
    re.MULTILINE,
)
# Old app compatibility is a choice per application, kept as one persistent property per package
# (persist.sys.kernarium.oldapp.<package>: the turn-on time in epoch seconds, empty for off) and
# written by the shell command kernarium_oldapp, which Andarium runs as the adb shell user
# (overlay/oldapp_switch). The command is a Java tool started through app_process, so the image
# carries its wrapper script and its jar on system_ext. The property has a type of its own under
# the existing persist.sys.kernarium. policy: the shell domain sets it, every application domain
# reads it (the framework hook reads it in the application's own process) and no other application
# domain may set it. Two application domains are not core domains, which a system_internal_prop
# keeps from reading, so the type is system_restricted_prop.
OLDAPP_COMMAND = "kernarium_oldapp"
OLDAPP_WRAPPER = "SYSTEM_EXT/bin/kernarium_oldapp"
OLDAPP_JAR = "SYSTEM_EXT/framework/kernarium_oldapp.jar"
OLDAPP_CLASS = "com.kernarium.oldapp.Main"
OLDAPP_WRAPPER_REQUIRED = (
    "#!/system/bin/sh",
    "CLASSPATH=$base/framework/kernarium_oldapp.jar",
    "base=/system_ext",
    f"exec app_process $base/bin {OLDAPP_CLASS}",
)
# The launcher-application command of Add to Home Screen (overlay/app_shortcuts) is the same kind of
# shell tool: a wrapper script and a jar on system_ext. It keeps no property and no file, so it has no
# policy of its own for the verifier to check.
APPS_COMMAND = "kernarium_apps"
APPS_WRAPPER = "SYSTEM_EXT/bin/kernarium_apps"
APPS_JAR = "SYSTEM_EXT/framework/kernarium_apps.jar"
APPS_CLASS = "com.kernarium.apps.Main"
APPS_WRAPPER_REQUIRED = (
    "#!/system/bin/sh",
    "CLASSPATH=$base/framework/kernarium_apps.jar",
    "base=/system_ext",
    f"exec app_process $base/bin {APPS_CLASS}",
)
OLDAPP_PROPERTY_PREFIX = "persist.sys.kernarium.oldapp."
OLDAPP_PROPERTY_TYPE = "kernarium_oldapp_prop"
OLDAPP_SETTER = "shell"
# The command serialises its changes with an exclusive lock on a file in the shell user's own data
# directory (/data/local/tmp, shell_data_file): the shell domain must create and lock files there, and
# tighten (setattr) one that someone left open to other users.
OLDAPP_LOCK_TYPE = "shell_data_file"
OLDAPP_LOCK_PERMISSIONS = (
    ("file", frozenset({"create", "open", "read", "write", "lock", "getattr", "setattr"})),
    ("dir", frozenset({"search", "add_name", "write"})),
)
# What get_prop grants for a property type: a read of the property area needs all four.
OLDAPP_READ_PERMISSIONS = frozenset({"read", "getattr", "map", "open"})
# The partition the host grew is filled on every boot, once /data is mounted
# (init.vendor.rc says why): the kernel's online resize needs neither a fsck nor
# a recorded check. resize2fs runs as root with the group of /data's owner, so
# it opens /data (mode 0771) without a DAC capability, which the platform
# neverallows its domain.
# The fsck domain runs resize2fs. The kernel's online resize asks for CAP_SYS_RESOURCE and for
# EXT4_IOC_RESIZE_FS on the mount point (overlay/sepolicy/vendor/fsck.te).
USERDATA_GROW_RULES = (
    ("self", "capability", frozenset({"sys_resource"})),
    ("system_data_root_file", "dir", frozenset({"open", "read", "ioctl"})),
)
USERDATA_GROW_IOCTL = "0x6610"
CIL_ALLOWX_RULE = re.compile(
    rb"^\(allowx (\S+) (\S+) \(ioctl (\w+) \(([^()]*)\)\)\)$", re.MULTILINE
)
USERDATA_GROW_COMMAND = "exec - root system -- /system/bin/resize2fs /dev/block/by-name/userdata"
USERDATA_GROW_TRIGGER = "post-fs-data"
USERDATA_DEVICE = "/dev/block/by-name/userdata"
# adbd keeps running for the host whatever the debugging setting: init starts it again after
# any stop.
ADBD_KEEP_ALIVE_TRIGGER = "property:init.svc.adbd=stopped"
ADBD_KEEP_ALIVE_COMMAND = "start adbd"
UML_INIT_REQUIRED = (
    "mount_all --early",
    "mount_all --late",
    "ro.hardware.audio.primary",
    "setprop service.adb.tcp.port 5555",
    "start adbd",
)
# Only Kernarium passes a display density, so the product init script copies
# it inside an action gated on its presence; an unconditional copy fails on
# every standalone macOS boot.
UML_INIT_DENSITY_TRIGGER = "early-init && property:ro.boot.lcd_density=*"
UML_INIT_DENSITY_COPY = "setprop ro.sf.lcd_density ${ro.boot.lcd_density}"
# The framework scales its ANR, watchdog and input timeouts by the hardware timeout
# multiplier, which every launcher passes from the guest's vCPU count; the tracked
# command line does not set it. The copy defaults to 5 for a launch that passes none,
# because an unset property leaves the framework's scale at 1.
UML_INIT_TIMEOUT_COPY = "setprop ro.hw_timeout_multiplier ${ro.boot.hw_timeout_multiplier:-5}"
UML_INIT_TIMEOUT_PROPERTY = "setprop ro.hw_timeout_multiplier "
# The boot command line expedites RCU grace periods until boot completes; the
# product init script then restores normal ones.
UML_INIT_RCU_TRIGGER = "property:sys.boot_completed=1"
UML_INIT_RCU_RESTORE = "write /sys/kernel/rcu_expedited 0"
# Compressed swap: one zram entry in the fstab, turned on once boot completes so
# that boot itself compresses nothing.
UML_ZRAM_DEVICE = "/dev/block/zram0"
UML_SWAPON_COMMAND = "swapon_all /vendor/etc/fstab.uml"
# The inherited Cuttlefish graphics script also copied the goldfish composer
# mode and the ANGLE feature overrides; this product has neither, and their
# androidboot sources never exist.
UML_INIT_FORBIDDEN = (
    "ro.vendor.hwcomposer.mode",
    "debug.angle.",
    "metrics_helper",
    "rename_eth0",
    "setup_wifi",
    "socket_vsock_proxy",
    "/dev/hvc",
    "/dev/block/by-name/custom",
)


def error(message: str) -> SystemExit:
    return SystemExit(f"Android target-files verifier: {message}")


def fail(message: str) -> None:
    raise error(message)


def sha256_member(archive: zipfile.ZipFile, info: zipfile.ZipInfo) -> str:
    digest = hashlib.sha256()
    with archive.open(info) as source:
        while chunk := source.read(1 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def safe_name(name: str) -> PurePosixPath:
    if not name or "\\" in name or "\0" in name:
        fail(f"unsafe archive path: {name!r}")
    path = PurePosixPath(name)
    if path.is_absolute() or any(part in ("", ".", "..") for part in path.parts):
        fail(f"unsafe archive path: {name!r}")
    if str(path) != name:
        fail(f"non-normalized archive path: {name!r}")
    return path


def reject_module_payload(path: PurePosixPath) -> None:
    lowered = tuple(part.lower() for part in path.parts)
    if (any(part in DLKM_COMPONENTS for part in lowered)
            and str(path) not in DLKM_COMPATIBILITY_ENTRIES):
        fail(f"DLKM payload is forbidden: {path}")
    basename = lowered[-1]
    if basename == "dlkm_loader":
        fail(f"kernel module loader is forbidden: {path}")
    if android_image.is_module_payload(basename):
        fail(f"kernel module or module metadata is forbidden: {path}")
    if any(
        lowered[index : index + 2] == ("lib", "modules")
        for index in range(len(lowered) - 1)
    ):
        fail(f"kernel module directory is forbidden: {path}")


def reject_nonstatic_payload(path: PurePosixPath) -> None:
    canonical = str(path)
    if path.parts[0] == "SYSTEM_OTHER":
        fail(f"system_other payload is forbidden: {path}")
    if canonical in FORBIDDEN_STATIC_ENTRIES:
        fail(f"nonstatic image or metadata is forbidden: {path}")
    if path.name in FORBIDDEN_APEX_FILENAMES and "apex" in path.parts:
        fail(f"unneeded APEX payload is forbidden: {path}")
    if path.name in FORBIDDEN_EXECUTABLE_FILENAMES:
        fail(f"unneeded static-layout executable is forbidden: {path}")
    if path.parts[:2] == ("OTA", "bin"):
        fail(f"OTA tool payload is forbidden: {path}")
    if path.parts[0] == "IMAGES" and path.name.endswith("_dlkm.img"):
        fail(f"DLKM image is forbidden: {path}")
    if path.name.startswith("fstab.cf.") and path.parts[0] in {"VENDOR", "VENDOR_BOOT"}:
        fail(f"Cuttlefish fstab is forbidden: {path}")
    if any(part in FORBIDDEN_APP_DIRECTORIES for part in path.parts):
        fail(f"unneeded built-in application payload is forbidden: {path}")
    if path.name in FORBIDDEN_FEATURE_DECLARATIONS and "permissions" in path.parts:
        fail(f"absent hardware feature declaration is forbidden: {path}")


# The platform's compiled policy (plat_sepolicy.cil) is 3.4 MB; the bound leaves
# room for it to grow about fivefold. The vendor and system_ext policies are
# a few hundred kilobytes and keep the default bound.
PLATFORM_POLICY_LIMIT = 16 << 20


def read_small(
    archive: zipfile.ZipFile, info: zipfile.ZipInfo, limit: int = 1 << 20
) -> bytes:
    if info.is_dir() or info.file_size > limit:
        fail(f"policy file is missing or unexpectedly large: {info.filename}")
    try:
        return archive.read(info)
    except (zipfile.BadZipFile, zlib.error, OSError) as problem:
        fail(f"cannot read {info.filename} from the archive: {problem}")


def check_apex_payload_filesystem(
    archive: zipfile.ZipFile, info: zipfile.ZipInfo
) -> None:
    """An APEX payload is EROFS compressed as the partitions are, or the prebuilt shim's ext4."""
    try:
        with zipfile.ZipFile(io.BytesIO(archive.read(info))) as apex:
            payload = apex.getinfo("apex_payload.img")
            with apex.open(payload) as source:
                prefix = source.read(APEX_PAYLOAD_MAGIC_SIZE)
    except (KeyError, OSError, zipfile.BadZipFile) as error:
        fail(f"cannot inspect APEX payload {info.filename}: {error}")
    if prefix[EXT4_MAGIC_OFFSET : EXT4_MAGIC_OFFSET + len(EXT4_MAGIC)] == EXT4_MAGIC:
        if info.filename in APEX_EXT4_PAYLOADS:
            return
        fail(f"APEX payload must use compressed erofs, not ext4: {info.filename}")
    if prefix[android_image.EROFS_SUPERBLOCK_OFFSET :][:4] != android_image.EROFS_MAGIC:
        fail(f"APEX payload must use compressed erofs: {info.filename}")
    check_erofs_superblock(f"APEX payload {info.filename}", prefix)


def init_services(payload: str) -> dict[str, tuple[str, list[str]]]:
    """Map each service of an init script to its executable and its option lines."""
    services: dict[str, tuple[str, list[str]]] = {}
    options: list[str] | None = None
    for raw in payload.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        if fields[0] == "service" and len(fields) >= 3:
            options = []
            services[fields[1]] = (fields[2], options)
        elif fields[0] in ("on", "import"):
            options = None
        elif options is not None and raw[:1].isspace():
            options.append(" ".join(fields))
    return services


def check_init_keychords(init_payload: str) -> None:
    """No service starts on a keychord: init counts keys across every input device, so with a
    hardware keyboard attached ordinary typing would start it (Cuttlefish's bugreport chord was A+B)."""
    for name, (_, options) in init_services(init_payload).items():
        for option in options:
            if option.split()[0] == "keycodes":
                fail(
                    f"UML init policy starts {name} on a keychord ('{option}'): init fires it whenever "
                    "those keys are down together on any input device, so typing on a hardware keyboard "
                    "would start it"
                )


def check_suspend_blocker(init_payload: str, actions: dict[str, list[str]]) -> None:
    """The wakelock that keeps the guest awake runs exactly while an adb host is connected."""
    name, executable = SUSPEND_BLOCKER_SERVICE
    service = init_services(init_payload).get(name)
    if service is None or service[0] != executable:
        fail(f"UML init policy does not declare the {name} service at {executable}")
    if "disabled" not in service[1]:
        fail(f"UML init policy starts {name} with its class instead of on an adb host")
    for trigger, commands in actions.items():
        if f"start {name}" in commands and trigger != f"property:{ADB_HOST_PROPERTY}=1":
            fail(f"UML init policy starts {name} under '{trigger}'")
    if f"start {name}" not in actions.get(f"property:{ADB_HOST_PROPERTY}=1", []):
        fail(f"UML init policy does not start {name} while an adb host is connected")
    if f"stop {name}" not in actions.get(f"property:{ADB_HOST_PROPERTY}=0", []):
        fail(f"UML init policy does not stop {name} when the last adb host leaves")


def check_zram_swap(actions: dict[str, list[str]]) -> None:
    """Swap goes on when boot completes and at no other point."""
    if actions.get(UML_INIT_RCU_TRIGGER, []).count(UML_SWAPON_COMMAND) != 1:
        fail(f"UML init policy does not run '{UML_SWAPON_COMMAND}' once at {UML_INIT_RCU_TRIGGER}")
    for trigger, commands in actions.items():
        for command in commands:
            if command.startswith("swapon_all") and (
                trigger != UML_INIT_RCU_TRIGGER or command != UML_SWAPON_COMMAND
            ):
                fail(f"UML init policy turns swap on under '{trigger}': {command}")


def check_zram_fstab(fstab: bytes, label: str) -> None:
    """The fstab names one zram swap entry that sizes itself from the RAM."""
    entries = [
        fields
        for fields in (line.split() for line in fstab.decode("utf-8", errors="strict").splitlines())
        if fields and not fields[0].startswith("#") and fields[2:3] == ["swap"]
    ]
    if len(entries) != 1 or entries[0][0] != UML_ZRAM_DEVICE:
        fail(f"UML fstab must have exactly one swap entry, on {UML_ZRAM_DEVICE}: {label}")
    flags = entries[0][4].split(",") if len(entries[0]) > 4 else []
    if not any(flag.startswith("zramsize=") and flag.endswith("%") for flag in flags):
        fail(f"UML fstab swap entry has no zramsize percentage: {label}")


def check_userdata_grow(actions: dict[str, list[str]]) -> None:
    """Growing userdata works on every boot, not only the first, and happens while it is mounted."""
    if actions.get(USERDATA_GROW_TRIGGER, []).count(USERDATA_GROW_COMMAND) != 1:
        fail(f"UML init policy does not run '{USERDATA_GROW_COMMAND}' once at {USERDATA_GROW_TRIGGER}")
    for trigger, commands in actions.items():
        for command in commands:
            if USERDATA_DEVICE in command and (
                trigger != USERDATA_GROW_TRIGGER or command != USERDATA_GROW_COMMAND
            ):
                fail(f"UML init policy handles userdata differently under '{trigger}': {command}")


def check_userdata_grow_policy(vendor_policy: bytes) -> None:
    """The fsck domain may do what the online resize needs on /data, and no more than that."""
    def domain(name: str) -> str:
        return CIL_PLATFORM_VERSION_SUFFIX.sub("", name)

    granted: dict[tuple[str, str], set[str]] = {}
    for match in CIL_ALLOW_RULE.finditer(vendor_policy):
        if domain(match.group(1).decode()) == "fsck":
            key = (domain(match.group(2).decode()), match.group(3).decode())
            granted.setdefault(key, set()).update(match.group(4).decode().split())
    for target, object_class, permissions in USERDATA_GROW_RULES:
        if not permissions <= granted.get((target, object_class), set()):
            fail(
                f"vendor policy does not let the fsck domain have {sorted(permissions)} on "
                f"{target}:{object_class}: the online grow of userdata needs it"
            )
    ioctls = {
        command
        for match in CIL_ALLOWX_RULE.finditer(vendor_policy)
        if domain(match.group(1).decode()) == "fsck"
        and domain(match.group(2).decode()) == "system_data_root_file"
        and match.group(3) == b"dir"
        for command in match.group(4).decode().split()
    }
    if USERDATA_GROW_IOCTL not in ioctls:
        fail(
            "vendor policy does not let the fsck domain issue EXT4_IOC_RESIZE_FS "
            f"({USERDATA_GROW_IOCTL}) on system_data_root_file:dir"
        )
    if ioctls != {USERDATA_GROW_IOCTL}:
        fail(
            "vendor policy lets the fsck domain issue more ioctls on /data than "
            f"EXT4_IOC_RESIZE_FS: {sorted(ioctls)}"
        )


def check_network_setup(init_payload: str, actions: dict[str, list[str]]) -> None:
    """init limits ARP, then waits for the tool that addresses eth0 and builds the Wi-Fi."""
    name, executable = NET_SETUP_SERVICE
    service = init_services(init_payload).get(name)
    if service is None or service[0] != executable:
        fail(f"UML init policy does not declare the {name} service at {executable}")
    for option in ("oneshot", "disabled"):
        if option not in service[1]:
            fail(f"UML init policy does not declare {name} as {option}")
    start = f"exec_start {name}"
    for trigger, commands in actions.items():
        if trigger != NET_SETUP_TRIGGER and (start in commands or f"start {name}" in commands):
            fail(f"UML init policy starts {name} under '{trigger}'")
    commands = actions.get(NET_SETUP_TRIGGER, [])
    if start not in commands:
        fail(f"UML init policy does not wait for {name} at {NET_SETUP_TRIGGER}")
    for sysctl in NET_SETUP_SYSCTLS:
        if sysctl not in commands or commands.index(sysctl) > commands.index(start):
            fail(f"UML init policy does not run '{sysctl}' before {name}")
    boot = actions.get("boot", [])
    for directory in WIFI_DATA_DIRECTORIES:
        if directory not in boot:
            fail(f"UML init policy does not run '{directory}' at boot")


def init_actions(payload: str) -> dict[str, list[str]]:
    """Map each action trigger of an init script to its commands."""
    actions: dict[str, list[str]] = {}
    commands: list[str] | None = None
    for raw in payload.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        keyword, _, rest = line.partition(" ")
        if keyword == "on":
            commands = actions.setdefault(" ".join(rest.split()), [])
        elif keyword in ("service", "import"):
            commands = None
        elif commands is not None and raw[:1].isspace():
            commands.append(" ".join(line.split()))
    return actions


# The Android product's identity: what the guest reports as its brand, maker, device,
# product name and model, and so the prefix of its fingerprint. The product is Andarium's
# virtual device, not a Cuttlefish or "Darwin" one; the host platform keeps the name darwin.
PRODUCT_IDENTITY = {
    "brand": "Andarium",
    "manufacturer": "Kernarium",
    "device": "andarium",
    "name": "lineage_andarium",
    "model": "Andarium Virtual Device",
}


# The board and the SoC the guest reports as Build.BOARD, Build.SOC_MANUFACTURER and
# Build.SOC_MODEL. The board is the device's name, not Cuttlefish's. The SoC is the virtual CPU,
# split as a chip vendor writes it, the maker and the chip (Qualcomm and SM8550): joined with a
# space, the pair is the name the host gives the CPU (darwin/cpu/guest_identity.hpp), which a
# tooling test requires. The ODM partition states it (overlay/config/odm.prop); the vendor
# partition carries no SoC property, since the Cuttlefish values it would inherit are dropped.
BOARD_NAME = "andarium"
SOC_IDENTITY = {"ro.soc.manufacturer": "Kernarium", "ro.soc.model": "Virtual CPU"}
# What CTS accepts (android.os.cts.BuildTest#testBuildConstants): each value is trimmed, matches
# its pattern, and is not "unknown" (the test compares exactly; the check here ignores case).
BOARD_PATTERN = re.compile(r"^[0-9A-Za-z._-]+$")
SOC_PATTERNS = {
    "ro.soc.manufacturer": re.compile(r"^[0-9A-Za-z ]+$"),
    "ro.soc.model": re.compile(r"^[0-9A-Za-z ._/+-]+$"),
}


# brand/name/device:release/id/incremental:type/tags, the form of ro.build.fingerprint.
BUILD_FINGERPRINT = re.compile(
    r"^[^/]+/[^/]+/[^:/]+:[^/]+/(?P<id>[^/]+)/(?P<incremental>[^:/]+):(?P<type>[^/]+)/(?P<tags>.+)$"
)


def check_product_identity(
    system_properties: dict[str, str],
    product_properties: dict[str, str],
    vendor_properties: dict[str, str],
    build_variant: str,
) -> None:
    """The identity lives in the product and vendor partitions and in the public fingerprint.

    The system partition is AOSP's generic system (generic_system.mk): its ro.product.system.*
    and ro.system.build.fingerprint say Android / generic / Generic System, and the product
    leaves them so. What the guest reports as Build.BRAND, MODEL and the rest is the product
    partition's (and the vendor's) ro.product.<partition>.* through
    ro.product.property_source_order, and Build.FINGERPRINT is ro.build.fingerprint.
    """
    for partition, properties in (("product", product_properties), ("vendor", vendor_properties)):
        for key, expected in PRODUCT_IDENTITY.items():
            name = f"ro.product.{partition}.{key}"
            if properties.get(name) != expected:
                fail(f"unexpected product identity {name}: {properties.get(name)!r}")
    fingerprint = system_properties.get("ro.build.fingerprint", "")
    prefix = (
        f"{PRODUCT_IDENTITY['brand']}/{PRODUCT_IDENTITY['name']}/"
        f"{PRODUCT_IDENTITY['device']}:"
    )
    if not fingerprint.startswith(prefix) or f":{build_variant}/" not in fingerprint:
        fail(f"unexpected build fingerprint for a {build_variant} build: {fingerprint!r}")
    # The vendor partition states its own build identity, which the guest reports as
    # ro.vendor.build.*. It must be the build's, not the identity of an earlier or smaller build
    # (a canary build leaves "eng.build" and a userdebug type there) that the partition kept.
    parts = BUILD_FINGERPRINT.match(fingerprint)
    if parts is None:
        fail(f"the build fingerprint has no build identity: {fingerprint!r}")
    for name, expected in (
        ("ro.vendor.build.fingerprint", fingerprint),
        ("ro.vendor.build.type", parts["type"]),
        ("ro.vendor.build.tags", parts["tags"]),
        ("ro.vendor.build.id", parts["id"]),
        ("ro.vendor.build.version.incremental", parts["incremental"]),
    ):
        if vendor_properties.get(name) != expected:
            fail(
                f"the vendor partition does not carry the build's identity: {name} is "
                f"{vendor_properties.get(name)!r}, expected {expected!r} from {fingerprint!r}"
            )


def cts_build_value_problems(name: str, value: str, pattern: re.Pattern[str]) -> list[str]:
    """Why CTS' BuildTest would reject a Build constant of this value, if it would."""
    problems = []
    if value != value.strip():
        problems.append("has surrounding whitespace")
    if pattern.fullmatch(value) is None:
        problems.append(f"does not match {pattern.pattern}")
    if value.strip().lower() == "unknown":
        problems.append("is unknown")
    return [f"{name} {value!r} {problem}" for problem in problems]


def check_hardware_identity(
    odm_properties: dict[str, str], vendor_properties: dict[str, str]
) -> None:
    """The board is the device's and the SoC is the virtual CPU, each stated once.

    ro.product.board is the vendor partition's, from the board's bootloader name. The SoC
    identity is the ODM partition's: the vendor partition carries no ro.soc.* property, because
    the inherited Cuttlefish product states Google and its device name there and the build drops
    them. The expected values themselves must pass CTS, so that a change to them fails here and
    not in BuildTest."""
    board = vendor_properties.get("ro.product.board")
    if board != BOARD_NAME:
        fail(
            f"unexpected board name ro.product.board in VENDOR/build.prop: {board!r}, "
            f"not {BOARD_NAME!r}"
        )
    problems = cts_build_value_problems("ro.product.board", BOARD_NAME, BOARD_PATTERN)
    for name, expected in SOC_IDENTITY.items():
        problems += cts_build_value_problems(name, expected, SOC_PATTERNS[name])
        value = odm_properties.get(name)
        if value != expected:
            fail(
                f"unexpected SoC identity {name} in ODM/etc/build.prop: {value!r}, "
                f"not {expected!r}"
            )
    if problems:
        fail(f"the expected hardware identity fails CTS' BuildTest: {'; '.join(problems)}")
    for name, value in vendor_properties.items():
        if name.startswith("ro.soc."):
            fail(
                f"VENDOR/build.prop sets {name}={value}: the SoC identity is the ODM partition's, "
                "and PRODUCT_VENDOR_PROPERTY_BLACKLIST drops the inherited Cuttlefish values"
            )


def check_debuggable_payload(infos: dict[str, zipfile.ZipInfo], build_variant: str) -> None:
    """A user build carries nothing a debuggable build adds; a userdebug build carries su."""
    if build_variant == "user":
        for entry in DEBUG_ONLY_ENTRIES:
            if entry in infos:
                fail(f"a user build must not carry {entry}")
    else:
        for entry in DEBUG_REQUIRED_ENTRIES:
            if entry not in infos:
                fail(f"a {build_variant} build lacks {entry}")


def check_boot_policy(
    archive: zipfile.ZipFile,
    infos: dict[str, zipfile.ZipInfo],
    build_variant: str,
) -> None:
    system_properties = read_small(
        archive, infos["SYSTEM/build.prop"]
    ).decode("utf-8", errors="strict").splitlines()
    system_property_map = {
        line.partition("=")[0]: line.partition("=")[2].strip()
        for line in system_properties
        if line.partition("=")[1]
    }
    for property_name, expected in BUILD_VARIANTS[build_variant].items():
        if system_property_map.get(property_name) != expected:
            fail(
                f"unexpected {build_variant}-build property {property_name}: "
                f"{system_property_map.get(property_name)!r}"
            )
    check_product_identity(
        system_property_map,
        parse_properties(
            read_small(archive, infos["PRODUCT/etc/build.prop"]).decode("utf-8", errors="strict")
        ),
        parse_properties(
            read_small(archive, infos["VENDOR/build.prop"]).decode("utf-8", errors="strict")
        ),
        build_variant,
    )
    check_hardware_identity(
        parse_properties(
            read_small(archive, infos["ODM/etc/build.prop"]).decode("utf-8", errors="strict")
        ),
        parse_properties(
            read_small(archive, infos["VENDOR/build.prop"]).decode("utf-8", errors="strict")
        ),
    )
    check_debuggable_payload(infos, build_variant)

    fstab = android_image.tracked_fstab(error)
    for entry in UML_FSTAB_ENTRIES:
        check_userdata_quota(read_small(archive, infos[entry]), entry)
        check_zram_fstab(read_small(archive, infos[entry]), entry)
        if read_small(archive, infos[entry]) != fstab:
            fail(f"UML fstab differs from the fixed UBD contract: {entry}")

    misc_info: dict[str, str] = {}
    for line in read_small(archive, infos["META/misc_info.txt"]).decode(
        "utf-8", errors="strict"
    ).splitlines():
        key, separator, value = line.partition("=")
        if separator:
            misc_info[key] = value
    for key, expected in READ_ONLY_FILESYSTEMS.items():
        if misc_info.get(key) != expected:
            fail(f"{key} must select read-only erofs images")
    check_erofs_policy(archive, infos, misc_info)
    if misc_info.get("userdata_extfs_inode_count") != USERDATA_EXTFS_INODE_COUNT:
        fail(
            "userdata_extfs_inode_count must be "
            f"{USERDATA_EXTFS_INODE_COUNT}: {misc_info.get('userdata_extfs_inode_count')!r}"
        )

    init_payload = read_small(
        archive, infos["VENDOR/etc/init/init.kernarium.rc"]
    ).decode("utf-8", errors="strict")
    for token in UML_INIT_REQUIRED:
        if token not in init_payload:
            fail(f"UML init policy is missing required action: {token}")
    for token in UML_INIT_FORBIDDEN:
        if token in init_payload:
            fail(f"UML init policy retains unsupported Cuttlefish action: {token}")
    actions = init_actions(init_payload)
    early_init = actions.get("early-init", [])
    for name in sorted(BOOT_HAL_SELECTIONS):
        copy = f"setprop ro.hardware.{name} ${{ro.boot.hardware.{name}}}"
        if copy not in early_init:
            fail(f"UML init policy does not copy the {name} HAL selection at early-init")
    if UML_INIT_OPENGLES_COPY not in early_init:
        fail("UML init policy does not copy the OpenGL ES version at early-init")
    for trigger, commands in actions.items():
        for command in commands:
            if command.startswith(UML_INIT_TIMEOUT_PROPERTY) and (
                trigger != "early-init" or command != UML_INIT_TIMEOUT_COPY
            ):
                fail(
                    f"UML init policy runs '{command}' at '{trigger}', not "
                    f"'{UML_INIT_TIMEOUT_COPY}' at early-init"
                )
    if UML_INIT_TIMEOUT_COPY not in early_init:
        fail("UML init policy does not copy the hardware timeout multiplier at early-init")
    for trigger, commands in actions.items():
        for command in commands:
            if command.startswith(f"setprop {VENUS_PERF_PROPERTY} ") and (
                trigger != "early-init" or command != VENUS_PERF_COPY
            ):
                fail(
                    f"UML init policy runs '{command}' at '{trigger}', not "
                    f"'{VENUS_PERF_COPY}' at early-init"
                )
    if VENUS_PERF_COPY not in early_init:
        fail(
            f"UML init policy does not run '{VENUS_PERF_COPY}' at early-init: a host GPU "
            "budget refusal would abort a Vulkan application"
        )
    for trigger, commands in actions.items():
        if UML_INIT_DENSITY_COPY in commands and trigger != UML_INIT_DENSITY_TRIGGER:
            fail(f"UML init policy copies the display density under '{trigger}'")
    if UML_INIT_DENSITY_COPY not in actions.get(UML_INIT_DENSITY_TRIGGER, []):
        fail("UML init policy does not copy a host-supplied display density")
    if UML_INIT_RCU_RESTORE not in actions.get(UML_INIT_RCU_TRIGGER, []):
        fail("UML init policy does not restore normal RCU grace periods after boot")
    if ADBD_KEEP_ALIVE_COMMAND not in actions.get(ADBD_KEEP_ALIVE_TRIGGER, []):
        fail(
            f"UML init policy does not run '{ADBD_KEEP_ALIVE_COMMAND}' at "
            f"{ADBD_KEEP_ALIVE_TRIGGER}: adbd would stay down after a stop"
        )
    check_zram_swap(actions)
    check_userdata_grow(actions)
    check_init_keychords(init_payload)
    check_suspend_blocker(init_payload, actions)
    check_network_setup(init_payload, actions)
    check_wifi_network(
        read_small(archive, infos["SYSTEM_EXT/etc/init/kernarium_wifi_network.rc"]).decode(
            "utf-8", errors="strict"
        ),
        read_small(archive, infos["SYSTEM_EXT/bin/kernarium_wifi_network.sh"]),
    )
    check_reclaim_boot_apps(
        read_small(archive, infos["SYSTEM_EXT/etc/init/kernarium_reclaim_boot_apps.rc"]).decode(
            "utf-8", errors="strict"
        ),
        read_small(archive, infos["SYSTEM_EXT/bin/kernarium_reclaim_boot_apps.sh"]),
    )
    check_time_zone(
        read_small(archive, infos["SYSTEM_EXT/etc/init/kernarium_time_zone.rc"]).decode(
            "utf-8", errors="strict"
        ),
        read_small(archive, infos["SYSTEM_EXT/bin/kernarium_time_zone.sh"]),
    )
    check_keyboard_default(
        read_small(archive, infos["SYSTEM_EXT/etc/init/kernarium_keyboard_default.rc"]).decode(
            "utf-8", errors="strict"
        ),
        read_small(archive, infos["SYSTEM_EXT/bin/kernarium_keyboard_default.sh"]),
    )
    check_oldapp_switch(
        read_small(archive, infos[OLDAPP_WRAPPER]),
        read_small(archive, infos[OLDAPP_JAR]),
        set(infos),
    )
    check_apps_command(
        read_small(archive, infos[APPS_WRAPPER]),
        read_small(archive, infos[APPS_JAR]),
        set(infos),
    )
    check_icons_app(
        archive,
        infos,
        "\n".join(
            read_small(archive, infos[name], PLATFORM_POLICY_LIMIT).decode("utf-8", errors="strict")
            for name in SEAPP_CONTEXT_FILES
            if name in infos
        ),
    )
    check_media_codecs_c2(read_small(archive, infos["VENDOR/etc/media_codecs_c2.xml"]))

    ueventd_payload = read_small(
        archive, infos["VENDOR/etc/ueventd.rc"]
    ).decode("utf-8", errors="strict")
    ueventd_entries = {
        line.strip()
        for line in ueventd_payload.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    }
    for entry in (
        "/dev/ttyS0 0660 root shell",
        "/dev/hvc0 0660 root shell",
    ):
        if entry not in ueventd_entries:
            fail(f"UML ueventd policy is missing console entry: {entry}")
    for entry in ueventd_entries:
        device = entry.split(maxsplit=1)[0]
        if device.startswith("/dev/hvc") and device != "/dev/hvc0":
            fail(f"UML ueventd policy retains unsupported HVC device: {device}")
        for prefix in ("/dev/ion", "/dev/pmem", "/dev/vhci"):
            if device.startswith(prefix):
                fail(f"UML ueventd policy retains unsupported device: {device}")


# The runtime resource overlay Soong generates from the product's SimpleDeviceConfig
# overlays, and the DeviceConfig key whose value lists the app ops recorded into the discrete
# history. The default ops are DiscreteOpsRegistry's LEGACY_OPS and IMPORTANT_OPS_FOR_SECURITY:
# LineageOS's list predates most of them, and an op that is not listed never reaches the history.
DEVICE_CONFIG_OVERLAY = (
    "PRODUCT/overlay/SimpleDeviceConfig__lineage_andarium__auto_generated_rro_product.apk"
)
DISCRETE_OPS_KEY = b"privacy/discrete_history_ops_cslist="
DISCRETE_OPS_DEFAULT = frozenset({
    0, 1, 2, 25, 26, 27, 41, 42, 63, 73, 88, 89, 100, 101,
    120, 136, 141, 147, 149, 150, 152, 170, 171, 172, 173,
})


def check_discrete_ops(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    """The discrete app-op history list must keep AOSP's default ops."""
    info = infos.get(DEVICE_CONFIG_OVERLAY)
    if info is None:
        fail(f"{DEVICE_CONFIG_OVERLAY} is missing")
    try:
        with zipfile.ZipFile(io.BytesIO(read_small(archive, info))) as overlay:
            table = overlay.read("resources.arsc")
    except (KeyError, OSError, zipfile.BadZipFile) as error:
        fail(f"{DEVICE_CONFIG_OVERLAY} has no readable resource table: {error}")
    lists = [
        {int(op) for op in match.group(1).split(b",") if op}
        for match in re.finditer(re.escape(DISCRETE_OPS_KEY) + rb"([0-9,]+)", table)
    ]
    if not lists:
        fail(f"{DEVICE_CONFIG_OVERLAY} does not set {DISCRETE_OPS_KEY.decode()}")
    missing = min((sorted(DISCRETE_OPS_DEFAULT - ops) for ops in lists), key=len)
    if missing:
        fail(
            f"no discrete app-op list in {DEVICE_CONFIG_OVERLAY} keeps AOSP's default ops; "
            f"the closest lacks {', '.join(map(str, missing))}"
        )


# LineageOS's anonymous statistics uploader posts the device name, build, country, carrier and a
# hash of the Android ID to stats.lineageos.org once its setting is on, and the Setup Wizard asks
# for that setting. The product removes both: the two applications carry no statistics
# components, no endpoint and no switch for the setting, so nothing can post whatever the setting
# says. Names are searched in the compiled manifest, resource table, layouts and every dex, as
# UTF-8 and as UTF-16 (the two string-pool encodings).
STATISTICS_APKS = (
    "SYSTEM_EXT/priv-app/LineageParts/LineageParts.apk",
    "SYSTEM_EXT/priv-app/LineageSetupWizard/LineageSetupWizard.apk",
)
STATISTICS_MARKERS = (
    b"lineagestats",
    b"stats.lineageos.org",
    b"lineageos.org",
    b"stats_collection",
    b"send_metrics",
    b"services_metrics",
    b"anonymous_statistics",
    b"StatsUploadJobService",
    b"ReportingService",
)
STATISTICS_APK_LIMIT = 64 << 20


def check_no_statistics_uploader(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    """No application can post statistics to LineageOS, whatever the setting says."""
    for entry in STATISTICS_APKS:
        info = infos.get(entry)
        if info is None:
            fail(f"{entry} is missing")
        try:
            with zipfile.ZipFile(io.BytesIO(read_small(archive, info, STATISTICS_APK_LIMIT))) as apk:
                for name in apk.namelist():
                    if not (
                        name in ("AndroidManifest.xml", "resources.arsc")
                        or (name.startswith("classes") and name.endswith(".dex"))
                        or (name.startswith("res/") and name.endswith(".xml"))
                    ):
                        continue
                    payload = apk.read(name)
                    for marker in STATISTICS_MARKERS:
                        if marker in payload or marker.decode().encode("utf-16-le") in payload:
                            fail(
                                f"{entry} still carries LineageOS's statistics uploader: "
                                f"{name} contains {marker.decode()}"
                            )
        except (OSError, zipfile.BadZipFile) as error:
            fail(f"{entry} is not a readable application archive: {error}")


# The Setup Wizard names the system Andarium. Its strings come from a runtime resource overlay that
# the product's overlay directory contributes to Soong's one overlay for the application; the
# resource table must hold the product's name and recovery strings and none of LineageOS's. The
# table is searched as UTF-8 and as UTF-16, the two string-pool encodings.
SETUP_WIZARD_OVERLAY = (
    "PRODUCT/overlay/LineageSetupWizard__lineage_andarium__auto_generated_rro_product.apk"
)
SETUP_WIZARD_NAMES = (
    b"Andarium features",
    b"Update Andarium Recovery",
    b"Updates Andarium Recovery on first boot subsequent to every update.",
)


# The Setup Wizard's two logos are the product's: the overlay replaces the vector drawable of the
# Finish screen and the services page header, and the Lottie animation of the Welcome screen. The
# animation is a raw file, which the overlay carries unchanged; the compiled vector keeps its path
# data as strings, and a vector that holds every path of the tracked one is that vector.
SETUP_WIZARD_ART = (
    Path(__file__).resolve().parents[1]
    / "overlay/overlay/packages/apps/SetupWizard/app/src/main/res"
)
SETUP_WIZARD_WELCOME = "res/raw/lottie_welcome.json"


def check_setup_wizard_art(overlay: zipfile.ZipFile) -> None:
    paths = re.findall(
        r'android:pathData="([^"]+)"', (SETUP_WIZARD_ART / "drawable/logo.xml").read_text(encoding="utf-8")
    )
    if len(paths) < 2:
        fail(f"{SETUP_WIZARD_ART}/drawable/logo.xml has fewer than two paths: the tracked logo is not the product's")
    logos = [name for name in overlay.namelist() if re.fullmatch(r"res/drawable[^/]*/logo\.xml", name)]
    if not any(
        all(path.encode() in table or path.encode("utf-16-le") in table for path in paths)
        for table in (overlay.read(name) for name in logos)
    ):
        fail(f"{SETUP_WIZARD_OVERLAY} has no res/drawable/logo.xml that holds the product's logo")
    try:
        welcome = overlay.read(SETUP_WIZARD_WELCOME)
    except KeyError:
        fail(f"{SETUP_WIZARD_OVERLAY} has no {SETUP_WIZARD_WELCOME}")
    if welcome != (SETUP_WIZARD_ART / "raw/lottie_welcome.json").read_bytes():
        fail(f"{SETUP_WIZARD_OVERLAY}'s {SETUP_WIZARD_WELCOME} is not the product's animation")
    check_setup_wizard_flow(overlay)


# The Setup Wizard shows the pages the product has reviewed and no others. LineageOS's flow lists
# eleven more: each is either decided against or can never apply to this product (the README of the
# release names the reason for every one). The overlay carries two scripts, which are raw files and
# so reach the overlay unchanged: the owner's flow, and the one a secondary user runs. A script is
# reviewed as the flow its first action reaches, in order, and it may define nothing else.
SETUP_WIZARD_NAMESPACE = "http://schemas.android.com/apk/res/com.google.android.setupwizard"
SETUP_WIZARD_FLOWS = {
    "lineage_wizard_script.xml": ("welcome", "locale", "datetime", "theme_settings"),
    "lineage_wizard_script_user.xml": ("welcome", "theme_settings"),
}
SETUP_WIZARD_HIDDEN = (
    "bluetooth_setup",
    "network_setup",
    "sim_missing",
    "restore",
    "location_settings",
    "device_specific",
    "recovery_update",
    "lineage_settings",
    "biometric_settings",
    "lockscreen_settings",
    "navigation_settings",
)


def check_setup_wizard_script(source: str, text: str, reviewed: tuple[str, ...]) -> None:
    """The script's flow is the reviewed pages in order, and no hidden page is defined or reached."""
    try:
        root = ET.fromstring(text)
    except ET.ParseError as error:
        fail(f"{source} is not well-formed XML: {error}")
    if root.tag != "WizardScript":
        fail(f"{source} is not a WizardScript")
    first = root.get(f"{{{SETUP_WIZARD_NAMESPACE}}}firstAction")
    if first is None:
        fail(f"{source} names no firstAction")
    actions: dict[str, list[str]] = {}
    for action in root.findall("WizardAction"):
        identifier = action.get("id")
        if identifier is None or identifier in actions:
            fail(f"{source} has a WizardAction without a unique id: {identifier!r}")
        actions[identifier] = [
            target
            for result in action.findall("result")
            if (target := result.get(f"{{{SETUP_WIZARD_NAMESPACE}}}action")) is not None
        ]
    reached: list[str] = []
    pending = [first]
    while pending:
        identifier = pending.pop(0)
        if identifier in reached:
            continue
        if identifier not in actions:
            fail(f"{source}: the flow reaches {identifier!r}, which the script does not define")
        reached.append(identifier)
        pending.extend(actions[identifier])
    hidden = [page for page in SETUP_WIZARD_HIDDEN if page in actions]
    if hidden:
        fail(f"{source} defines pages the product hides: {', '.join(hidden)}")
    if tuple(reached) != reviewed:
        fail(f"{source} reaches {' > '.join(reached)}, not the reviewed {' > '.join(reviewed)}")
    unreached = sorted(set(actions) - set(reached))
    if unreached:
        fail(f"{source} defines pages its flow never reaches: {', '.join(unreached)}")


def check_setup_wizard_flow(overlay: zipfile.ZipFile) -> None:
    """The overlay's scripts are the tracked ones, and the tracked ones are the reviewed flows."""
    for name, reviewed in SETUP_WIZARD_FLOWS.items():
        tracked = SETUP_WIZARD_ART / "raw" / name
        check_setup_wizard_script(str(tracked), tracked.read_text(encoding="utf-8"), reviewed)
        member = f"res/raw/{name}"
        try:
            shipped = overlay.read(member)
        except KeyError:
            fail(f"{SETUP_WIZARD_OVERLAY} has no {member}")
        if shipped != tracked.read_bytes():
            fail(f"{SETUP_WIZARD_OVERLAY}'s {member} is not the product's script")


def check_setup_wizard_branding(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    """The Setup Wizard's strings say Andarium, in the default locale, and carry no LineageOS name."""
    info = infos.get(SETUP_WIZARD_OVERLAY)
    if info is None:
        fail(f"{SETUP_WIZARD_OVERLAY} is missing")
    try:
        with zipfile.ZipFile(io.BytesIO(read_small(archive, info, 16 << 20))) as overlay:
            table = overlay.read("resources.arsc")
            check_setup_wizard_art(overlay)
    except (KeyError, OSError, zipfile.BadZipFile) as error:
        fail(f"{SETUP_WIZARD_OVERLAY} has no readable resource table: {error}")

    def holds(text: bytes) -> bool:
        return text in table or text.decode().encode("utf-16-le") in table

    for text in SETUP_WIZARD_NAMES:
        if not holds(text):
            fail(f"{SETUP_WIZARD_OVERLAY} does not set the string {text.decode()!r}")
    if holds(b"Lineage"):
        fail(f"{SETUP_WIZARD_OVERLAY} still names LineageOS or Lineage")


# The navigation mode is the platform's. LineageOS's Setup Wizard switched a device without an
# app-switch key to gestural navigation when it started, before its first page; the product's
# wizard topic (route p0026) removes that call, so a fresh guest runs the framework's default,
# three buttons, and Settings can switch it either way: both mode overlays stay in the image. The
# wizard's dex no longer names the function (R8 keeps method names), no overlay configuration
# enables a mode overlay, and no property enables one at boot, which would override the default.
SETUP_WIZARD_APK = "SYSTEM_EXT/priv-app/LineageSetupWizard/LineageSetupWizard.apk"
GESTURAL_FORCING = b"enableGesturalNavigation"
NAVIGATION_OVERLAY_DIRECTORIES = (
    "PRODUCT/overlay/NavigationBarMode3Button/",
    "PRODUCT/overlay/NavigationBarModeGestural/",
)
NAVIGATION_OVERLAY_PACKAGES = "com.android.internal.systemui.navbar."
OVERLAY_CONFIGURATION = re.compile(r"[A-Z_]+/overlay/config/[^/]+\.xml")
OVERLAY_THEME_PROPERTY = "ro.boot.vendor.overlay.theme"


# Settings' "Android version" row opens the platform logo screen, and that screen starts the easter
# egg by its PLATLOGO category, so the image carries the egg's package. The framework's product list
# decides where it lands, hence any of the three application directories.
EASTER_EGG_APK = re.compile(r"(?:SYSTEM|SYSTEM_EXT|PRODUCT)/(?:priv-)?app/EasterEgg/EasterEgg\.apk")


def check_easter_egg(infos: dict[str, zipfile.ZipInfo]) -> None:
    """The easter egg that the platform logo screen opens is in the image."""
    if not any(EASTER_EGG_APK.fullmatch(name) for name in infos):
        fail(
            "EasterEgg.apk is missing from the image: the Android version row opens the logo screen, "
            "which starts the egg (com.android.egg)"
        )


# TalkBack and eSpeak NG, which the image builds from source (external/talkback
# and external/espeak-ng): Andarium's Settings turns TalkBack on and off, and
# eSpeak NG is the only speech engine it can speak with.
ACCESSIBILITY_APKS = {
    "TalkBack": re.compile(r"PRODUCT/app/TalkBack/TalkBack\.apk"),
    "eSpeakNG": re.compile(r"PRODUCT/app/eSpeakNG/eSpeakNG\.apk"),
}


# TalkBack and eSpeak NG hold no network access and no Google service permission, so the built
# APKs request only the permissions the build is known to need and every other request is refused,
# in all three request spellings (the verifier does not enumerate what to refuse: a permission that
# a library manifest adds or a platform release introduces is a finding). TalkBack's list is its
# manifests' requests after the series left the network and Google ones out
# (dependencies/patches/talkback) and the one request that androidx core's manifest adds to every
# application that embeds it: the signature permission that guards its unexported receivers, named
# after the installed package (the installed TalkBack declares it too, as the image's package dump
# shows). eSpeak NG requests none. A name that is a resource reference cannot be read here, so it
# is refused as well.
ACCESSIBILITY_PERMISSIONS = {
    "TalkBack": {
        "android.permission.USE_FINGERPRINT": "fingerprint-sensor gestures",
        "android.permission.READ_PHONE_STATE": "call state, which pauses speech during a call",
        "android.permission.VIBRATE": "haptic feedback (the screen reader and the braille keyboard)",
        "android.permission.WAKE_LOCK": "keeps the screen on while reading continuously",
        "android.permission.MODIFY_AUDIO_SETTINGS": "audio levels while it speaks",
        "android.permission.RECORD_AUDIO": "voice commands and voice input",
        "android.permission.QUERY_ALL_PACKAGES": "the names of the installed applications",
        "android.permission.RECEIVE_BOOT_COMPLETED": "starts with the system",
        "android.permission.FOREGROUND_SERVICE": "its foreground notification",
        "android.permission.POST_NOTIFICATIONS": "its notifications on Android 13 and later",
        "android.permission.BLUETOOTH_CONNECT": "connects to a Bluetooth braille display",
        "android.permission.BLUETOOTH_SCAN": "finds a Bluetooth braille display",
        "android.permission.BLUETOOTH": "braille display up to Android 11 (maxSdkVersion 30)",
        "android.permission.BLUETOOTH_ADMIN": "braille display up to Android 11 (maxSdkVersion 30)",
        "android.permission.ACCESS_COARSE_LOCATION": "Bluetooth scan up to Android 11 (maxSdkVersion 30)",
        "android.permission.ACCESS_FINE_LOCATION": "Bluetooth scan up to Android 11 (maxSdkVersion 30)",
        "com.android.talkback.DYNAMIC_RECEIVER_NOT_EXPORTED_PERMISSION": (
            "androidx core's guard for its unexported receivers, named after the package"
        ),
    },
    "eSpeakNG": {},
}


def accessibility_refused_permissions(module: str, requested: set[str]) -> list[str]:
    """The requested permissions that TalkBack or eSpeak NG ("TalkBack", "eSpeakNG") may not hold."""
    return sorted(requested - ACCESSIBILITY_PERMISSIONS[module].keys())


def check_accessibility(infos: dict[str, zipfile.ZipInfo]) -> None:
    """TalkBack and the voice it speaks with are in the image."""
    for module, pattern in ACCESSIBILITY_APKS.items():
        if not any(pattern.fullmatch(name) for name in infos):
            fail(
                f"{module}.apk is missing from the image: the image build supplies TalkBack "
                "and eSpeak NG (external/talkback and external/espeak-ng)"
            )


def check_navigation_mode(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    """The wizard leaves the navigation mode alone, and both modes stay available."""
    info = infos.get(SETUP_WIZARD_APK)
    if info is None:
        fail(f"{SETUP_WIZARD_APK} is missing")
    try:
        with zipfile.ZipFile(io.BytesIO(read_small(archive, info, STATISTICS_APK_LIMIT))) as apk:
            for name in apk.namelist():
                if name.startswith("classes") and name.endswith(".dex") and GESTURAL_FORCING in apk.read(name):
                    fail(
                        f"{SETUP_WIZARD_APK} still forces gestural navigation: "
                        f"{name} contains {GESTURAL_FORCING.decode()}"
                    )
    except (OSError, zipfile.BadZipFile) as error:
        fail(f"{SETUP_WIZARD_APK} is not a readable application archive: {error}")
    for directory in NAVIGATION_OVERLAY_DIRECTORIES:
        if not any(name.startswith(directory) and name.endswith(".apk") for name in infos):
            fail(f"the navigation mode overlay under {directory} is missing: Settings could not switch modes")
    for name in sorted(infos):
        if OVERLAY_CONFIGURATION.fullmatch(name) is None:
            continue
        try:
            configuration = ET.fromstring(read_small(archive, infos[name], 1 << 20))
        except ET.ParseError as error:
            fail(f"{name} is not well-formed XML: {error}")
        for element in configuration.iter():
            package = element.get("package", "")
            if package.startswith(NAVIGATION_OVERLAY_PACKAGES) and element.get("enabled") != "false":
                fail(f"{name} enables the navigation mode overlay {package}: the mode is the platform's")


# Trebuchet's stock default workspace names Google applications and widgets that the image does not
# ship, so a fresh guest lost most of its items. The product's runtime resource overlay for
# Launcher3QuickStep carries one layout per grid the launcher can pick, and each holds the dock
# (hotseat) of Files and Settings and nothing else: no item on the workspace and no widget. Calendar,
# Gallery, Music and Calculator stay installed and reach the person through the app drawer. The tracked
# layouts are checked as a table of reviewed components (each one's APK must be in the image) and the
# overlay must hold every layout. The overlay compiles the layouts, so the check reads their strings
# in the overlay's string pools, as the Setup Wizard check does, and a layout of the stock launcher
# fails on its Google names.
LAUNCHER_OVERLAY = (
    "PRODUCT/overlay/Launcher3QuickStep__lineage_andarium__auto_generated_rro_product.apk"
)
LAUNCHER_LAYOUTS = (
    Path(__file__).resolve().parents[1] / "overlay/overlay/packages/apps/Launcher3/res/xml"
)
LAUNCHER_NAMESPACE = "http://schemas.android.com/apk/res-auto/com.android.launcher3"
LAUNCHER_GRIDS = ("4x5", "5x5", "5x6", "5x7", "6x5", "6x6")
LAUNCHER_FILES = ("com.android.documentsui", "com.android.documentsui.LauncherActivity")
LAUNCHER_SETTINGS = ("com.android.settings", "com.android.settings.Settings")
# (package, class) of each application with a launcher activity, and where its APK is. All six stay in
# the image; the app drawer lists them.
LAUNCHER_APPS = {
    LAUNCHER_SETTINGS: "SYSTEM_EXT/priv-app/Settings/Settings.apk",
    ("org.lineageos.etar", "com.android.calendar.AllInOneActivity"): "PRODUCT/app/Etar/Etar.apk",
    ("org.lineageos.glimpse", "org.lineageos.glimpse.MainActivity"): "PRODUCT/app/Glimpse/Glimpse.apk",
    ("org.lineageos.twelve", "org.lineageos.twelve.MainActivity"): "PRODUCT/app/Twelve/Twelve.apk",
    ("com.android.calculator2", "com.android.calculator2.Calculator"):
        "PRODUCT/app/ExactCalculator/ExactCalculator.apk",
    LAUNCHER_FILES: "SYSTEM/priv-app/DocumentsUI/DocumentsUI.apk",
}
# What the home screen holds: the dock, in the order Files then Settings, and nothing on the workspace.
LAUNCHER_DOCK = (LAUNCHER_FILES, LAUNCHER_SETTINGS)
LAUNCHER_HOTSEAT_CONTAINER = "-101"
# Each grid's dock slots and the slots of Files and Settings: the pair is centred in the dock.
LAUNCHER_DOCK_SLOTS = {"4x5": 4, "5x5": 5, "5x6": 5, "5x7": 5, "6x5": 6, "6x6": 6}
LAUNCHER_DOCK_RANKS = {"4x5": (1, 2), "5x5": (1, 3), "5x6": (1, 3), "5x7": (1, 3), "6x5": (2, 3), "6x6": (2, 3)}
LAUNCHER_FORBIDDEN_NAMES = ("google", "vending", "deskclock", "alarmclock", "@string/", "@xml/")
LAUNCHER_ATTRIBUTES = ("packageName", "className", "container", "screen", "x", "y")


def check_launcher_layout(grid: str, source: str, text: str) -> None:
    """One tracked layout: Files and Settings in the dock of a grid, and nothing on the workspace."""
    try:
        root = ET.fromstring(text)
    except ET.ParseError as error:
        fail(f"{source} is not well-formed XML: {error}")
    if root.tag != "favorites":
        fail(f"{source} is not a favorites layout")
    # Comments are not in the parsed tree: the names that matter are the ones the launcher reads.
    for element in root.iter():
        for value in [element.tag, element.text or "", *element.attrib.values()]:
            for name in LAUNCHER_FORBIDDEN_NAMES:
                if name in value.lower():
                    fail(f"{source} names {name!r} in {value!r}: the image ships no such application")
    slots = LAUNCHER_DOCK_SLOTS[grid]
    seen: dict[tuple[str, str], int] = {}
    ranks: set[int] = set()
    for item in root:
        if item.tag != "favorite":
            fail(f"{source} holds a <{item.tag}>: the layout may hold only applications")
        attributes = {
            (name[len(LAUNCHER_NAMESPACE) + 2:] if name.startswith("{") else name): value
            for name, value in item.attrib.items()
        }
        if sorted(attributes) != sorted(LAUNCHER_ATTRIBUTES):
            fail(
                f"{source} has a favorite with attributes {sorted(attributes)}, "
                f"not {sorted(LAUNCHER_ATTRIBUTES)} (a folder, uri or title item is not allowed)"
            )
        if any(not name.startswith("{" + LAUNCHER_NAMESPACE + "}") for name in item.attrib):
            fail(f"{source} has a favorite attribute outside the launcher namespace")
        component = (attributes["packageName"], attributes["className"])
        if component not in LAUNCHER_APPS:
            fail(f"{source} names {component[0]}/{component[1]}, which is not in the reviewed table")
        if attributes["container"] != LAUNCHER_HOTSEAT_CONTAINER:
            fail(
                f"{source} puts {component[1]} in container {attributes['container']}: the workspace is "
                "empty, and only the dock holds items"
            )
        if component not in LAUNCHER_DOCK:
            fail(f"{source} puts {component[1]} in the dock: only Files and Settings are there, the app drawer holds the rest")
        if component in seen:
            fail(f"{source} names {component[0]}/{component[1]} twice")
        try:
            screen, x, y = (int(attributes[name]) for name in ("screen", "x", "y"))
        except ValueError:
            fail(f"{source} has a favorite whose screen, x or y is not a number")
        if not 0 <= screen < slots:
            fail(f"{source} puts {component[1]} in dock slot {screen}, outside the {slots} slots of the {grid} dock")
        if x != screen or y != 0:
            fail(f"{source} puts {component[1]} at ({x}, {y}) in slot {screen}: a dock item's column is its slot and its row 0")
        if screen in ranks:
            fail(f"{source} puts two items in dock slot {screen}")
        ranks.add(screen)
        seen[component] = screen
    if set(seen) != set(LAUNCHER_DOCK):
        missing = sorted(app[1] for app in set(LAUNCHER_DOCK) - set(seen))
        fail(f"{source} lacks {', '.join(missing)}: the dock holds exactly Files and Settings")
    if tuple(seen[component] for component in LAUNCHER_DOCK) != LAUNCHER_DOCK_RANKS[grid]:
        fail(
            f"{source} puts Files and Settings in slots {seen[LAUNCHER_FILES]} and {seen[LAUNCHER_SETTINGS]}, "
            f"not the centred {LAUNCHER_DOCK_RANKS[grid][0]} and {LAUNCHER_DOCK_RANKS[grid][1]} of the {slots} slots"
        )


def check_launcher_workspace(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    """The overlay carries the dock layouts, and every application of the image is installed."""
    for apk in LAUNCHER_APPS.values():
        if apk not in infos:
            fail(f"{apk} is missing: the launcher's default workspace names its application")
    info = infos.get(LAUNCHER_OVERLAY)
    if info is None:
        fail(f"{LAUNCHER_OVERLAY} is missing")
    try:
        with zipfile.ZipFile(io.BytesIO(read_small(archive, info, 16 << 20))) as overlay:
            members = {name: overlay.read(name) for name in overlay.namelist() if name.startswith("res/xml/")}
    except (OSError, zipfile.BadZipFile) as error:
        fail(f"{LAUNCHER_OVERLAY} is not a readable overlay: {error}")
    for grid in LAUNCHER_GRIDS:
        name = f"default_workspace_{grid}.xml"
        tracked = LAUNCHER_LAYOUTS / name
        check_launcher_layout(grid, str(tracked), tracked.read_text(encoding="utf-8"))
        payload = members.get(f"res/xml/{name}")
        if payload is None:
            fail(f"{LAUNCHER_OVERLAY} has no res/xml/{name}: a {grid} guest would show the stock layout")

        def holds(text: str) -> bool:
            return text.encode() in payload or text.encode("utf-16-le") in payload

        for _package, component in LAUNCHER_DOCK:
            if not holds(component):
                fail(f"{LAUNCHER_OVERLAY}'s res/xml/{name} does not name {component}")
        for _package, component in sorted(set(LAUNCHER_APPS) - set(LAUNCHER_DOCK)):
            if holds(component):
                fail(f"{LAUNCHER_OVERLAY}'s res/xml/{name} names {component}: the home screen holds only the dock")
        for stock in LAUNCHER_FORBIDDEN_NAMES[:4]:
            if holds(stock):
                fail(f"{LAUNCHER_OVERLAY}'s res/xml/{name} is a stock layout: it names {stock}")


# The framework's runtime resource overlay holds the product's replacement for LineageOS's
# default wallpaper. Its two tablet variants are what every display of 600dp and more draws, and
# SystemUI sizes the wallpaper surface from the bitmap, so a variant of the default 1920 pixels
# keeps a 14 MiB buffer alive for a window that is the display's long side wide.
FRAMEWORK_OVERLAY = (
    "PRODUCT/overlay/framework-res__lineage_andarium__auto_generated_rro_product.apk"
)
FRAMEWORK_OVERLAY_LIMIT = 16 << 20
DEFAULT_WALLPAPER_ENTRIES = tuple(
    f"res/{directory}-v13/{default_wallpaper.FILE_NAME}"
    for directory in default_wallpaper.VARIANT_DIRECTORIES
)


def check_default_wallpaper(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    """The tablet default wallpapers are the overlay's bounded square, not LineageOS's 1920."""
    info = infos.get(FRAMEWORK_OVERLAY)
    if info is None:
        fail(f"{FRAMEWORK_OVERLAY} is missing")
    try:
        with zipfile.ZipFile(io.BytesIO(read_small(archive, info, FRAMEWORK_OVERLAY_LIMIT))) as overlay:
            for entry in DEFAULT_WALLPAPER_ENTRIES:
                try:
                    header = default_wallpaper.png_size(overlay.read(entry))
                except KeyError:
                    fail(f"{FRAMEWORK_OVERLAY} has no {entry}")
                except default_wallpaper.WallpaperError as error:
                    fail(f"{entry} in {FRAMEWORK_OVERLAY} is {error}")
                width, height = header[:2]
                if width != height or width > default_wallpaper.MAX_SIDE:
                    fail(
                        f"{entry} in {FRAMEWORK_OVERLAY} is {width}x{height}: the tablet default "
                        f"wallpaper must be a square of at most {default_wallpaper.MAX_SIDE} pixels "
                        "(LineageOS's overlay wins the resource when the product lists "
                        f"{default_wallpaper.PRIORITY_OVERLAY} after its inherit-product calls)"
                    )
    except (OSError, zipfile.BadZipFile) as error:
        fail(f"{FRAMEWORK_OVERLAY} is not a readable archive: {error}")


# The guest's clock is the host's: the runtime hands the guest the host's wall clock at boot and at
# resume, so the platform gets no network time. NetworkTimeUpdateService polls NTP whenever a default
# network exists, whatever the time detector's origin priorities say, and NtpTrustedTime takes its
# servers from config_ntpServers. The one value that leaves it without a server is a list whose
# entries are not ntp:// URIs: the client rejects the list and refreshes nothing before any lookup,
# whereas an empty list makes NtpConfig throw in system_server. The origin priorities then leave
# the network out as well. An empty priority list means the platform default, which holds the
# network, and a list of telephony alone, on a device without telephony, would make the platform
# drop "Set time automatically"; "external" keeps automatic detection supported.
NETWORK_TIME_SERVERS = "config_ntpServers"
NETWORK_TIME_ORIGINS = "config_autoTimeSourcesPriority"


def resource_string_pool(table: bytes, offset: int) -> list[str]:
    """The strings of the ResStringPool chunk at offset."""
    count, _styles, flags, strings_start = struct.unpack_from("<IIII", table, offset + 8)
    offsets = struct.unpack_from(f"<{count}I", table, offset + 28)
    strings = []
    for start in offsets:
        position = offset + strings_start + start
        if flags & 0x100:
            # UTF-8: the length in characters, then the length in bytes, each one or two bytes.
            for _ in range(2):
                length = table[position]
                position += 1
                if length & 0x80:
                    length = ((length & 0x7F) << 8) | table[position]
                    position += 1
            strings.append(table[position:position + length].decode("utf-8", "replace"))
        else:
            length = struct.unpack_from("<H", table, position)[0]
            position += 2
            if length & 0x8000:
                length = ((length & 0x7FFF) << 16) | struct.unpack_from("<H", table, position)[0]
                position += 2
            strings.append(table[position:position + 2 * length].decode("utf-16-le", "replace"))
    return strings


def resource_entries(table: bytes, type_name: str, *, configurations: bool = False):
    """The (name, position, flags) of every entry of one resource type in a compiled resource
    table, in each of its configurations. The position is that of the ResTable_entry, or of the
    compact entry whose key sits where a full entry keeps its size. With `configurations`, a
    fourth item says whether the entry is in the default configuration (no qualifier set)."""
    position = struct.unpack_from("<H", table, 2)[0]
    chunk, _, size = struct.unpack_from("<HHI", table, position)
    if chunk != 0x0001:
        raise ValueError("the table does not start with a string pool")
    position += size
    while position + 8 <= len(table):
        chunk, header_size, size = struct.unpack_from("<HHI", table, position)
        if size < 8:
            break
        if chunk == 0x0200:
            type_pool, _, key_pool = struct.unpack_from("<III", table, position + 268)
            types = resource_string_pool(table, position + type_pool)
            keys = resource_string_pool(table, position + key_pool)
            inner, end = position + header_size, position + size
            while inner + 8 <= end:
                inner_chunk, inner_header, inner_size = struct.unpack_from("<HHI", table, inner)
                if inner_size < 8:
                    break
                if inner_chunk == 0x0201 and types[table[inner + 8] - 1] == type_name:
                    count, entries_start = struct.unpack_from("<II", table, inner + 12)
                    kind = table[inner + 9]
                    # The ResTable_config at +20 starts with its own size; the default one sets nothing after it.
                    config_size = struct.unpack_from("<I", table, inner + 20)[0]
                    default = not any(table[inner + 24:inner + 20 + config_size])
                    if kind & 0x01:  # sparse: (index, offset / 4) pairs of 16 bits
                        offsets = [4 * pair[1] for pair in struct.iter_unpack("<HH", table[inner + inner_header:inner + inner_header + 4 * count])]
                    elif kind & 0x02:  # 16-bit offsets in units of 4 bytes
                        offsets = [0xFFFFFFFF if value == 0xFFFF else 4 * value
                                   for value in struct.unpack_from(f"<{count}H", table, inner + inner_header)]
                    else:
                        offsets = struct.unpack_from(f"<{count}I", table, inner + inner_header)
                    for entry_offset in offsets:
                        if entry_offset == 0xFFFFFFFF:
                            continue
                        entry = inner + entries_start + entry_offset
                        key, flags = struct.unpack_from("<HH", table, entry)
                        if not flags & 0x08:  # a full entry: size, flags, then a 32-bit key
                            key = struct.unpack_from("<I", table, entry + 4)[0]
                        if configurations:
                            yield keys[key], entry, flags, default
                        else:
                            yield keys[key], entry, flags
                inner += inner_size
        position += size


def resource_default_names(table: bytes, type_name: str) -> set[str]:
    """The names of the resources of one type that a compiled resource table defines in its default
    configuration: the definitions that apply whatever the device's qualifiers are."""
    return {name for name, _, _, default in resource_entries(table, type_name, configurations=True) if default}


def resource_arrays(table: bytes) -> dict[str, list[list[str]]]:
    """Every array resource of a compiled resource table, by name: one list of its items'
    strings for each definition (an item that is not a string reads as "<type:data>")."""
    arrays: dict[str, list[list[str]]] = {}
    values = resource_string_pool(table, struct.unpack_from("<H", table, 2)[0])
    for name, entry, flags in resource_entries(table, "array"):
        if not flags & 1:
            continue
        item_count = struct.unpack_from("<I", table, entry + 12)[0]
        items = []
        for index in range(item_count):
            _, _, data_type, data = struct.unpack_from("<HBBI", table, entry + 16 + 12 * index + 4)
            items.append(values[data] if data_type == 3 else f"<{data_type}:{data}>")
        arrays.setdefault(name, []).append(items)
    return arrays


def resource_booleans(table: bytes) -> dict[str, list[bool]]:
    """Every bool resource of a compiled resource table, by name: its value in each definition."""
    booleans: dict[str, list[bool]] = {}
    for name, entry, flags in resource_entries(table, "bool"):
        if flags & 0x08:  # compact: the value type is the flags' high byte, the data follows
            data_type, data = flags >> 8, struct.unpack_from("<I", table, entry + 4)[0]
        elif flags & 0x01:
            continue
        else:
            size = struct.unpack_from("<H", table, entry)[0]
            _, _, data_type, data = struct.unpack_from("<HBBI", table, entry + size)
        if data_type == 0x12:  # TYPE_INT_BOOLEAN
            booleans.setdefault(name, []).append(data != 0)
    return booleans


def resource_integers(table: bytes) -> dict[str, list[int]]:
    """Every integer resource of a compiled resource table, by name: its signed value in each
    definition."""
    integers: dict[str, list[int]] = {}
    for name, entry, flags in resource_entries(table, "integer"):
        if flags & 0x08:  # compact: the value type is the flags' high byte, the data follows
            data_type, data = flags >> 8, struct.unpack_from("<I", table, entry + 4)[0]
        elif flags & 0x01:
            continue
        else:
            size = struct.unpack_from("<H", table, entry)[0]
            _, _, data_type, data = struct.unpack_from("<HBBI", table, entry + size)
        if data_type in (0x10, 0x11):  # TYPE_INT_DEC, TYPE_INT_HEX
            integers.setdefault(name, []).append(data - (1 << 32) if data >> 31 else data)
    return integers


# The chunks of a compiled resource table that declare an <overlayable> section and its policies.
RES_TABLE_PACKAGE_TYPE = 0x0200
RES_TABLE_OVERLAYABLE_TYPES = (0x0204, 0x0205)


RES_TABLE_TYPE = 0x0002


def resource_chunks(table: bytes, start: int, end: int):
    """The (type, header size, size, position) of each chunk that fills table[start:end] exactly.
    A chunk that is smaller than its header, or does not fit, raises ValueError."""
    position = start
    while position < end:
        if position + 8 > end:
            raise ValueError(f"a chunk header at {position} runs past the end of its parent ({end})")
        chunk, header_size, size = struct.unpack_from("<HHI", table, position)
        if header_size < 8 or size < header_size or position + size > end:
            raise ValueError(f"the chunk 0x{chunk:04x} at {position} declares header {header_size} and size {size} "
                             f"in a parent that ends at {end}")
        yield chunk, header_size, size, position
        position += size


def resource_table_defines_overlayable(table: bytes) -> bool:
    """Whether a package of a compiled resource table declares an <overlayable> section. A table whose
    header, declared size or chunks do not hold together raises ValueError instead of reading as one
    without the section."""
    if len(table) < 12:
        raise ValueError(f"{len(table)} bytes are shorter than a resource table header")
    chunk, header_size, size = struct.unpack_from("<HHI", table, 0)
    if chunk != RES_TABLE_TYPE or header_size < 12 or size < header_size or size > len(table):
        raise ValueError(f"not a resource table: type 0x{chunk:04x}, header {header_size}, size {size} of {len(table)} bytes")
    for chunk, chunk_header, chunk_size, position in resource_chunks(table, header_size, size):
        if chunk == RES_TABLE_PACKAGE_TYPE:
            for inner_chunk, _, _, _ in resource_chunks(table, position + chunk_header, position + chunk_size):
                if inner_chunk in RES_TABLE_OVERLAYABLE_TYPES:
                    return True
    return False


def manifest_element_attributes(manifest: bytes, element_name: str) -> dict[str, str | int | bool | None] | None:
    """The attributes, by name, of the first element of this name in a compiled manifest, or None when
    it has no such element. A value is a string, an integer or a boolean as the manifest typed it, and
    None for any other type."""
    if len(manifest) < 8 or struct.unpack_from("<HH", manifest, 0)[0] != 0x0003:
        raise ValueError("not a compiled XML file")
    strings: list[str] = []
    offset = struct.unpack_from("<H", manifest, 2)[0]
    while offset + 8 <= len(manifest):
        kind, header, size = struct.unpack_from("<HHI", manifest, offset)
        if size < 8 or offset + size > len(manifest):
            raise ValueError("truncated compiled XML chunk")
        if kind == AXML_STRING_POOL:
            strings = axml_strings(manifest, offset)
        elif kind == AXML_START_ELEMENT:
            body = offset + header
            element = struct.unpack_from("<I", manifest, body + 4)[0]
            if element < len(strings) and strings[element] == element_name:
                attribute_start, attribute_size, count = struct.unpack_from("<HHH", manifest, body + 8)
                attributes: dict[str, str | int | bool | None] = {}
                for index in range(count):
                    position = body + attribute_start + index * attribute_size
                    name, _, _, data_type, data = struct.unpack_from("<IIHxBI", manifest, position + 4)
                    if name >= len(strings):
                        raise ValueError("an attribute name outside the string pool")
                    if data_type == AXML_STRING_TYPE:
                        if data >= len(strings):
                            raise ValueError("an attribute value outside the string pool")
                        attributes[strings[name]] = strings[data]
                    elif data_type in AXML_INTEGER_TYPES:
                        attributes[strings[name]] = data
                    elif data_type == AXML_BOOLEAN_TYPE:
                        attributes[strings[name]] = data != 0
                    else:
                        attributes[strings[name]] = None
                return attributes
        offset += size
    return None


def check_network_time(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    """The platform queries no NTP server and takes no time from the network."""
    info = infos.get(FRAMEWORK_OVERLAY)
    if info is None:
        fail(f"{FRAMEWORK_OVERLAY} is missing")
    try:
        with zipfile.ZipFile(io.BytesIO(read_small(archive, info, FRAMEWORK_OVERLAY_LIMIT))) as overlay:
            arrays = resource_arrays(overlay.read("resources.arsc"))
    except (KeyError, OSError, ValueError, struct.error, IndexError, zipfile.BadZipFile) as error:
        fail(f"{FRAMEWORK_OVERLAY} has no readable resource table: {error}")
    for name in (NETWORK_TIME_SERVERS, NETWORK_TIME_ORIGINS):
        if name not in arrays:
            fail(f"{FRAMEWORK_OVERLAY} does not set {name}: the platform would query time.android.com")
    for items in arrays[NETWORK_TIME_SERVERS]:
        if not items:
            fail(f"{NETWORK_TIME_SERVERS} is empty in {FRAMEWORK_OVERLAY}: NtpConfig throws on an empty list in system_server")
        for item in items:
            if re.match(r"(?i)ntp:", item):
                fail(f"{NETWORK_TIME_SERVERS} names the NTP server {item} in {FRAMEWORK_OVERLAY}: no NTP query may be left")
    for items in arrays[NETWORK_TIME_ORIGINS]:
        if not items:
            fail(f"{NETWORK_TIME_ORIGINS} is empty in {FRAMEWORK_OVERLAY}: the platform default holds the network")
        if "network" in items:
            fail(f"{NETWORK_TIME_ORIGINS} lists the network in {FRAMEWORK_OVERLAY}: network time is not a time source")
        if "external" not in items:
            fail(f"{NETWORK_TIME_ORIGINS} in {FRAMEWORK_OVERLAY} lacks external: without it a device without telephony loses \"Set time automatically\"")


# The power manager's FaceDownDetector turns the screen off when the device lies face down. While
# it is enabled it holds an accelerometer connection (every 200 ms, batched over 2 s) for as long
# as the screen is on, so the host's motion sensor stays running although no application asked for
# a reading, and a virtual device that is never "face down" gains nothing from it. The detector is
# enabled only when the framework resource is true (and the attention_manager_service flag the
# platform leaves on), so the product's framework overlay turns it off for every boot.
FLIP_TO_SCREEN_OFF = "config_flipToScreenOffEnabled"


def check_flip_to_screen_off(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    """The framework overlay disables flip-to-screen-off, which would hold the accelerometer."""
    info = infos.get(FRAMEWORK_OVERLAY)
    if info is None:
        fail(f"{FRAMEWORK_OVERLAY} is missing")
    try:
        with zipfile.ZipFile(io.BytesIO(read_small(archive, info, FRAMEWORK_OVERLAY_LIMIT))) as overlay:
            booleans = resource_booleans(overlay.read("resources.arsc"))
    except (KeyError, OSError, ValueError, struct.error, IndexError, zipfile.BadZipFile) as error:
        fail(f"{FRAMEWORK_OVERLAY} has no readable resource table: {error}")
    if FLIP_TO_SCREEN_OFF not in booleans:
        fail(
            f"{FRAMEWORK_OVERLAY} does not set {FLIP_TO_SCREEN_OFF}: the power manager's "
            "FaceDownDetector would hold an accelerometer connection while the screen is on"
        )
    if any(booleans[FLIP_TO_SCREEN_OFF]):
        fail(
            f"{FLIP_TO_SCREEN_OFF} is true in {FRAMEWORK_OVERLAY}: the power manager's "
            "FaceDownDetector would hold an accelerometer connection while the screen is on"
        )


def check_wifi_overlay(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    """The Wi-Fi module's overlay never lets a failed internet check disable the one network."""
    info = infos.get(WIFI_OVERLAY)
    if info is None:
        fail(f"{WIFI_OVERLAY} is missing")
    try:
        payload = read_small(archive, info, FRAMEWORK_OVERLAY_LIMIT)
        with zipfile.ZipFile(io.BytesIO(payload)) as overlay:
            integers = resource_integers(overlay.read("resources.arsc"))
    except (KeyError, OSError, ValueError, struct.error, IndexError, zipfile.BadZipFile) as error:
        fail(f"{WIFI_OVERLAY} has no readable resource table: {error}")
    for name in WIFI_NO_INTERNET_THRESHOLDS:
        if name not in integers:
            fail(
                f"{WIFI_OVERLAY} does not set {name}: the platform would disable the guest's one "
                "network at its first failed internet check, and never join it again"
            )
        if any(value != WIFI_NEVER_DISABLED_THRESHOLD for value in integers[name]):
            fail(
                f"{name} is not {WIFI_NEVER_DISABLED_THRESHOLD} in {WIFI_OVERLAY}: a lower "
                "threshold is reached by the first failed internet check and disables the guest's "
                "one network"
            )


# PinnerService's pin sizes are framework resources that the platform does not declare
# overlayable. A runtime resource overlay may replace a resource its target has not declared
# overlayable only when the overlay targets SDK 28 or lower; from Android 10 on the target's
# overlayable policies bind it, and the overlay then applies to nothing without a message. The
# overlay releases the home app's pin, about 54 MiB locked at idle.
PINNER_OVERLAY = "VENDOR/overlay/KernariumPinnerOverlay.apk"
PINNER_OVERLAY_RESOURCE = b"config_pinnerHomePinBytes"
PINNER_OVERLAY_MAX_TARGET_SDK = 28
# The framework attributes android:targetSdkVersion and android:debuggable, and the types a
# compiled manifest gives them: a number (TYPE_INT_DEC and TYPE_INT_HEX) and a literal
# boolean (TYPE_INT_BOOLEAN, all ones for true).
ATTR_TARGET_SDK_VERSION = 0x01010270
ATTR_DEBUGGABLE = 0x0101000F
ATTR_NAME = 0x01010003
AXML_STRING_POOL = 0x0001
AXML_START_ELEMENT = 0x0102
AXML_RESOURCE_MAP = 0x0180
AXML_REFERENCE_TYPE = 0x01
AXML_STRING_TYPE = 0x03
AXML_UTF8_POOL = 0x100
AXML_INTEGER_TYPES = (0x10, 0x11)
AXML_BOOLEAN_TYPE = 0x12


def manifest_attribute(manifest: bytes, attribute: int) -> tuple[int, int] | None:
    """The (type, data) of the first element attribute with this framework ID in a compiled manifest, or None."""
    if len(manifest) < 8 or struct.unpack_from("<HH", manifest, 0)[0] != 0x0003:
        raise ValueError("not a compiled XML file")
    resource_ids: list[int] = []
    offset = struct.unpack_from("<H", manifest, 2)[0]
    while offset + 8 <= len(manifest):
        kind, header, size = struct.unpack_from("<HHI", manifest, offset)
        if size < 8 or offset + size > len(manifest):
            raise ValueError("truncated compiled XML chunk")
        if kind == AXML_RESOURCE_MAP:
            resource_ids = list(struct.unpack_from(f"<{(size - header) // 4}I", manifest, offset + header))
        elif kind == AXML_START_ELEMENT:
            body = offset + header
            attribute_start, attribute_size, count = struct.unpack_from("<HHH", manifest, body + 8)
            for index in range(count):
                position = body + attribute_start + index * attribute_size
                name, _, _, data_type, data = struct.unpack_from("<IIHxBI", manifest, position + 4)
                if name < len(resource_ids) and resource_ids[name] == attribute:
                    return data_type, data
        offset += size
    return None


def axml_strings(manifest: bytes, chunk: int) -> list[str]:
    """The strings of the compiled XML string pool chunk at this offset."""
    _, header, size, count, _, flags, strings_start, _ = struct.unpack_from("<HHIIIIII", manifest, chunk)

    def length8(position: int) -> tuple[int, int]:
        # One byte, or two when the high bit of the first is set.
        (first,) = struct.unpack_from("B", manifest, position)
        if first & 0x80:
            (second,) = struct.unpack_from("B", manifest, position + 1)
            return (first & 0x7F) << 8 | second, position + 2
        return first, position + 1

    strings = []
    for index in range(count):
        position = chunk + strings_start + struct.unpack_from("<I", manifest, chunk + header + 4 * index)[0]
        if flags & AXML_UTF8_POOL:
            _, position = length8(position)  # the character count, then the byte count
            length, position = length8(position)
            encoding = "utf-8"
        else:
            (length,) = struct.unpack_from("<H", manifest, position)
            position += 2
            if length & 0x8000:
                length = (length & 0x7FFF) << 16 | struct.unpack_from("<H", manifest, position)[0]
                position += 2
            length *= 2
            encoding = "utf-16-le"
        text = manifest[position:position + length]
        if position + length > chunk + size or len(text) != length:
            raise ValueError("truncated compiled XML string")
        strings.append(text.decode(encoding))
    return strings


# The manifest elements that request a permission.
PERMISSION_REQUEST_ELEMENTS = {"uses-permission", "uses-permission-sdk-23", "uses-permission-sdk-m"}


def manifest_requested_permissions(manifest: bytes) -> set[str]:
    """The permissions a compiled manifest requests with <uses-permission>.

    A manifest that declares a permission (framework-res.apk declares every platform permission)
    or guards a component with it does not request it, and is not listed."""
    if len(manifest) < 8 or struct.unpack_from("<HH", manifest, 0)[0] != 0x0003:
        raise ValueError("not a compiled XML file")
    strings: list[str] = []
    resource_ids: list[int] = []
    requested: set[str] = set()
    offset = struct.unpack_from("<H", manifest, 2)[0]
    while offset + 8 <= len(manifest):
        kind, header, size = struct.unpack_from("<HHI", manifest, offset)
        if size < 8 or offset + size > len(manifest):
            raise ValueError("truncated compiled XML chunk")
        if kind == AXML_STRING_POOL:
            strings = axml_strings(manifest, offset)
        elif kind == AXML_RESOURCE_MAP:
            resource_ids = list(struct.unpack_from(f"<{(size - header) // 4}I", manifest, offset + header))
        elif kind == AXML_START_ELEMENT:
            body = offset + header
            element = struct.unpack_from("<I", manifest, body + 4)[0]
            if element < len(strings) and strings[element] in PERMISSION_REQUEST_ELEMENTS:
                attribute_start, attribute_size, count = struct.unpack_from("<HHH", manifest, body + 8)
                for index in range(count):
                    position = body + attribute_start + index * attribute_size
                    name, raw, _, data_type, data = struct.unpack_from("<IIHxBI", manifest, position + 4)
                    if name < len(resource_ids) and resource_ids[name] == ATTR_NAME:
                        if data_type == AXML_STRING_TYPE:
                            if data >= len(strings):
                                raise ValueError("a permission name outside the string pool")
                            requested.add(strings[data])
                        elif data_type == AXML_REFERENCE_TYPE:
                            # The name is a resource, which this tool does not resolve: listed
                            # by its id, so that a check against a list of permissions refuses it.
                            requested.add(f"@resource 0x{data:08x}")
        offset += size
    return requested


def manifest_target_sdk(manifest: bytes) -> int | None:
    """The targetSdkVersion of a compiled manifest, or None when it has none or it is not a number."""
    value = manifest_attribute(manifest, ATTR_TARGET_SDK_VERSION)
    if value is None:
        return None
    data_type, data = value
    return data if data_type in AXML_INTEGER_TYPES else None


def manifest_debuggable(manifest: bytes) -> bool:
    """Whether a compiled manifest sets android:debuggable to true; a value that is not a literal boolean is an error."""
    value = manifest_attribute(manifest, ATTR_DEBUGGABLE)
    if value is None:
        return False
    data_type, data = value
    if data_type != AXML_BOOLEAN_TYPE:
        raise ValueError("android:debuggable is not a literal boolean")
    return data != 0


def check_pinner_overlay(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    """The home pin overlay is built for SDK 28 or lower and carries the pin size it sets."""
    info = infos.get(PINNER_OVERLAY)
    if info is None:
        fail(f"{PINNER_OVERLAY} is missing")
    try:
        with zipfile.ZipFile(io.BytesIO(read_small(archive, info))) as overlay:
            manifest = overlay.read("AndroidManifest.xml")
            table = overlay.read("resources.arsc")
    except (KeyError, OSError, zipfile.BadZipFile) as error:
        fail(f"{PINNER_OVERLAY} has no readable manifest and resource table: {error}")
    try:
        target = manifest_target_sdk(manifest)
    except (ValueError, struct.error) as error:
        fail(f"{PINNER_OVERLAY} has an unreadable manifest: {error}")
    if target is None or target > PINNER_OVERLAY_MAX_TARGET_SDK:
        fail(
            f"{PINNER_OVERLAY} targets SDK {target}: PinnerService's pin sizes are not overlayable, so "
            f"the overlay applies only when it targets {PINNER_OVERLAY_MAX_TARGET_SDK} or lower "
            "(its Android.bp builds it with sdk_version \"28\")"
        )
    # Resource names sit in the table's key string pool as UTF-8 or UTF-16.
    if PINNER_OVERLAY_RESOURCE not in table and PINNER_OVERLAY_RESOURCE.decode().encode("utf-16-le") not in table:
        fail(f"{PINNER_OVERLAY} does not set {PINNER_OVERLAY_RESOURCE.decode()}")


# The permission that lets an application start Android's trade-in mode (TradeInModeService in
# system_server). An unprovisioned device enters it a few tens of seconds after boot when an
# application asks, which turns USB debugging on and restarts adbd twice. A virtual device has no
# use for it, and the service has no configuration switch, so the product carries no application
# that asks at boot: AOSP's DeviceDiagnostics, whose boot receiver does, is excluded from the
# product. The shell package holds the permission too, for the commands a person types over adb;
# nothing in it starts the mode by itself. Any other APK of the image partitions that requests the
# permission is refused.
TRADE_IN_MODE_PERMISSION = "android.permission.ENTER_TRADE_IN_MODE"
TRADE_IN_MODE_REQUESTERS = {"SYSTEM/priv-app/Shell/Shell.apk"}


def check_application_manifests(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo], build_variant: str
) -> None:
    """No application opts in to debugging (a user build) or can start trade-in mode (every build).

    A debuggable application is open to `run-as` and to a debugger whatever ro.debuggable says,
    and release signing would not change that, so a test probe left in the shared product
    output (or a prebuilt built with --debug-mode) must stop the build here. Only a user build
    is checked for it: a userdebug build is the diagnostics variant. Trade-in mode can start only
    for an application that requests ENTER_TRADE_IN_MODE, and the privapp allowlist's entry for
    a package that is not installed grants nothing, so the request is what the check looks for,
    in every variant (a userdebug build never enters it, but the product list is one list), in
    every APK but the shell's (TRADE_IN_MODE_REQUESTERS). TalkBack and eSpeak NG request no
    permission beyond the ones the build is known to need (ACCESSIBILITY_PERMISSIONS), in
    every variant. The check reads every APK of the image partitions; APKs inside an APEX payload
    are compressed EROFS and out of its reach.
    """
    for entry in sorted(infos):
        info = infos[entry]
        path = PurePosixPath(entry)
        if (
            info.is_dir()
            or path.suffix != ".apk"
            or path.parts[0] not in TARGET_PARTITIONS
            # A symbolic link holds its target's path; the target is checked as its own entry.
            or stat.S_ISLNK(info.external_attr >> 16)
        ):
            continue
        try:
            with zipfile.ZipFile(io.BytesIO(archive.read(info))) as apk:
                manifest = apk.read("AndroidManifest.xml")
            debuggable = build_variant == "user" and manifest_debuggable(manifest)
            requested = manifest_requested_permissions(manifest)
        except (KeyError, OSError, zipfile.BadZipFile, ValueError, struct.error) as error:
            fail(f"{entry} has no readable manifest: {error}")
        if debuggable:
            fail(f"a user build must not carry a debuggable application: {entry}")
        if TRADE_IN_MODE_PERMISSION in requested and entry not in TRADE_IN_MODE_REQUESTERS:
            fail(
                f"{entry} requests {TRADE_IN_MODE_PERMISSION}: trade-in mode would start on an "
                "unprovisioned first boot and restart adbd twice; the product must not carry "
                "an application that can start it (DeviceDiagnostics is excluded)"
            )
        for module, pattern in ACCESSIBILITY_APKS.items():
            refused = accessibility_refused_permissions(module, requested) if pattern.fullmatch(entry) else []
            if refused:
                fail(
                    f"{entry} requests {', '.join(refused)}: TalkBack and eSpeak NG hold no "
                    "network access and no Google service permission, and request only the "
                    "permissions the build is known to need (ACCESSIBILITY_PERMISSIONS; the "
                    "TalkBack series, dependencies/patches/talkback, leaves the others out of "
                    "the manifests)"
                )


# The AOSP example HALs the product keeps, each packaged as the vendor APEX that Cuttlefish
# installs. The APEX payload holds the service binary, the VINTF fragment that declares the AIDL
# interface, and the init script that starts the service. The payload is compressed EROFS: the
# fragment and the script stay readable in its bytes because the payload's compress hints
# (overlay/config/erofs_apex_payload_hints.txt) store every .rc and .xml file uncompressed, and
# the binary is looked up in the payload's directories, since the init script names it too.
APEX_PAYLOAD_READ_LIMIT = 64 << 20
# MediaCas and CTS MediaCasTest need the default CAS HAL, which loads the ClearKey CAS plugin
# (CAS system id 63192) from /vendor/lib*/mediacas.
CAS_HAL = {
    "apex": "VENDOR/apex/com.android.hardware.cas.apex",
    "binary": b"android.hardware.cas-service.example",
    "fragment": (
        b"<name>android.hardware.cas</name>",
        b"<fqname>IMediaCasService/default</fqname>",
    ),
    "interface": "IMediaCasService/default",
    "service": "vendor.cas-default",
    "need": "the default CAS HAL",
}
# The virtual Wi-Fi needs the Wi-Fi HAL and the supplicant, which Cuttlefish packages as vendor
# APEXes (the supplicant one holds hostapd too). The HAL APEX's init script names the binary
# below /apex/com.android.hardware.wifi.
WIFI_HAL = {
    "apex": "VENDOR/apex/com.android.hardware.wifi.apex",
    "binary": b"android.hardware.wifi-service",
    "fragment": (
        b"<name>android.hardware.wifi</name>",
        b"<fqname>IWifi/default</fqname>",
    ),
    "interface": "IWifi/default",
    "service": "vendor.wifi_hal_legacy",
    "need": "the Wi-Fi HAL",
}
WIFI_SUPPLICANT = {
    "apex": "VENDOR/apex/com.google.cf.wpa_supplicant.apex",
    "binary": b"wpa_supplicant",
    "fragment": (
        b"<name>android.hardware.wifi.supplicant</name>",
        b"<fqname>ISupplicant/default</fqname>",
    ),
    "interface": "ISupplicant/default",
    "service": "wpa_supplicant",
    "need": "the Wi-Fi supplicant",
}
CAS_PLUGINS = (
    "VENDOR/lib64/mediacas/libclearkeycasplugin.so",
    "VENDOR/lib/mediacas/libclearkeycasplugin.so",
)


def read_apex_payload(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo], apex_name: str, need: str
) -> bytes:
    """The EROFS payload of a required APEX."""
    info = infos.get(apex_name)
    if info is None:
        fail(f"{apex_name} is missing: the product needs {need}")
    try:
        with zipfile.ZipFile(io.BytesIO(read_small(archive, info, 16 << 20))) as apex:
            payload_info = apex.getinfo("apex_payload.img")
            if payload_info.file_size > APEX_PAYLOAD_READ_LIMIT:
                fail(f"{apex_name} has an unexpectedly large payload")
            payload = apex.read(payload_info)
    except (KeyError, OSError, zipfile.BadZipFile) as problem:
        fail(f"cannot inspect {apex_name}: {problem}")
    if payload[android_image.EROFS_SUPERBLOCK_OFFSET :][:4] != android_image.EROFS_MAGIC:
        fail(f"cannot inspect {apex_name}: the payload is not EROFS")
    return payload


def check_payload_regular_file(payload: bytes, path: str, apex_name: str, what: str) -> None:
    """The payload's directories hold a regular file at this path."""
    try:
        file_type = android_image.erofs_file_type(payload, path, apex_name, error)
    except (struct.error, ValueError) as parse_error:
        fail(f"cannot read the directories of {apex_name}: {parse_error}")
    if file_type != android_image.EROFS_FILE_TYPE_REGULAR:
        fail(f"{apex_name} does not hold {what} {path}")


# The framework's AdbService carries the p0007 topics that record the USB debugging setting in
# persist.sys.usb.config and restart adbd, where stock stops it, when debugging is turned off.
# The jar stores its dex files uncompressed, so the topic's log line is in the bytes. A build
# without it would forget the user's choice at every boot and stop the host's channel.
# The display topic of the same route lets a display without a refresh rate throttling map use the
# default configuration's; without it the thermal HAL's SEVERE status holds no frame rate back.
# The SystemConfig topic reads the compass feature's directory when the launch says the host has a
# magnetometer; without it a host that has one would run an image with a sensor and no feature.
# The SensorPrivacyService topic withholds the reminder (dialog and notification) of a camera that the
# hardware toggle blocks; without it a session started with Camera off would offer, in SystemUI's
# "Unblock device camera?" dialog, an Unblock that cannot lift the hardware toggle.
SERVICES_JAR = "SYSTEM/framework/services.jar"
SERVICES_JAR_MARKERS = (
    (b"kernarium: USB debugging is now ", "the AdbService topics"),
    (
        b"RefreshRateThrottling: using the map of the default configuration",
        "the display topic that reads the default configuration's refresh rate throttling map",
    ),
    (
        b"ro.boot.kernarium.compass",
        "the SystemConfig topic that reads the compass feature for a host with a magnetometer",
    ),
    (b"kernarium_compass", "the directory that SystemConfig topic reads"),
    (
        b"kernarium: no reminder for a camera that the hardware toggle blocks",
        "the SensorPrivacyService topic that withholds the reminder of a camera under the hardware toggle",
    ),
)
SERVICES_JAR_LIMIT = 48 << 20


def check_services_jar(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    info = infos.get(SERVICES_JAR)
    if info is None:
        fail(f"{SERVICES_JAR} is missing")
    try:
        with zipfile.ZipFile(io.BytesIO(read_small(archive, info, SERVICES_JAR_LIMIT))) as jar:
            dex = [
                name for name in jar.namelist() if re.fullmatch(r"classes[0-9]*\.dex", name)
            ]
            if not dex:
                fail(f"{SERVICES_JAR} holds no dex file")
            for marker, topic in SERVICES_JAR_MARKERS:
                if not any(marker in jar.read(name) for name in dex):
                    fail(
                        f"{SERVICES_JAR} lacks {topic} of route p0007 "
                        f"({marker.decode()!r} is not in its dex files)"
                    )
    except (OSError, zipfile.BadZipFile) as problem:
        fail(f"cannot inspect {SERVICES_JAR}: {problem}")


# The old-application layer (overlay/oldapp) is a 32-bit library that ActivityThread, in framework.jar,
# loads into the process of a 32-bit application that targets below API 14 and that the person
# switched it on for (a p0007 topic; the property of the store is the switch, so a framework that
# loads the layer for every old application lacks it). A build
# without the library, or with a framework that does not load it, leaves such a game with the ART
# failures the layer exists to prevent; a library that is not a 32-bit ARM shared object with a
# JNI_OnLoad would fail when the framework loads it. The jar stores its dex files uncompressed, so the
# topic's strings are in the bytes.
OLD_APP_LIBRARY = "SYSTEM/lib/libkernarium_oldapp.so"
FRAMEWORK_JAR = "SYSTEM/framework/framework.jar"
FRAMEWORK_JAR_MARKERS = (
    (b"/system/lib/libkernarium_oldapp.so", "the ActivityThread topic that loads the old-application layer"),
    (b"kernarium: old-application layer not loaded", "the log line of that topic"),
    (b"persist.sys.kernarium.oldapp.", "the per-application switch (the store's property) of that topic"),
)
FRAMEWORK_JAR_LIMIT = 256 << 20
OLD_APP_LIBRARY_LIMIT = 16 << 20
ELF_CLASS_32, ELF_LITTLE_ENDIAN, ELF_TYPE_SHARED = 1, 1, 3


def check_old_application_layer(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    info = infos.get(OLD_APP_LIBRARY)
    if info is None:
        fail(f"{OLD_APP_LIBRARY} is missing")
    data = read_small(archive, info, OLD_APP_LIBRARY_LIMIT)
    if len(data) < 20 or data[:4] != b"\x7fELF" or (data[4], data[5]) != (ELF_CLASS_32, ELF_LITTLE_ENDIAN):
        fail(f"{OLD_APP_LIBRARY} is not a 32-bit little-endian ELF object")
    elf_type, machine = struct.unpack_from("<HH", data, 16)
    if elf_type != ELF_TYPE_SHARED or machine != android_image.EM_ARM:
        fail(f"{OLD_APP_LIBRARY} is not a shared object for ARM (type {elf_type}, machine {machine})")
    if b"\0JNI_OnLoad\0" not in data:
        fail(f"{OLD_APP_LIBRARY} has no JNI_OnLoad, which the framework's System.load runs")
    framework = infos.get(FRAMEWORK_JAR)
    if framework is None:
        fail(f"{FRAMEWORK_JAR} is missing")
    try:
        with zipfile.ZipFile(io.BytesIO(read_small(archive, framework, FRAMEWORK_JAR_LIMIT))) as jar:
            dex = [name for name in jar.namelist() if re.fullmatch(r"classes[0-9]*\.dex", name)]
            if not dex:
                fail(f"{FRAMEWORK_JAR} holds no dex file")
            for marker, topic in FRAMEWORK_JAR_MARKERS:
                if not any(marker in jar.read(name) for name in dex):
                    fail(
                        f"{FRAMEWORK_JAR} lacks {topic} of route p0007 "
                        f"({marker.decode()!r} is not in its dex files)"
                    )
    except (OSError, zipfile.BadZipFile) as problem:
        fail(f"cannot inspect {FRAMEWORK_JAR}: {problem}")


# The p0029 topic makes NetworkMonitor validate the virtual Wi-Fi before it checks it, so a blocked
# validation server cannot leave the guest's one network unvalidated, and report a check that failed,
# or that a sign-in or block page answered, only as the HTTPS probe attempted and not succeeded.
# NetworkStack is a priv-app of the system partition, and its APK stores the dex file uncompressed,
# so the topic's validation log lines are in the bytes. A build without them would probe the virtual
# Wi-Fi as a network that has to pass its probes (the first line) or leave a failed check unreported
# (the second).
NETWORK_STACK_APK = "SYSTEM/priv-app/NetworkStack/NetworkStack.apk"
NETWORK_STACK_MARKERS = (
    b"Wi-Fi is validated before its internet check",
    b"Wi-Fi internet check failed, the network stays validated",
)
NETWORK_STACK_LIMIT = 16 << 20


def check_network_stack(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    info = infos.get(NETWORK_STACK_APK)
    if info is None:
        fail(f"{NETWORK_STACK_APK} is missing")
    try:
        with zipfile.ZipFile(io.BytesIO(read_small(archive, info, NETWORK_STACK_LIMIT))) as apk:
            dex = [
                name for name in apk.namelist() if re.fullmatch(r"classes[0-9]*\.dex", name)
            ]
            if not dex:
                fail(f"{NETWORK_STACK_APK} holds no dex file")
            for marker in NETWORK_STACK_MARKERS:
                if not any(marker in apk.read(name) for name in dex):
                    fail(
                        f"{NETWORK_STACK_APK} lacks the NetworkMonitor topic of route p0029 "
                        f"({marker.decode()!r} is not in its dex files): the guest's Wi-Fi would not "
                        "be validated before its check, or a failed check would not be reported"
                    )
    except (OSError, zipfile.BadZipFile) as problem:
        fail(f"cannot inspect {NETWORK_STACK_APK}: {problem}")


# The p0033 topic makes WifiTrackerLib show a failed check of a validated network: the Wi-Fi mark in
# the status bar and in the Wi-Fi list. Settings and SystemUI link the library statically and store
# their dex files uncompressed. The optimizer inlines a small method away, so the check looks for the
# key of the connectivity report that the topic's predicate reads (the bitmask of the probes
# attempted): a string constant of the live code, which no other code of either application holds.
# A build without the topic would show nothing for a network whose check failed.
WIFI_TRACKER_APKS = (
    "SYSTEM_EXT/priv-app/Settings/Settings.apk",
    "SYSTEM_EXT/priv-app/SystemUI/SystemUI.apk",
)
WIFI_TRACKER_MARKERS = (b"networkProbesAttempted",)
WIFI_TRACKER_LIMIT = 192 << 20


def check_wifi_tracker_users(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    for apk_path in WIFI_TRACKER_APKS:
        info = infos.get(apk_path)
        if info is None:
            fail(f"{apk_path} is missing")
        try:
            with zipfile.ZipFile(io.BytesIO(read_small(archive, info, WIFI_TRACKER_LIMIT))) as apk:
                dex = [
                    name for name in apk.namelist() if re.fullmatch(r"classes[0-9]*\.dex", name)
                ]
                if not dex:
                    fail(f"{apk_path} holds no dex file")
                contents = [apk.read(name) for name in dex]
                for marker in WIFI_TRACKER_MARKERS:
                    if not any(marker in content for content in contents):
                        fail(
                            f"{apk_path} lacks the WifiTrackerLib topic of route p0033 "
                            f"({marker.decode()!r} is not in its dex files): a Wi-Fi network whose "
                            "internet check failed would show no warning"
                        )
        except (OSError, zipfile.BadZipFile) as problem:
            fail(f"cannot inspect {apk_path}: {problem}")


def check_ota_certificates(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo], build_variant: str, signed: bool
) -> None:
    """The system's and the recovery's otacerts.zip each hold the one member the build state names.

    An unsigned build of either variant holds the AOSP test certificate's file name. The signed
    target files of a release trust the release certificate alone: sign_target_files_apks names it
    after the path it is given for the release key, so a signed archive holds that one member and
    an archive still carrying the test certificate holds another. Only a user build is signed, with
    the one release key set; the signer checks the certificate's digest, which needs the keys.
    """
    member = ota_certificate_member(build_variant, signed)
    state = "signed" if signed else "unsigned"
    for name in OTA_CERTIFICATE_ARCHIVES:
        if name not in infos:
            fail(f"{name} is missing from the {state} target files")
        try:
            with zipfile.ZipFile(io.BytesIO(read_small(archive, infos[name]))) as certificates:
                members = sorted(certificates.namelist())
        except zipfile.BadZipFile as problem:
            fail(f"{name} is not a zip archive: {problem}")
        if members != [member]:
            fail(
                f"{name} holds the members {members}, not only {member}: the target files are "
                + ("not signed with the release keys" if signed else "not an unsigned build")
            )


def check_adbd_launch_properties(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    """The APEX's adbd reads the host application's key and the launch's TCP policy.

    The payload hints store bin/adbd uncompressed, so the names that the p0011 topics add are
    in the payload's bytes. A build whose adbd lacks them would ignore the key and keep
    listening on guest TCP, which no other check would notice.
    """
    payload = read_apex_payload(archive, infos, ADBD_APEX, "the adb daemon")
    check_payload_regular_file(payload, ADBD_BINARY, ADBD_APEX, "the adb daemon")
    for name in ADBD_LAUNCH_PROPERTIES + ADBD_DEBUGGING_PROPERTIES:
        if name.encode() not in payload:
            fail(
                f"{ADBD_APEX} holds an adbd that does not read {name}: the p0011 topics are "
                "missing from the build, or the payload hints no longer store bin/adbd "
                "uncompressed"
            )
    if ADBD_REFUSAL_MARKER not in payload:
        fail(
            f"{ADBD_APEX} holds an adbd without the key rule of route p0011 "
            f"({ADBD_REFUSAL_MARKER.decode()!r} is not in bin/adbd): it would offer every "
            "key a prompt while debugging is off"
        )


def check_adb_root_service(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    """The adb_root service must not turn rooted debugging on for a build that is not debuggable.

    Stock adb_root has no such rule and the bypass topic of route p0011 would otherwise enable it on
    every build whose boot policy skips authorization, which makes `adb root` on a user build print
    "restarting adbd as root" and wait for a restart that no init trigger performs. The service binary
    is in the target files as a file, so the guarded topic's log line is in its bytes.
    """
    info = infos.get(ADB_ROOT_BINARY)
    if info is None or info.is_dir():
        fail(f"{ADB_ROOT_BINARY} is missing: the product needs LineageOS's adb_root service")
    if ADB_ROOT_GUARD_MARKER not in read_small(archive, info, 64 << 20):
        fail(
            f"{ADB_ROOT_BINARY} does not hold the debuggable-build rule of route p0011 "
            f"({ADB_ROOT_GUARD_MARKER.decode()!r} is not in it): it would enable rooted debugging on "
            "a user build, whose `adb root` then waits for an adbd restart that never comes"
        )


# The input topic of route p0022 lights LED_MISC on a cursor device that advertises it while pointer
# capture is on, which is how the host's mouse learns that an application captured the pointer. The
# mapper's dump line for it is in libinputflinger.so's bytes. A build without the topic never tells the
# host, and a game that captures the mouse would hold a pointer that the host does not release.
INPUT_FLINGER = "SYSTEM/lib64/libinputflinger.so"
INPUT_FLINGER_MARKER = b"PointerCaptureLed: "
INPUT_FLINGER_LIMIT = 64 << 20


def check_input_flinger(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    info = infos.get(INPUT_FLINGER)
    if info is None or info.is_dir():
        fail(f"{INPUT_FLINGER} is missing")
    if INPUT_FLINGER_MARKER not in read_small(archive, info, INPUT_FLINGER_LIMIT):
        fail(
            f"{INPUT_FLINGER} lacks the pointer capture LED topic of route p0022 "
            f"({INPUT_FLINGER_MARKER.decode()!r} is not in it): the mouse would not tell the host "
            "that an application captured the pointer"
        )


def check_example_hal(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo], hal: dict
) -> None:
    """The vendor APEX must hold the example service, its VINTF fragment and its init script."""
    apex_name = hal["apex"]
    binary = hal["binary"]
    payload = read_apex_payload(archive, infos, apex_name, hal["need"])
    check_payload_regular_file(payload, f"bin/hw/{binary.decode()}", apex_name, "the service binary")
    if not all(marker in payload for marker in hal["fragment"]):
        fail(f"{apex_name} does not declare {hal['interface']} in a VINTF fragment")
    apex_dir = PurePosixPath(apex_name).stem
    service = f"service {hal['service']} /apex/{apex_dir}/bin/hw/".encode() + binary
    if service not in payload:
        fail(f"{apex_name} does not start the {hal['service']} service")


def check_wifi_hals(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    check_example_hal(archive, infos, WIFI_HAL)
    check_example_hal(archive, infos, WIFI_SUPPLICANT)


def check_cas_hal(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    check_example_hal(archive, infos, CAS_HAL)
    for plugin in CAS_PLUGINS:
        if plugin not in infos:
            fail(f"the ClearKey CAS plugin is missing: {plugin}")


# The motion sub-HAL lists a device-orientation sensor, which the window manager then follows instead of
# judging the accelerometer, when the launch passes androidboot.kernarium.orientation=1. The sub-HAL logs
# the property when it lists its sensors, and the format string of that line is in the library's bytes.
# A build whose sub-HAL lacks it never offers the sensor, and no other check would notice: Andarium's
# host would wait for a guest that does not say it takes orientations.
MOTION_SUB_HAL = "VENDOR/lib64/hw/android.hardware.sensors@2.1-impl.andarium.so"
MOTION_SUB_HAL_MARKER = b"Andarium motion: ro.boot.kernarium.orientation="
MOTION_SUB_HAL_LIMIT = 16 << 20
# Only the launch states it (the host's "androidboot.kernarium.orientation=1" argument); a build.prop that
# set it would list the sensor for a host that does not feed it, and the window manager would follow a
# sensor that nothing drives.
MOTION_ORIENTATION_PROPERTY = "ro.boot.kernarium.orientation"
# The same sub-HAL lists a magnetometer when the launch passes androidboot.kernarium.compass=1, which the
# guest reads as ro.boot.kernarium.compass; its log line names the property and the sensor's name is in
# the bytes. The compass feature is declared for that launch alone: the file below is read by SystemConfig
# (the p0007 topic) only when the property is true, and nothing else may declare the feature, since a host
# without a magnetometer would then claim a compass no application can use. The directory holds that file
# alone for the same reason, and no build.prop sets the property, as for the orientation sensor.
MOTION_SUB_HAL_COMPASS_MARKERS = (
    (b"ro.boot.kernarium.compass=", "the property that decides whether it lists the magnetometer"),
    (b"Host Magnetometer", "the magnetometer"),
)
MOTION_COMPASS_PROPERTY = "ro.boot.kernarium.compass"
COMPASS_FEATURE = "android.hardware.sensor.compass"
COMPASS_PERMISSION_DIRECTORY = "VENDOR/etc/permissions/kernarium_compass"
COMPASS_FEATURE_FILE = f"{COMPASS_PERMISSION_DIRECTORY}/{COMPASS_FEATURE}.xml"


def check_motion_sensors(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    configuration = read_small(archive, infos["VENDOR/etc/sensors/hals.conf"]).decode().strip()
    if configuration != "/vendor/lib64/hw/android.hardware.sensors@2.1-impl.andarium.so":
        fail("motion multi-HAL must load only the Andarium backend")
    fragment = ET.fromstring(read_small(
        archive, infos["VENDOR/etc/vintf/manifest/android.hardware.sensors-multihal.xml"]
    ))
    if not any(hal.get("format") == "aidl"
               and hal.findtext("name") == "android.hardware.sensors"
               and hal.findtext("fqname") == "ISensors/default" for hal in fragment.findall("hal")):
        fail("motion sensor HAL is not declared as AIDL ISensors/default")
    script = read_small(archive, infos[
        "VENDOR/etc/init/android.hardware.sensors-service-multihal.rc"
    ]).decode()
    if not re.search(r"^service \S+ /vendor/bin/hw/android\.hardware\.sensors-service\.multihal$", script, re.M):
        fail("motion sensor HAL service does not start the multi-HAL binary")
    # The marker counts only in a shared library the image can load: bytes that merely contain the text
    # would pass for a sub-HAL.
    info = infos[MOTION_SUB_HAL]
    prefix = read_elf_prefix(archive, info)
    if prefix is None:
        fail(f"{MOTION_SUB_HAL} is not an ELF file")
    elf_type, machine = struct.unpack_from("<HH", prefix, 16)
    if elf_type != ET_DYN or machine != android_image.EM_AARCH64:
        fail(f"{MOTION_SUB_HAL} is not an AArch64 shared library")
    check_target_elf(info, prefix)
    library = read_small(archive, info, MOTION_SUB_HAL_LIMIT)
    if MOTION_SUB_HAL_MARKER not in library:
        fail(
            f"{MOTION_SUB_HAL} lacks the device-orientation sensor ({MOTION_SUB_HAL_MARKER.decode()!r} "
            "is not in it): a launch that asks for the sensor would find none"
        )
    for marker, what in MOTION_SUB_HAL_COMPASS_MARKERS:
        if marker not in library:
            fail(
                f"{MOTION_SUB_HAL} lacks {what} ({marker.decode()!r} is not in it): "
                "a launch that says the host has a magnetometer would find none"
            )
    check_compass_feature(archive, infos)


def check_compass_feature(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    """The compass feature is one file in a directory of its own, which only a launch that says the
    host has a magnetometer makes SystemConfig read."""
    info = infos.get(COMPASS_FEATURE_FILE)
    if info is None or info.is_dir():
        fail(f"{COMPASS_FEATURE_FILE} is missing: a host with a magnetometer would have no compass feature")
    try:
        root = ET.fromstring(read_small(archive, info))
    except ET.ParseError as error:
        fail(f"cannot parse {COMPASS_FEATURE_FILE}: {error}")
    features = [
        element.attrib.get("name") for element in root.iter() if element.tag.rsplit("}", 1)[-1] == "feature"
    ]
    if features != [COMPASS_FEATURE]:
        fail(f"{COMPASS_FEATURE_FILE} declares {features}, not {COMPASS_FEATURE} alone")
    # SystemConfig reads <permissions> files; the file is that root with the one feature and nothing that
    # takes a feature away again.
    if root.tag != "permissions":
        fail(f"{COMPASS_FEATURE_FILE} has the root element <{root.tag}>, not <permissions>")
    if any(element.tag.rsplit("}", 1)[-1] == "unavailable-feature" for element in root.iter()):
        fail(f"{COMPASS_FEATURE_FILE} holds an unavailable-feature element: it declares the compass feature alone")
    children = list(root)
    if len(children) != 1 or children[0].tag != "feature" or set(children[0].attrib) != {"name"}:
        fail(f"{COMPASS_FEATURE_FILE} is not one <feature name=...> in <permissions>: {[c.tag for c in children]}")
    for name in sorted(infos):
        if name.startswith(f"{COMPASS_PERMISSION_DIRECTORY}/") and name != COMPASS_FEATURE_FILE:
            fail(f"{name} is in {COMPASS_PERMISSION_DIRECTORY}, which holds the compass feature alone")
        # SystemConfig reads etc/sysconfig of every partition with the flags that allow features, as it
        # reads etc/permissions: a declaration there would be read for every launch.
        path = PurePosixPath(name)
        if infos[name].is_dir() or path.suffix != ".xml" or "sysconfig" not in path.parts:
            continue
        try:
            other = ET.fromstring(read_small(archive, infos[name]))
        except ET.ParseError:
            continue
        if any(
            element.tag.rsplit("}", 1)[-1] == "feature" and element.attrib.get("name") == COMPASS_FEATURE
            for element in other.iter()
        ):
            fail(
                f"the compass feature is declared in the sysconfig file {name}: only a host with a "
                f"magnetometer has a compass, and SystemConfig reads {COMPASS_PERMISSION_DIRECTORY} for "
                "such a host alone"
            )
    check_motion_properties_in_init(archive, infos)


def init_commands(text: str) -> list[list[str]]:
    """The logical commands of an init script as init's tokenizer reads them: words split at blanks, double
    quotes group (and are dropped), a backslash continues the line at a newline and escapes inside a word, and a
    # that starts a word comments out the rest of the line. A newline outside a quote ends the command."""
    commands: list[list[str]] = []
    words: list[str] = []
    word: list[str] = []
    in_word = in_quote = False
    index, length = 0, len(text)

    def end_word() -> None:
        nonlocal in_word
        if in_word:
            words.append("".join(word))
            word.clear()
        in_word = False

    def end_command() -> None:
        nonlocal in_quote
        end_word()
        in_quote = False
        if words:
            commands.append(list(words))
        words.clear()

    while index < length:
        character = text[index]
        if character == "\\":
            following = text[index + 1] if index + 1 < length else ""
            if following in ("\n", "\r"):
                # A continued line: the newline and the blanks after it are not part of the command.
                index += 2 if following == "\n" or text[index + 1:index + 3] != "\r\n" else 3
                while index < length and text[index] in " \t":
                    index += 1
                if not in_quote:
                    end_word()
                continue
            word.append({"n": "\n", "r": "\r", "t": "\t"}.get(following, following))
            in_word = True
            index += 2
            continue
        if in_quote:
            if character == '"':
                in_quote = False
            elif character == "\n":
                end_command()
            else:
                word.append(character)
            index += 1
            continue
        if character == '"':
            in_quote = in_word = True
        elif character == "\n":
            end_command()
        elif character in " \t\r":
            end_word()
        elif character == "#" and not in_word:
            while index < length and text[index] != "\n":
                index += 1
            continue
        else:
            word.append(character)
            in_word = True
        index += 1
    end_command()
    return commands


def check_motion_properties_in_init(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    """The launch alone states what the host feeds: no init script of the image sets the properties the
    sensors' list and the compass feature follow (build.prop is checked with every property file). A command
    is read as init reads it, so a quoted word or a continued line is the command it is."""
    for name in sorted(infos):
        if infos[name].is_dir() or not name.endswith(".rc"):
            continue
        for command in init_commands(read_small(archive, infos[name]).decode("utf-8", errors="replace")):
            if len(command) >= 2 and command[0] == "setprop" and command[1] in (
                    MOTION_ORIENTATION_PROPERTY, MOTION_COMPASS_PROPERTY):
                fail(
                    f"{name} sets {command[1]}: the launch alone states whether the host feeds the "
                    "device-orientation sensor and has a magnetometer"
                )


# The vibrator HAL exists only on a device with haptics. The host passes the "haptics" ODM SKU for
# one; the SKU's manifest is the one place that declares a vibrator, and the service's init script
# starts it for that SKU alone. Cuttlefish's vibrator (an APEX of the AOSP example HAL) is not
# installed, and no other manifest, fragment or APEX declares the HAL.
VIBRATOR_HAL_NAME = "android.hardware.vibrator"
VIBRATOR_SKU = "haptics"
VIBRATOR_SKU_MANIFEST = f"ODM/etc/vintf/manifest_{VIBRATOR_SKU}.xml"
VIBRATOR_SERVICE = "android.hardware.vibrator-service.andarium"
VIBRATOR_INTERFACE = "android.hardware.vibrator.IVibrator/default"
# The file_contexts line itself is a regular expression, so the check looks for its text.
VIBRATOR_FILE_CONTEXT = re.compile(
    re.escape(rb"/(vendor|system/vendor)/bin/hw/android\.hardware\.vibrator-service\.andarium")
    + rb"\s+u:object_r:hal_vibrator_default_exec:s0"
)


def declared_hals(root: ET.Element) -> list[tuple[str | None, str | None, list[str]]]:
    """The (name, version, fully qualified names) of each HAL a VINTF manifest declares."""
    hals = []
    for hal in root.iter("hal"):
        interfaces = [
            f"{interface.findtext('name')}/{instance.text}"
            for interface in hal.iter("interface")
            for instance in interface.iter("instance")
        ]
        interfaces += [fqname.text or "" for fqname in hal.iter("fqname")]
        hals.append((hal.findtext("name"), hal.findtext("version"), interfaces))
    return hals


def check_haptics_vibrator(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    for name, info in infos.items():
        path = PurePosixPath(name)
        if (
            info.is_dir()
            or path.suffix != ".xml"
            or path.parts[0] not in TARGET_PARTITIONS
            or path.parts[1:3] != ("etc", "vintf")
            or not (path.name.startswith("manifest") or "manifest" in path.parts)
            or name == VIBRATOR_SKU_MANIFEST
        ):
            continue
        try:
            root = ET.fromstring(read_small(archive, info))
        except ET.ParseError as error:
            fail(f"cannot parse VINTF manifest {name}: {error}")
        if any(hal_name == VIBRATOR_HAL_NAME for hal_name, _, _ in declared_hals(root)):
            fail(
                f"{name} declares a vibrator HAL: only {VIBRATOR_SKU_MANIFEST} may, so that a "
                "device without haptics has none"
            )
    try:
        manifest = ET.fromstring(read_small(archive, infos[VIBRATOR_SKU_MANIFEST]))
    except ET.ParseError as error:
        fail(f"cannot parse {VIBRATOR_SKU_MANIFEST}: {error}")
    hals = declared_hals(manifest)
    if (
        len(hals) != 1
        or hals[0][0] != VIBRATOR_HAL_NAME
        or hals[0][1] != "4"
        or hals[0][2] != ["IVibrator/default"]
        or any(hal.get("format") != "aidl" for hal in manifest.iter("hal"))
    ):
        fail(f"{VIBRATOR_SKU_MANIFEST} must declare exactly the AIDL IVibrator/default at version 4: {hals}")
    skus = sorted(
        PurePosixPath(name).name
        for name in infos
        if re.fullmatch(r"ODM/etc/vintf/manifest_[^/]+\.xml", name)
    )
    if skus != [PurePosixPath(VIBRATOR_SKU_MANIFEST).name]:
        fail(f"the ODM SKU manifests must be exactly the {VIBRATOR_SKU} one: {skus}")
    skus_line = [
        line
        for line in read_small(archive, infos["META/misc_info.txt"]).decode("utf-8").splitlines()
        if line.startswith("vintf_odm_manifest_skus=")
    ]
    if skus_line != [f"vintf_odm_manifest_skus={VIBRATOR_SKU}"]:
        fail(f"META/misc_info.txt must name only the {VIBRATOR_SKU} ODM manifest SKU: {skus_line}")
    script = read_small(
        archive, infos[f"VENDOR/etc/init/{VIBRATOR_SERVICE}.rc"]
    ).decode("utf-8", errors="strict")
    lines = [line.strip() for line in script.splitlines() if line.strip() and not line.strip().startswith("#")]
    service = [line for line in lines if line.startswith("service ")]
    if len(service) != 1 or not re.fullmatch(
        rf"service \S+ /vendor/bin/hw/{re.escape(VIBRATOR_SERVICE)}", service[0]
    ):
        fail("the vibrator init script must define one service that runs the Andarium vibrator HAL")
    for required in (f"interface aidl {VIBRATOR_INTERFACE}", "disabled"):
        if required not in lines:
            fail(f"the vibrator service must carry '{required}': it starts for the haptics SKU alone")
    name = service[0].split()[1]
    trigger = f"on boot && property:ro.boot.product.hardware.sku={VIBRATOR_SKU}"
    if trigger not in lines:
        fail(f"the vibrator service must start on '{trigger}'")
    after = lines[lines.index(trigger) + 1 :]
    if after[:2] != [f"enable {name}", f"start {name}"]:
        fail(f"'{trigger}' must enable and start {name}")
    if any(line.startswith("on ") for line in lines if line != trigger):
        fail("the vibrator init script may have no trigger but the haptics SKU's boot trigger")
    if not VIBRATOR_FILE_CONTEXT.search(read_small(archive, infos["VENDOR/etc/selinux/vendor_file_contexts"])):
        fail("vendor_file_contexts does not give the vibrator HAL its hal_vibrator_default_exec label")


# The GNSS HAL (overlay/gnss) reads the host's receiver, /dev/gnss0, which every device has, so
# every device declares it: one vendor fragment, an init script that starts the service at boot
# with no condition, and no other declaration of android.hardware.gnss anywhere (the example
# HAL's APEX is not installed). The image carries no assistance server: the sample
# gps_debug.conf has comments only.
GNSS_HAL_NAME = "android.hardware.gnss"
GNSS_SERVICE = "android.hardware.gnss-service.andarium"
GNSS_FRAGMENT = f"VENDOR/etc/vintf/manifest/{GNSS_SERVICE}.xml"
GNSS_INTERFACE = "android.hardware.gnss.IGnss/default"
GNSS_VERSION = "7"
GNSS_FILE_CONTEXT = re.compile(
    re.escape(rb"/(vendor|system/vendor)/bin/hw/android\.hardware\.gnss-service\.andarium")
    + rb"\s+u:object_r:hal_gnss_default_exec:s0"
)
GNSS_DEBUG_CONF = "SYSTEM/etc/gps_debug.conf"


def check_gnss_hal(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    for name, info in infos.items():
        path = PurePosixPath(name)
        if (
            info.is_dir()
            or path.suffix != ".xml"
            or path.parts[0] not in TARGET_PARTITIONS
            or path.parts[1:3] != ("etc", "vintf")
            or not (path.name.startswith("manifest") or "manifest" in path.parts)
            or name == GNSS_FRAGMENT
        ):
            continue
        try:
            root = ET.fromstring(read_small(archive, info))
        except ET.ParseError as error:
            fail(f"cannot parse VINTF manifest {name}: {error}")
        if any(hal_name == GNSS_HAL_NAME for hal_name, _, _ in declared_hals(root)):
            fail(f"{name} declares a GNSS HAL: only {GNSS_FRAGMENT} may")
    try:
        fragment = ET.fromstring(read_small(archive, infos[GNSS_FRAGMENT]))
    except ET.ParseError as error:
        fail(f"cannot parse {GNSS_FRAGMENT}: {error}")
    hals = declared_hals(fragment)
    if (
        len(hals) != 1
        or hals[0][0] != GNSS_HAL_NAME
        or hals[0][1] != GNSS_VERSION
        or hals[0][2] != ["IGnss/default"]
        or any(hal.get("format") != "aidl" for hal in fragment.iter("hal"))
    ):
        fail(
            f"{GNSS_FRAGMENT} must declare exactly the AIDL IGnss/default at version "
            f"{GNSS_VERSION}: {hals}"
        )
    script = read_small(archive, infos[f"VENDOR/etc/init/{GNSS_SERVICE}.rc"]).decode(
        "utf-8", errors="strict"
    )
    lines = [line.strip() for line in script.splitlines() if line.strip() and not line.strip().startswith("#")]
    service = [line for line in lines if line.startswith("service ")]
    if len(service) != 1 or not re.fullmatch(
        rf"service \S+ /vendor/bin/hw/{re.escape(GNSS_SERVICE)}", service[0]
    ):
        fail("the GNSS init script must define one service that runs the Andarium GNSS HAL")
    for required in (f"interface aidl {GNSS_INTERFACE}", "class hal", "user gps", "group gps"):
        if required not in lines:
            fail(f"the GNSS service must carry '{required}'")
    if "disabled" in lines or "oneshot" in lines:
        fail("the GNSS service starts at boot on every device: it is neither disabled nor oneshot")
    if any(line.startswith("on ") for line in lines):
        fail("the GNSS init script may have no trigger: the service starts with its class")
    if not GNSS_FILE_CONTEXT.search(read_small(archive, infos["VENDOR/etc/selinux/vendor_file_contexts"])):
        fail("vendor_file_contexts does not give the GNSS HAL its hal_gnss_default_exec label")
    if GNSS_DEBUG_CONF in infos:
        active = [
            line.strip()
            for line in read_small(archive, infos[GNSS_DEBUG_CONF]).decode("utf-8", errors="replace").splitlines()
            if line.strip() and not line.strip().startswith("#")
        ]
        if active:
            fail(f"{GNSS_DEBUG_CONF} configures an assistance server or a position: {active}")


# The thermal HAL (overlay/thermal) reports the host's thermal state, and every device has a host,
# so every device declares it: one vendor fragment, an init script that starts the service at boot
# with no condition, and no other declaration of android.hardware.thermal anywhere (the example
# HAL's APEX is not installed). system_server's thermal manager and CTS need a thermal HAL.
THERMAL_HAL_NAME = "android.hardware.thermal"
THERMAL_SERVICE = "android.hardware.thermal-service.andarium"
THERMAL_FRAGMENT = f"VENDOR/etc/vintf/manifest/{THERMAL_SERVICE}.xml"
THERMAL_INTERFACE = "android.hardware.thermal.IThermal/default"
THERMAL_VERSION = "3"
THERMAL_FILE_CONTEXT = re.compile(
    re.escape(rb"/(vendor|system/vendor)/bin/hw/android\.hardware\.thermal-service\.andarium")
    + rb"\s+u:object_r:hal_thermal_default_exec:s0"
)
# The default display configuration holds one point: the render rate is held to 30 Hz from the
# SEVERE status on, which the thermal HAL reports while the host is hot. The framework gives a
# display that has no refresh rate throttling map the default configuration's (the display topic of
# route p0007), and reads the first default.xml it finds in the product, vendor and system
# partitions, so a product copy would take the map away.
DISPLAY_CONFIG_DEFAULT = "VENDOR/etc/displayconfig/default.xml"
DISPLAY_CONFIG_SHADOW = "PRODUCT/etc/displayconfig/default.xml"
DISPLAY_CONFIG_POINTS = (("moderate", "0", "60"), ("severe", "0", "30"))


def check_single_hal_declaration(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo], hal_name: str, allowed: str
) -> None:
    """No VINTF manifest but `allowed` declares the HAL."""
    for name, info in infos.items():
        path = PurePosixPath(name)
        if (
            info.is_dir()
            or path.suffix != ".xml"
            or path.parts[0] not in TARGET_PARTITIONS
            or path.parts[1:3] != ("etc", "vintf")
            or not (path.name.startswith("manifest") or "manifest" in path.parts)
            or name == allowed
        ):
            continue
        try:
            root = ET.fromstring(read_small(archive, info))
        except ET.ParseError as error:
            fail(f"cannot parse VINTF manifest {name}: {error}")
        if any(declared == hal_name for declared, _, _ in declared_hals(root)):
            fail(f"{name} declares {hal_name}: only {allowed} may")


def check_thermal_hal(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    check_single_hal_declaration(archive, infos, THERMAL_HAL_NAME, THERMAL_FRAGMENT)
    try:
        fragment = ET.fromstring(read_small(archive, infos[THERMAL_FRAGMENT]))
    except ET.ParseError as error:
        fail(f"cannot parse {THERMAL_FRAGMENT}: {error}")
    hals = declared_hals(fragment)
    if (
        len(hals) != 1
        or hals[0][0] != THERMAL_HAL_NAME
        or hals[0][1] != THERMAL_VERSION
        or hals[0][2] != ["IThermal/default"]
        or any(hal.get("format") != "aidl" for hal in fragment.iter("hal"))
    ):
        fail(
            f"{THERMAL_FRAGMENT} must declare exactly the AIDL IThermal/default at version "
            f"{THERMAL_VERSION}: {hals}"
        )
    script = read_small(archive, infos[f"VENDOR/etc/init/{THERMAL_SERVICE}.rc"]).decode(
        "utf-8", errors="strict"
    )
    lines = [line.strip() for line in script.splitlines() if line.strip() and not line.strip().startswith("#")]
    service = [line for line in lines if line.startswith("service ")]
    if len(service) != 1 or not re.fullmatch(
        rf"service \S+ /vendor/bin/hw/{re.escape(THERMAL_SERVICE)}", service[0]
    ):
        fail("the thermal init script must define one service that runs the Andarium thermal HAL")
    for required in (f"interface aidl {THERMAL_INTERFACE}", "class hal", "user nobody", "group system"):
        if required not in lines:
            fail(f"the thermal service must carry '{required}'")
    if "disabled" in lines or "oneshot" in lines:
        fail("the thermal service starts at boot on every device: it is neither disabled nor oneshot")
    if any(line.startswith("on ") for line in lines):
        fail("the thermal init script may have no trigger: the service starts with its class")
    if not THERMAL_FILE_CONTEXT.search(read_small(archive, infos["VENDOR/etc/selinux/vendor_file_contexts"])):
        fail("vendor_file_contexts does not give the thermal HAL its hal_thermal_default_exec label")
    check_default_display_config(archive, infos)


# The host's cameras (overlay/camera; routes p0030 and p0031). The camera device gives the guest V4L2
# nodes, and the emulated camera HAL of hardware/google/camera takes the cameras from them: the lazy
# provider is started by the init file of a layout that has cameras whenever cameraserver starts (a
# restart finds no provider otherwise: cameraserver lists only a provider that has registered), runs
# while cameraserver lists the cameras and while an application holds a camera or the torch, and exits
# otherwise (init starts it again through its interface); its HWL library holds the V4L2 source of
# route p0030. The host composes one camera layout from its cameras and passes it as the vendor SKU:
# the permission directory of that name declares the camera features, an init trigger selects the
# media profiles that match its size caps and a second one starts the provider, all of them generated
# (camera_layouts.py).
# Nothing here fails loudly when it is missing: a stock library lists no camera, a missing declaration
# lists no feature, and a missing file leaves the default profiles.
CAMERA_HAL_NAME = "android.hardware.camera.provider"
CAMERA_HAL_INTERFACE = "ICameraProvider/internal/0"
CAMERA_PROVIDER = "android.hardware.camera.provider@2.7-service-google-lazy"
CAMERA_EAGER_PROVIDER = "android.hardware.camera.provider@2.7-service-google"
CAMERA_PROVIDER_RC_OPTIONS = (
    f"interface aidl {CAMERA_HAL_NAME}.{CAMERA_HAL_INTERFACE}",
    "oneshot",
    "disabled",
    "class hal",
    "user system",
)
CAMERA_PROVIDER_FILE_CONTEXT = re.compile(
    re.escape(rb"/vendor/bin/hw/android\.hardware\.camera\.provider@2\.7-service-google-lazy")
    + rb"\s+u:object_r:hal_camera_default_exec:s0"
)
CAMERA_HWL = "VENDOR/lib64/libgooglecamerahwl_impl.so"
CAMERA_HWL_LIMIT = 64 << 20
# What only route p0030 puts in the library: the bus name of the camera nodes, the template of the
# characteristics, and a message of the host-driven sensor. The library of the stock tree has none.
CAMERA_HWL_MARKERS = (
    b"virtio-camera:",
    b"emu_camera_v4l2.json",
    b"was not queued completely",
)
CAMERA_TEMPLATE = "VENDOR/etc/config/emu_camera_v4l2.json"
CAMERA_CONFIG_DIRECTORY = "VENDOR/etc/config/"
CAMERA_APP = "PRODUCT/app/Aperture/Aperture.apk"
CAMERA_APP_LIMIT = 256 << 20
# The product's overlay directory is enforced as runtime overlays: the build leaves the application's own
# resources at their defaults and installs the product's values in a static overlay beside the others
# (SimpleDeviceConfig, Launcher3QuickStep, framework-res). Android applies a preinstalled overlay to
# an application that declares no overlayable section, so the check reads the overlay, and requires
# the target to stay without such a section: one that defined it would bind the overlay to a policy.
CAMERA_APP_PACKAGE = "org.lineageos.aperture"
CAMERA_APP_OVERLAY = "PRODUCT/overlay/Aperture__lineage_andarium__auto_generated_rro_product.apk"
CAMERA_AUX_CAMERAS = "config_enableAuxCameras"
CAMERA_IGNORED_AUX_CAMERAS = "config_ignoredAuxCameraIds"
CAMERA_IGNORED_AUX_IDS = ["2"]
CAMERA_ZOOM = "config_enableZoom"
CAMERA_HARDWARE_TOGGLE = "config_supportsHardwareCamToggle"
CAMERA_LAZY_HAL_PROPERTY = ("PRODUCT/etc/build.prop", "ro.camera.enableLazyHal", "true")
CAMERA_CONFIG_PROPERTY = ("VENDOR/build.prop", "ro.vendor.camera.config", "v4l2")
CAMERA_EXTENSIONS_PROPERTY = "ro.camerax.extensions.enabled"


def camera_resources(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo], entry: str, limit: int
) -> bytes:
    """The compiled resource table of an APK of the image."""
    info = infos.get(entry)
    if info is None or info.is_dir():
        fail(f"{entry} is missing")
    try:
        with zipfile.ZipFile(io.BytesIO(read_small(archive, info, limit))) as apk:
            return apk.read("resources.arsc")
    except (KeyError, OSError, ValueError, zipfile.BadZipFile) as error:
        fail(f"{entry} has no readable resource table: {error}")


def check_camera_layout_files(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    """The permission directories, the media profiles and the init file are the generator's, and
    nothing else uses the vendor SKU axis."""
    expected = camera_layouts.installed()
    for entry, payload in sorted(expected.items()):
        info = infos.get(entry)
        if info is None or info.is_dir():
            fail(f"{entry} is missing: the camera layouts are generated by tools/camera_layouts.py")
        if read_small(archive, info) != payload:
            fail(f"{entry} differs from what tools/camera_layouts.py generates")
    layout_manifests = {f"manifest_{name}.xml" for name in camera_layouts.by_name()}
    for entry in sorted(infos):
        path = PurePosixPath(entry)
        if path.parts[0] not in TARGET_PARTITIONS:
            continue
        # The vendor SKU axis belongs to the layouts: a directory they do not generate would
        # declare features or system configuration to a device of that name alone.
        if len(path.parts) > 3 and path.parts[3].startswith("sku_") and not infos[entry].is_dir():
            if path.parts[:3] == ("VENDOR", "etc", "permissions") and entry not in expected:
                fail(f"{entry} is not a file of a camera layout: the vendor SKU axis is the layouts' alone")
            if path.parts[:3] == ("VENDOR", "etc", "sysconfig"):
                fail(f"{entry} is a SKU configuration beside the camera layouts")
        if path.name.startswith("media_profiles_cam") and entry not in expected:
            fail(f"{entry} is a media profiles variant that no camera layout selects")
        # A VINTF manifest named after a layout would be read for a device of that layout alone,
        # and the camera provider is declared in the main manifest of every device.
        if "vintf" in path.parts and path.name in layout_manifests:
            fail(f"{entry} is a VINTF manifest of a camera layout: the layouts differ in features, not in HALs")


def camera_init_commands(script: str, service: str) -> dict[str, list[str]]:
    """The init script's actions that start or restart the service: each trigger (the text after `on `) with
    those commands of its action. An action with no such command is not listed."""
    commands: dict[str, list[str]] = {}
    trigger = None
    for raw in script.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("on "):
            trigger = line[len("on "):].strip()
        elif line in (f"start {service}", f"restart {service}") and trigger is not None:
            commands.setdefault(trigger, []).append(line)
    return commands


def check_camera_provider(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    """The lazy provider is installed alone, declared once, restarted by an action of each layout that has
    cameras when cameraserver is running and started by its interface afterwards, and loads the V4L2 HWL."""
    binary = f"VENDOR/bin/hw/{CAMERA_PROVIDER}"
    for entry in (binary, CAMERA_HWL):
        info = infos.get(entry)
        if info is None or info.is_dir():
            fail(f"{entry} is missing: the image has no camera provider")
        if read_small(archive, info, CAMERA_HWL_LIMIT)[:4] != b"\x7fELF":
            fail(f"{entry} is not an ELF file")
    for entry in (f"VENDOR/bin/hw/{CAMERA_EAGER_PROVIDER}", f"VENDOR/etc/init/{CAMERA_EAGER_PROVIDER}.rc"):
        if entry in infos:
            fail(f"{entry} is installed: only the lazy camera provider runs, which exits when cameraserver "
                 "holds no camera")
    library = read_small(archive, infos[CAMERA_HWL], CAMERA_HWL_LIMIT)
    for marker in CAMERA_HWL_MARKERS:
        if marker not in library:
            fail(
                f"{CAMERA_HWL} lacks {marker.decode()!r}: route p0030 is missing from the build, so "
                "the camera HAL would list no camera of the host"
            )
    script = read_small(archive, infos[f"VENDOR/etc/init/{CAMERA_PROVIDER}.rc"]).decode("utf-8", errors="strict")
    lines = [line.strip() for line in script.splitlines() if line.strip() and not line.strip().startswith("#")]
    service = [line for line in lines if line.startswith("service ")]
    if len(service) != 1 or not re.fullmatch(rf"service \S+ /vendor/bin/hw/{re.escape(CAMERA_PROVIDER)}", service[0]):
        fail("the camera provider init script must define one service that runs the lazy provider")
    for required in CAMERA_PROVIDER_RC_OPTIONS:
        if required not in lines:
            fail(f"the camera provider service must carry '{required}'")
    if any(line.startswith("on ") for line in lines):
        fail("the camera provider init script may have no trigger: the layouts' provider init file starts the "
             "service and its interface line restarts it")
    # cameraserver lists a lazy AIDL provider only if it registered when it looked or registers later, and
    # nothing but an init action starts it: not the first time, and not when cameraserver starts again
    # (a crash, a restart of the framework) after the provider has exited. So each layout that has cameras
    # has the action that restarts this service when cameraserver is running and the layout is the SKU
    # (restart, because start leaves a provider that has unregistered and is about to exit, and starts
    # nothing again), and no other action starts or restarts it (one in the early profile action, in a
    # layout without cameras, or on a property other than cameraserver's state would list no camera after
    # a restart).
    service_name = service[0].split()[1]
    if service_name != camera_layouts.PROVIDER_SERVICE:
        fail(f"the camera provider service is {service_name}, not {camera_layouts.PROVIDER_SERVICE}, "
             "which the init file of the camera layouts starts")
    profile_info = infos.get(f"VENDOR/etc/init/{camera_layouts.INIT_FILE}")
    if profile_info is not None and camera_init_commands(
            read_small(archive, profile_info).decode("utf-8", errors="strict"), service_name):
        fail(f"VENDOR/etc/init/{camera_layouts.INIT_FILE} starts the camera provider: init reads a vendor script "
             "in the vendor_init context and would drop the action on cameraserver's status, so the layouts' "
             f"SYSTEM_EXT/etc/init/{camera_layouts.PROVIDER_INIT_FILE} starts it")
    init_info = infos.get(f"SYSTEM_EXT/etc/init/{camera_layouts.PROVIDER_INIT_FILE}")
    if init_info is None or init_info.is_dir():
        fail(f"SYSTEM_EXT/etc/init/{camera_layouts.PROVIDER_INIT_FILE} is missing: nothing starts the camera provider")
    starts = camera_init_commands(read_small(archive, init_info).decode("utf-8", errors="strict"), service_name)
    wanted = {camera_layouts.provider_trigger(layout.name): [f"{camera_layouts.PROVIDER_COMMAND} {service_name}"]
              for layout in camera_layouts.all_layouts() if layout.has_front}
    if starts != wanted:
        missing = sorted(set(wanted) - set(starts))
        extra = sorted(set(starts) - set(wanted))
        repeated = sorted(trigger for trigger, commands in starts.items() if len(commands) != 1)
        fail("the init file of the camera provider must restart the camera provider in one action for each "
             "layout that has cameras, with the conditions cameraserver running and the layout's SKU, and nowhere else: "
             f"missing {missing[:3]}{' ...' if len(missing) > 3 else ''} ({len(missing)}), "
             f"extra {extra[:3]}{' ...' if len(extra) > 3 else ''} ({len(extra)}), repeated {repeated[:3]}")
    declarations = []
    for entry, info in sorted(infos.items()):
        path = PurePosixPath(entry)
        if (
            info.is_dir()
            or path.suffix != ".xml"
            or path.parts[0] not in TARGET_PARTITIONS
            or path.parts[1:3] != ("etc", "vintf")
            or not (path.name.startswith("manifest") or "manifest" in path.parts)
        ):
            continue
        try:
            root = ET.fromstring(read_small(archive, info))
        except ET.ParseError as error:
            fail(f"cannot parse VINTF manifest {entry}: {error}")
        for hal in root.iter("hal"):
            if hal.findtext("name") == CAMERA_HAL_NAME:
                declarations.append((entry, hal.get("format"), declared_hals(hal)[0][2]))
    if len(declarations) != 1:
        fail(f"{CAMERA_HAL_NAME} must be declared exactly once: {[entry for entry, _, _ in declarations]}")
    entry, hal_format, interfaces = declarations[0]
    if not entry.startswith("VENDOR/") or hal_format != "aidl" or interfaces != [CAMERA_HAL_INTERFACE]:
        fail(f"{entry} must declare the AIDL {CAMERA_HAL_INTERFACE} of {CAMERA_HAL_NAME} in the vendor: {interfaces}")
    if not CAMERA_PROVIDER_FILE_CONTEXT.search(read_small(archive, infos["VENDOR/etc/selinux/vendor_file_contexts"])):
        fail("vendor_file_contexts does not give the lazy camera provider its hal_camera_default_exec label")


def check_camera_configuration(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    """One characteristics template, the properties that select it, and no vendor extensions."""
    info = infos.get(CAMERA_TEMPLATE)
    if info is None or info.is_dir():
        fail(f"{CAMERA_TEMPLATE} is missing: the camera HAL has no template for the cameras of the host")
    try:
        template = json.loads(read_small(archive, info))
    except ValueError as error:
        fail(f"cannot parse {CAMERA_TEMPLATE}: {error}")
    if not isinstance(template, dict) or "android.lens.facing" not in template:
        fail(f"{CAMERA_TEMPLATE} is not a camera characteristics template")
    for entry in sorted(infos):
        if entry.startswith(CAMERA_CONFIG_DIRECTORY + "emu_camera_") and entry != CAMERA_TEMPLATE:
            fail(f"{entry} is a camera of the stock emulated HAL: the host's cameras are the only ones")
        if PurePosixPath(entry).name.endswith("camera_extensions.xml") or PurePosixPath(entry).name.startswith(
            "androidx.camera.extensions.impl"
        ):
            fail(f"{entry} is a vendor camera extension: the image has none")
    for entry, key, value in (CAMERA_LAZY_HAL_PROPERTY, CAMERA_CONFIG_PROPERTY):
        properties = parse_properties(read_small(archive, infos[entry]).decode("utf-8", errors="strict"))
        if properties.get(key) != value:
            fail(f"{entry} lacks {key}={value}: {properties.get(key)!r}")
    for entry, info in infos.items():
        if PurePosixPath(entry).name in {"build.prop", "default.prop"} and not info.is_dir():
            if CAMERA_EXTENSIONS_PROPERTY in parse_properties(read_small(archive, info).decode("utf-8", errors="strict")):
                fail(f"{entry} sets {CAMERA_EXTENSIONS_PROPERTY}: the image has no vendor camera extensions")


def check_camera_app(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    """The camera application lists each lens the host has and offers no free zoom, and the sensor
    privacy service reads the camera switch."""
    app_table = camera_resources(archive, infos, CAMERA_APP, CAMERA_APP_LIMIT)
    try:
        defines_overlayable = resource_table_defines_overlayable(app_table)
        app_defaults = {"bool": resource_default_names(app_table, "bool"),
                        "array": resource_default_names(app_table, "array")}
    except (ValueError, struct.error, IndexError) as error:
        fail(f"{CAMERA_APP} has no readable resource table: {error}")
    if defines_overlayable:
        fail(
            f"{CAMERA_APP} declares an overlayable section: Android would apply a product overlay to the "
            "resources it names under its policies alone, and this check cannot tell which"
        )
    # An overlay replaces resources its target defines; one for a name the application lost (an uprev
    # renamed it) applies to nothing, and the overlay's own values then look right.
    for name, kind in ((CAMERA_AUX_CAMERAS, "bool"), (CAMERA_IGNORED_AUX_CAMERAS, "array"), (CAMERA_ZOOM, "bool")):
        if name not in app_defaults[kind]:
            fail(
                f"{CAMERA_APP} does not define the {kind} resource {name} in its default configuration: "
                "the overlay would apply to nothing"
            )
    info = infos.get(CAMERA_APP_OVERLAY)
    if info is None or info.is_dir():
        fail(
            f"{CAMERA_APP_OVERLAY} is missing: the application's own resources keep the defaults that "
            "hide each lens of the host as a camera of its own and leave free zoom on"
        )
    try:
        with zipfile.ZipFile(io.BytesIO(read_small(archive, info, CAMERA_APP_LIMIT))) as overlay_apk:
            manifest = overlay_apk.read("AndroidManifest.xml")
            table = overlay_apk.read("resources.arsc")
        overlay = manifest_element_attributes(manifest, "overlay")
        booleans = resource_booleans(table)
        arrays = resource_arrays(table)
        overlay_defaults = {"bool": resource_default_names(table, "bool"),
                            "array": resource_default_names(table, "array")}
    except (KeyError, OSError, ValueError, struct.error, IndexError, zipfile.BadZipFile) as error:
        fail(f"{CAMERA_APP_OVERLAY} has no readable manifest and resource table: {error}")
    if overlay is None or overlay.get("targetPackage") != CAMERA_APP_PACKAGE or overlay.get("isStatic") is not True:
        fail(
            f"{CAMERA_APP_OVERLAY} must be a static overlay of {CAMERA_APP_PACKAGE}: Android would not "
            f"apply it otherwise: {overlay}"
        )
    if booleans.get(CAMERA_AUX_CAMERAS) != [True]:
        fail(
            f"{CAMERA_APP_OVERLAY} must set {CAMERA_AUX_CAMERAS} to true: {booleans.get(CAMERA_AUX_CAMERAS)}; "
            "each lens of the host is its own camera device"
        )
    if arrays.get(CAMERA_IGNORED_AUX_CAMERAS) != [CAMERA_IGNORED_AUX_IDS]:
        fail(
            f"{CAMERA_APP_OVERLAY} must list the camera IDs {CAMERA_IGNORED_AUX_IDS} in "
            f"{CAMERA_IGNORED_AUX_CAMERAS}: {arrays.get(CAMERA_IGNORED_AUX_CAMERAS)}; "
            "a host that still publishes the wide lens as camera 2 beside camera 0 would show it twice"
        )
    if booleans.get(CAMERA_ZOOM) != [False]:
        fail(
            f"{CAMERA_APP_OVERLAY} must set {CAMERA_ZOOM} to false: {booleans.get(CAMERA_ZOOM)}; "
            "the lens buttons are the camera's only zoom"
        )
    # The one definition of each name is the sole one, and it carries no qualifier: a value under a
    # resource configuration applies only on a device that matches it.
    for name, kind in ((CAMERA_AUX_CAMERAS, "bool"), (CAMERA_IGNORED_AUX_CAMERAS, "array"), (CAMERA_ZOOM, "bool")):
        if name not in overlay_defaults[kind]:
            fail(f"{CAMERA_APP_OVERLAY} defines {name} only under a resource configuration: "
                 "it must be in the default configuration")
    try:
        toggle_table = camera_resources(archive, infos, FRAMEWORK_OVERLAY, FRAMEWORK_OVERLAY_LIMIT)
        toggle = resource_booleans(toggle_table)
        toggle_defaults = resource_default_names(toggle_table, "bool")
    except (ValueError, struct.error, IndexError) as error:
        fail(f"{FRAMEWORK_OVERLAY} has no readable resource table: {error}")
    if toggle.get(CAMERA_HARDWARE_TOGGLE) != [True]:
        fail(
            f"{FRAMEWORK_OVERLAY} must set {CAMERA_HARDWARE_TOGGLE} to true: {toggle.get(CAMERA_HARDWARE_TOGGLE)}; "
            "a session started with Camera off could not show the camera blocked"
        )
    if CAMERA_HARDWARE_TOGGLE not in toggle_defaults:
        fail(f"{FRAMEWORK_OVERLAY} defines {CAMERA_HARDWARE_TOGGLE} only under a resource configuration: "
             "it must be in the default configuration")


# A session started with Camera off holds the camera's hardware toggle. The sensor privacy service
# would start the activity that config_sensorUseStartedActivity_hwToggle names (or post a notification)
# to offer an Unblock that writes the software toggle, which a hardware toggle outranks; the
# SensorPrivacyService topic of route p0007 withholds both for the camera (SERVICES_JAR_MARKERS) and
# keeps the microphone's reminder. A product string for either activity would also name the camera's
# and the microphone's, so the overlay defines neither, in any configuration and with any value (a
# reference included): a name found among the overlay's string entries is the failure. An overlay's
# resource map (android:resourcesMap) can point those two names at a string of another name, so the
# generated overlay, which never has one, must not declare one.
SENSOR_USE_ACTIVITY_STRINGS = (
    "config_sensorUseStartedActivity",
    "config_sensorUseStartedActivity_hwToggle",
)
RESOURCES_MAP_ATTRIBUTE = "resourcesMap"


def check_camera_reminder(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    """The framework overlay leaves the sensor privacy service's reminder activities at the platform's
    defaults, by name and by resource map."""
    info = infos.get(FRAMEWORK_OVERLAY)
    if info is None or info.is_dir():
        fail(f"{FRAMEWORK_OVERLAY} is missing")
    try:
        with zipfile.ZipFile(io.BytesIO(read_small(archive, info, FRAMEWORK_OVERLAY_LIMIT))) as overlay:
            table = overlay.read("resources.arsc")
            manifest = overlay.read("AndroidManifest.xml")
        entries = [(name, default) for name, _, _, default
                   in resource_entries(table, "string", configurations=True)]
        declaration = manifest_element_attributes(manifest, "overlay")
    except (KeyError, OSError, ValueError, struct.error, IndexError, zipfile.BadZipFile) as error:
        fail(f"{FRAMEWORK_OVERLAY} has no readable resource table or manifest: {error}")
    if declaration is not None and RESOURCES_MAP_ATTRIBUTE in declaration:
        fail(
            f"{FRAMEWORK_OVERLAY} declares a resource map (android:{RESOURCES_MAP_ATTRIBUTE}): it can point "
            f"{' and '.join(SENSOR_USE_ACTIVITY_STRINGS)} at a string of another name, and the generated "
            "overlay has none"
        )
    for name, default in entries:
        if name in SENSOR_USE_ACTIVITY_STRINGS:
            fail(
                f"{FRAMEWORK_OVERLAY} defines {name} "
                f"{'in the default configuration' if default else 'under a resource configuration'}: "
                "the reminder of a camera under the hardware toggle is withheld by the framework, "
                "and the platform's activity is the microphone's"
            )


def check_camera(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    # The provider's contract first: its message names what a changed init file broke, where the layout files'
    # byte comparison only says that they differ.
    check_camera_provider(archive, infos)
    check_camera_layout_files(archive, infos)
    check_camera_configuration(archive, infos)
    check_camera_app(archive, infos)
    check_camera_reminder(archive, infos)



# The gamepad relay (overlay/gamepad) turns the host's game controllers into HID devices through
# /dev/uhid, and every device has a host, so every device runs it: a daemon that init starts with
# its class at boot, in a domain of its own (not a HAL, so no VINTF fragment) that may use the uhid
# node and vsock and nothing else the product grants.
GAMEPAD_SERVICE = ("kernarium_gamepad", "/vendor/bin/kernarium_gamepad")
GAMEPAD_RC = "VENDOR/etc/init/kernarium_gamepad.rc"
GAMEPAD_RC_OPTIONS = ("class main", "user nobody", "group uhid")
GAMEPAD_TRANSITION = re.compile(
    rb"^\(typetransition init(?:_[0-9]+)? kernarium_gamepad_exec process kernarium_gamepad\)$",
    re.MULTILINE,
)
GAMEPAD_UHID_RULE = re.compile(
    rb"^\(allow kernarium_gamepad uhid_device(?:_[0-9]+)? \(chr_file \(([^)]*)\)\)\)$", re.MULTILINE
)
GAMEPAD_UHID_PERMISSIONS = frozenset({"open", "read", "write"})
GAMEPAD_VSOCK_RULE = re.compile(
    rb"^\(allow kernarium_gamepad self \(vsock_socket \(([^)]*)\)\)\)$", re.MULTILINE
)
GAMEPAD_VSOCK_PERMISSIONS = frozenset({"create", "bind", "listen", "accept", "read", "write", "shutdown"})


def check_gamepad_relay(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    """The init script runs the relay at boot as the user and group that may open /dev/uhid."""
    name, executable = GAMEPAD_SERVICE
    script = read_small(archive, infos[GAMEPAD_RC]).decode("utf-8", errors="strict")
    lines = [line.strip() for line in script.splitlines() if line.strip() and not line.strip().startswith("#")]
    service = [line for line in lines if line.startswith("service ")]
    if service != [f"service {name} {executable}"]:
        fail(f"the gamepad init script must define the one service {name} that runs {executable}: {service}")
    for required in GAMEPAD_RC_OPTIONS:
        if required not in lines:
            fail(f"the gamepad service must carry '{required}'")
    if "disabled" in lines or "oneshot" in lines:
        fail("the gamepad service starts at boot on every device: it is neither disabled nor oneshot")
    if any(line.startswith("on ") for line in lines):
        fail("the gamepad init script may have no trigger: the service starts with its class")
    if any(line.startswith(("seclabel ", "capabilities ")) for line in lines):
        fail("the gamepad service takes its domain from its file label and holds no capability")


def check_gamepad_policy(vendor_policy: bytes, vendor_file_contexts: bytes) -> None:
    """The relay runs in its own domain, with the uhid node and vsock."""
    if file_context_label(vendor_file_contexts, GAMEPAD_SERVICE[1]) != "u:object_r:kernarium_gamepad_exec:s0":
        fail("vendor file contexts do not label /vendor/bin/kernarium_gamepad")
    if not GAMEPAD_TRANSITION.search(vendor_policy):
        fail("vendor policy does not move init into the gamepad relay domain")
    if not any(
        GAMEPAD_UHID_PERMISSIONS <= set(match.group(1).decode().split())
        for match in GAMEPAD_UHID_RULE.finditer(vendor_policy)
    ):
        fail("vendor policy does not let the gamepad relay open, read and write the uhid device")
    if not any(
        GAMEPAD_VSOCK_PERMISSIONS <= set(match.group(1).decode().split())
        for match in GAMEPAD_VSOCK_RULE.finditer(vendor_policy)
    ):
        fail("vendor policy does not give the gamepad relay vsock sockets to listen on")
    if "kernarium_gamepad" not in cil_attribute_members(vendor_policy, "unconstrained_vsock_violators"):
        fail("vendor policy does not classify the gamepad relay as an unconstrained vsock user")


def check_default_display_config(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    if DISPLAY_CONFIG_SHADOW in infos:
        fail(
            f"{DISPLAY_CONFIG_SHADOW} exists: the framework reads the first default display "
            f"configuration it finds, so it would hide {DISPLAY_CONFIG_DEFAULT}"
        )
    try:
        root = ET.fromstring(read_small(archive, infos[DISPLAY_CONFIG_DEFAULT]))
    except ET.ParseError as error:
        fail(f"cannot parse {DISPLAY_CONFIG_DEFAULT}: {error}")
    shape = [child.tag for child in root]
    throttling = root.find("thermalThrottling")
    if root.tag != "displayConfiguration" or shape != ["thermalThrottling"] or throttling is None:
        fail(f"{DISPLAY_CONFIG_DEFAULT} must hold a displayConfiguration with thermalThrottling and nothing else")
    maps = list(throttling)
    if len(maps) != 1 or maps[0].tag != "refreshRateThrottlingMap" or maps[0].get("id") not in (None, "default"):
        fail(f"{DISPLAY_CONFIG_DEFAULT} must hold the one default refreshRateThrottlingMap")
    points = list(maps[0])
    if len(points) != len(DISPLAY_CONFIG_POINTS) or any(point.tag != "refreshRateThrottlingPoint" for point in points):
        fail(f"{DISPLAY_CONFIG_DEFAULT} must hold the {len(DISPLAY_CONFIG_POINTS)} refreshRateThrottlingPoints")
    found_points = []
    for point in points:
        refresh_rates = point.find("refreshRateRange")
        if [child.tag for child in point] != ["thermalStatus", "refreshRateRange"]:
            fail(f"{DISPLAY_CONFIG_DEFAULT} holds a point that is not a thermalStatus and a refreshRateRange")
        # Exactly one minimum and one maximum, in that order, so that no reader of the range can take a
        # value other than the one checked below.
        if refresh_rates is None or [child.tag for child in refresh_rates] != ["minimum", "maximum"]:
            fail(f"{DISPLAY_CONFIG_DEFAULT} holds a refreshRateRange that is not one minimum and one maximum")
        found = (
            (point.findtext("thermalStatus") or "").strip(),
            (refresh_rates.findtext("minimum") or "").strip(),
            (refresh_rates.findtext("maximum") or "").strip(),
        )
        found_points.append(found)
    if tuple(found_points) != DISPLAY_CONFIG_POINTS:
        fail(
            f"{DISPLAY_CONFIG_DEFAULT} must hold the moderate point with the refresh rate range "
            f"0 to 60 Hz and the severe point with 0 to 30 Hz: {found_points}"
        )


def check_hardware_features(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    declared: set[str] = set()
    deqp_levels: list[str | None] = []
    for name, info in infos.items():
        path = PurePosixPath(name)
        if (
            info.is_dir()
            or path.suffix != ".xml"
            or "permissions" not in path.parts
        ):
            continue
        try:
            root = ET.fromstring(read_small(archive, info))
        except ET.ParseError as error:
            fail(f"cannot parse hardware permission XML {name}: {error}")
        for element in root.iter():
            if element.tag.rsplit("}", 1)[-1] != "feature":
                continue
            feature = element.attrib.get("name")
            if feature in FORBIDDEN_FEATURE_NAMES:
                fail(f"absent hardware feature is advertised in {name}: {feature}")
            if (
                feature
                and feature.startswith(CAMERA_FEATURE_PREFIX)
                and name not in camera_layout_files()
            ):
                fail(
                    f"a camera feature is declared outside the camera layouts in {name}: {feature}; "
                    "the host's cameras decide it, through the permission directory of the layout"
                )
            if feature == COMPASS_FEATURE and name != COMPASS_FEATURE_FILE:
                fail(
                    f"the compass feature is declared outside {COMPASS_PERMISSION_DIRECTORY} in {name}: "
                    "only a host with a magnetometer has a compass, and SystemConfig reads that directory "
                    "for such a host alone"
                )
            if feature == "android.software.vulkan.deqp.level":
                fail(f"a Vulkan dEQP level is declared in {name}: the Vulkan lists are not qualified")
            if feature == OPENGLES_DEQP_LEVEL_FEATURE:
                deqp_levels.append(element.attrib.get("version"))
            if feature:
                declared.add(feature)
    for feature in sorted(REQUIRED_FEATURE_NAMES - declared):
        fail(f"required feature is not declared: {feature}")
    if deqp_levels != [str(OPENGLES_DEQP_LEVEL)]:
        fail(
            f"{OPENGLES_DEQP_LEVEL_FEATURE} must be declared once at the qualified "
            f"level {OPENGLES_DEQP_LEVEL}: {deqp_levels}"
        )


def app_preopt_paths(apk: str, isa: str = "arm64") -> tuple[str, str]:
    apk_path = PurePosixPath(apk)
    artifact_base = apk_path.parent / "oat" / isa / apk_path.stem
    return f"{artifact_base}.odex", f"{artifact_base}.vdex"


def check_app_dexpreopt(seen: set[str]) -> None:
    for apk in sorted(REQUIRED_PREOPT_APKS):
        if apk not in seen:
            fail(f"required preoptimized application is missing: {apk}")
        for artifact in app_preopt_paths(apk):
            if artifact not in seen:
                fail(f"required application dexpreopt artifact is missing: {artifact}")
    # An application that carries libraries for both ABIs runs under either
    # zygote, so ART Service would compile its secondary ISA on first boot.
    for apk in sorted(REQUIRED_ARM_PREOPT_APKS):
        if apk not in seen:
            fail(f"required multi-ABI application is missing: {apk}")
        for artifact in app_preopt_paths(apk, "arm"):
            if artifact not in seen:
                fail(f"required 32-bit application dexpreopt artifact is missing: {artifact}")

    for canonical in sorted(seen):
        path = PurePosixPath(canonical)
        if path.suffix not in {".odex", ".vdex"}:
            continue
        if len(path.parts) < 6 or path.parts[1] not in {"app", "priv-app"}:
            continue
        if path.parts[-3] != "oat":
            continue
        if path.parts[-2] not in APP_ISAS:
            fail(f"application dexpreopt artifact is not for an ISA of this product: {path}")
        companion_suffix = ".vdex" if path.suffix == ".odex" else ".odex"
        companion = str(path.with_suffix(companion_suffix))
        if companion not in seen:
            fail(f"application dexpreopt artifact lacks its pair: {path}")
        if path.parts[-2] == "arm":
            # The secondary ISA only ever adds to the primary one.
            primary = path.parent.parent / "arm64" / path.name
            if str(primary) not in seen:
                fail(f"32-bit application dexpreopt artifact has no ARM64 one: {path}")


# The virtual Wi-Fi needs these built in: kernarium_net_setup stacks a macvlan on eth0 and a
# virt_wifi device on that, and a kernel without them leaves apps no network at all.
WIFI_KERNEL_OPTIONS = ("CONFIG_MACVLAN=y", "CONFIG_CFG80211=y", "CONFIG_VIRT_WIFI=y")
# First-stage init mounts the five read-only partitions as compressed EROFS before any module
# could load, so the kernel needs the file system and the product's decompressor built in.
EROFS_KERNEL_OPTIONS = (
    "CONFIG_EROFS_FS=y",
    "CONFIG_EROFS_FS_ZIP=y",
    "CONFIG_EROFS_FS_ZIP_ZSTD=y",
)


def check_kernel_policy(
    archive: zipfile.ZipFile,
    infos: dict[str, zipfile.ZipInfo],
    kernel_config: Path,
    kernel_release_file: Path,
    build_variant: str,
) -> None:
    packaged_config = read_small(archive, infos["META/kernel_configs.txt"])
    if packaged_config != kernel_config.read_bytes():
        fail("packaged kernel configuration differs from the UML build")
    packaged_options = {
        line.strip() for line in packaged_config.decode("utf-8", errors="replace").splitlines()
    }
    for option in WIFI_KERNEL_OPTIONS:
        if option not in packaged_options:
            fail(
                f"packaged kernel configuration lacks {option}: the virtual Wi-Fi needs it, "
                "and without it apps have no network"
            )
    for option in EROFS_KERNEL_OPTIONS:
        if option not in packaged_options:
            fail(
                f"packaged kernel configuration lacks {option}: the read-only partitions "
                "are compressed EROFS, and first-stage init cannot mount them without it"
            )

    packaged_release = read_small(
        archive, infos["META/kernel_version.txt"]
    ).decode("utf-8", errors="strict").strip()
    kernel_release = kernel_release_file.read_text(
        encoding="utf-8", errors="strict"
    ).strip()
    if not kernel_release or packaged_release != kernel_release:
        fail("packaged kernel release differs from the UML build")

    vendor_policy = read_small(
        archive, infos["VENDOR/etc/selinux/vendor_sepolicy.cil"]
    )
    if b"(policycap memfd_class)" not in vendor_policy:
        fail("Android 17 vendor policy does not enable policycap memfd_class")
    # Applications must read the vendor patch level that LineageOS lets
    # system_server report to them (overlay/sepolicy/vendor/appdomain.te).
    if not VENDOR_SPL_APP_READ.search(vendor_policy):
        fail("vendor policy does not let applications read vendor_security_patch_level_prop")
    if not AUDIO_HAL_SELF_BINDER.search(vendor_policy):
        fail("vendor policy does not let the audio HAL processes call each other's binder")
    if not WIFI_HAL_DEBUG_PROPERTY_READ.search(vendor_policy):
        fail("vendor policy does not let the Wi-Fi HAL read persist_vendor_debug_wifi_prop")
    if not CAMERA_HAL_COMPOSER_FENCE.search(vendor_policy):
        fail("vendor policy does not let the camera HAL use the display composer's file descriptors")
    check_clearkey_memfd_policy(vendor_policy)
    check_userdata_grow_policy(vendor_policy)
    check_network_setup_policy(
        vendor_policy, read_small(archive, infos["VENDOR/etc/selinux/vendor_file_contexts"])
    )
    check_gamepad_policy(vendor_policy, read_small(archive, infos["VENDOR/etc/selinux/vendor_file_contexts"]))
    platform_policy = read_small(
        archive, infos["SYSTEM/etc/selinux/plat_sepolicy.cil"], PLATFORM_POLICY_LIMIT
    )
    system_ext_policy = read_small(archive, infos["SYSTEM_EXT/etc/selinux/system_ext_sepolicy.cil"])
    check_wifi_network_policy(
        system_ext_policy,
        read_small(archive, infos["SYSTEM_EXT/etc/selinux/system_ext_file_contexts"]),
        read_small(archive, infos["SYSTEM_EXT/etc/selinux/system_ext_property_contexts"]).decode(
            "utf-8", errors="strict"
        ),
    )
    check_reclaim_boot_apps_policy(
        system_ext_policy,
        read_small(archive, infos["SYSTEM_EXT/etc/selinux/system_ext_file_contexts"]),
    )
    check_time_zone_policy(
        system_ext_policy,
        read_small(archive, infos["SYSTEM_EXT/etc/selinux/system_ext_file_contexts"]),
        read_small(archive, infos["SYSTEM_EXT/etc/selinux/system_ext_property_contexts"]).decode(
            "utf-8", errors="strict"
        ),
    )
    check_keyboard_default_policy(
        system_ext_policy,
        read_small(archive, infos["SYSTEM_EXT/etc/selinux/system_ext_file_contexts"]),
        read_small(archive, infos["SYSTEM_EXT/etc/selinux/system_ext_property_contexts"]).decode(
            "utf-8", errors="strict"
        ),
    )
    check_property_writers(
        platform_policy,
        vendor_policy,
        system_ext_policy,
        read_small(archive, infos["SYSTEM_EXT/etc/selinux/system_ext_property_contexts"]).decode(
            "utf-8", errors="strict"
        ),
    )
    check_policy_relaxations((platform_policy, vendor_policy, system_ext_policy), build_variant)
    check_mediaserver_property_audit((platform_policy, vendor_policy, system_ext_policy))
    check_shell_density_property_audit((platform_policy, vendor_policy, system_ext_policy))
    contexts = [
        line
        for name in PROPERTY_CONTEXT_FILES
        if name in infos
        for line in read_small(archive, infos[name], PLATFORM_POLICY_LIMIT)
        .decode("utf-8", errors="strict")
        .splitlines()
    ]
    for entry in sorted(infos):
        if (
            PurePosixPath(entry).parts[0] in ("VENDOR", "ODM")
            and entry.endswith(".rc")
            and not infos[entry].is_dir()
        ):
            check_vendor_init_triggers(
                init_actions(read_small(archive, infos[entry]).decode("utf-8", errors="strict")),
                contexts,
                (platform_policy, vendor_policy, system_ext_policy),
                f"{entry}: ",
            )
    check_adbd_launch_policy(
        contexts,
        (platform_policy, vendor_policy, system_ext_policy)
        + tuple(read_small(archive, infos[name]) for name in APP_POLICY_CIL if name in infos),
    )
    check_oldapp_switch_policy(
        contexts,
        (platform_policy, vendor_policy, system_ext_policy)
        + tuple(read_small(archive, infos[name]) for name in APP_POLICY_CIL if name in infos),
        system_ext_policy,
    )
    check_lmkd_gpu_policy(contexts, (platform_policy, vendor_policy, system_ext_policy), vendor_policy)
    check_venus_option_policy(contexts, (platform_policy, vendor_policy, system_ext_policy))
    check_audio_ring_policy(contexts, (platform_policy, vendor_policy, system_ext_policy))
    check_sms_policy(
        contexts,
        (platform_policy, vendor_policy, system_ext_policy)
        + tuple(read_small(archive, infos[name]) for name in APP_POLICY_CIL if name in infos),
        "\n".join(
            read_small(archive, infos[name], PLATFORM_POLICY_LIMIT).decode("utf-8", errors="strict")
            for name in SEAPP_CONTEXT_FILES
            if name in infos
        ),
        read_small(archive, infos["SYSTEM_EXT/etc/selinux/system_ext_file_contexts"]),
        build_variant,
    )


def media_codec_limit(codec: ET.Element, name: str) -> ET.Element:
    limits = [limit for limit in codec.findall("Limit") if limit.get("name") == name]
    if len(limits) != 1:
        fail(f"media_codecs_c2.xml: {codec.get('name')} needs exactly one {name} limit")
    return limits[0]


def check_media_codecs_c2(payload: bytes) -> None:
    """The software HEVC and VP9 decoders must be allowed 3840x2160."""
    try:
        root = ET.fromstring(payload)
    except ET.ParseError as error:
        fail(f"media_codecs_c2.xml is not well-formed: {error}")
    decoders = {
        codec.get("name"): codec for codec in root.findall("./Decoders/MediaCodec")
    }
    for name in ("c2.android.hevc.decoder", "c2.android.vp9.decoder"):
        codec = decoders.get(name)
        if codec is None or codec.get("update") != "true":
            fail(f"media_codecs_c2.xml does not update {name}")

    def upper(limit: ET.Element) -> int:
        return int(limit.get("range", "0-0").split("-")[1])

    hevc_rate = upper(
        media_codec_limit(decoders["c2.android.hevc.decoder"], "blocks-per-second")
    )
    if hevc_rate < MEDIA_C2_HEVC_BLOCKS_PER_SECOND:
        fail(
            f"media_codecs_c2.xml allows the HEVC decoder only {hevc_rate} blocks per second; "
            f"3840x2160 at 30 frames per second needs {MEDIA_C2_HEVC_BLOCKS_PER_SECOND}"
        )
    vp9 = decoders["c2.android.vp9.decoder"]
    width, height = (int(side) for side in media_codec_limit(vp9, "size").get("max", "0x0").split("x"))
    if width < MEDIA_C2_SIZE[0] or height < MEDIA_C2_SIZE[1]:
        fail(f"media_codecs_c2.xml limits the VP9 decoder to {width}x{height}, below 3840x2160")
    vp9_blocks = upper(media_codec_limit(vp9, "block-count"))
    if vp9_blocks < MEDIA_C2_VP9_BLOCK_COUNT:
        fail(
            f"media_codecs_c2.xml allows the VP9 decoder {vp9_blocks} blocks, below "
            f"{MEDIA_C2_VP9_BLOCK_COUNT} for 3840x2160"
        )


@functools.lru_cache(maxsize=64)
def _cil_attribute_members(policy: bytes, attribute: str) -> frozenset[str]:
    pattern = re.compile(
        rb"^\(typeattributeset " + re.escape(attribute.encode()) + rb" \(([^)]*)\)\)$",
        re.MULTILINE,
    )
    return frozenset(
        CIL_PLATFORM_VERSION_SUFFIX.sub("", name.decode())
        for match in pattern.finditer(policy)
        for name in match.group(1).split()
    )


def cil_attribute_members(policy: bytes, attribute: str) -> set[str]:
    """Types of a CIL attribute, platform version suffixes removed. A policy is scanned once for an
    attribute; each call returns a set of its own."""
    return set(_cil_attribute_members(policy, attribute))


def check_property_writers(
    platform_policy: bytes,
    vendor_policy: bytes,
    system_ext_policy: bytes,
    system_ext_contexts: str,
) -> None:
    """A core domain may set no vendor-owned property, and adbd reports its host.

    The first rule repeats the platform neverallow of private/property.te so a
    product rule that breaks it fails here and not only in the policy build.
    """
    core_domains = cil_attribute_members(platform_policy, "coredomain") - {"init"}
    vendor_properties = cil_attribute_members(
        vendor_policy, "vendor_property_type"
    ) | cil_attribute_members(system_ext_policy, "vendor_property_type")
    for label, policy in (("vendor", vendor_policy), ("system_ext", system_ext_policy)):
        for match in CIL_PROPERTY_SET_RULE.finditer(policy):
            source = CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(1).decode())
            target = CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(2).decode())
            if b"set" not in match.group(3).split():
                continue
            if source in core_domains and target in vendor_properties:
                fail(
                    f"{label} policy lets the core domain {source} set the vendor "
                    f"property type {target}; a core domain may set only a "
                    "system-owned property"
                )
    system_properties = cil_attribute_members(system_ext_policy, "system_property_type")
    if ADB_HOST_PROPERTY_TYPE not in system_properties:
        fail(f"system_ext policy does not define the system-owned {ADB_HOST_PROPERTY_TYPE}")
    allowed = False
    for match in CIL_PROPERTY_SET_RULE.finditer(system_ext_policy):
        if (
            match.group(1) == b"adbd"
            and match.group(2).decode() == ADB_HOST_PROPERTY_TYPE
            and b"set" in match.group(3).split()
        ):
            allowed = True
    if not allowed:
        fail(f"system_ext policy does not let adbd set {ADB_HOST_PROPERTY}")
    if not any(
        line.split()[:2] == [ADB_HOST_PROPERTY, f"u:object_r:{ADB_HOST_PROPERTY_TYPE}:s0"]
        for line in system_ext_contexts.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ):
        fail(f"system_ext property contexts do not label {ADB_HOST_PROPERTY}")


def check_clearkey_memfd_policy(vendor_policy: bytes) -> None:
    """The ClearKey HAL may use the memfd files of application domains, not of isolated apps."""
    for match in CLEARKEY_MEMFD_RULE.finditer(vendor_policy):
        permissions = set(match.group(2).decode().split())
        if not CLEARKEY_MEMFD_PERMISSIONS <= permissions:
            continue
        target = match.group(1).decode()
        expression = re.search(
            rb"^\(typeattributeset " + re.escape(target.encode())
            + rb" \(and \(appdomain(?:_[0-9]+)? ?\) \(not \(([^)]*)\) ?\)\)\)$",
            vendor_policy,
            re.MULTILINE,
        )
        if expression is not None and "isolated_app_all" in {
            CIL_PLATFORM_VERSION_SUFFIX.sub("", name)
            for name in expression.group(1).decode().split()
        }:
            return
    fail(
        "vendor policy does not let the ClearKey DRM HAL read, write and map the memfd files "
        "of application domains other than isolated apps"
    )


def file_context_label(contexts: bytes, path: str) -> str | None:
    """The context of the last file_contexts line whose expression matches |path|."""
    label = None
    for line in contexts.decode("utf-8", errors="strict").splitlines():
        fields = line.split()
        if len(fields) < 2 or fields[0].startswith("#"):
            continue
        try:
            matched = re.fullmatch(fields[0], path)
        except re.error:
            continue
        if matched:
            label = fields[-1]
    return label


def check_network_setup_policy(vendor_policy: bytes, vendor_file_contexts: bytes) -> None:
    """The network setup tool runs in its own domain, not in init's."""
    if file_context_label(vendor_file_contexts, NET_SETUP_SERVICE[1]) != (
        "u:object_r:kernarium_net_setup_exec:s0"
    ):
        fail("vendor file contexts do not label /vendor/bin/kernarium_net_setup")
    if not NET_SETUP_TRANSITION.search(vendor_policy):
        fail("vendor policy does not move init into the network setup domain")
    if not cil_allows(
        (vendor_policy,), NET_SETUP_DOMAIN, NET_SETUP_SYSCTL_LABEL, "file", {"open", "write"}
    ):
        fail(
            f"vendor policy does not let {NET_SETUP_DOMAIN} open and write {NET_SETUP_SYSCTL_LABEL} "
            "files: it could not turn IPv6 off on eth0 and the macvlan, and would exit before "
            "it made wlan0"
        )


def check_wifi_network(rc_payload: str, script: bytes) -> None:
    """The script that saves VirtWifi runs once, after boot, and leaves the Wi-Fi switch alone."""
    name, executable = WIFI_NETWORK_SERVICE
    service = init_services(rc_payload).get(name)
    if service is None or service[0] != executable:
        fail(f"system_ext init policy does not declare the {name} service at {executable}")
    for option in ("oneshot", "disabled"):
        if option not in service[1]:
            fail(f"system_ext init policy does not declare {name} as {option}")
    actions = init_actions(rc_payload)
    for trigger, commands in actions.items():
        if f"start {name}" in commands and trigger != WIFI_NETWORK_TRIGGER:
            fail(f"system_ext init policy starts {name} under '{trigger}'")
    if f"start {name}" not in actions.get(WIFI_NETWORK_TRIGGER, []):
        fail(f"system_ext init policy does not start {name} when boot completes")
    code = "\n".join(
        line
        for line in script.decode("utf-8", errors="strict").splitlines()
        if not line.lstrip().startswith("#")
    )
    for token in WIFI_NETWORK_SCRIPT_REQUIRED:
        if token not in code:
            fail(f"{executable} does not contain '{token}'")
    for token in WIFI_NETWORK_SCRIPT_FORBIDDEN:
        if token in code:
            fail(f"{executable} contains '{token}': the user's Wi-Fi switch is not the product's")


def property_type_accepts(type_tokens: list[str], value: str) -> bool:
    """Whether a property_contexts type (`exact enum 1 2`, `exact bool`...) lets init store value."""
    tokens = type_tokens[1:] if type_tokens[:1] in (["exact"], ["prefix"]) else type_tokens
    if not tokens:
        return True
    kind, arguments = tokens[0], tokens[1:]
    if kind == "string":
        return True
    if kind == "bool":
        return value in ("true", "false", "1", "0")
    if kind == "enum":
        return value in arguments
    if kind in ("int", "double"):
        return re.fullmatch(r"-?[0-9]+(\.[0-9]+)?", value) is not None
    if kind == "uint":
        return re.fullmatch(r"[0-9]+", value) is not None
    return False


def check_wifi_network_policy(
    system_ext_policy: bytes, system_ext_file_contexts: bytes, system_ext_property_contexts: str
) -> None:
    """The script runs in its own core domain and may set only its own marker."""
    if file_context_label(system_ext_file_contexts, WIFI_NETWORK_SERVICE[1]) != (
        "u:object_r:kernarium_wifi_network_exec:s0"
    ):
        fail(f"system_ext file contexts do not label {WIFI_NETWORK_SERVICE[1]}")
    if not WIFI_NETWORK_TRANSITION.search(system_ext_policy):
        fail("system_ext policy does not move init into the Wi-Fi network domain")
    if WIFI_NETWORK_MARKER_TYPE not in cil_attribute_members(
        system_ext_policy, "system_property_type"
    ):
        fail(f"system_ext policy does not define the system-owned {WIFI_NETWORK_MARKER_TYPE}")
    if not any(
        match.group(1) == b"kernarium_wifi_network"
        and match.group(2).decode() == WIFI_NETWORK_MARKER_TYPE
        and b"set" in match.group(3).split()
        for match in CIL_PROPERTY_SET_RULE.finditer(system_ext_policy)
    ):
        fail(f"system_ext policy does not let the Wi-Fi network domain set {WIFI_NETWORK_MARKER}")
    if not any(
        CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(1).decode()) == "kernarium_wifi_network"
        and CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(2).decode()) == "system_server"
        and match.group(3) == b"binder"
        and b"transfer" in match.group(4).split()
        for match in CIL_ALLOW_RULE.finditer(system_ext_policy)
    ):
        fail(
            "system_ext policy does not let the Wi-Fi network domain transfer binders to "
            "system_server: cmd hands the Wi-Fi service the receiver of its result, and "
            "without the rule the network is never saved"
        )
    if not any(
        CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(1).decode()) == "system_server"
        and CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(2).decode()) == "kernarium_wifi_network"
        and match.group(3) == b"binder"
        and b"call" in match.group(4).split()
        for match in CIL_ALLOW_RULE.finditer(system_ext_policy)
    ):
        fail(
            "system_ext policy does not let system_server call the Wi-Fi network domain's "
            "binder: cmd waits for the receiver of its result, so without the rule it never "
            "returns and the script never sets its marker"
        )
    entries = [
        line.split()
        for line in system_ext_property_contexts.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    marker = [
        tokens
        for tokens in entries
        if tokens[:2] == [WIFI_NETWORK_MARKER, f"u:object_r:{WIFI_NETWORK_MARKER_TYPE}:s0"]
    ]
    if not marker:
        fail(f"system_ext property contexts do not label {WIFI_NETWORK_MARKER}")
    for value in WIFI_NETWORK_MARKER_VALUES:
        if not property_type_accepts(marker[0][2:], value):
            fail(
                f"system_ext property contexts type {WIFI_NETWORK_MARKER} as "
                f"'{' '.join(marker[0][2:])}', which rejects {value}: init refuses the "
                "setprop, so the script's repair would run at every boot"
            )


def check_reclaim_boot_apps(rc_payload: str, script: bytes) -> None:
    """The reclaim script runs once after boot, kills only cached processes of the reviewed list."""
    name, executable = RECLAIM_SERVICE
    service = init_services(rc_payload).get(name)
    if service is None or service[0] != executable:
        fail(f"system_ext init policy does not declare the {name} service at {executable}")
    for option in ("oneshot", "disabled"):
        if option not in service[1]:
            fail(f"system_ext init policy does not declare {name} as {option}")
    actions = init_actions(rc_payload)
    for trigger, commands in actions.items():
        if f"start {name}" in commands and trigger != RECLAIM_TRIGGER:
            fail(f"system_ext init policy starts {name} under '{trigger}'")
    if f"start {name}" not in actions.get(RECLAIM_TRIGGER, []):
        fail(f"system_ext init policy does not start {name} when boot completes")
    text = script.decode("utf-8", errors="strict")
    code = "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#"))
    for token in RECLAIM_SCRIPT_REQUIRED:
        if token not in code:
            fail(f"{executable} does not contain '{token}'")
    for token in RECLAIM_SCRIPT_FORBIDDEN:
        if token in code.replace(RECLAIM_SETTLE, ""):
            fail(
                f"{executable} contains '{token}': it may only kill the cached processes of "
                "the reviewed list, which keeps alarms, jobs and bindings"
            )
    kills = code.count(RECLAIM_KILL)
    if kills != 1:
        fail(f"{executable} holds the reviewed kill {kills} times, not once")
    other = RECLAIM_OTHER_KILL.search(code.replace(RECLAIM_KILL, "").replace(RECLAIM_SETTLE, ""))
    if other is not None:
        fail(
            f"{executable} contains '{other.group(0)}': it may only kill the cached processes of "
            "the reviewed list, with the one reviewed command"
        )
    listed = RECLAIM_LIST.search(text)
    if listed is None:
        fail(f"{executable} has no reclaim_list block")
    entries = tuple(
        (fields[0], tuple(fields[1:]))
        for fields in (line.split() for line in listed.group(1).splitlines())
        if fields
    )
    if entries != RECLAIM_APPS:
        fail(
            f"{executable} lists {entries}, not the reviewed packages {RECLAIM_APPS}: "
            "a change to the list is a reviewed change to this verifier too"
        )
    assignments = len(RECLAIM_LIST_ASSIGNMENT.findall(code))
    if assignments != 1:
        fail(f"{executable} assigns reclaim_list {assignments} times, not once: the list is the reviewed block")


def check_reclaim_boot_apps_policy(system_ext_policy: bytes, system_ext_file_contexts: bytes) -> None:
    """The reclaim script runs in its own core domain and may only use the activity manager."""
    domain = RECLAIM_SERVICE[0]
    if file_context_label(system_ext_file_contexts, RECLAIM_SERVICE[1]) != (
        f"u:object_r:{domain}_exec:s0"
    ):
        fail(f"system_ext file contexts do not label {RECLAIM_SERVICE[1]}")
    if not RECLAIM_TRANSITION.search(system_ext_policy):
        fail("system_ext policy does not move init into the reclaim domain")

    def has(source: str, target: str, object_class: bytes, permission: bytes) -> bool:
        return any(
            CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(1).decode()) == source
            and CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(2).decode()) == target
            and match.group(3) == object_class
            and permission in match.group(4).split()
            for match in CIL_ALLOW_RULE.finditer(system_ext_policy)
        )

    for source, target, object_class, permission, why in (
        (domain, "activity_service", b"service_manager", b"find",
         "dumpsys and cmd find the activity manager"),
        (domain, "system_server", b"binder", b"transfer",
         "cmd hands the activity manager the receiver of its result"),
        ("system_server", domain, b"binder", b"call",
         "cmd waits for the receiver of its result"),
        ("system_server", domain, b"fd", b"use",
         "the activity manager writes to the descriptors the tools pass"),
        ("system_server", domain, b"fifo_file", b"write",
         "dumpsys reads the process list from a pipe"),
    ):
        if not has(source, target, object_class, permission):
            fail(
                f"system_ext policy does not allow {source} {target} {object_class.decode()} "
                f"{permission.decode()}: {why}"
            )


def check_time_zone(rc_payload: str, script: bytes) -> None:
    """The script that applies the host's time zone runs once after boot and never writes a setting."""
    name, executable = TIME_ZONE_SERVICE
    service = init_services(rc_payload).get(name)
    if service is None or service[0] != executable:
        fail(f"system_ext init policy does not declare the {name} service at {executable}")
    for option in ("oneshot", "disabled"):
        if option not in service[1]:
            fail(f"system_ext init policy does not declare {name} as {option}")
    actions = init_actions(rc_payload)
    for trigger, commands in actions.items():
        if f"start {name}" in commands and trigger != TIME_ZONE_TRIGGER:
            fail(f"system_ext init policy starts {name} under '{trigger}'")
    if f"start {name}" not in actions.get(TIME_ZONE_TRIGGER, []):
        fail(f"system_ext init policy does not start {name} when boot completes")
    code = "\n".join(
        line
        for line in script.decode("utf-8", errors="strict").splitlines()
        if not line.lstrip().startswith("#")
    )
    for token in TIME_ZONE_SCRIPT_REQUIRED:
        if token not in code:
            fail(f"{executable} does not contain '{token}'")
    for token in TIME_ZONE_SCRIPT_FORBIDDEN:
        if token in code:
            fail(
                f"{executable} contains '{token}': it may only read the user's automatic time "
                "zone setting and set the zone through the platform's setter"
            )


def check_time_zone_policy(
    system_ext_policy: bytes, system_ext_file_contexts: bytes, system_ext_property_contexts: str
) -> None:
    """The script runs in its own core domain, uses only the settings and alarm services, sets only its marker."""
    domain = TIME_ZONE_SERVICE[0]
    if file_context_label(system_ext_file_contexts, TIME_ZONE_SERVICE[1]) != (
        f"u:object_r:{domain}_exec:s0"
    ):
        fail(f"system_ext file contexts do not label {TIME_ZONE_SERVICE[1]}")
    if not TIME_ZONE_TRANSITION.search(system_ext_policy):
        fail("system_ext policy does not move init into the time zone domain")

    def has(source: str, target: str, object_class: bytes, permission: bytes) -> bool:
        return any(
            CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(1).decode()) == source
            and CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(2).decode()) == target
            and match.group(3) == object_class
            and permission in match.group(4).split()
            for match in CIL_ALLOW_RULE.finditer(system_ext_policy)
        )

    for source, target, object_class, permission, why in (
        (domain, "settings_service", b"service_manager", b"find",
         "cmd finds the settings service to read the automatic time zone setting"),
        (domain, "alarm_service", b"service_manager", b"find",
         "cmd finds the alarm service, the platform's time zone setter"),
        (domain, "system_server", b"binder", b"transfer",
         "cmd hands both services the receiver of its result"),
        ("system_server", domain, b"binder", b"call",
         "cmd waits for the receiver of its result"),
        ("system_server", domain, b"fd", b"use",
         "the services write to the descriptors cmd passes"),
        ("system_server", domain, b"fifo_file", b"write",
         "the settings service writes the setting it reads to a pipe"),
    ):
        if not has(source, target, object_class, permission):
            fail(
                f"system_ext policy does not allow {source} {target} {object_class.decode()} "
                f"{permission.decode()}: {why}"
            )
    if TIME_ZONE_MARKER_TYPE not in cil_attribute_members(system_ext_policy, "system_property_type"):
        fail(f"system_ext policy does not define the system-owned {TIME_ZONE_MARKER_TYPE}")
    settable = {
        CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(2).decode())
        for match in CIL_PROPERTY_SET_RULE.finditer(system_ext_policy)
        if CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(1).decode()) == domain
        and b"set" in match.group(3).split()
    }
    if TIME_ZONE_MARKER_TYPE not in settable:
        fail(f"system_ext policy does not let the time zone domain set {TIME_ZONE_MARKER}")
    if settable != {TIME_ZONE_MARKER_TYPE}:
        fail(
            f"system_ext policy lets the time zone domain set {sorted(settable)}, not only its "
            f"own marker {TIME_ZONE_MARKER_TYPE}: system_server writes persist.sys.timezone"
        )
    if not any(
        line.split()[:2] == [TIME_ZONE_MARKER, f"u:object_r:{TIME_ZONE_MARKER_TYPE}:s0"]
        for line in system_ext_property_contexts.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ):
        fail(f"system_ext property contexts do not label {TIME_ZONE_MARKER}")


def check_keyboard_default(rc_payload: str, script: bytes) -> None:
    """The script that moves an older guest to the on-screen keyboard default runs once after boot."""
    name, executable = KEYBOARD_DEFAULT_SERVICE
    service = init_services(rc_payload).get(name)
    if service is None or service[0] != executable:
        fail(f"system_ext init policy does not declare the {name} service at {executable}")
    for option in ("oneshot", "disabled"):
        if option not in service[1]:
            fail(f"system_ext init policy does not declare {name} as {option}")
    actions = init_actions(rc_payload)
    for trigger, commands in actions.items():
        if f"start {name}" in commands and trigger != KEYBOARD_DEFAULT_TRIGGER:
            fail(f"system_ext init policy starts {name} under '{trigger}'")
    if f"start {name}" not in actions.get(KEYBOARD_DEFAULT_TRIGGER, []):
        fail(f"system_ext init policy does not start {name} when boot completes")
    code = "\n".join(
        line
        for line in script.decode("utf-8", errors="strict").splitlines()
        if not line.lstrip().startswith("#")
    )
    for token in KEYBOARD_DEFAULT_SCRIPT_REQUIRED:
        if token not in code:
            fail(f"{executable} does not contain '{token}'")
    for token in KEYBOARD_DEFAULT_SCRIPT_FORBIDDEN:
        if token in code:
            fail(
                f"{executable} contains '{token}': it may only read the on-screen keyboard setting "
                "and turn it off, once"
            )


def check_keyboard_default_policy(
    system_ext_policy: bytes, system_ext_file_contexts: bytes, system_ext_property_contexts: str
) -> None:
    """The script runs in its own core domain, uses only the settings service, sets only its marker."""
    domain = KEYBOARD_DEFAULT_SERVICE[0]
    if file_context_label(system_ext_file_contexts, KEYBOARD_DEFAULT_SERVICE[1]) != (
        f"u:object_r:{domain}_exec:s0"
    ):
        fail(f"system_ext file contexts do not label {KEYBOARD_DEFAULT_SERVICE[1]}")
    if not KEYBOARD_DEFAULT_TRANSITION.search(system_ext_policy):
        fail("system_ext policy does not move init into the keyboard default domain")

    def has(source: str, target: str, object_class: bytes, permission: bytes) -> bool:
        return any(
            CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(1).decode()) == source
            and CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(2).decode()) == target
            and match.group(3) == object_class
            and permission in match.group(4).split()
            for match in CIL_ALLOW_RULE.finditer(system_ext_policy)
        )

    for source, target, object_class, permission, why in (
        (domain, "settings_service", b"service_manager", b"find",
         "cmd finds the settings service to read and write the on-screen keyboard setting"),
        (domain, "system_server", b"binder", b"transfer",
         "cmd hands the service the receiver of its result"),
        ("system_server", domain, b"binder", b"call",
         "cmd waits for the receiver of its result"),
        ("system_server", domain, b"fd", b"use",
         "the service writes to the descriptors cmd passes"),
        ("system_server", domain, b"fifo_file", b"write",
         "the settings service writes the setting it reads to a pipe"),
    ):
        if not has(source, target, object_class, permission):
            fail(
                f"system_ext policy does not allow {source} {target} {object_class.decode()} "
                f"{permission.decode()}: {why}"
            )
    if KEYBOARD_DEFAULT_MARKER_TYPE not in cil_attribute_members(
        system_ext_policy, "system_property_type"
    ):
        fail(f"system_ext policy does not define the system-owned {KEYBOARD_DEFAULT_MARKER_TYPE}")
    settable = {
        CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(2).decode())
        for match in CIL_PROPERTY_SET_RULE.finditer(system_ext_policy)
        if CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(1).decode()) == domain
        and b"set" in match.group(3).split()
    }
    if KEYBOARD_DEFAULT_MARKER_TYPE not in settable:
        fail(f"system_ext policy does not let the keyboard default domain set {KEYBOARD_DEFAULT_MARKER}")
    if settable != {KEYBOARD_DEFAULT_MARKER_TYPE}:
        fail(
            f"system_ext policy lets the keyboard default domain set {sorted(settable)}, not only "
            f"its own marker {KEYBOARD_DEFAULT_MARKER_TYPE}"
        )
    if not any(
        line.split()[:2] == [KEYBOARD_DEFAULT_MARKER, f"u:object_r:{KEYBOARD_DEFAULT_MARKER_TYPE}:s0"]
        for line in system_ext_property_contexts.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ):
        fail(f"system_ext property contexts do not label {KEYBOARD_DEFAULT_MARKER}")


def check_oldapp_switch(wrapper: bytes, jar: bytes, packaged: set[str]) -> None:
    check_shell_java_tool(
        wrapper, jar, packaged, OLDAPP_COMMAND, OLDAPP_JAR, OLDAPP_CLASS, OLDAPP_WRAPPER_REQUIRED
    )


def check_apps_command(wrapper: bytes, jar: bytes, packaged: set[str]) -> None:
    check_shell_java_tool(wrapper, jar, packaged, APPS_COMMAND, APPS_JAR, APPS_CLASS, APPS_WRAPPER_REQUIRED)


def check_shell_java_tool(
    wrapper: bytes,
    jar: bytes,
    packaged: set[str],
    command: str,
    jar_path: str,
    main_class: str,
    wrapper_required: tuple[str, ...],
) -> None:
    """The shell command is a wrapper script that starts the Java tool's class from its jar, and
    the image carries the tool's code: in the jar, or preoptimized beside it."""
    text = "\n".join(
        line
        for line in wrapper.decode("utf-8", errors="strict").splitlines()
        if not line.lstrip().startswith("#") or line.startswith("#!")
    )
    for token in wrapper_required:
        if token not in text:
            fail(f"/system_ext/bin/{command} does not contain '{token}'")
    try:
        with zipfile.ZipFile(io.BytesIO(jar)) as archive:
            names = archive.namelist()
            dex = archive.read("classes.dex") if "classes.dex" in names else None
    except (zipfile.BadZipFile, OSError, zlib.error) as problem:
        fail(f"{jar_path} is not a readable jar: {problem}")
    if dex is not None:
        if b"L" + main_class.replace(".", "/").encode() + b";" not in dex:
            fail(f"{jar_path} holds code without the class {main_class}")
        return
    # Dexpreopting may strip the code from the jar into the odex and vdex beside it: one such pair
    # must be packaged, or there is no code to start.
    stem = PurePosixPath(jar_path)
    for isa in sorted(APP_ISAS):
        pair = [f"{stem.parent}/oat/{isa}/{stem.stem}.{suffix}" for suffix in ("odex", "vdex")]
        if all(name in packaged for name in pair):
            return
    fail(
        f"{jar_path} has no classes.dex and no preoptimized odex and vdex of it is packaged "
        f"(framework/oat/<isa>/{stem.stem}.odex and .vdex): the command {command} has no code"
    )


# The icon renderer (overlay/app_icons): the application that draws the launcher applications' icons for
# kernarium_apps, so that no icon is drawn in the adb shell user's process. It is an ordinary application: in
# app/ and not priv-app/, signed with the default key and not a platform one (so its seinfo is default and it
# runs in untrusted_app), no sharedUserId, no permission asked, no activity, receiver or launcher entry, no
# backup, direct-boot aware, and in the manifest one provider that only the holders of
# ACCESS_CONTENT_PROVIDERS_EXTERNALLY (the shell user and the system) may use and one service that runs in an
# isolated process and is not exported. The product gives it no SELinux rule of its own: its package selects
# no seapp_contexts entry, and the platform's own entries put its two processes in an untrusted_app domain and
# isolated_app. usesNonSdkApi lets the isolated process add the application's resource files the way the framework does.
ICONS_PACKAGE = "com.kernarium.icons"
ICONS_APK = "SYSTEM_EXT/app/KernariumIcons/KernariumIcons.apk"
ICONS_APK_NAME = "KernariumIcons.apk"
ICONS_PERMISSION = "android.permission.ACCESS_CONTENT_PROVIDERS_EXTERNALLY"
APKCERTS = "META/apkcerts.txt"
# The keys an application is signed with when the build gives it no certificate of its own, before and after release signing.
DEFAULT_APP_KEYS = frozenset({"testkey", "releasekey"})
ORDINARY_APP_DOMAIN = re.compile(r"untrusted_app(_\d+)?")
ICONS_ABSENT_ATTRIBUTES = {
    "manifest": ("sharedUserId", "sharedUserLabel"),
    "application": ("debuggable", "persistent", "process", "sharedUserId", "testOnly", "backupAgent", "usesCleartextTraffic"),
    "provider": ("readPermission", "writePermission", "grantUriPermissions", "multiprocess", "process", "enabled", "isolatedProcess"),
    "service": ("permission", "process", "enabled", "externalService", "foregroundServiceType", "useAppZygote"),
}


def manifest_elements(manifest: bytes) -> list[tuple[str, dict[str, str | int | bool | None]]]:
    """Every element of a compiled manifest in document order, with its attributes by name, typed as
    manifest_element_attributes gives them."""
    if len(manifest) < 8 or struct.unpack_from("<HH", manifest, 0)[0] != 0x0003:
        raise ValueError("not a compiled XML file")
    strings: list[str] = []
    elements: list[tuple[str, dict[str, str | int | bool | None]]] = []
    offset = struct.unpack_from("<H", manifest, 2)[0]
    while offset + 8 <= len(manifest):
        kind, header, size = struct.unpack_from("<HHI", manifest, offset)
        if size < 8 or offset + size > len(manifest):
            raise ValueError("truncated compiled XML chunk")
        if kind == AXML_STRING_POOL:
            strings = axml_strings(manifest, offset)
        elif kind == AXML_START_ELEMENT:
            body = offset + header
            element = struct.unpack_from("<I", manifest, body + 4)[0]
            if element >= len(strings):
                raise ValueError("an element name outside the string pool")
            attribute_start, attribute_size, count = struct.unpack_from("<HHH", manifest, body + 8)
            attributes: dict[str, str | int | bool | None] = {}
            for index in range(count):
                position = body + attribute_start + index * attribute_size
                name, _, _, data_type, data = struct.unpack_from("<IIHxBI", manifest, position + 4)
                if name >= len(strings):
                    raise ValueError("an attribute name outside the string pool")
                if data_type == AXML_STRING_TYPE:
                    if data >= len(strings):
                        raise ValueError("an attribute value outside the string pool")
                    attributes[strings[name]] = strings[data]
                elif data_type in AXML_INTEGER_TYPES:
                    attributes[strings[name]] = data
                elif data_type == AXML_BOOLEAN_TYPE:
                    attributes[strings[name]] = data != 0
                else:
                    attributes[strings[name]] = None
            elements.append((strings[element], attributes))
        offset += size
    return elements


def seapp_entry_names(entry: dict[str, str], name: str) -> bool:
    """Whether an seapp_contexts entry's name field, a package or a prefix ending in * or :*, selects `name` (none selects all)."""
    wanted = entry.get("name")
    if wanted is None:
        return True
    if wanted.endswith("*"):
        return name.startswith(wanted[:-1])
    return wanted == name


def seapp_applies(entry: dict[str, str], seinfo: str, name: str, target_sdk: int) -> tuple[int, int, int] | None:
    """How specifically an seapp_contexts entry applies to the process of an ordinary installed application (not
    privileged, not ephemeral, not started by run-as, not a system server, sdk sandbox or shared compute process) of this
    seinfo, package and target SDK, as the key of its precedence (seinfo stated, name stated, the highest
    minTargetSdkVersion), or None when a selector of the entry leaves it out."""
    if any(entry.get(key) not in (None, "false") for key in
           ("isSystemServer", "isEphemeralApp", "isPrivApp", "fromRunAs", "isIsolatedComputeApp", "isSdkSandboxNext",
            "isSdkSandboxAudit")):
        return None
    if entry.get("seinfo") not in (None, seinfo) or not seapp_entry_names(entry, name):
        return None
    minimum = int(entry.get("minTargetSdkVersion", "0"))
    if target_sdk < minimum or ("maxTargetSdkVersion" in entry and target_sdk > int(entry["maxTargetSdkVersion"])):
        return None
    return (1 if "seinfo" in entry else 0, 1 if "name" in entry else 0, minimum)


def seapp_domain(entries: list[dict[str, str]], user: str, seinfo: str, name: str, target_sdk: int) -> str | None:
    """The domain the platform picks for a process of this seapp user (_app or _isolated) of an application of this
    seinfo, package and target SDK: the applicable entry of highest precedence, the earlier one of equals."""
    best: tuple[tuple[int, int, int], dict[str, str]] | None = None
    for entry in entries:
        if entry.get("user") != user or "domain" not in entry:
            continue
        key = seapp_applies(entry, seinfo, name, target_sdk)
        if key is not None and (best is None or key > best[0]):
            best = (key, entry)
    return best[1]["domain"] if best else None


def seapp_app_domain(entries: list[dict[str, str]], seinfo: str, name: str, target_sdk: int) -> str | None:
    """The domain of the process of an ordinary installed application (its own user id in the app range)."""
    return seapp_domain(entries, "_app", seinfo, name, target_sdk)


def seapp_isolated_domain(entries: list[dict[str, str]], seinfo: str, name: str, target_sdk: int) -> str | None:
    """The domain of an isolated process of this application that does not share it with others."""
    return seapp_domain(entries, "_isolated", seinfo, name, target_sdk)


def manifest_component_class(package: str, name: object) -> str | None:
    """The class a manifest names for a component: a name that starts with a dot, or has none, is the package's."""
    if not isinstance(name, str) or not name:
        return None
    if name.startswith("."):
        return package + name
    return name if "." in name else f"{package}.{name}"


def check_icons_app(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo], seapp_contexts: str) -> None:
    """The icon renderer is the unprivileged, guarded application described above."""
    for entry in sorted(infos):
        parts = PurePosixPath(entry).parts
        if "KernariumIcons" in parts and "priv-app" in parts:
            fail(f"{entry}: the icon renderer is an ordinary application of app/, not a privileged one")
    info = infos.get(ICONS_APK)
    if info is None:
        fail(f"{ICONS_APK} is missing: kernarium_apps has nothing to draw icons with")
    try:
        with zipfile.ZipFile(io.BytesIO(read_small(archive, info, 4 << 20))) as apk:
            elements = manifest_elements(apk.read("AndroidManifest.xml"))
    except (KeyError, OSError, ValueError, struct.error, IndexError, zipfile.BadZipFile) as problem:
        fail(f"{ICONS_APK} has no readable manifest: {problem}")
    names = [name for name, _ in elements]
    if names[:1] != ["manifest"]:
        fail(f"{ICONS_APK}: the manifest does not start with the manifest element")
    first = {name: attributes for name, attributes in reversed(elements)}
    if first["manifest"].get("package") != ICONS_PACKAGE:
        fail(f"{ICONS_APK} is the package {first['manifest'].get('package')}, not {ICONS_PACKAGE}")
    for kind in (*sorted(PERMISSION_REQUEST_ELEMENTS), "permission", "activity", "activity-alias", "receiver",
                 "intent-filter", "uses-library", "uses-native-library", "meta-data", "grant-uri-permission", "path-permission"):
        if kind in names:
            fail(f"{ICONS_APK} has {kind}: the renderer asks for no permission and has no activity, receiver, filter or launcher entry")
    for kind, wanted in (("application", 1), ("provider", 1), ("service", 1)):
        if names.count(kind) != wanted:
            fail(f"{ICONS_APK} has {names.count(kind)} {kind} elements, not {wanted}")
    for kind, attributes in ICONS_ABSENT_ATTRIBUTES.items():
        for attribute in attributes:
            # By the key: a value that is a resource reference reads as no value, and is set all the same.
            if attribute in first[kind] and not (attribute == "enabled" and first[kind][attribute] is True):
                fail(f"{ICONS_APK}: the {kind} sets {attribute}")
    application = first["application"]
    for attribute, value in (("allowBackup", False), ("directBootAware", True), ("usesNonSdkApi", True)):
        if application.get(attribute) is not value:
            fail(f"{ICONS_APK}: the application's {attribute} is not {str(value).lower()}")
    provider, service = first["provider"], first["service"]
    for attribute, value in (("authorities", ICONS_PACKAGE), ("exported", True), ("permission", ICONS_PERMISSION),
                             ("directBootAware", True)):
        if provider.get(attribute) != value:
            fail(f"{ICONS_APK}: the provider's {attribute} is {provider.get(attribute)!r}, not {value!r}: it is open only "
                 f"to the holders of {ICONS_PERMISSION}")
    for attribute, value in (("exported", False), ("isolatedProcess", True), ("directBootAware", True)):
        if service.get(attribute) is not value:
            fail(f"{ICONS_APK}: the service's {attribute} is {service.get(attribute)!r}, not {value!r}: it runs in an isolated process and is not exported")
    for kind, component, wanted in (("provider", provider, "IconProvider"), ("service", service, "IconRenderService")):
        found = manifest_component_class(ICONS_PACKAGE, component.get("name"))
        if found != f"{ICONS_PACKAGE}.{wanted}":
            fail(f"{ICONS_APK}: the {kind} is {found}, not {ICONS_PACKAGE}.{wanted}")
    target = first.get("uses-sdk", {}).get("targetSdkVersion")
    if not isinstance(target, int) or isinstance(target, bool):
        fail(f"{ICONS_APK} has no integer targetSdkVersion: the domain of its process cannot be told")
    if APKCERTS not in infos:
        fail(f"{APKCERTS} is missing: the key of the icon renderer cannot be checked")
    certificate = None
    for line in read_small(archive, infos[APKCERTS]).decode("utf-8", errors="strict").splitlines():
        fields = dict(re.findall(r'(\w+)="([^"]*)"', line))
        if fields.get("name") == ICONS_APK_NAME:
            certificate = fields
    if certificate is None:
        fail(f"{APKCERTS} has no line for {ICONS_APK_NAME}")
    key = PurePosixPath(certificate.get("certificate", "")).name.removesuffix(".x509.pem")
    if key not in DEFAULT_APP_KEYS or certificate.get("partition") != "system_ext":
        fail(f"{ICONS_APK_NAME} is signed with {certificate.get('certificate')!r} on {certificate.get('partition')!r}: the renderer "
             "takes the default key (never a platform key: its seinfo would be platform) and lives on system_ext")
    entries = parse_seapp_contexts(seapp_contexts)
    for entry in entries:
        if "name" in entry and seapp_entry_names(entry, ICONS_PACKAGE) and entry["name"] != "":
            fail(f"seapp_contexts select {entry.get('domain')} for the icon renderer by name ({entry['name']}): it has no policy of its own")
    domain = seapp_app_domain(entries, "default", ICONS_PACKAGE, target)
    if domain is None or ORDINARY_APP_DOMAIN.fullmatch(domain) is None:
        fail(f"seapp_contexts put the icon renderer's provider in {domain}, not an untrusted_app domain")
    isolated = seapp_isolated_domain(entries, "default", ICONS_PACKAGE, target)
    if isolated != "isolated_app":
        fail(f"seapp_contexts put the icon renderer's isolated service in {isolated}, not isolated_app")


def check_oldapp_switch_policy(
    contexts: list[str], policies: tuple[bytes, ...], system_ext_policy: bytes
) -> None:
    """The choice's property has a type of its own: the shell domain sets it, every application
    domain reads it, and no application domain but the shell may set it.

    The type is declared system_restricted_prop: an internal type forbids the read to the
    application domains that are not core domains (gceservice, vzwomatrigger_app), and the
    restricted macro's own neverallow names only the non-core domains for the write, so a
    neverallow of the application domains is checked here by what its source set covers."""
    sample = OLDAPP_PROPERTY_PREFIX + "org.example.package"
    label = property_label(sample, contexts) or "default_prop"
    if label != OLDAPP_PROPERTY_TYPE:
        fail(
            f"{sample} is labelled {label}, not {OLDAPP_PROPERTY_TYPE} "
            "(overlay/sepolicy/system_ext/private/property_contexts): the shell command could not "
            "set it, or any application could set it"
        )
    if OLDAPP_PROPERTY_TYPE not in cil_attribute_members(system_ext_policy, "system_property_type"):
        fail(f"system_ext policy does not define the system-owned {OLDAPP_PROPERTY_TYPE}")
    if OLDAPP_PROPERTY_TYPE in cil_attribute_members(system_ext_policy, "system_internal_property_type"):
        fail(
            f"{OLDAPP_PROPERTY_TYPE} is a system_internal_prop: the platform forbids the application "
            "domains that are not core domains to read it"
        )
    if OLDAPP_PROPERTY_TYPE not in cil_attribute_members(
        system_ext_policy, "system_restricted_property_type"
    ):
        fail(f"{OLDAPP_PROPERTY_TYPE} is not declared with system_restricted_prop")
    if not cil_allows_property_set(policies, OLDAPP_SETTER, label):
        fail(
            f"the {OLDAPP_SETTER} domain may not set {OLDAPP_PROPERTY_PREFIX}<package>, labelled "
            f"{label}: Andarium's adb could not switch Old app compatibility"
        )
    for object_class, permissions in OLDAPP_LOCK_PERMISSIONS:
        if not cil_allows(policies, OLDAPP_SETTER, OLDAPP_LOCK_TYPE, object_class, set(permissions)):
            fail(
                f"the {OLDAPP_SETTER} domain may not create and lock files of {OLDAPP_LOCK_TYPE} "
                f"({object_class} {', '.join(sorted(permissions))}): {OLDAPP_COMMAND} could not take "
                "its lock"
            )
    # The concrete types that run applications (the shell among them), not the attribute names: a
    # grant that reaches each of them through the attribute or on its own is the same grant.
    members = {
        member
        for policy in policies
        for attribute in ADB_KEY_APP_ATTRIBUTES
        for member in cil_attribute_members(policy, attribute)
    }
    if not any(cil_attribute_members(policy, "appdomain") for policy in policies):
        fail("the policy lists no type for appdomain: the check could not tell who runs applications")
    for domain in sorted(members | {OLDAPP_SETTER}):
        if not cil_allows(policies, domain, label, "file", set(OLDAPP_READ_PERMISSIONS)):
            fail(
                f"{domain} may not read {OLDAPP_PROPERTY_PREFIX}<package>, labelled {label} "
                f"({', '.join(sorted(OLDAPP_READ_PERMISSIONS))} on the file): the framework hook "
                "(the shell command for the shell domain) could not read the choice"
            )
    applications = sorted(members - {OLDAPP_SETTER})
    for domain in applications:
        if cil_allows_property_set(policies, domain, label):
            fail(
                f"{domain} may set {OLDAPP_PROPERTY_PREFIX}<package>, labelled {label}: an "
                "application could turn Old app compatibility on for itself"
            )
    targets = cil_types_and_attributes(policies, label)
    for domain in applications:
        sources = cil_types_and_attributes(policies, domain)
        if not any(
            cil_type_name(source) in sources
            and cil_type_name(target) in targets
            and object_class == b"property_service"
            and b"set" in permissions
            for policy in policies
            for source, target, object_class, permissions in cil_rules(policy, CIL_NEVERALLOW_RULE)
        ):
            fail(
                f"system_ext policy has no neverallow that keeps application domains from setting "
                f"{label}: none covers {domain}"
            )


# The host's keyboard and mouse are virtio-input devices that Android finds by vendor and product
# (overlay/input, the product makefile). The side-key keyboard (1af4:0002) carries Power, Volume Up
# and Down, Back and Home, and its key layout holds those five keys alone: Android would otherwise
# give it Generic.kl, count it as a full keyboard that is always attached, and never hide its
# on-screen keyboard for a real one. The keyboard (0003) and the mouse (0004) are external, as a USB
# keyboard and mouse are, so that a key or a click wakes the screen; the absolute pointer (0005)
# follows the display's orientation and wakes it too. The three have no key layout or character map
# of their own: Android's keyboard layout choice belongs to the user.
INPUT_SIDE_KEY_LAYOUT = "VENDOR/usr/keylayout/Vendor_1af4_Product_0002.kl"
INPUT_SIDE_KEYS = {116: "POWER", 115: "VOLUME_UP", 114: "VOLUME_DOWN", 158: "BACK", 172: "HOME"}
INPUT_DEVICE_CONFIGURATIONS = {
    "VENDOR/usr/idc/Vendor_1af4_Product_0003.idc": {"device.internal": "0"},
    "VENDOR/usr/idc/Vendor_1af4_Product_0004.idc": {"device.internal": "0"},
    "VENDOR/usr/idc/Vendor_1af4_Product_0005.idc": {"touch.orientationAware": "1", "touch.wake": "1"},
}
INPUT_HOST_DEVICE_LAYOUT = re.compile(
    r"^VENDOR/usr/(?:keylayout|keychars)/Vendor_1af4_Product_000[345](?:_Version_[0-9a-f]{4})?\.(?:kl|kcm)$"
)


def input_file_lines(payload: bytes, path: str) -> list[tuple[int, str]]:
    """The lines of an Android input configuration file that are not blank or comments."""
    try:
        text = payload.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        fail(f"{path} is not UTF-8: {error}")
    return [
        (number, line.strip())
        for number, line in enumerate(text.splitlines(), 1)
        if line.strip() and not line.lstrip().startswith("#")
    ]


def check_input_device_configuration(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    keys: dict[int, str] = {}
    for number, line in input_file_lines(
        read_small(archive, infos[INPUT_SIDE_KEY_LAYOUT]), INPUT_SIDE_KEY_LAYOUT
    ):
        fields = line.split()
        if len(fields) != 3 or fields[0] != "key" or not fields[1].isdecimal():
            fail(f"{INPUT_SIDE_KEY_LAYOUT}:{number} must be a 'key <scan code> <label>' line: {line}")
        if int(fields[1]) in keys:
            fail(f"{INPUT_SIDE_KEY_LAYOUT}:{number} maps scan code {fields[1]} twice")
        keys[int(fields[1])] = fields[2]
    if keys != INPUT_SIDE_KEYS:
        fail(
            f"{INPUT_SIDE_KEY_LAYOUT} must map the five side keys alone ({INPUT_SIDE_KEYS}), so that "
            f"Android does not count the side-key keyboard as a full keyboard: {keys}"
        )
    for path, expected in INPUT_DEVICE_CONFIGURATIONS.items():
        found: dict[str, str] = {}
        for number, line in input_file_lines(read_small(archive, infos[path]), path):
            name, separator, value = line.partition("=")
            if not separator or not name.strip() or not value.strip():
                fail(f"{path}:{number} must be a 'name = value' line: {line}")
            if name.strip() in found:
                fail(f"{path}:{number} sets {name.strip()} twice")
            found[name.strip()] = value.strip()
        if found != expected:
            fail(f"{path} must set {expected} and nothing else: {found}")
    for name in sorted(infos):
        if INPUT_HOST_DEVICE_LAYOUT.match(name):
            fail(
                f"{name} gives a host input device a key layout or character map of its own: "
                "Android's keyboard layout choice is the user's"
            )


def check_policy_relaxations(policies: tuple[bytes, ...], build_variant: str) -> None:
    """The policy holds none of the LineageOS grants that Android's neverallow tests refuse.

    LineageOS serves an ext4 SD card and addon.d updates with rules that the
    platform build tolerates only because its fork relaxed the matching
    neverallows; the CTS carries AOSP's. This product has neither feature, and
    its tree restores the neverallows (the system/sepolicy route), so the rules
    are gone. This repeats the checks on the built policy.
    """
    mls_trusted = set()
    for policy in policies:
        mls_trusted |= cil_attribute_members(policy, "mlstrustedsubject")
    if "mediaprovider_app" in mls_trusted:
        fail(
            "policy makes mediaprovider_app an mlstrustedsubject; Android's neverallow "
            "tests refuse it to fork and to modify privapp_data_file"
        )
    groups: dict[str, set[str]] = {}
    for policy in policies:
        for match in CIL_ALLOW_RULE.finditer(policy):
            source, target, object_class, permissions = (
                CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(index).decode())
                for index in (1, 2, 3, 4)
            )
            perms = set(permissions.split())
            if source == "mediaprovider_app" and target == "privapp_data_file":
                fail("policy lets mediaprovider_app use privapp_data_file beyond the app rules")
            if (
                source == "fsck_untrusted"
                and object_class in ("capability", "cap_userns")
                and "sys_admin" in perms
            ):
                fail("policy gives fsck_untrusted sys_admin; Android's neverallow tests refuse it")
            if object_class == "filesystem" and "relabelto" in perms:
                if target not in groups:
                    groups[target] = cil_types_and_attributes(policies, target)
                if "fs_type" in groups[target] and "contextmount_type" not in groups[target]:
                    fail(
                        f"policy lets {source} relabel a filesystem to {target}; only a "
                        "contextmount_type may be a relabelto target"
                    )
        for match in CIL_PERMISSIVE_TYPE.finditer(policy):
            domain = CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(1).decode())
            if domain not in PERMISSIVE_DOMAINS[build_variant]:
                fail(
                    f"policy makes the domain {domain} permissive; in a {build_variant} build "
                    f"only {sorted(PERMISSIVE_DOMAINS[build_variant]) or 'no domain'} may be"
                )


def check_mediaserver_property_audit(policies: tuple[bytes, ...]) -> None:
    """mediaserver's read of a vendor property is silenced as it is for the other graphics clients.

    The vendor's gralloc mapper, which mediaserver loads to allocate decoder
    buffers, reads vendor.minigbm.debug when it initialises. That name is a
    vendor_default_prop, which a core domain may not read, and the lookup
    carries on without a value. Without this rule every first use logs an
    audit denial that nothing needs to act on.
    """
    for policy in policies:
        for match in CIL_DONTAUDIT_RULE.finditer(policy):
            source, target, object_class, permissions = (
                CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(index).decode())
                for index in (1, 2, 3, 4)
            )
            if (
                source == "mediaserver"
                and target == "vendor_default_prop"
                and object_class == "file"
                and "read" in permissions.split()
            ):
                return
    fail(
        "policy does not silence the denial of mediaserver reading vendor_default_prop "
        "(overlay/sepolicy/system_ext/private/mediaserver.te)"
    )


def check_shell_density_property_audit(policies: tuple[bytes, ...]) -> None:
    """The shell user's read of the emulator's density property is silenced, as mediaserver's property read is.

    kernarium_apps runs as the adb shell user. The resources code it uses reads
    qemu.sf.lcd_density in a static initialiser and carries on without a value
    when the shell domain may not read it. Without this rule every list and
    icons call logs an audit denial that nothing needs to act on.
    """
    for policy in policies:
        for match in CIL_DONTAUDIT_RULE.finditer(policy):
            source, target, object_class, permissions = (
                CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(index).decode())
                for index in (1, 2, 3, 4)
            )
            if (
                source == "shell"
                and target == "qemu_sf_lcd_density_prop"
                and object_class == "file"
                and "read" in permissions.split()
            ):
                return
    fail(
        "policy does not silence the denial of the shell domain reading qemu_sf_lcd_density_prop "
        "(overlay/sepolicy/system_ext/private/kernarium_apps.te)"
    )


def property_match_is_exact(fields: list[str]) -> bool:
    """Whether the fields of a property contexts line make it an exact entry. Android reads the match operation
    from the third field alone (libpropertyinfoserializer, ParsePropertyInfoLine): a later field that reads exact
    is a type, and the entry is a prefix."""
    return len(fields) >= 3 and fields[2] == "exact"


def property_label(name: str, contexts: list[str]) -> str | None:
    """The SELinux type that property_contexts give a property: an exact entry, else the longest prefix."""
    best: tuple[int, str] | None = None
    for line in contexts:
        fields = line.split()
        if not fields or fields[0].startswith("#") or len(fields) < 2:
            continue
        label = fields[1].split(":")[2] if fields[1].count(":") >= 3 else None
        if label is None:
            continue
        if property_match_is_exact(fields):
            if fields[0] == name:
                return label
        elif name.startswith(fields[0]) and (best is None or len(fields[0]) > best[0]):
            best = (len(fields[0]), label)
    return best[1] if best else None


def property_entry_is_exact(name: str, contexts: list[str]) -> bool:
    """Whether the property contexts have an entry for exactly this name that is marked exact: without the marker
    the entry is a prefix and labels every longer name too."""
    return any(
        fields[:1] == [name] and property_match_is_exact(fields)
        for fields in (line.split() for line in contexts if not line.lstrip().startswith("#"))
    )


@functools.lru_cache(maxsize=16)
def cil_attribute_sets(
    policy: bytes, exact: bool = False
) -> tuple[tuple[tuple[str, frozenset[str]], ...], tuple[tuple[str, list[object]], ...]]:
    """The attributes a policy defines, without version suffixes unless |exact|: those whose set is a list
    of names, with the names, and those whose set is an expression, as nested lists of names."""
    version = (lambda name: name) if exact else (lambda name: CIL_PLATFORM_VERSION_SUFFIX.sub("", name))
    lists = []
    expressions = []
    for match in CIL_TYPE_ATTRIBUTE_SET.finditer(policy):
        stack: list[list[object]] = [[]]
        for token in re.findall(r"[()]|[^\s()]+", match.group(2).decode()):
            if token == "(":
                stack.append([])
            elif token != ")":
                stack[-1].append(version(token))
            elif len(stack) > 1:
                closed = stack.pop()
                stack[-1].append(closed)
        if len(stack) != 1 or len(stack[0]) != 1 or not isinstance(stack[0][0], list):
            continue
        attribute = version(match.group(1).decode())
        members = stack[0][0]
        if all(isinstance(member, str) for member in members) and not (
            members and members[0] in CIL_SET_OPERATORS
        ):
            lists.append((attribute, frozenset(members)))
        else:
            expressions.append((attribute, members))
    return tuple(lists), tuple(expressions)


@functools.lru_cache(maxsize=32)
def cil_rules(
    policy: bytes, rule: re.Pattern[bytes]
) -> tuple[tuple[bytes, bytes, bytes, tuple[bytes, ...]], ...]:
    """The rules that |rule| matches in a policy, each as (source, target, object class,
    permissions) of raw bytes, the permissions split in the order they stand. A rule pattern with no
    object class group (CIL_FILE_ALLOW_RULE, CIL_PROPERTY_SET_RULE) yields an empty class. Nothing
    is decoded here: a caller decodes the names it compares (cil_type_name), when it compares them,
    so a line no query reaches is never decoded. A policy is scanned once for each pattern, not
    once for each query."""
    if rule.groups not in (3, 4):
        raise ValueError(
            f"a CIL rule pattern has a source, a target, optionally an object class, and permissions "
            f"as its groups; {rule.pattern!r} has {rule.groups}"
        )
    return tuple(
        (groups[0], groups[1], groups[2] if rule.groups == 4 else b"", tuple(groups[-1].split()))
        for groups in (match.groups() for match in rule.finditer(policy))
    )


@functools.lru_cache(maxsize=1 << 16)
def cil_type_name(raw: bytes) -> str:
    """A type name of a CIL rule, platform version suffix removed."""
    return CIL_PLATFORM_VERSION_SUFFIX.sub("", raw.decode())


def cil_set_contains(members: object, names: set[str]) -> bool:
    """Whether a CIL set includes a type whose own name and attributes are |names|."""
    if isinstance(members, str):
        return members in names
    assert isinstance(members, list)
    operator = members[0] if members and isinstance(members[0], str) else None
    if operator not in CIL_SET_OPERATORS:
        return any(cil_set_contains(member, names) for member in members)
    operands = [cil_set_contains(operand, names) for operand in members[1:]]
    if operator == "all":
        return True
    if operator == "not":
        return not any(operands)
    if operator == "and":
        return all(operands)
    if operator == "or":
        return any(operands)
    return operands.count(True) == 1


def cil_types_and_attributes(policies: tuple[bytes, ...], type_name: str) -> set[str]:
    """A type and every attribute that includes it, directly, through another attribute or
    through a set expression.

    Lists of names are closed first; set expressions, which may negate, are then evaluated
    against that closure, and the lists that name the attributes they add are closed again.
    """
    lists = [entry for policy in policies for entry in cil_attribute_sets(policy)[0]]
    expressions = [entry for policy in policies for entry in cil_attribute_sets(policy)[1]]

    def close(names: set[str]) -> None:
        changed = True
        while changed:
            changed = False
            for attribute, members in lists:
                if attribute not in names and not members.isdisjoint(names):
                    names.add(attribute)
                    changed = True

    names = {type_name}
    close(names)
    names |= {
        attribute for attribute, members in expressions if cil_set_contains(members, names)
    }
    close(names)
    return names


def cil_allows_file_read(policies: tuple[bytes, ...], domain: str, label: str) -> bool:
    """Whether a domain may read the properties of a label, directly or through attributes."""
    readers = cil_types_and_attributes(policies, domain)
    readable = cil_types_and_attributes(policies, label)
    return any(
        cil_type_name(source) in readers and cil_type_name(target) in readable and b"read" in permissions
        for policy in policies
        for source, target, _object_class, permissions in cil_rules(policy, CIL_FILE_ALLOW_RULE)
    )


# The text message service runs only while messages are in flight, in a domain of its own: KernariumSms
# (overlay/sms) in kernarium_sms with its data in kernarium_sms_data_file (overlay/sepolicy/system_ext/
# private/kernarium_sms.te and seapp_contexts). A small native holder (overlay/sms/listener, domain
# kernarium_sms_listener) keeps vsock port 5560 from boot and never reads it; when a connection waits it starts
# the service through a waker (kernarium_sms_wake) and passes it the listening socket over its own unix socket.
# The service reads a request only from the host's CID, so what keeps an adb shell or an installed application
# from causing a delivery is that none of them can make a vsock socket or listen on one, and that none can reach
# the holder's sockets, the service's record or the handoff.
SMS_DOMAIN = "kernarium_sms"
SMS_DATA_TYPE = "kernarium_sms_data_file"
SMS_PACKAGE = "android.kernarium.sms"
SMS_SERVICES = frozenset({"activity_service", "package_service", "role_service"})
SMS_LISTENER = "kernarium_sms_listener"
SMS_WAKE = "kernarium_sms_wake"
SMS_SOCKET_TYPE = "kernarium_sms_socket"
SMS_APK = "SYSTEM_EXT/app/KernariumSms/KernariumSms.apk"
SMS_INIT_RC = "SYSTEM_EXT/etc/init/kernarium_sms_listener.rc"
SMS_WAKE_SCRIPT = "SYSTEM_EXT/bin/kernarium_sms_wake.sh"
SMS_SOCKET_PATH = "/dev/socket/kernarium_sms"
# The init services of the one script: the holder starts with its class and the waker only through its control
# property. The options are the whole list. The holder's group is nobody: init leaves the group at root when the
# service names none.
SMS_INIT_SERVICES = {
    SMS_LISTENER: ("/system_ext/bin/kernarium_sms_listener",
                   ("class main", "user nobody", "group nobody", "socket kernarium_sms stream 0660 system system")),
    SMS_WAKE: ("/system_ext/bin/kernarium_sms_wake.sh", ("user system", "group system", "oneshot", "disabled")),
}
# What the build's <uses-sdk> may carry in the text message application's manifest: SDK levels.
SMS_USES_SDK_ATTRIBUTES = frozenset({"minSdkVersion", "targetSdkVersion", "maxSdkVersion"})
SMS_WAKE_COMMAND = "exec cmd activity start-service --user 0 -n android.kernarium.sms/.HostService >/dev/null 2>&1"
# The property the holder sets to start the waker, and the type that keeps every other domain from setting it.
SMS_WAKE_PROPERTY = "ctl.start$kernarium_sms_wake"
SMS_WAKE_PROPERTY_TYPE = "ctl_kernarium_sms_wake_prop"
# The type that labels the other control keys of both services, which only init may set.
SMS_LOCKED_PROPERTY_TYPE = "ctl_kernarium_sms_locked_prop"
# The control messages init accepts for a service (its control message table; the interface_ forms look a service up
# by an interface name, which neither service declares). init authorises a request through the property key
# ctl.<message>$<service>; for start, stop and restart it checks the legacy key ctl.<service> first and acts when
# the requester may set either.
SMS_CONTROL_MESSAGES = ("start", "stop", "restart", "sigstop_on", "sigstop_off", "oneshot_on", "oneshot_off")
SMS_LEGACY_CONTROL_MESSAGES = frozenset({"start", "stop", "restart"})
# The keys of both init services that the product's own property contexts label, each with an exact entry: the
# legacy key of each service and the oneshot keys, which the platform leaves to the default label, and the one key
# the holder sets.
SMS_CONTROL_OWN_KEYS = tuple(
    [f"ctl.{service}" for service in (SMS_LISTENER, SMS_WAKE)]
    + [f"ctl.oneshot_{state}${service}" for service in (SMS_LISTENER, SMS_WAKE) for state in ("on", "off")]
    + [SMS_WAKE_PROPERTY]
)
# The holder and the waker are plain daemons: they hold no right in these classes, whatever attribute or set
# expression grants it, and the platform grants none of them to every domain.
SMS_DAEMON_NO_GRANT_CLASSES = frozenset({
    "capability", "capability2", "blk_file", "tcp_socket", "udp_socket", "rawip_socket", "packet_socket",
    "netlink_socket", "netlink_route_socket", "hwservice_manager", "vndservice_manager",
})
# What the platform grants every domain in the binder family and the holder and the waker therefore hold in effect
# (the rules over the attribute domain with its few exclusions): a binder call permission to system_server and the
# lookup of binder_stats_consumer_service. The holder's rights in these classes are these and no more (and on the
# binder device nodes, SMS_BINDER_DEVICES): it never opens a binder device, and without the call permission to the
# service manager, which no domain-wide rule gives, it can obtain no handle to call. The waker is the framework's
# client for one call: it also finds the activity manager, and its own binder rules are the service manager's and
# system_server's (SMS_WAKE_BINDER_RIGHTS). A platform update that adds a grant fails the image build here until
# someone has looked at it.
SMS_PLATFORM_DOMAIN_GRANTS: dict[str, dict[str, frozenset[str]]] = {
    "binder": {"system_server": frozenset({"call"})},
    "service_manager": {"binder_stats_consumer_service": frozenset({"find"})},
}
SMS_WAKE_REVIEWED_LOOKUPS = {"activity_service": frozenset({"find"}), **SMS_PLATFORM_DOMAIN_GRANTS["service_manager"]}
# The waker's binder rights in effect: it calls the service manager to find the activity manager and the activity
# manager to start the service, and the replies and the calls transfer. They are its own rules (the platform's
# call permission to system_server is among them), and no other target or permission is accepted.
SMS_WAKE_BINDER_RIGHTS: dict[str, frozenset[str]] = {
    "servicemanager": frozenset({"call", "transfer"}),
    "system_server": frozenset({"call", "transfer"}),
}
# What the platform grants every domain on the binder device nodes, which the holder and the waker hold in effect
# (the rules over the attribute domain), and the vendor binder device, which is no one's but the vendor's. A platform
# update that adds a right fails the image build here until someone has looked at it.
SMS_BINDER_DEVICE_RIGHTS = frozenset({
    "append", "getattr", "ioctl", "lock", "map", "open", "read", "watch", "watch_reads", "write",
})
SMS_BINDER_DEVICES: dict[str, frozenset[str]] = {
    "binder_device": SMS_BINDER_DEVICE_RIGHTS,
    "hwbinder_device": SMS_BINDER_DEVICE_RIGHTS,
    "vndbinder_device": frozenset(),
}
# A debuggable build's platform policy lets every domain call and transfer through binder to the root shell (su): the
# holder and the waker hold exactly that pair on it there, and none in a user build.
SMS_DEBUGGABLE_SU_BINDER_RIGHTS = frozenset({"call", "transfer"})
# What the service holds on the holder's vsock sockets: it accepts, and reads and writes the connections that
# inherit the holder's label, and shuts them down.
SMS_SERVICE_VSOCK_PERMISSIONS = frozenset({"accept", "read", "write", "shutdown", "getattr", "getopt"})
# What the holder holds on its own: it makes the socket, binds it and listens, and never accepts, reads, writes,
# shuts down or connects.
SMS_HOLDER_VSOCK_PERMISSIONS = frozenset({"create", "bind", "listen", "getattr", "getopt", "setopt"})
# The holder is not the framework's client and owns no file: what it may not hold of its own, whatever the
# platform's grants through the attributes it carries.
SMS_HOLDER_FORBIDDEN_CLASSES = frozenset({
    "binder", "service_manager", "hwservice_manager", "vndservice_manager", "capability", "capability2", "dir",
    "lnk_file", "blk_file", "chr_file", "fifo_file", "tcp_socket", "udp_socket", "rawip_socket", "packet_socket",
    "netlink_socket", "netlink_route_socket",
})
# The platform grants of fd use over (nearly) every domain: the crash and trace daemons (crash_dump, tombstoned,
# heapprofd, traced_perf) and mediaprovider_app. They reach the holder as they reach every other domain. A
# platform update that adds one fails the image build here until someone has looked at it.
SMS_PLATFORM_FD_USERS = frozenset({"crash_dump", "heapprofd", "mediaprovider_app", "tombstoned", "traced_perf"})
CIL_TYPE_TRANSITION_RULE = re.compile(rb"^\(typetransition (\S+) (\S+) process (\S+)\)$", re.MULTILINE)
# The types the platform's policy lets create, bind or listen on a vsock socket, platform version suffixes
# removed (adbd and its compatibility type adbd_<version> are one). A platform update that adds a
# listener fails the image build here until someone has looked at it.
VSOCK_SOCKET_TYPES = frozenset({
    "adbd", "adbd_tradeinmode", "aisealhostservice", "aisealhostservice_virtualizationmanager", "compos_fd_server",
    "compos_verify_virtualizationmanager", "composd_virtualizationmanager", "early_virtmgr_vendor",
    "hal_camera_default", "hal_graphics_composer_default", "hal_light_cuttlefish", "hal_sensors_default",
    "hal_thermal_default", "hal_vibrator_default", "kernarium_gamepad", "libcuttlefish_rild",
    "platform_app_all_virtualizationmanager", "priv_app_all_virtualizationmanager", "shell_virtualizationmanager",
    "socket_vsock_proxy", "storageproxyd_system", "tombstone_transmit", "traced",
    "trusty_security_vm_launcher_early_virtmgr", "untrusted_app_all_virtualizationmanager",
    "virtualizationmanager_vendor", "virtualizationservice", "virtualizationservice_virtualizationmanager",
    "vmlauncher_app",
})
# Of those, the ones whose rules have no listen: they connect.
VSOCK_TYPES_WITHOUT_LISTEN = frozenset({
    "aisealhostservice", "hal_graphics_composer_default", "libcuttlefish_rild", "storageproxyd_system",
    "tombstone_transmit",
})
# The one grant on the service's sockets that is not the service's: the platform's own rule
# (allow dumpstate domain (vsock_socket (getattr))), the vsock instance of its getattr rule for every socket
# class. Over the attribute domain it reaches the service as it reaches every domain. getattr is the permission
# to stat a socket and to read its local and peer address from a descriptor already held: no read, write,
# connect, bind, listen or accept, and no data. Only that rule is accepted, as the policy writes it (the source
# and target tokens, not the types a version suffix maps them to), and only from the platform's policy.
SMS_PLATFORM_VSOCK_GRANT = (b"dumpstate", b"domain", frozenset({"getattr"}))
# The ordinary application domains: what the service holds beyond the reviewed list is only what every one of
# them holds.
SMS_ORDINARY_APP_DOMAINS = frozenset({"untrusted_app", "platform_app", "priv_app", "system_app"})
# The domains that manage every application's data directories, and so reach the service's: the platform's own
# rules through the attributes of an application data type. The first image build with the service shows the
# set; a holder that is not named here fails the check until someone has looked at it.
SMS_DATA_MANAGERS = frozenset({
    "artd", "dex2oat", "dexopt_chroot_setup", "dumpstate", "init", "installd", "otapreopt_chroot", "otapreopt_slot",
    "system_server", "vold", "vold_prepare_subdirs", "zygote",
})
# service.adb.listen_addrs moves adbd's own listener: only init and these domains set it, and no adb shell or
# installed application does.
ADB_LISTEN_ADDRS_PROPERTY = "service.adb.listen_addrs"
ADB_LISTEN_ADDRS_SETTERS = frozenset({"charger", "init", "surfaceflinger", "system_app", "system_server"})
SEAPP_CONTEXT_FILES = (
    "SYSTEM/etc/selinux/plat_seapp_contexts",
    "SYSTEM_EXT/etc/selinux/system_ext_seapp_contexts",
    "PRODUCT/etc/selinux/product_seapp_contexts",
    "VENDOR/etc/selinux/vendor_seapp_contexts",
)


class CilSets:
    """The members of the types and attributes of a set of policies, with every set expression expanded.
    The names have their platform version suffixes removed, except with |exact|: then a type that carries
    one is its own type, and a compatibility mapping gives membership only where a set lists it."""

    def __init__(self, policies: tuple[bytes, ...], exact: bool = False) -> None:
        self.definitions: dict[str, list[object]] = {}
        for policy in policies:
            lists, expressions = cil_attribute_sets(policy, exact)
            for attribute, members in lists:
                self.definitions.setdefault(attribute, []).append(sorted(members))
            for attribute, members in expressions:
                self.definitions.setdefault(attribute, []).append(members)
        self._members: dict[str, frozenset[str]] = {}
        self._universe: frozenset[str] | None = None

    @property
    def universe(self) -> frozenset[str]:
        """Every type the policies name: the leaves of the attribute sets and the declared types."""
        if self._universe is None:
            names: set[str] = set()
            for definition in self.definitions.values():
                for members in definition:
                    names |= self._leaves(members)
            self._universe = frozenset(n for n in names if n not in self.definitions)
        return self._universe

    def _leaves(self, expression: object) -> set[str]:
        if isinstance(expression, str):
            return set() if expression in CIL_SET_OPERATORS else {expression}
        assert isinstance(expression, list)
        return {leaf for member in expression for leaf in self._leaves(member)}

    def members(self, name: str) -> frozenset[str]:
        """The types a name stands for: itself for a type, the expanded set for an attribute."""
        known = self._members.get(name)
        if known is not None:
            return known
        if name not in self.definitions:
            self._members[name] = frozenset({name})
            return self._members[name]
        self._members[name] = frozenset()  # a cycle adds nothing
        found: set[str] = set()
        for definition in self.definitions[name]:
            found |= self.expand(definition)
        self._members[name] = frozenset(found)
        return self._members[name]

    def expand(self, expression: object) -> frozenset[str]:
        if isinstance(expression, str):
            return self.members(expression)
        assert isinstance(expression, list)
        operator = expression[0] if expression and isinstance(expression[0], str) else None
        if operator not in CIL_SET_OPERATORS:
            return frozenset().union(*(self.expand(member) for member in expression))
        operands = [self.expand(operand) for operand in expression[1:]]
        if operator == "all":
            return self.universe
        if operator == "not":
            return self.universe - frozenset().union(*operands)
        if operator == "and":
            return frozenset.intersection(*operands) if operands else frozenset()
        if operator == "or":
            return frozenset().union(*operands)
        return frozenset(member for member in frozenset().union(*operands)
                         if sum(member in operand for operand in operands) == 1)


def cil_domain_rules(
    policies: tuple[bytes, ...], sets: CilSets, object_class: str
) -> list[tuple[frozenset[str], frozenset[str] | None, frozenset[str]]]:
    """The allow rules of an object class as (source types, target types, permissions); a target of
    None is the rule's own domain (self)."""
    wanted = object_class.encode()
    return [
        (
            sets.members(cil_type_name(source)),
            None if target == b"self" else sets.members(cil_type_name(target)),
            frozenset(permission.decode() for permission in permissions),
        )
        for policy in policies
        for source, target, rule_class, permissions in cil_rules(policy, CIL_ALLOW_RULE)
        if rule_class == wanted
    ]


def parse_seapp_contexts(text: str) -> list[dict[str, str]]:
    """The entries of seapp_contexts files, in the order they stand: key=value fields."""
    entries = []
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        entry = dict(field.split("=", 1) for field in line.split() if "=" in field)
        if entry:
            entries.append(entry)
    return entries


def seapp_context_for(entries: list[dict[str, str]], user: str, seinfo: str, name: str) -> dict[str, str] | None:
    """The entry the platform picks for a system-uid application that is neither privileged nor
    ephemeral: those that match, the most specific first (an exact user before a prefix, a seinfo, an exact
    name before a prefix before none), the earlier one of equals."""
    def rank(value: str | None, wanted: str) -> int | None:
        if value is None:
            return 0
        if value.endswith("*"):
            return 1 if wanted.startswith(value[:-1]) else None
        return 2 if value == wanted else None

    best: tuple[tuple[int, ...], int, dict[str, str]] | None = None
    for index, entry in enumerate(entries):
        if any(entry.get(key) not in (None, "false") for key in
               ("isSystemServer", "isEphemeralApp", "isPrivApp", "fromRunAs")):
            continue
        if any(key in entry for key in ("minTargetSdkVersion", "maxTargetSdkVersion", "isIsolatedComputeApp")):
            continue
        scores = (rank(entry.get("user"), user), 1 if entry.get("seinfo") == seinfo else (0 if "seinfo" not in entry else None),
                  rank(entry.get("name"), name))
        if None in scores:
            continue
        key = tuple(score for score in scores if score is not None)
        if best is None or key > best[0]:
            best = (key, index, entry)
    return best[2] if best else None


def check_sms_holder_policy(contexts: list[str], policies: tuple[bytes, ...], sets: "CilSets",
                            rules_of: Callable[[str], list["SmsRule"]], file_contexts: bytes,
                            build_variant: str) -> None:
    """The holder of port 5560 and the waker are the plain daemons that init starts, and the handoff between
    the holder and the service is the service's alone."""
    for domain, path in ((SMS_LISTENER, SMS_INIT_SERVICES[SMS_LISTENER][0]), (SMS_WAKE, SMS_INIT_SERVICES[SMS_WAKE][0])):
        if file_context_label(file_contexts, path) != f"u:object_r:{domain}_exec:s0":
            fail(f"system_ext file contexts do not label {path} as {domain}_exec")
        transitions = {
            (CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(1).decode()),
             CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(2).decode()))
            for policy in policies
            for match in CIL_TYPE_TRANSITION_RULE.finditer(policy)
            if CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(3).decode()) == domain
        }
        if transitions != {("init", f"{domain}_exec")}:
            fail(f"{domain} is entered by {sorted(transitions)}, not only by init through {domain}_exec")
        for rule in rules_of("process"):
            if rule.targets is not None and domain in rule.targets and "transition" in rule.permissions \
                    and rule.sources - {"init"}:
                fail(f"{sorted(rule.sources - {'init'})} may transition into {domain}: only init does")
    if file_context_label(file_contexts, SMS_SOCKET_PATH) != f"u:object_r:{SMS_SOCKET_TYPE}:s0":
        fail(f"system_ext file contexts do not label {SMS_SOCKET_PATH} as {SMS_SOCKET_TYPE}")

    # The holder: no framework client, no file, no other socket of its own.
    classes: set[str] = set()
    for object_class in sorted(SMS_HOLDER_FORBIDDEN_CLASSES):
        for rule in rules_of(object_class):
            if rule.source == SMS_LISTENER.encode():
                classes.add(object_class)
    if classes:
        fail(f"{SMS_LISTENER} holds rights of {sorted(classes)} classes: it is not the framework's client and owns "
             "no file")

    # The waker finds the activity manager and nothing else.
    found = set()
    for rule in rules_of("service_manager"):
        if rule.source == SMS_WAKE.encode() and rule.targets is not None and "find" in rule.permissions:
            found |= rule.targets
    if "activity_service" not in found:
        fail(f"{SMS_WAKE} may not find activity_service: it could not start the service")
    if found != {"activity_service"}:
        fail(f"{SMS_WAKE} may find {sorted(found)}; it needs only ['activity_service']")

    # In effect, whatever attribute grants it: what each of the two holds in the classes that reach the framework or
    # the network. The rules above judge the rules that name them; these judge every rule that stands for them,
    # among them the platform's grants to every domain.
    def held(domain: str, object_class: str) -> dict[str, set[str]]:
        rights: dict[str, set[str]] = {}
        for rule in rules_of(object_class):
            if domain in rule.sources:
                for target in (rule.targets if rule.targets is not None else {domain}):
                    rights.setdefault(target, set()).update(rule.permissions)
        return rights

    for domain, reviewed in ((SMS_LISTENER, SMS_PLATFORM_DOMAIN_GRANTS),
                             (SMS_WAKE, {"service_manager": SMS_WAKE_REVIEWED_LOOKUPS, "binder": SMS_WAKE_BINDER_RIGHTS})):
        if build_variant != "user":
            reviewed = {**reviewed, "binder": {**reviewed.get("binder", {}), "su": SMS_DEBUGGABLE_SU_BINDER_RIGHTS}}
        for object_class in sorted(SMS_DAEMON_NO_GRANT_CLASSES):
            rights = held(domain, object_class)
            if rights:
                fail(f"{domain} holds rights of the {object_class} class in effect ({sorted(rights)}): it is a plain "
                     "daemon, and no attribute or set expression may give it any")
        devices = held(domain, "chr_file")
        if "vndbinder_device" in devices:
            fail(f"{domain} may use the vendor binder device in effect: it is a plain daemon")
        for device, accepted_rights in SMS_BINDER_DEVICES.items():
            extra = devices.get(device, set()) - accepted_rights
            if extra:
                fail(f"{domain} holds {sorted(extra)} on {device} of the chr_file class in effect: only the "
                     f"platform's grants to every domain ({sorted(accepted_rights)}) are accepted")
        for object_class, accepted in reviewed.items():
            for target, permissions in sorted(held(domain, object_class).items()):
                extra = permissions - accepted.get(target, frozenset())
                if extra:
                    fail(f"{domain} holds {sorted(extra)} on {target} of the {object_class} class in effect: only "
                         f"the platform's grants to every domain ({sorted(accepted)}) are accepted"
                         + (", and the lookup of the activity manager" if domain == SMS_WAKE else ""))
        if build_variant != "user":
            su_rights = held(domain, "binder").get("su", set())
            if su_rights != SMS_DEBUGGABLE_SU_BINDER_RIGHTS:
                fail(f"{domain} holds {sorted(su_rights)} on su of the binder class in effect in a {build_variant} "
                     f"build: the platform gives every domain {sorted(SMS_DEBUGGABLE_SU_BINDER_RIGHTS)} there")

    # The handoff socket: init makes it and binds it and does not listen on it, so the holder listens and accepts on it
    # itself (a socket that is only bound polls as hung up and takes no connection).
    own: set[str] = set()
    for rule in rules_of("unix_stream_socket"):
        if SMS_LISTENER in rule.sources and (rule.targets is None or SMS_LISTENER in rule.targets):
            own |= rule.permissions
    if {"accept", "listen"} - own:
        fail(f"{SMS_LISTENER} may not {sorted({'accept', 'listen'} - own)} on its own unix socket: init binds the "
             "handoff socket and does not listen on it, so the holder could not take the service's connection")

    # The handoff: the service alone connects to the holder's unix socket, writes its file and takes the descriptor.
    for rule in rules_of("unix_stream_socket"):
        if rule.targets is not None and SMS_LISTENER in rule.targets and "connectto" in rule.permissions \
                and rule.sources - {SMS_DOMAIN}:
            fail(f"{sorted(rule.sources - {SMS_DOMAIN})} may connect to the socket of {SMS_LISTENER}: only "
                 f"{SMS_DOMAIN} does")
    if not any(rule.targets is not None and SMS_LISTENER in rule.targets and "connectto" in rule.permissions
               and SMS_DOMAIN in rule.sources for rule in rules_of("unix_stream_socket")):
        fail(f"{SMS_DOMAIN} may not connect to the socket of {SMS_LISTENER}: it could not take the port")
    writers: set[str] = set()
    for rule in rules_of("sock_file"):
        if rule.targets is not None and SMS_SOCKET_TYPE in rule.targets and rule.permissions & {"write", "append"}:
            writers |= rule.sources
    if SMS_DOMAIN not in writers:
        fail(f"{SMS_DOMAIN} may not write {SMS_SOCKET_TYPE}: it could not connect to the holder's socket")
    if writers != {SMS_DOMAIN}:
        fail(f"{sorted(writers - {SMS_DOMAIN})} may write {SMS_SOCKET_TYPE}: only {SMS_DOMAIN} does")
    users: set[str] = set()
    for rule in rules_of("fd"):
        if rule.targets is not None and SMS_LISTENER in rule.targets and "use" in rule.permissions:
            users |= rule.sources
    if SMS_DOMAIN not in users:
        fail(f"{SMS_DOMAIN} may not use the listening descriptor of {SMS_LISTENER}: it could not take the port")
    if users - {SMS_DOMAIN} - SMS_PLATFORM_FD_USERS:
        fail(f"{sorted(users - {SMS_DOMAIN} - SMS_PLATFORM_FD_USERS)} may use the descriptors of {SMS_LISTENER}")

    # The control property that starts the waker is the holder's to set, and init's (the platform lets init set every property).
    label = property_label(SMS_WAKE_PROPERTY, contexts)
    if label != SMS_WAKE_PROPERTY_TYPE:
        fail(f"the property contexts label {SMS_WAKE_PROPERTY} {label}, not {SMS_WAKE_PROPERTY_TYPE}")
    setters: set[str] = set()
    for policy in policies:
        for source, target, _object_class, permissions in cil_rules(policy, CIL_PROPERTY_SET_RULE):
            if b"set" in permissions and label in sets.members(cil_type_name(target)):
                setters |= sets.members(cil_type_name(source))
    if SMS_LISTENER not in setters:
        fail(f"{SMS_LISTENER} may not set {SMS_WAKE_PROPERTY}, labelled {label}: it could not start the waker")
    # init owns the property service, and the platform lets it set every property type (allow init property_type).
    if setters - {SMS_LISTENER, "init"}:
        fail(f"{sorted(setters - {SMS_LISTENER, 'init'})} may set {SMS_WAKE_PROPERTY}, labelled {label}: only "
             f"{SMS_LISTENER} does, and init, which owns the property service")
    check_sms_control_properties(contexts, policies, sets)


def check_sms_control_properties(contexts: list[str], policies: tuple[bytes, ...], sets: "CilSets") -> None:
    """Only init controls the holder and the waker, and the holder starts the waker: of every control message init
    accepts for either service, the domains that may set the property keys that authorise it are init, and for
    starting the waker the holder as well. init checks the legacy ctl.<service> key first for start, stop and
    restart, and the property ctl.<message>$<service> for every message, and acts when the requester may set
    either; a key without an entry of its own gets the platform's label, and ctl.<service> is ctl_default_prop,
    which system_app and others may set."""
    for key in SMS_CONTROL_OWN_KEYS:
        if not property_entry_is_exact(key, contexts):
            fail(f"the property contexts have no exact entry for {key}: the platform's label would decide who may "
                 "control the text message services")

    def setters_of(label: str) -> set[str]:
        found: set[str] = set()
        for policy in policies:
            for source, target, _object_class, permissions in cil_rules(policy, CIL_PROPERTY_SET_RULE):
                if b"set" in permissions and label in sets.members(cil_type_name(target)):
                    found |= sets.members(cil_type_name(source))
        return found

    for service in (SMS_LISTENER, SMS_WAKE):
        for message in SMS_CONTROL_MESSAGES:
            keys = ([f"ctl.{service}"] if message in SMS_LEGACY_CONTROL_MESSAGES else []) + [f"ctl.{message}${service}"]
            allowed = {SMS_LISTENER, "init"} if (service, message) == (SMS_WAKE, "start") else {"init"}
            for key in keys:
                label = property_label(key, contexts) or "default_prop"
                extra = setters_of(label) - allowed
                if extra:
                    fail(f"{sorted(extra)} may {message} {service} through {key}, labelled {label}: only "
                         f"{sorted(allowed)} may")

    # The holder's start of the waker is denied on the legacy key, which init checks first, and allowed on the
    # qualified one. The denial is the meant one and is silenced, or every wake logs an avc denial: the platform
    # silences only its own services' control types. The one rule that does it names the holder and the locked type
    # exactly, and no other property_service dontaudit of the product's policies reaches the holder (a rule over an
    # attribute would hide every denied property write of it); the platform's own, the first policy's, are its
    # control types for every domain.
    legacy = property_label(f"ctl.{SMS_WAKE}", contexts) or "default_prop"
    silenced = False
    for index, policy in enumerate(policies):
        for source, target, object_class, permissions in cil_rules(policy, CIL_DONTAUDIT_RULE):
            if index == 0 or object_class != b"property_service" or SMS_LISTENER not in sets.members(cil_type_name(source)):
                continue
            if (source.decode(), target.decode(), set(permissions)) != (SMS_LISTENER, SMS_LOCKED_PROPERTY_TYPE, {b"set"}):
                fail(f"policy silences a property_service denial of {SMS_LISTENER} beyond its start of the waker: "
                     f"(dontaudit {source.decode()} {target.decode()} (property_service ({b' '.join(permissions).decode()}))) "
                     f"may be only (dontaudit {SMS_LISTENER} {SMS_LOCKED_PROPERTY_TYPE} (property_service (set)))")
            silenced = True
    if not silenced or legacy != SMS_LOCKED_PROPERTY_TYPE:
        fail(
            f"policy does not silence the denial of {SMS_LISTENER} setting the legacy key ctl.{SMS_WAKE}, labelled "
            f"{legacy}, which init checks before the qualified key that allows the start: it needs "
            f"(dontaudit {SMS_LISTENER} {SMS_LOCKED_PROPERTY_TYPE} (property_service (set))) "
            "(overlay/sepolicy/system_ext/private/kernarium_sms_listener.te)"
        )


class SmsRule(NamedTuple):
    """An allow rule as the text-message check reads it: the types its source and target stand for (a target of
    None is the rule's own domain), its permissions, the source and target as the policy writes them, and
    whether it stands in the platform's policy."""

    sources: frozenset[str]
    targets: frozenset[str] | None
    permissions: frozenset[str]
    source: bytes
    target: bytes
    from_platform: bool


def check_sms_policy(contexts: list[str], policies: tuple[bytes, ...], seapp_contexts: str,
                     file_contexts: bytes, build_variant: str) -> None:
    """Only the host can cause a text message to be delivered: of the types that may create, bind or listen
    on a vsock socket exactly the reviewed ones and the holder of port 5560 may, whatever attribute grants it;
    nothing else holds the holder's sockets or the service's data, except the platform's own getattr for
    dumpstate (SMS_PLATFORM_VSOCK_GRANT), and the service holds only accept, read, write and shutdown on the
    sockets of the holder, none on its own, and nothing beyond what every ordinary application domain holds
    (SMS_ORDINARY_APP_DOMAINS) and the reviewed list (it finds only the services it needs); the holder never
    accepts, reads, writes or connects a vsock socket and is not the framework's client; init alone enters the
    holder's and the waker's domains, the handoff socket is the service's alone to connect to, and the control
    property that starts the waker is set by the holder and by init only; the package selects the service's domain and
    no other package does; and the shell cannot move adbd's own listener. Every grant counts, through
    attributes and set expressions. The first policy is the platform's."""
    sets = CilSets(policies)
    if not sets.members("appdomain") or not sets.members("coredomain"):
        fail("the policy lists no type for appdomain or coredomain: the check could not tell who runs applications")

    def rules_of(object_class: str) -> list[SmsRule]:
        """The allow rules of a class, in the order of the policies, the platform's first."""
        wanted = object_class.encode()
        return [
            SmsRule(
                sets.members(cil_type_name(source)),
                None if target == b"self" else sets.members(cil_type_name(target)),
                frozenset(permission.decode() for permission in permissions),
                source,
                target,
                index == 0,
            )
            for index, policy in enumerate(policies)
            for source, target, rule_class, permissions in cil_rules(policy, CIL_ALLOW_RULE)
            if rule_class == wanted
        ]

    vsock = rules_of("vsock_socket")

    def holders(wanted: set[str]) -> set[str]:
        found: set[str] = set()
        for rule in vsock:
            if wanted & rule.permissions:
                found |= rule.sources if rule.targets is None else rule.sources & rule.targets
        return found

    creators = holders({"create", "bind", "listen"})
    reviewed = VSOCK_SOCKET_TYPES | {SMS_LISTENER}
    if creators != reviewed:
        fail(
            "the types that may create, bind or listen on a vsock socket are not the reviewed ones "
            f"(unreviewed: {sorted(creators - reviewed)}; reviewed and missing: {sorted(reviewed - creators)})"
        )
    listeners = holders({"listen"})
    reviewed_listeners = (VSOCK_SOCKET_TYPES - VSOCK_TYPES_WITHOUT_LISTEN) | {SMS_LISTENER}
    if listeners != reviewed_listeners:
        fail(
            "the types that may listen on a vsock socket are not the reviewed ones "
            f"(unreviewed: {sorted(listeners - reviewed_listeners)}; "
            f"reviewed and missing: {sorted(reviewed_listeners - listeners)})"
        )
    shut_out = {"shell", "system_app", "platform_app", "priv_app"} | set(sets.members("untrusted_app_all"))
    if creators & shut_out:
        fail(f"{sorted(creators & shut_out)} may create, bind or listen on a vsock socket")

    # An application domain inherits the platform's grants to every one of them, among them vsock rights: the
    # holder and the waker are plain daemons.
    if SMS_LISTENER in sets.members("appdomain") or SMS_WAKE in sets.members("appdomain"):
        fail(f"{SMS_LISTENER} and {SMS_WAKE} are plain daemons: neither is an application domain")

    exact = CilSets(policies, exact=True)

    def specific(rule: SmsRule) -> bool:
        """The service's own rule, not one it inherits. The service inherits a rule when the platform's policy
        gives it to every ordinary application domain through that one rule, one source set holding all four
        and the service, each by its own name (a type that differs from one of them by a version suffix is
        another type); a rule that names the service without all four, and every such rule of another
        policy file (ours), is its own and must be on the reviewed list."""
        return SMS_DOMAIN in rule.sources and not (
            rule.from_platform and exact.members(rule.source.decode()) >= SMS_ORDINARY_APP_DOMAINS | {SMS_DOMAIN}
        )

    # The holder's sockets: nothing but the holder and the service holds a permission on them (the platform's
    # grant to dumpstate apart), the service only what it needs to accept from the descriptor it is given, the
    # holder only what makes and binds the socket. The service owns no vsock socket, so nothing holds a
    # permission on one of its own either, and what it holds on the sockets of other types is only what every
    # ordinary application domain holds.
    for rule in vsock:
        if rule.from_platform and (rule.source, rule.target, rule.permissions) == SMS_PLATFORM_VSOCK_GRANT:
            continue
        if rule.targets is not None and SMS_DOMAIN in rule.targets and rule.sources - {SMS_DOMAIN}:
            fail(f"{sorted(rule.sources - {SMS_DOMAIN})} hold {sorted(rule.permissions)} on a vsock socket of "
                 f"{SMS_DOMAIN}")
        if rule.targets is not None and rule.targets - {SMS_DOMAIN, SMS_LISTENER} and specific(rule):
            fail(f"{SMS_DOMAIN} holds {sorted(rule.permissions)} on the vsock sockets of "
                 f"{sorted(rule.targets - {SMS_DOMAIN, SMS_LISTENER})}: only what every ordinary application domain "
                 "holds is allowed")
        if SMS_DOMAIN in rule.sources and (rule.targets is None or rule.targets == {SMS_DOMAIN}) and specific(rule):
            fail(f"{SMS_DOMAIN} holds {sorted(rule.permissions)} on its own vsock sockets: it makes none, it accepts "
                 f"from the descriptor that {SMS_LISTENER} passes it")
        if rule.targets is not None and SMS_LISTENER in rule.targets:
            if rule.sources - {SMS_LISTENER, SMS_DOMAIN}:
                fail(f"{sorted(rule.sources - {SMS_LISTENER, SMS_DOMAIN})} hold {sorted(rule.permissions)} on a vsock "
                     f"socket of {SMS_LISTENER}")
            if SMS_DOMAIN in rule.sources and rule.permissions - SMS_SERVICE_VSOCK_PERMISSIONS:
                fail(f"{SMS_DOMAIN} holds {sorted(rule.permissions - SMS_SERVICE_VSOCK_PERMISSIONS)} on the vsock "
                     f"sockets of {SMS_LISTENER}: only {sorted(SMS_SERVICE_VSOCK_PERMISSIONS)} are allowed")
        if SMS_LISTENER in rule.sources:
            if rule.targets is not None and rule.targets != {SMS_LISTENER}:
                fail(f"{SMS_LISTENER} holds {sorted(rule.permissions)} on the vsock sockets of "
                     f"{sorted(rule.targets - {SMS_LISTENER})}: it connects to none")
            if rule.permissions - SMS_HOLDER_VSOCK_PERMISSIONS:
                fail(f"{SMS_LISTENER} holds {sorted(rule.permissions - SMS_HOLDER_VSOCK_PERMISSIONS)} on its vsock "
                     "sockets: it never accepts, reads, writes, shuts down or connects")
        if rule.source == SMS_WAKE.encode():
            fail(f"{SMS_WAKE} holds {sorted(rule.permissions)} on a vsock socket: the waker touches none")

    for attribute in ("coredomain", "appdomain", "unconstrained_vsock_violators"):
        if SMS_DOMAIN not in sets.members(attribute):
            fail(f"{SMS_DOMAIN} is not in {attribute}")
    # The platform gives a user's directories that user's categories at run time, in the preparation of the user's
    # storage (/data/user_de/<id> from the second boot of a data image on, /data/data from the first), not through its
    # file contexts. The multi-level constraint on searching a directory that is not application data lets a subject
    # through only when its level dominates, or it is mlstrustedsubject. The service has no categories and every
    # system application is trusted: without it the service cannot reach its record at all.
    trusted = sets.members("mlstrustedsubject")
    if SMS_DOMAIN not in trusted:
        fail(f"{SMS_DOMAIN} is not in mlstrustedsubject: the directories of the user it runs for carry categories "
             "that its level lacks, so it could not search the path of its record")
    for daemon in (SMS_LISTENER, SMS_WAKE):
        if daemon in trusted:
            fail(f"{daemon} is in mlstrustedsubject: it searches no directory of the user")
    for attribute in ("coredomain", "unconstrained_vsock_violators"):
        if SMS_LISTENER not in sets.members(attribute):
            fail(f"{SMS_LISTENER} is not in {attribute}")
    if SMS_WAKE not in sets.members("coredomain"):
        fail(f"{SMS_WAKE} is not in coredomain")
    if SMS_WAKE in sets.members("unconstrained_vsock_violators"):
        fail(f"{SMS_WAKE} is in unconstrained_vsock_violators: the waker touches no vsock socket")
    found: set[str] = set()
    for rule in rules_of("service_manager"):
        if rule.targets is not None and "find" in rule.permissions and specific(rule):
            found |= rule.targets
    if found - SMS_SERVICES:
        fail(f"{SMS_DOMAIN} may find {sorted(found - SMS_SERVICES)}; it needs only {sorted(SMS_SERVICES)}")
    for object_class in ("file", "dir", "lnk_file", "sock_file", "fifo_file", "chr_file", "blk_file"):
        for rule in rules_of(object_class):
            if rule.targets is not None and "system_app_data_file" in rule.targets and specific(rule):
                fail(f"{SMS_DOMAIN} has a rule on system_app_data_file ({sorted(rule.permissions)}): "
                     "its data has a type of its own")

    # Who may touch the service's data, through any attribute it carries.
    data_holders: set[str] = set()
    touching = {"read", "write", "open", "create", "rename", "unlink"}
    for object_class in ("file", "dir", "lnk_file", "sock_file", "fifo_file", "chr_file", "blk_file"):
        for sources, targets, permissions in cil_domain_rules(policies, sets, object_class):
            if targets is None or SMS_DATA_TYPE not in targets:
                continue
            data_holders |= sources
            if sources & shut_out and permissions & touching:
                fail(f"{sorted(sources & shut_out)} may {sorted(permissions & touching)} {SMS_DATA_TYPE}")
    if data_holders - {SMS_DOMAIN} - SMS_DATA_MANAGERS:
        fail(f"{sorted(data_holders - {SMS_DOMAIN} - SMS_DATA_MANAGERS)} hold rights on {SMS_DATA_TYPE}: only "
             f"{SMS_DOMAIN} and the managers of application data may")

    check_sms_holder_policy(contexts, policies, sets, rules_of, file_contexts, build_variant)

    # service.adb.listen_addrs: who may set it.
    label = property_label(ADB_LISTEN_ADDRS_PROPERTY, contexts)
    if label is None:
        fail(f"the property contexts do not label {ADB_LISTEN_ADDRS_PROPERTY}")
    setters: set[str] = set()
    for policy in policies:
        for source, target, _object_class, permissions in cil_rules(policy, CIL_PROPERTY_SET_RULE):
            if b"set" in permissions and label in sets.members(cil_type_name(target)):
                setters |= sets.members(cil_type_name(source))
    if setters - ADB_LISTEN_ADDRS_SETTERS:
        fail(f"{sorted(setters - ADB_LISTEN_ADDRS_SETTERS)} may set {ADB_LISTEN_ADDRS_PROPERTY}, labelled {label}: "
             "that moves adbd's own listener")
    if setters & ((set(sets.members("appdomain")) | {"shell"}) - {"system_app"}):
        fail(f"an adb shell or an application may set {ADB_LISTEN_ADDRS_PROPERTY}")

    # Which package selects which domain.
    entries = parse_seapp_contexts(seapp_contexts)
    selected = seapp_context_for(entries, "system", "platform", SMS_PACKAGE)
    if selected is None or (selected.get("domain"), selected.get("type")) != (SMS_DOMAIN, SMS_DATA_TYPE):
        fail(f"seapp_contexts do not select {SMS_DOMAIN} and {SMS_DATA_TYPE} for {SMS_PACKAGE}: {selected}")
    for package in ("com.android.settings", "com.android.systemui", "android.kernarium.other"):
        other = seapp_context_for(entries, "system", "platform", package)
        if other is None or other.get("domain") != "system_app":
            fail(f"seapp_contexts select {other} for {package}: it stays in system_app")


def check_sms_service_files(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    """The text message service runs only while messages are in flight: its application has one service that no
    application can start and is not persistent; the holder of its vsock port starts with its class and the waker
    only through the holder's control property, and the waker runs one command."""
    rc = read_small(archive, infos[SMS_INIT_RC]).decode("utf-8", errors="strict")
    services = init_services(rc)
    if sorted(services) != sorted(SMS_INIT_SERVICES):
        fail(f"the text message init script must define the services {sorted(SMS_INIT_SERVICES)}: {sorted(services)}")
    for name, (executable, options) in SMS_INIT_SERVICES.items():
        found_executable, found_options = services[name]
        if found_executable != executable:
            fail(f"the text message service {name} runs {found_executable}, not {executable}")
        if sorted(found_options) != sorted(options):
            fail(f"the text message service {name} has the options {found_options}, not exactly {list(options)}: it "
                 "takes its domain from its file label and holds no capability")
    if init_actions(rc):
        fail("the text message init script has a trigger: the holder starts with its class and the waker only "
             "through its control property")
    code = [
        line.strip() for line in read_small(archive, infos[SMS_WAKE_SCRIPT]).decode("utf-8", errors="strict").splitlines()
        if line.strip() and not line.strip().startswith("#")
    ]
    if code != [SMS_WAKE_COMMAND]:
        fail(f"{SMS_WAKE_SCRIPT} must run exactly '{SMS_WAKE_COMMAND}': {code}")
    try:
        with zipfile.ZipFile(io.BytesIO(read_small(archive, infos[SMS_APK], 4 << 20))) as apk:
            elements = manifest_elements(apk.read("AndroidManifest.xml"))
    except (KeyError, OSError, ValueError, struct.error, IndexError, zipfile.BadZipFile) as problem:
        fail(f"{SMS_APK} has no readable manifest: {problem}")
    names = [name for name, _ in elements]
    first = {name: attributes for name, attributes in reversed(elements)}
    if names[:1] != ["manifest"] or first["manifest"].get("package") != SMS_PACKAGE:
        fail(f"{SMS_APK} is the package {first.get('manifest', {}).get('package')}, not {SMS_PACKAGE}")
    if first["manifest"].get("sharedUserId") != "android.uid.system":
        fail(f"{SMS_APK} does not share the system uid: the protected SMS broadcasts need it")
    for kind, wanted in (("application", 1), ("service", 1)):
        if names.count(kind) != wanted:
            fail(f"{SMS_APK} has {names.count(kind)} {kind} elements, not {wanted}: the application has one service "
                 "and nothing else")
    # The build injects one <uses-sdk> into every packaged manifest (the SDK levels of the platform it is built with);
    # it is the only element beside the manifest, the application and its service, and it carries nothing but levels.
    if names.count("uses-sdk") > 1:
        fail(f"{SMS_APK} has {names.count('uses-sdk')} uses-sdk elements, not at most 1")
    allowed = {"manifest", "application", "service", "uses-sdk"}
    if set(names) - allowed:
        fail(f"{SMS_APK} has {sorted(set(names) - allowed)}: the application has one service, no activity, receiver, "
             "provider, filter or permission request, so that nothing but the holder's wake reaches it")
    if "uses-sdk" in first and set(first["uses-sdk"]) - SMS_USES_SDK_ATTRIBUTES:
        fail(f"{SMS_APK}: the uses-sdk element sets {sorted(set(first['uses-sdk']) - SMS_USES_SDK_ATTRIBUTES)}: it "
             "carries SDK levels and nothing else")
    application = first["application"]
    if "persistent" in application:
        fail(f"{SMS_APK}: the application sets persistent: nothing may run between messages")
    if "name" in application:
        fail(f"{SMS_APK}: the application names a class of its own")
    service = first["service"]
    if manifest_component_class(SMS_PACKAGE, service.get("name")) != f"{SMS_PACKAGE}.HostService":
        fail(f"{SMS_APK}: the service is {service.get('name')!r}, not .HostService")
    if service.get("exported") is not False:
        fail(f"{SMS_APK}: the service's exported is {service.get('exported')!r}, not false")
    for attribute in ("permission", "process", "enabled", "isolatedProcess", "externalService", "foregroundServiceType",
                      "useAppZygote", "directBootAware"):
        if attribute in service:
            fail(f"{SMS_APK}: the service sets {attribute}")


def check_adbd_launch_policy(contexts: list[str], policies: tuple[bytes, ...]) -> None:
    """adbd may read the launch properties and the debugging setting it acts on, and no
    application may read the installation's adb key."""
    key_label = property_label(ADB_KEY_PROPERTY, contexts) or "default_prop"
    if key_label != ADB_KEY_PROPERTY_TYPE:
        fail(
            f"{ADB_KEY_PROPERTY} is labelled {key_label}, not {ADB_KEY_PROPERTY_TYPE} "
            "(overlay/sepolicy/system_ext/private/property_contexts): every application could read "
            "the key that the host makes for the installation"
        )
    members = {
        member
        for policy in policies
        for attribute in ADB_KEY_APP_ATTRIBUTES
        for member in cil_attribute_members(policy, attribute)
    }
    if not any(cil_attribute_members(policy, "appdomain") for policy in policies):
        fail("the policy lists no type for appdomain: the check could not tell who runs applications")
    for domain in (*ADB_KEY_APP_DOMAINS, *sorted(members - set(ADB_KEY_APP_DOMAINS))):
        if cil_allows_file_read(policies, domain, key_label):
            fail(
                f"{domain} may read {ADB_KEY_PROPERTY}, labelled {key_label}: an application could "
                "read the key that the host makes for the installation"
            )
    for name in ADBD_LAUNCH_PROPERTIES + ADBD_DEBUGGING_PROPERTIES:
        label = property_label(name, contexts) or "default_prop"
        if not cil_allows_file_read(policies, "adbd", label):
            fail(f"adbd may not read {name}, labelled {label}: it would ignore it")


# The prefixes that init accepts in a vendor script without asking the policy
# (system/core/init/action_parser.cpp, IsActionableProperty).
INIT_PARTNER_PREFIXES = (
    "init.svc.vendor.", "ro.vendor.", "persist.vendor.", "vendor.", "init.svc.odm.", "ro.odm.",
    "persist.odm.", "odm.", "ro.boot.",
)


def check_vendor_init_triggers(
    actions: dict[str, list[str]], contexts: list[str], policies: tuple[bytes, ...], where: str = ""
) -> None:
    """Every property that an init trigger of a vendor or odm script names is readable by vendor_init.

    Init parses such a script in the vendor_init subcontext and drops a trigger
    whose property that domain may not read ("unexported property trigger"), so a
    service that only such a trigger starts never runs. The platform's
    neverallow keeps vendor_init off system_internal_prop types, and init.svc.<name>
    is one of them for every service but a few.
    """
    for trigger in actions:
        for name in INIT_PROPERTY_TRIGGER.findall(trigger):
            if name.startswith(INIT_PARTNER_PREFIXES):
                continue
            label = property_label(name, contexts) or "default_prop"
            if not cil_allows_file_read(policies, "vendor_init", label):
                fail(
                    f"{where}the trigger 'on {trigger}' names {name}, labelled {label}, which "
                    "vendor_init may not read; init drops the trigger"
                )


def cil_allows_property_set(policies: tuple[bytes, ...], domain: str, label: str) -> bool:
    """Whether a domain may set the properties of a label, directly or through attributes."""
    setters = cil_types_and_attributes(policies, domain)
    settable = cil_types_and_attributes(policies, label)
    return any(
        cil_type_name(source) in setters and cil_type_name(target) in settable and b"set" in permissions
        for policy in policies
        for source, target, _object_class, permissions in cil_rules(policy, CIL_PROPERTY_SET_RULE)
    )


def check_venus_option_policy(contexts: list[str], policies: tuple[bytes, ...]) -> None:
    """vendor_init may set the Venus driver option, and every domain that loads the driver may read it."""
    label = property_label(VENUS_PERF_PROPERTY, contexts) or "default_prop"
    if not cil_allows_property_set(policies, "vendor_init", label):
        fail(
            f"vendor_init may not set {VENUS_PERF_PROPERTY}, labelled {label}: Venus would "
            "allocate asynchronously"
        )
    for domain in VENUS_DRIVER_DOMAINS:
        if not cil_allows_file_read(policies, domain, label):
            fail(
                f"{domain} may not read {VENUS_PERF_PROPERTY}, labelled {label}: Venus would "
                "allocate asynchronously there"
            )


# lmkd's GPU-aware reclaim (route p0027): while the virtio-gpu driver reports host graphics memory
# pressure in this sysfs file, lmkd ends the cached app that holds the most of it. The product turns
# it on with a property; the binary, the property's label and lmkd's access to the file's type are
# what make that work, and none of them fails loudly when it is missing.
LMKD_BINARY = "SYSTEM/bin/lmkd"
LMKD_GPU_RECLAIM_PROPERTY = "ro.lmk.gpu_reclaim"
LMKD_GPU_REPORT_PATH = "/sys/class/drm/card0/gpu_memory_report"
LMKD_GPU_REPORT_GENFS_PATH = "/devices/virtio-uml-cmdline/virtio-uml.0/drm/card0/gpu_memory_report"
LMKD_GPU_REPORT_TYPE = "sysfs_gpu_memory"
# The directories on the way to the file carry the virtio-gpu device's own type.
LMKD_GPU_DEVICE_TYPE = "sysfs_gpu"
# What only the p0027 topic puts in lmkd.
LMKD_GPU_MARKERS = (
    LMKD_GPU_RECLAIM_PROPERTY.encode(),
    LMKD_GPU_REPORT_PATH.encode(),
    b"graphics memory pressure",
)
CIL_GENFS_RULE = re.compile(
    rb'^\(genfscon sysfs "([^"]*)" (?:\w+ )?\(\S+ \S+ (\S+) ', re.MULTILINE
)


def check_lmkd_gpu_reclaim(archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]) -> None:
    """lmkd holds the GPU reclaim of route p0027, and the product turns it on."""
    info = infos.get(LMKD_BINARY)
    if info is None or info.is_dir():
        fail(f"{LMKD_BINARY} is missing")
    payload = read_small(archive, info, 64 << 20)
    for marker in LMKD_GPU_MARKERS:
        if marker not in payload:
            fail(
                f"{LMKD_BINARY} lacks {marker.decode()!r}: the p0027 topic is missing from the "
                "build, so lmkd would never end the app that holds the host's graphics memory"
            )
    values = [
        line.partition("=")[2].strip()
        for line in read_small(archive, infos["PRODUCT/etc/build.prop"])
        .decode("utf-8", errors="strict")
        .splitlines()
        if line.partition("=")[0] == LMKD_GPU_RECLAIM_PROPERTY
    ]
    if values != ["true"]:
        fail(f"unexpected product property {LMKD_GPU_RECLAIM_PROPERTY}: {values}")


def cil_allows(
    policies: tuple[bytes, ...], domain: str, label: str, object_class: str, permissions: set[str]
) -> bool:
    """Whether a domain holds every one of the permissions of an object class on a label, summed
    over the allow rules that reach it directly or through attributes."""
    subjects = cil_types_and_attributes(policies, domain)
    objects = cil_types_and_attributes(policies, label)
    granted: set[str] = set()
    # The pattern's class group is \w+ over bytes: ASCII, so equal bytes are equal names.
    wanted = object_class.encode()
    for policy in policies:
        for source, target, rule_class, rule_permissions in cil_rules(policy, CIL_ALLOW_RULE):
            if (
                rule_class == wanted
                and cil_type_name(source) in subjects
                and cil_type_name(target) in objects
            ):
                granted.update(permission.decode() for permission in rule_permissions)
    return permissions <= granted


def check_lmkd_gpu_policy(
    contexts: list[str], policies: tuple[bytes, ...], vendor_policy: bytes
) -> None:
    """lmkd may read the setting, the report file has its own sysfs type that the vendor's
    genfs_contexts gives it, and lmkd may walk to the file and read and write it."""
    label = property_label(LMKD_GPU_RECLAIM_PROPERTY, contexts) or "default_prop"
    if not cil_allows_file_read(policies, "lmkd", label):
        fail(
            f"lmkd may not read {LMKD_GPU_RECLAIM_PROPERTY}, labelled {label}: it would keep the "
            "GPU reclaim off"
        )
    if "sysfs_type" not in cil_types_and_attributes(policies, LMKD_GPU_REPORT_TYPE):
        fail(
            f"{LMKD_GPU_REPORT_TYPE} is not a sysfs type of the policy: the report file cannot "
            "be labelled"
        )
    genfs = {
        match.group(1).decode(): CIL_PLATFORM_VERSION_SUFFIX.sub("", match.group(2).decode())
        for match in CIL_GENFS_RULE.finditer(vendor_policy)
    }
    if genfs.get(LMKD_GPU_REPORT_GENFS_PATH) != LMKD_GPU_REPORT_TYPE:
        fail(
            f"the vendor policy does not label {LMKD_GPU_REPORT_GENFS_PATH} {LMKD_GPU_REPORT_TYPE} "
            f"(it has {genfs.get(LMKD_GPU_REPORT_GENFS_PATH)}): lmkd could not open the report"
        )
    if not cil_allows(policies, "lmkd", LMKD_GPU_REPORT_TYPE, "file", {"open", "read", "write"}):
        fail(f"lmkd may not open, read and write {LMKD_GPU_REPORT_TYPE}: the GPU reclaim stays off")
    if not cil_allows(policies, "lmkd", LMKD_GPU_DEVICE_TYPE, "dir", {"search"}):
        fail(
            f"lmkd may not search {LMKD_GPU_DEVICE_TYPE} directories: it could not reach the "
            "report file under the virtio-gpu device"
        )


# The virtio-gpu device paces the display with a vblank timer, so the present fences of
# drm_hwcomposer are reliable. The inherited Cuttlefish property marks them unreliable, which
# keeps SurfaceFlinger from taking present timestamps from them (service.sf.present_timestamp)
# and the Vulkan loader from exposing VK_GOOGLE_display_timing.
PRESENT_FENCE_PROPERTY = "ro.vendor.hwc.drm.present_fence_not_reliable"
FALSE_PROPERTY_VALUES = {"0", "n", "no", "off", "false"}
# The inherited Cuttlefish configuration adds ro.adb.secure=0 to the product partition of a user
# build, and the product build.prop is read after system's and system_ext's ro.adb.secure=1, so
# the guest reads 0. adbd takes its policy from ro.boot.kernarium.adb_auth, but Settings hides
# "Revoke USB debugging authorizations" when the property is false: the user could no longer
# revoke the computers they allowed. The Cuttlefish topic (route p0001) drops the entry when the
# product sets UML_ANDROID_KEEP_ADB_SECURE.
ADB_SECURE_PROPERTY = "ro.adb.secure"
# Incremental installs (adb install --incremental, the Incremental FS service) need this
# vendor property and the kernel's incfs.
INCREMENTAL_PROPERTY = "ro.incremental.enable"


def parse_properties(payload: str) -> dict[str, str]:
    properties: dict[str, str] = {}
    for line in payload.splitlines():
        key, separator, value = line.partition("=")
        if separator and not key.startswith("#"):
            properties[key] = value.strip()
    return properties


def check_property_file(archive: zipfile.ZipFile, info: zipfile.ZipInfo) -> None:
    if info.file_size > 1 << 20:
        fail(f"property file is unexpectedly large: {info.filename}")
    payload = archive.read(info).decode("utf-8", errors="strict")
    for key, value in parse_properties(payload).items():
        if key.startswith("ro.virtual_ab."):
            fail(f"Virtual A/B property is forbidden in {info.filename}: {key}")
        if key == "ro.build.ab_update" and value == "true":
            fail(f"A/B update property is enabled in {info.filename}")
        abi_property = ABI_PROPERTY.fullmatch(key)
        if abi_property is not None:
            expected = EXPECTED_ABIS[abi_property.group(1)]
            if value != expected:
                fail(f"unexpected ABI in {info.filename}: {key}={value!r}, not {expected!r}")
        if key == "ro.zygote" and value != VENDOR_MULTILIB_PROPERTIES["ro.zygote"]:
            fail(f"unexpected zygote in {info.filename}: {value}")
        if key == OVERLAY_THEME_PROPERTY:
            fail(
                f"{info.filename} sets {key}={value}: a vendor overlay theme enables overlays at "
                "boot, and the navigation mode overlays are the platform's to choose"
            )
        if key == MOTION_ORIENTATION_PROPERTY:
            fail(
                f"{info.filename} sets {key}={value}: the launch alone states whether the host feeds the "
                "device-orientation sensor"
            )
        if key == MOTION_COMPASS_PROPERTY:
            fail(
                f"{info.filename} sets {key}={value}: the launch alone states whether the host has a "
                "magnetometer, which lists the sensor and declares the compass feature"
            )
        if key == "persist.sys.usb.config":
            fail(
                f"{info.filename} sets {key}={value}: USB debugging is off on a fresh data "
                "partition, and AdbService records the user's choice there"
            )
        if key == ADB_SECURE_PROPERTY and value.lower() in FALSE_PROPERTY_VALUES:
            fail(
                f"{info.filename} sets {key}={value}: adb authorization is the stock one, and "
                "Settings hides the revocation of allowed computers when the property is false; "
                "the Cuttlefish topic (route p0001) drops the inherited Cuttlefish value at its source"
            )
        if key == PRESENT_FENCE_PROPERTY and value.lower() not in FALSE_PROPERTY_VALUES:
            fail(
                f"{info.filename} marks the present fences unreliable: {key}={value}; "
                "the virtio-gpu display is paced by a vblank timer"
            )


def check_multilib_properties(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    """The partitions state both ABIs, and the vendor describes the second."""
    for name, partition in ABI_LIST_PROPERTY_FILES.items():
        properties = parse_properties(
            read_small(archive, infos[name]).decode("utf-8", errors="strict")
        )
        for suffix in ("abilist", "abilist32", "abilist64"):
            key = f"ro.{partition}.product.cpu.{suffix}"
            if properties.get(key) != EXPECTED_ABIS[suffix]:
                fail(f"{name} lacks {key}={EXPECTED_ABIS[suffix]}: {properties.get(key)!r}")
    vendor = parse_properties(
        read_small(archive, infos["VENDOR/build.prop"]).decode("utf-8", errors="strict")
    )
    for key, expected in VENDOR_MULTILIB_PROPERTIES.items():
        if vendor.get(key) != expected:
            fail(f"VENDOR/build.prop lacks {key}={expected}: {vendor.get(key)!r}")
    if vendor.get(INCREMENTAL_PROPERTY) != "1":
        fail(f"VENDOR/build.prop lacks {INCREMENTAL_PROPERTY}=1: {vendor.get(INCREMENTAL_PROPERTY)!r}")
    system = parse_properties(
        read_small(archive, infos["SYSTEM/build.prop"]).decode("utf-8", errors="strict")
    )
    if system.get("ro.product.cpu.abi") != EXPECTED_ABIS["abi"]:
        fail(f"SYSTEM/build.prop lacks ro.product.cpu.abi=arm64-v8a: {system.get('ro.product.cpu.abi')!r}")


def check_erofs_policy(
    archive: zipfile.ZipFile,
    infos: dict[str, zipfile.ZipInfo],
    misc_info: dict[str, str],
) -> None:
    """The images were built with the product's one compressor and cluster size."""
    compressor, pcluster_size = erofs_parameters()
    if misc_info.get("erofs_default_compressor") != compressor:
        fail(
            f"erofs_default_compressor must be {compressor}: "
            f"{misc_info.get('erofs_default_compressor')!r}"
        )
    if misc_info.get("erofs_pcluster_size") != pcluster_size:
        fail(
            f"erofs_pcluster_size must be {pcluster_size}: "
            f"{misc_info.get('erofs_pcluster_size')!r}"
        )
    for key in sorted(misc_info):
        if key in EROFS_FORBIDDEN_POLICY or EROFS_PARTITION_POLICY.match(key):
            fail(f"EROFS policy beyond the product's two parameters is set: {key}")
    hints = infos.get(EROFS_COMPRESS_HINTS_ENTRY)
    if hints is None:
        fail(f"{EROFS_COMPRESS_HINTS_ENTRY} is missing: the .apex files would be compressed")
    tracked_hints = (android_image.ANDROID_ROOT / "overlay/config/erofs_compress_hints.txt").read_bytes()
    if read_small(archive, hints) != tracked_hints:
        fail("EROFS compress hints differ from the tracked product file")
    if misc_info.get("erofs_default_compress_hints") is None:
        fail("erofs_default_compress_hints is not set")
    for partition in READ_ONLY_PARTITIONS:
        name = f"IMAGES/{partition}.img"
        with archive.open(infos[name]) as source:
            prefix = android_image.read_image_prefix(
                source,
                android_image.EROFS_SUPERBLOCK_OFFSET + android_image.EROFS_SUPERBLOCK_SIZE,
                error,
            )
        check_erofs_superblock(name, prefix)


@functools.cache
def erofs_parameters() -> tuple[str, str]:
    return android_image.tracked_erofs_parameters(error)


def check_erofs_superblock(name: str, prefix: bytes) -> None:
    """The image is EROFS on 4 KiB blocks, compressed with the product's algorithm alone and
    with clusters larger than a block, and uses none of the extended layout options."""
    compressor = erofs_parameters()[0]
    superblock = android_image.erofs_superblock(prefix, name, error)
    if superblock.block_size_bits != 12:
        fail(f"{name} does not use 4 KiB blocks")
    if superblock.compression_algorithms != 1 << android_image.EROFS_ALGORITHM_ZSTD:
        fail(
            f"{name} is not compressed with {compressor.partition(',')[0]} alone: "
            f"algorithms {superblock.compression_algorithms:#x}"
        )
    if not superblock.feature_incompat & android_image.EROFS_FEATURE_INCOMPAT_BIG_PCLUSTER:
        fail(f"{name} has no physical cluster larger than a block")
    if superblock.feature_incompat & (
        android_image.EROFS_FEATURE_INCOMPAT_ZTAILPACKING
        | android_image.EROFS_FEATURE_INCOMPAT_FRAGMENTS
    ):
        fail(f"{name} uses an extended option (ztailpacking, fragments or dedupe)")


def check_userdata_quota(fstab: bytes, label: str) -> None:
    """Every userdata line carries the fs_mgr `quota` flag.

    installd answers StorageStatsManager only from a /data device whose user quota
    is on, and fs_mgr's tune2fs step enables it only for an entry with the flag.
    """
    lines = [
        fields
        for fields in (line.split() for line in fstab.decode("utf-8", errors="strict").splitlines())
        if fields and not fields[0].startswith("#") and fields[0] == "/dev/block/by-name/userdata"
    ]
    if not lines:
        fail(f"UML fstab has no userdata entry: {label}")
    for fields in lines:
        flags = fields[4].split(",") if len(fields) > 4 else []
        if "quota" not in flags:
            fail(f"UML fstab userdata entry lacks the fs_mgr quota flag: {label}")


def check_video_encoders(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    """The codec list names the hardware encoders and decoders, and only they are enabled."""
    try:
        codecs = ET.fromstring(read_small(archive, infos["VENDOR/etc/media_codecs_c2.xml"]))
    except ET.ParseError as error:
        fail(f"VENDOR/etc/media_codecs_c2.xml is not valid XML: {error}")
    encoders = {
        codec.get("name"): codec.get("type")
        for codec in codecs.iterfind("./Encoders/MediaCodec")
    }
    if encoders != VIDEO_ENCODER_CODECS:
        fail(f"the Codec2 list must name exactly the hardware encoders: {encoders}")
    for codec in codecs.iterfind("./Encoders/MediaCodec"):
        limits = {limit.get("name"): limit for limit in codec.iterfind("./Limit")}
        size = limits.get("size")
        if size is None or size.get("min") != VIDEO_ENCODER_MINIMUM_SIZE:
            fail(f"{codec.get('name')} must start at {VIDEO_ENCODER_MINIMUM_SIZE} pixels: "
                 "the host lists no smaller picture")
        bitrate = limits.get("bitrate")
        if bitrate is None or (bitrate.get("range") or "").split("-")[0] != VIDEO_ENCODER_MINIMUM_BITRATE:
            fail(f"{codec.get('name')} must start at {VIDEO_ENCODER_MINIMUM_BITRATE} bit/s: "
                 "the host lists no lower bit rate")
    # A decoder is one of the hardware decoders, or an update to a software decoder's limits.
    hardware_decoders = {
        codec.get("name"): codec.get("type")
        for codec in codecs.iterfind("./Decoders/MediaCodec")
        if (codec.get("name") or "").startswith("c2.v4l2.")
    }
    if hardware_decoders != VIDEO_DECODER_CODECS:
        fail(f"the Codec2 list must name the hardware decoders: {hardware_decoders}")
    decoders = list(codecs.iterfind("./Decoders/MediaCodec"))
    if any(
        (codec.get("name") or "") not in VIDEO_DECODER_CODECS
        and (
            not (codec.get("name") or "").startswith("c2.android.") or codec.get("update") != "true"
        )
        for codec in decoders
    ) or len(list(codecs.iter("MediaCodec"))) != len(encoders) + len(decoders):
        fail("the Codec2 list names a codec outside the hardware codecs and the software updates")
    for codec in decoders:
        if codec.get("name") not in VIDEO_DECODER_CODECS:
            continue
        # CTS wants a decoder's width and height ranges to match, and the adaptive playback and
        # dynamic colour aspects features it implements to be published.
        limits = {limit.get("name"): limit for limit in codec.iterfind("./Limit")}
        size = limits.get("size")
        if size is None or "x" not in (size.get("min") or "") or "x" not in (size.get("max") or ""):
            fail(f"{codec.get('name')} publishes no size limit")
        features = {feature.get("name") for feature in codec.iterfind("./Feature")}
        for feature in ("adaptive-playback", "dynamic-color-aspects"):
            if feature not in features:
                fail(f"{codec.get('name')} does not publish {feature}")
    # CTS requires a hardware codec to publish what it sustains: the performance points that
    # the claims test reads and the measured frame rates that the performance tests check
    # against what they measure.
    hardware_codecs = list(codecs.iterfind("./Encoders/MediaCodec")) + [
        codec for codec in decoders if codec.get("name") in VIDEO_DECODER_CODECS
    ]
    for codec in hardware_codecs:
        limits = [limit.get("name", "") for limit in codec.iterfind("./Limit")]
        for prefix in ("performance-point-", "measured-frame-rate-"):
            if not any(name.startswith(prefix) for name in limits):
                fail(f"{codec.get('name')} publishes no {prefix}* limit: "
                     "a hardware codec states its performance")

    vendor = parse_properties(
        read_small(archive, infos["VENDOR/build.prop"]).decode("utf-8", errors="strict")
    )
    for key, expected in VIDEO_ENCODER_PROPERTIES.items():
        if vendor.get(key) != expected:
            fail(f"VENDOR/build.prop lacks {key}={expected}: {vendor.get(key)!r}")
    for key, expected in VIDEO_DECODER_PROPERTIES.items():
        if vendor.get(key) != expected:
            fail(f"VENDOR/build.prop lacks {key}={expected}: {vendor.get(key)!r}")
    # A codec cannot publish more than the device sustains: the service reports the pixel rate
    # as the capacity that real-time streams of its direction are counted against, and the
    # largest point must use a good share of it.
    for direction, group, key in (
        ("pixel rate", list(codecs.iterfind("./Encoders/MediaCodec")),
         "ro.vendor.v4l2_codec2.video_pixel_rate"),
        ("decode pixel rate",
         [codec for codec in decoders if codec.get("name") in VIDEO_DECODER_CODECS],
         "ro.vendor.v4l2_codec2.video_decode_pixel_rate"),
    ):
        capacity = int(vendor[key])
        largest = 0
        for codec in group:
            for limit in codec.iterfind("./Limit"):
                name = limit.get("name", "")
                if not name.startswith("performance-point-"):
                    continue
                try:
                    width, height = (int(side) for side in name.rpartition("-")[2].split("x"))
                    frame_rate = int((limit.get("range") or "").partition("-")[2])
                except ValueError:
                    fail(f"{codec.get('name')} publishes a malformed {name}: "
                         f"{limit.get('range')!r}")
                if width * height * frame_rate > capacity:
                    fail(f"{codec.get('name')} publishes {name} above the {direction} capacity "
                         f"{capacity}: {width * height * frame_rate}")
                largest = max(largest, width * height * frame_rate)
        if largest < capacity * VIDEO_MIN_CAPACITY_SHARE:
            fail(f"the largest performance point of the {direction} group uses {largest}, "
                 f"below {VIDEO_MIN_CAPACITY_SHARE:.0%} of the capacity {capacity}")
    for key, value in vendor.items():
        if (
            key.startswith(VIDEO_DECODER_PROPERTY_PREFIX)
            and key not in VIDEO_DECODER_PROPERTIES
            and value != "false"
        ):
            fail(f"VENDOR/build.prop enables a hardware decoder outside the approved set: {key}")

    ueventd = {
        line.strip()
        for line in read_small(archive, infos["VENDOR/etc/ueventd.rc"])
        .decode("utf-8", errors="strict")
        .splitlines()
    }
    if VIDEO_ENCODER_UEVENTD not in ueventd:
        fail(f"UML ueventd policy lacks the video nodes' group: {VIDEO_ENCODER_UEVENTD}")

    service_rc = read_small(archive, infos[f"VENDOR/etc/init/{VIDEO_ENCODER_SERVICE}.rc"])
    groups = {
        group
        for line in service_rc.decode("utf-8", errors="strict").splitlines()
        if line.split()[:1] == ["group"]
        for group in line.split()[1:]
    }
    if "camera" not in groups:
        fail("the video encoder service does not join the group that owns the video nodes")

    policy = read_small(
        archive, infos[f"VENDOR/etc/seccomp_policy/{VIDEO_ENCODER_SECCOMP_EXTENDED}"]
    ).decode("utf-8", errors="strict")
    allowed = {
        line.partition(":")[0].strip()
        for line in policy.splitlines()
        if not line.lstrip().startswith("#")
    }
    for call in VIDEO_ENCODER_SECCOMP_CALLS + VIDEO_CRASH_HANDLER_SECCOMP_CALLS:
        if call not in allowed:
            fail(f"the video codec seccomp policy does not allow {call}")

    contexts = read_small(archive, infos["VENDOR/etc/selinux/vendor_file_contexts"]).decode(
        "utf-8", errors="strict"
    )
    if not any(
        "c2-service-v4l2" in line and line.split()[-1:] == ["u:object_r:mediacodec_exec:s0"]
        for line in contexts.splitlines()
        if not line.lstrip().startswith("#")
    ):
        fail("the video encoder service is not labeled mediacodec_exec")


# The primary audio HAL (alsa_utils) sizes its ALSA streams from these vendor properties: four
# periods of one HAL buffer each. Without them it asks for periods of a quarter buffer, shorter
# than the Virtio sound driver's minimum, and the primary output does not open.
AUDIO_RING_PROPERTIES = {
    "ro.hardware.audio.tinyalsa.period_count": "4",
    "ro.hardware.audio.tinyalsa.period_size_multiplier": "4",
}
AUDIO_HAL_DOMAIN = "hal_audio_default"


def check_audio_ring_policy(contexts: list[str], policies: tuple[bytes, ...]) -> None:
    """init may load the ALSA ring properties from the vendor build.prop, and the audio HAL may
    read them."""
    for name in AUDIO_RING_PROPERTIES:
        label = property_label(name, contexts) or "default_prop"
        if not cil_allows_property_set(policies, "vendor_init", label):
            fail(
                f"vendor_init may not set {name}, labelled {label}: init skips it in the "
                "vendor build.prop"
            )
        if not cil_allows_file_read(policies, AUDIO_HAL_DOMAIN, label):
            fail(
                f"{AUDIO_HAL_DOMAIN} may not read {name}, labelled {label}: the primary "
                "output would not open"
            )


def check_audio_policy(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    main_name = "VENDOR/etc/audio_policy_configuration.xml"
    primary_name = "VENDOR/etc/primary_audio_policy_configuration.xml"
    try:
        main_root = ET.fromstring(read_small(archive, infos[main_name]))
        primary_payload = read_small(archive, infos[primary_name])
        primary_root = ET.fromstring(primary_payload)
    except ET.ParseError as error:
        fail(f"cannot parse UML audio policy: {error}")

    includes = {
        element.attrib["href"]
        for element in main_root.iter()
        if "href" in element.attrib
    }
    expected_includes = {
        "audio_policy_volumes.xml",
        "default_volume_tables.xml",
        "primary_audio_policy_configuration.xml",
        "r_submix_audio_policy_configuration.xml",
    }
    if includes != expected_includes:
        fail("UML audio policy does not select only primary PCM and remote submix")

    def elements(name: str) -> list[ET.Element]:
        return [
            element
            for element in primary_root.iter()
            if element.tag.rsplit("}", 1)[-1] == name
        ]

    mix_ports = elements("mixPort")
    profiles = elements("profile")
    device_ports = elements("devicePort")
    routes = elements("route")
    if [port.attrib for port in mix_ports] != [
        {
            "name": "primary output",
            "role": "source",
            "flags": "AUDIO_OUTPUT_FLAG_PRIMARY",
        },
        {"name": "primary input", "role": "sink"},
    ]:
        fail("UML primary audio policy must expose one ordinary mixer output "
             "and one ordinary mixer input")
    if [profile.attrib for profile in profiles] != [
        {
            "name": "",
            "format": "AUDIO_FORMAT_PCM_16_BIT",
            "samplingRates": "48000",
            "channelMasks": "AUDIO_CHANNEL_OUT_STEREO",
        },
        {
            "name": "",
            "format": "AUDIO_FORMAT_PCM_16_BIT",
            "samplingRates": "48000",
            "channelMasks": "AUDIO_CHANNEL_IN_STEREO AUDIO_CHANNEL_IN_MONO",
        },
    ]:
        fail("UML primary audio profiles must match the 48 kHz Virtio playback "
             "and capture streams")
    if [port.attrib for port in device_ports] != [
        {
            "tagName": "Speaker",
            "type": "AUDIO_DEVICE_OUT_SPEAKER",
            "role": "sink",
        },
        {
            "tagName": "Built-In Mic",
            "type": "AUDIO_DEVICE_IN_BUILTIN_MIC",
            "address": "bottom",
            "role": "source",
        },
    ]:
        fail("UML primary audio policy must expose only its speaker and built-in microphone")
    if [route.attrib for route in routes] != [
        {"type": "mix", "sink": "Speaker", "sources": "primary output"},
        {"type": "mix", "sink": "primary input", "sources": "Built-In Mic"},
    ]:
        fail("UML primary audio routes do not connect the mixer to the speaker "
             "and the built-in microphone to the input mixer")
    if [item.text for item in elements("item")] != ["Speaker", "Built-In Mic"]:
        fail("UML primary audio policy must attach only its speaker and built-in microphone")
    if set(re.findall(rb"AUDIO_DEVICE_IN_[A-Z0-9_]+", primary_payload)) != {
        b"AUDIO_DEVICE_IN_BUILTIN_MIC"
    }:
        fail("UML primary audio policy must name no input device but its built-in microphone")

    for forbidden in (
        b"AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD",
        b"AUDIO_OUTPUT_FLAG_DIRECT",
        b"AUDIO_OUTPUT_FLAG_MMAP_NOIRQ",
    ):
        if forbidden in primary_payload:
            fail(f"unsupported primary audio path remains enabled: {forbidden.decode()}")

    product_properties = read_small(
        archive, infos["PRODUCT/etc/build.prop"]
    ).decode("utf-8", errors="strict").splitlines()
    expected_properties = {
        "aaudio.mmap_policy": "1",
        "aaudio.mmap_exclusive_policy": "1",
        "ro.config.vc_call_vol_steps": "15",
        "ro.config.vc_call_vol_default": "15",
        "ro.config.media_vol_default": "15",
        "ro.config.alarm_vol_default": "7",
        "ro.config.system_vol_default": "7",
        "ro.config.assistant_vol_default": "15",
        # The guest boots without a lock screen, and `locksettings set-disabled` can turn
        # it on: LockSettingsService stores this default when it creates its database. The
        # Setup Wizard's screen lock page is hidden, so this is the lock a fresh guest keeps.
        "ro.lockscreen.disable.default": "true",
        # Userdata is a plain ext4 mount of its partition, which the mount holds exclusively:
        # apexd's pinned APEX images (a dm-linear table over that partition) fail with EBUSY,
        # so every staged APEX install is rejected unless apexd stages through /data/app-staging.
        "apexd.config.use_fiemap": "false",
    }
    for property_name, expected in expected_properties.items():
        values = [
            line.partition("=")[2].strip()
            for line in product_properties
            if line.partition("=")[0] == property_name
        ]
        if values != [expected]:
            fail(f"unexpected product property {property_name}: {values}")
    vendor_properties = read_small(
        archive, infos["VENDOR/build.prop"]
    ).decode("utf-8", errors="strict").splitlines()
    for property_name, expected in AUDIO_RING_PROPERTIES.items():
        values = [
            line.partition("=")[2].strip()
            for line in vendor_properties
            if line.partition("=")[0] == property_name
        ]
        if values != [expected]:
            fail(f"unexpected vendor audio property {property_name}: {values}")


def read_elf_prefix(archive: zipfile.ZipFile, info: zipfile.ZipInfo) -> bytes | None:
    """Return an ELF file's header and program-header table, or None for any
    other file."""
    try:
        return read_elf_prefix_unchecked(archive, info)
    except (zipfile.BadZipFile, zlib.error, OSError) as problem:
        fail(f"cannot read {info.filename} from the archive: {problem}")


def read_elf_prefix_unchecked(archive: zipfile.ZipFile, info: zipfile.ZipInfo) -> bytes | None:
    if info.file_size < android_image.ELF32_HEADER_SIZE:
        return None
    with archive.open(info) as source:
        header = source.read(android_image.ELF64_HEADER_SIZE)
        if header[:4] != android_image.ELF_MAGIC:
            return None
        elf = android_image.elf_format(info.filename, header, error)
        program_offset, entry_size, entry_count = android_image.program_table(elf, header)
        program_end = program_offset + entry_size * entry_count
        if program_end > info.file_size or program_end > MAX_PROGRAM_HEADERS_SIZE:
            fail(f"malformed ELF program headers: {info.filename}")
        prefix = header
        if program_end > len(prefix):
            prefix += source.read(program_end - len(prefix))
        if len(prefix) < program_end:
            fail(f"truncated ELF program headers: {info.filename}")
        return prefix


def elf_directories(path: PurePosixPath) -> set[tuple[str, str]]:
    return set(zip(path.parts, path.parts[1:]))


def elf32_placement_ok(path: PurePosixPath) -> bool:
    """Whether a 32-bit ELF may sit at this target-files path.

    The ABI directories hold the libraries: lib in a partition, and lib/arm
    beside an application's own. ART writes its images and compiled code under
    arm directories. An executable built for both ABIs takes the stem suffix
    32, and the bootstrap linker is the one 32-bit executable without it.
    Anything else is a 32-bit daemon, which this product does not have.
    """
    parts = path.parts
    directories = elf_directories(path)
    if len(parts) > 1 and parts[1] == "lib":
        return True
    if ("lib", "arm") in directories or ("oat", "arm") in directories:
        return True
    if parts[1:3] == ("framework", "arm"):
        return True
    if len(parts) > 2 and parts[1] == "bin":
        return path.name.endswith("32") or str(path) in ELF32_BIN_EXCEPTIONS
    return False


def elf64_placement_ok(path: PurePosixPath) -> bool:
    """Whether a 64-bit ELF may sit at this target-files path: not where the
    32-bit libraries live."""
    return not (
        (len(path.parts) > 1 and path.parts[1] == "lib")
        or ("lib", "arm") in elf_directories(path)
    )


def check_target_elf(info: zipfile.ZipInfo, payload: bytes) -> None:
    elf = android_image.elf_format(info.filename, payload, error)
    if struct.unpack_from("<H", payload, 16)[0] not in (ET_EXEC, ET_DYN):
        return
    path = PurePosixPath(info.filename)
    if elf is android_image.ELF32:
        android_image.check_arm_elf_header(info.filename, payload, error)
        if not elf32_placement_ok(path):
            fail(f"32-bit ELF outside the 32-bit library and runtime paths: {info.filename}")
    else:
        android_image.check_aarch64_elf_header(info.filename, payload, error)
        if not elf64_placement_ok(path):
            fail(f"64-bit ELF in a 32-bit library directory: {info.filename}")
    android_image.check_load_alignment(info.filename, payload, error)


def check_large_file_imports(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo], entry: str
) -> None:
    """Refuse a 32-bit stack library that imports mmap or lseek without the 64
    suffix when the same library has a 64-bit build."""
    parts = PurePosixPath(entry).parts
    if parts[1] != "lib":
        return
    twin = str(PurePosixPath(parts[0], "lib64", *parts[2:]))
    if twin not in infos:
        return
    info = infos[entry]
    plain = sorted(
        set(LARGE_FILE_FUNCTIONS)
        & android_image.elf_imports(info.filename, archive.read(info), error)
    )
    if plain:
        fail(
            f"32-bit {entry} imports {', '.join(plain)} instead of "
            f"{', '.join(name + '64' for name in plain)}: it was built without "
            "-D_FILE_OFFSET_BITS=64"
        )


def check_mesa_gles_stack(
    archive: zipfile.ZipFile, infos: dict[str, zipfile.ZipInfo]
) -> None:
    for entry in sorted(MESA_ELF_ENTRIES | MESA_HAL_ENTRIES):
        info = infos[entry]
        payload = read_elf_prefix(archive, info)
        if payload is None:
            fail(f"Mesa component is not an ELF file: {entry}")
        elf_type, machine = struct.unpack_from("<HH", payload, 16)
        expected_machine = MESA_ABI_DIRECTORIES[PurePosixPath(entry).parts[1]]
        if elf_type != ET_DYN or machine != expected_machine:
            fail(
                "Mesa component is not "
                f"{'AArch64' if expected_machine == android_image.EM_AARCH64 else 'Arm'}"
                f" ET_DYN: {entry}"
            )
        check_target_elf(info, payload)
        check_large_file_imports(archive, infos, entry)
        if entry in MESA_EGL_ENTRIES:
            needed = android_image.elf_needed(info.filename, archive.read(info), error)
            if MESA_GALLIUM_DRIVER not in needed:
                fail(f"Mesa GLES component does not link the gallium driver: {entry}")

    for name in infos:
        if name in RETIRED_EGL_ENTRIES:
            fail(f"retired AOSP Mesa EGL or GLES library is forbidden: {name}")
        if name.startswith(RETIRED_DRI_DIRECTORIES):
            fail(f"retired DRI driver payload is forbidden: {name}")

    vendor_properties = {
        line.partition("=")[0].strip()
        for line in read_small(archive, infos["VENDOR/build.prop"])
        .decode("utf-8", errors="strict")
        .splitlines()
    }
    for name in sorted(BOOT_HAL_SELECTIONS):
        if f"ro.hardware.{name}" in vendor_properties:
            fail(
                f"VENDOR/build.prop sets ro.hardware.{name}, which the boot "
                "command line owns"
            )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--target-files", type=Path, required=True)
    parser.add_argument("--kernel-elf", type=Path, required=True)
    parser.add_argument("--kernel-config", type=Path, required=True)
    parser.add_argument("--kernel-release-file", type=Path, required=True)
    parser.add_argument(
        "--build-variant",
        choices=sorted(BUILD_VARIANTS),
        help="build variant the target files must carry (default: tracked metadata)",
    )
    parser.add_argument(
        "--signed",
        action="store_true",
        help="the target files are the signed archive of a release: check that they trust the "
        "release certificate alone",
    )
    args = parser.parse_args()
    build_variant = args.build_variant or configured_build_variant(
        Path(__file__).resolve().parents[1]
    )

    kernel_config = args.kernel_config
    kernel_release_file = args.kernel_release_file

    if not args.target_files.is_file():
        fail(f"target-files archive does not exist: {args.target_files}")
    if not args.kernel_elf.is_file():
        fail(f"kernel ELF does not exist: {args.kernel_elf}")
    if not kernel_config.is_file():
        fail(f"kernel configuration does not exist: {kernel_config}")
    if not kernel_release_file.is_file():
        fail(f"kernel release file does not exist: {kernel_release_file}")

    seen: set[str] = set()
    infos: dict[str, zipfile.ZipInfo] = {}
    elf_count = 0
    apex_count = 0
    try:
        archive = zipfile.ZipFile(args.target_files)
    except (OSError, zipfile.BadZipFile) as error:
        fail(f"cannot open target-files archive: {error}")

    with archive:
        for info in archive.infolist():
            path = safe_name(info.filename.rstrip("/"))
            canonical = str(path)
            if canonical in seen:
                fail(f"duplicate archive entry: {canonical}")
            seen.add(canonical)
            infos[canonical] = info
            reject_module_payload(path)
            reject_nonstatic_payload(path)
            if info.is_dir():
                continue
            if len(path.parts) >= 3 and path.parts[1] == "apex":
                if path.suffix == ".capex":
                    fail(f"compressed APEX is forbidden: {path}")
                if path.suffix == ".apex":
                    check_apex_payload_filesystem(archive, info)
                    apex_count += 1
            if path.name in {"build.prop", "default.prop"}:
                check_property_file(archive, info)
            if path.parts[0] not in TARGET_PARTITIONS:
                continue
            payload = read_elf_prefix(archive, info)
            if payload is not None:
                elf_count += 1
                check_target_elf(info, payload)

        missing = sorted(REQUIRED_ENTRIES - seen)
        if missing:
            fail(f"required entries are missing: {', '.join(missing)}")
        if apex_count == 0:
            fail("target files do not contain an APEX payload")
        check_app_dexpreopt(seen)
        check_hardware_features(archive, infos)
        check_motion_sensors(archive, infos)
        check_haptics_vibrator(archive, infos)
        check_gnss_hal(archive, infos)
        check_discrete_ops(archive, infos)
        check_default_wallpaper(archive, infos)
        check_no_statistics_uploader(archive, infos)
        check_setup_wizard_branding(archive, infos)
        check_navigation_mode(archive, infos)
        check_easter_egg(infos)
        check_accessibility(infos)
        check_launcher_workspace(archive, infos)
        check_network_time(archive, infos)
        check_flip_to_screen_off(archive, infos)
        check_wifi_overlay(archive, infos)
        check_input_device_configuration(archive, infos)
        check_pinner_overlay(archive, infos)
        check_application_manifests(archive, infos, build_variant)
        check_thermal_hal(archive, infos)
        check_camera(archive, infos)
        check_gamepad_relay(archive, infos)
        check_sms_service_files(archive, infos)
        check_cas_hal(archive, infos)
        check_wifi_hals(archive, infos)
        check_adbd_launch_properties(archive, infos)
        check_adb_root_service(archive, infos)
        check_lmkd_gpu_reclaim(archive, infos)
        check_services_jar(archive, infos)
        check_old_application_layer(archive, infos)
        check_input_flinger(archive, infos)
        check_network_stack(archive, infos)
        check_wifi_tracker_users(archive, infos)
        check_audio_policy(archive, infos)
        check_multilib_properties(archive, infos)
        check_video_encoders(archive, infos)
        check_mesa_gles_stack(archive, infos)
        check_boot_policy(archive, infos, build_variant)
        check_kernel_policy(
            archive, infos, kernel_config, kernel_release_file, build_variant
        )
        check_ota_certificates(archive, infos, build_variant, args.signed)
        if sha256_member(archive, infos["BOOT/kernel"]) != sha256_path(args.kernel_elf):
            fail("BOOT/kernel does not match the verified UML ELF")

    print(
        f"verified AOSP 17 target files: {len(seen)} entries, "
        f"{elf_count} target executables, {apex_count} APEX payloads"
    )


if __name__ == "__main__":
    main()
