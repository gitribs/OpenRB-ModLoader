#!/usr/bin/env bash
# Sign, notarize, and zip the macOS VST3 / AU / standalone for sharing.
#
# Local ./scripts/build.sh is unchanged (ad-hoc / unsigned output/). This script
# is the Gatekeeper path: Developer ID Application + Apple notarization.
#
# One-time on this Mac:
#   1. Join the Apple Developer Program and install a "Developer ID Application"
#      certificate (Keychain Access should list it under login).
#   2. Create an app-specific password at https://appleid.apple.com
#   3. Store notary credentials (team id is the 10-character id on developer.apple.com):
#        xcrun notarytool store-credentials "openrb-notary" \
#          --apple-id "you@example.com" --team-id "TEAMID" \
#          --password "xxxx-xxxx-xxxx-xxxx"
#
# Then, for each new deliverable:
#   ./scripts/package-macos.sh
#
# Writes output/OpenRB-Modloader-macOS-vVERSION.zip
# Override the cert with CODESIGN_IDENTITY='Developer ID Application: Name (TEAMID)'.
# Override the notary profile with NOTARY_PROFILE=... (default openrb-notary).
# API key: APPLE_API_KEY, APPLE_API_KEY_ID, APPLE_API_ISSUER.
# Apple ID (instead of a profile): APPLE_ID, APPLE_TEAM_ID, APPLE_APP_PASSWORD.
#
#   --skip-build      use existing output/ bundles
#   --skip-notarize   sign and zip only; the zip name says it is not notarized
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

skip_build=0
skip_notarize=0
for arg in "$@"; do
    case "$arg" in
        --skip-build) skip_build=1 ;;
        --skip-notarize) skip_notarize=1 ;;
        -h|--help)
            awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"
            exit 0
            ;;
        *)
            echo "unknown argument: $arg (try --help)" >&2
            exit 2
            ;;
    esac
done

if [[ "$(uname -s)" != Darwin ]]; then
    echo "package-macos.sh is macOS only" >&2
    exit 1
fi

product="OpenRB Modloader"
vst3_bundle="output/VST3/${product}.vst3"
au_bundle="output/AU/${product}.component"
app_bundle="output/Standalone/${product}.app"
entitlements="$root/scripts/macos.entitlements"
notary_profile="${NOTARY_PROFILE:-openrb-notary}"

setup_help() {
    cat >&2 <<'EOF'

Need a Developer ID Application certificate in this login keychain, plus notarization credentials.

  1. Apple Developer Program → Certificates → Developer ID Application. Install it.
  2. App-specific password: https://appleid.apple.com
  3. xcrun notarytool store-credentials "openrb-notary" \
       --apple-id "you@example.com" --team-id "TEAMID" \
       --password "xxxx-xxxx-xxxx-xxxx"

Apple Development / Mac App Store certs will not pass Gatekeeper for other people.
EOF
}

resolve_identity() {
    if [[ -n "${CODESIGN_IDENTITY:-}" ]]; then
        printf '%s\n' "$CODESIGN_IDENTITY"
        return
    fi
    local ids
    ids="$(security find-identity -v -p codesigning 2>/dev/null \
        | awk -F'"' '/Developer ID Application/ { print $2 }' \
        | sort -u)" || true
    local n
    n="$(printf '%s\n' "$ids" | grep -c . || true)"
    if [[ "$n" -eq 1 ]]; then
        printf '%s\n' "$ids"
        return
    fi
    if [[ "$n" -gt 1 ]]; then
        echo "several Developer ID Application identities; set CODESIGN_IDENTITY to one of:" >&2
        printf '%s\n' "$ids" | sed 's/^/  /' >&2
        exit 1
    fi
    echo "no Developer ID Application identity in the login keychain." >&2
    setup_help
    exit 1
}

notary_args=()
resolve_notary() {
    if [[ -n "${APPLE_API_KEY:-}" || -n "${APPLE_API_KEY_ID:-}" || -n "${APPLE_API_ISSUER:-}" ]]; then
        if [[ -z "${APPLE_API_KEY:-}" || -z "${APPLE_API_KEY_ID:-}" || -z "${APPLE_API_ISSUER:-}" ]]; then
            echo "APPLE_API_KEY, APPLE_API_KEY_ID, and APPLE_API_ISSUER must all be set" >&2
            exit 1
        fi
        notary_args=(--key "$APPLE_API_KEY" --key-id "$APPLE_API_KEY_ID" --issuer "$APPLE_API_ISSUER")
        return
    fi
    if [[ -n "${APPLE_ID:-}" || -n "${APPLE_TEAM_ID:-}" || -n "${APPLE_APP_PASSWORD:-}" ]]; then
        if [[ -z "${APPLE_ID:-}" || -z "${APPLE_TEAM_ID:-}" || -z "${APPLE_APP_PASSWORD:-}" ]]; then
            echo "APPLE_ID, APPLE_TEAM_ID, and APPLE_APP_PASSWORD must all be set" >&2
            exit 1
        fi
        notary_args=(--apple-id "$APPLE_ID" --team-id "$APPLE_TEAM_ID" --password "$APPLE_APP_PASSWORD")
        return
    fi
    if ! xcrun notarytool history --keychain-profile "$notary_profile" >/dev/null 2>&1; then
        echo "notarytool profile '$notary_profile' is missing or unusable." >&2
        setup_help
        exit 1
    fi
    notary_args=(--keychain-profile "$notary_profile")
}

identity="$(resolve_identity)"
if [[ "$identity" != "Developer ID Application:"* ]]; then
    echo "CODESIGN_IDENTITY must be a Developer ID Application cert, not:" >&2
    echo "  $identity" >&2
    setup_help
    exit 1
fi
echo "Signing identity: $identity"

if [[ "$skip_notarize" -eq 0 ]]; then
    resolve_notary
fi

if [[ "$skip_build" -eq 0 ]]; then
    "$root/scripts/build.sh"
fi

for bundle in "$vst3_bundle" "$au_bundle" "$app_bundle"; do
    if [[ ! -d "$bundle" ]]; then
        echo "missing $bundle (run without --skip-build, or build first)" >&2
        exit 1
    fi
done

sign_bundle() {
    local bundle=$1
    echo "Signing $bundle"
    xattr -cr "$bundle"
    /usr/bin/codesign --force --sign "$identity" --options runtime --timestamp \
        --entitlements "$entitlements" --generate-entitlement-der "$bundle"
    /usr/bin/codesign --verify --deep --strict "$bundle"
}

sign_bundle "$vst3_bundle"
sign_bundle "$au_bundle"
sign_bundle "$app_bundle"

version="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' \
    "$vst3_bundle/Contents/Info.plist")"
work="$(mktemp -d "${TMPDIR:-/tmp}/openrb-modloader-pkg.XXXXXX")"
stage="$work/payload"
cleanup() { rm -rf "$work"; }
trap cleanup EXIT

ditto "$vst3_bundle" "$stage/VST3/${product}.vst3"
ditto "$au_bundle" "$stage/AU/${product}.component"
ditto "$app_bundle" "$stage/Standalone/${product}.app"
find "$stage" -name .DS_Store -delete

if [[ "$skip_notarize" -eq 1 ]]; then
    zip_path="output/OpenRB-Modloader-macOS-v${version}-signed-not-notarized.zip"
    echo "Skipping notarization; this zip will still fail Gatekeeper after a download."
else
    zip_path="output/OpenRB-Modloader-macOS-v${version}.zip"
    notary_zip="$work/notarize.zip"
    ditto -c -k "$stage" "$notary_zip"
    echo "Submitting to Apple notary (this can take several minutes)..."
    xcrun notarytool submit "$notary_zip" "${notary_args[@]}" --wait

    staple_one() {
        local bundle=$1
        if xcrun stapler staple "$bundle"; then
            xcrun stapler validate "$bundle"
        else
            echo "stapler could not attach a ticket to $bundle (common for .vst3 / .component)."
            echo "Gatekeeper still accepts it while online; the standalone .app is the one that needs a staple."
        fi
    }
    staple_one "$vst3_bundle"
    staple_one "$au_bundle"
    staple_one "$app_bundle"

    rm -rf "$stage"
    ditto "$vst3_bundle" "$stage/VST3/${product}.vst3"
    ditto "$au_bundle" "$stage/AU/${product}.component"
    ditto "$app_bundle" "$stage/Standalone/${product}.app"
    find "$stage" -name .DS_Store -delete
fi

rm -f "$zip_path"
ditto -c -k "$stage" "$zip_path"
echo "Deliverable: $zip_path"
echo "Share that zip. Keep using ./scripts/build.sh for local output/ copies."
