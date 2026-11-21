package app.vanta.patches.shared

import app.morphe.patcher.patch.ApkFileType
import app.morphe.patcher.patch.AppTarget
import app.morphe.patcher.patch.Compatibility
import app.morphe.patcher.patch.SupportedAbi

object Constants {
    const val PACKAGE_NAME = "com.innersloth.spacemafia"

    /** Activity that hosts UnityPlayer; the loader call is injected into its onCreate. */
    const val ACTIVITY = "Lcom/innersloth/spacemafia/EosUnityPlayerActivity;"

    const val LOADER_CLASS = "Lapp/vanta/extension/Loader;"

    const val NATIVE_LIB = "lib/arm64-v8a/libvanta.so"

    const val SUPPORTED_VERSION = "2026.11.3"
    const val SUPPORTED_VERSION_CODE = 7616

    val COMPATIBILITY_AMONG_US = Compatibility(
        name = "Among Us",
        packageName = PACKAGE_NAME,
        description = "Room-based social deduction game by Innersloth.",
        apkFileType = ApkFileType.APK,
        // App icon background: the red suit.
        appIconColor = 0xC51111.toInt(),
        targets = listOf(
            AppTarget(
                version = SUPPORTED_VERSION,
                versionCodes = mapOf(SupportedAbi.ARM64_V8A to SUPPORTED_VERSION_CODE),
            ),
            // Older and newer builds patch, but the native offset table only matches the
            // version above, so the menu stays inert until the table is regenerated.
            AppTarget(version = null, isExperimental = true),
        ),
    )
}
