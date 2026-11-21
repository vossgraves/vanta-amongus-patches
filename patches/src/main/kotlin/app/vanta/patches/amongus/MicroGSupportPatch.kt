package app.vanta.patches.amongus

import app.morphe.patcher.extensions.InstructionExtensions.getInstruction
import app.morphe.patcher.extensions.InstructionExtensions.replaceInstruction
import app.morphe.patcher.patch.mutableClassDefByOrNull
import app.morphe.patcher.patch.patch
import app.morphe.patcher.patch.resourceXml
import app.morphe.patcher.util.matchAllMethodIndicesForEach
import app.morphe.patcher.util.returnEarly
import app.morphe.patcher.string
import app.vanta.patches.shared.Constants.COMPATIBILITY_AMONG_US
import com.android.tools.smali.dexlib2.Opcode
import com.android.tools.smali.dexlib2.builder.instruction.BuilderInstruction31c
import com.android.tools.smali.dexlib2.iface.instruction.OneRegisterInstruction
import com.android.tools.smali.dexlib2.immutable.reference.ImmutableStringReference

private const val STOCK_GMS = "com.google.android.gms"
private const val MICROG = "app.revanced.android.gms"
private const val MICROG_LEGACY = "com.mgoogle.android.gms"

private const val GMS_AVAILABILITY = "Lcom/google/android/gms/common/GoogleApiAvailability;"
private const val GMS_AVAILABILITY_LIGHT = "Lcom/google/android/gms/common/GoogleApiAvailabilityLight;"
private const val GMS_SIGNATURE_VERIFIER = "Lcom/google/android/gms/common/GoogleSignatureVerifier;"
private const val GMS_VERIFICATION_RESULT = "Lcom/google/android/gms/common/PackageVerificationResult;"

/**
 * Points the bundled Play Games client at microG RE (ReVanced GmsCore).
 *
 * A repackaged APK is signed with a key Google's service does not know, so Play Games sign-in
 * fails on a non-root install. microG RE answers on its own package and accepts patched
 * callers, so every place the client hardcodes the Play Services identity has to agree with it:
 *
 *  - the package string constant, so the client binds to microG's service,
 *  - the availability probe, which otherwise reports the wrong Play Services version,
 *  - the signature verifier, which rejects a non-Google signer,
 *  - the package verification result, which the Play Games SDK consults before trusting a caller.
 *
 * The `com.google.android.gms.version` meta-data is removed because a rebuilt APK cannot carry
 * Google's manifest value and the probe throws on the mismatch before the string replacement
 * can help.
 */
@Suppress("unused")
val microGReSupportPatch = patch(
    name = "MicroG RE support",
    description = "Use microG RE (ReVanced GmsCore) for Play Games sign-in on a non-root " +
        "install instead of Google Play Services.",
    default = false,
) {
    compatibleWith(COMPATIBILITY_AMONG_US)

    execute {
        check(mutableClassDefByOrNull(GMS_AVAILABILITY) != null) {
            "Play Services client not found. This build of Among Us does not bundle Play Games."
        }

        // 1. Package constant. Only exact string constants are rewritten, so log lines and
        //    unrelated substrings survive untouched. The jumbo form is written unconditionally:
        //    a merged, patched APK can push the string pool past the 16-bit const-string index.
        string(STOCK_GMS).matchAllMethodIndicesForEach(requireMatches = false) { index ->
            val instruction = getInstruction<OneRegisterInstruction>(index)
            replaceInstruction(
                index,
                BuilderInstruction31c(
                    Opcode.CONST_STRING_JUMBO,
                    instruction.registerA,
                    ImmutableStringReference(MICROG),
                ),
            )
        }

        // 2. Manifest: declare the microG packages so Android 11+ package visibility lets the
        //    client bind to them, and drop the Play Services version meta-data.
        resourceXml("AndroidManifest.xml").use { manifest ->
            manifest.elements("meta-data")
                .firstOrNull { it.attribute("android:name") == "$STOCK_GMS.version" }
                ?.remove()

            val queries = manifest.elements("queries").firstOrNull()
                ?: manifest.root.appendChild("queries")

            listOf(MICROG, MICROG_LEGACY, STOCK_GMS, "com.android.vending")
                .filterNot { pkg ->
                    queries.children("package").any { it.attribute("android:name") == pkg }
                }
                .forEach { pkg ->
                    queries.appendChild("package").setAttribute("android:name", pkg)
                }
        }

        // 3. Availability probes: report SUCCESS (0) without asking the platform.
        listOf(GMS_AVAILABILITY, GMS_AVAILABILITY_LIGHT).forEach { descriptor ->
            mutableClassDefByOrNull(descriptor)
                ?.methods
                ?.filter { it.name == "isGooglePlayServicesAvailable" && it.returnType == "I" }
                ?.forEach { it.returnEarly(0) }
        }

        // 4. Signature checks: microG RE is not signed by Google.
        mutableClassDefByOrNull(GMS_SIGNATURE_VERIFIER)
            ?.methods
            ?.filter { it.returnType == "Z" }
            ?.forEach { it.returnEarly(true) }

        mutableClassDefByOrNull(GMS_VERIFICATION_RESULT)
            ?.methods
            ?.firstOrNull { it.name == "zzb" && it.returnType == "Z" }
            ?.returnEarly(true)
    }
}
