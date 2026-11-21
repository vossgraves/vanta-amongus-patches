package app.vanta.patches.amongus

import app.morphe.patcher.extensions.InstructionExtensions.addInstructions
import app.morphe.patcher.patch.listApkEntries
import app.morphe.patcher.patch.mutableClassDefBy
import app.morphe.patcher.patch.patch
import app.morphe.patcher.patch.resourceFile
import app.morphe.patcher.patch.resourceXml
import app.vanta.patches.shared.Constants.ACTIVITY
import app.vanta.patches.shared.Constants.COMPATIBILITY_AMONG_US
import app.vanta.patches.shared.Constants.LOADER_CLASS
import app.vanta.patches.shared.Constants.NATIVE_LIB

/**
 * Injects the native mod menu.
 *
 * Three moves, in order: drop libvanta.so next to libil2cpp.so so the loader resolves it
 * from the app's own native search path, grant the overlay permission the floating panel
 * needs, and call the loader from the Unity activity before Unity starts.
 *
 * The library itself refuses to hook when the app version code does not match its offset
 * table, so patching an unsupported build yields an inert library rather than a crash.
 */
@Suppress("unused")
val modMenuPatch = patch(
    name = "Mod menu",
    description = "Floating in-game menu: speed, kill cooldown, kill anyone, vent anywhere, " +
        "cosmetic and account unlocks, task tools. arm64-v8a only.",
    default = true,
) {
    compatibleWith(COMPATIBILITY_AMONG_US)

    extendWith("extensions/vanta.mpe")

    execute {
        val libs = listApkEntries("lib/")
        check("lib/arm64-v8a/libil2cpp.so" in libs) {
            "arm64-v8a libil2cpp.so not found. Among Us ships per-ABI splits; " +
                "patch the arm64 build (config.arm64_v8a) with its base APK."
        }

        val payload = object {}.javaClass.getResourceAsStream("/vanta/libvanta.so")
            ?: error("libvanta.so is missing from the patch bundle")
        payload.use { stream ->
            resourceFile(NATIVE_LIB, copy = false).writeBytes(stream.readBytes())
        }

        resourceXml("AndroidManifest.xml").use { manifest ->
            val granted = manifest.elements("uses-permission").any {
                it.attribute("android:name") == "android.permission.SYSTEM_ALERT_WINDOW"
            }
            if (!granted) {
                manifest.root.appendChild("uses-permission").apply {
                    setAttribute("android:name", "android.permission.SYSTEM_ALERT_WINDOW")
                }
            }
        }

        val activity = mutableClassDefBy(ACTIVITY)
        val onCreate = activity.methods.firstOrNull {
            it.name == "onCreate" &&
                it.parameterTypes.map { type -> type.toString() } == listOf("Landroid/os/Bundle;")
        } ?: error("onCreate(Landroid/os/Bundle;)V not found in $ACTIVITY")

        onCreate.addInstructions(
            0,
            "invoke-static {p0}, $LOADER_CLASS->install(Landroid/content/Context;)V",
        )
    }
}
