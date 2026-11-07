# Releasing a signed, notarised build

Pushing a `v*` tag (or running the `Release` workflow by hand with a version) builds the universal (arm64 + x86_64)
`Nocturne.app`, signs it, notarises it, packs `Nocturne-<version>.dmg` and attaches it with a SHA-256 file to a **draft**
GitHub Release. A human reads the draft and publishes it. The steps are the scripts below, so they run the same by
hand on a Mac that holds the certificate:

| Step | Script |
|---|---|
| Both bundles hold both slices | `tools/check-universal.sh` |
| Certificate into a throwaway keychain | `tools/import-signing-cert.sh` |
| One bundle, signed inside-out with the hardened runtime and `tools/nocturne.entitlements` | `tools/make-bundle.sh --identity` |
| Notarise and staple the app, then the DMG | `tools/notarize.sh` |
| DMG, signed | `tools/make-dmg.sh --identity` |

The workflow then checks `codesign --verify --deep --strict` and `spctl` on the app and the DMG. The release job runs in
the protected `release` environment and never on a pull request, so no PR can read a secret.

## Secrets

Create the environment under Settings > Environments > `release` (add required reviewers if a tag should wait for a
click), and add these **environment secrets**. Set all five or none: with none the run builds the unsigned preview
(`Nocturne-<version>-unsigned.dmg`, as on a fork) and says so in a warning; with some, it fails before building.

| Secret | Value |
|---|---|
| `APPLE_CERT_P12_BASE64` | The Developer ID Application certificate with its private key, as base64 |
| `APPLE_CERT_PASSWORD` | The password chosen when exporting that `.p12` |
| `APPLE_API_KEY_BASE64` | The App Store Connect API key `AuthKey_XXXX.p8`, as base64 |
| `APPLE_API_KEY_ID` | That key's Key ID |
| `APPLE_API_ISSUER` | The Issuer ID shown on the App Store Connect Keys page |

How a maintainer gets them (needs Apple Developer Program membership):

1. Certificate: developer.apple.com > Certificates > create a **Developer ID Application** certificate (a CSR from
   Keychain Access > Certificate Assistant). Install it, then in Keychain Access export the certificate with its private
   key as `.p12` and pick a password.
2. `base64 -i DeveloperID.p12 | pbcopy`, paste as `APPLE_CERT_P12_BASE64`; the password is `APPLE_CERT_PASSWORD`.
3. App Store Connect > Users and Access > Integrations > Team Keys: create a key with the Developer role, download the
   `.p8` once, note the Key ID and the Issuer ID. `base64 -i AuthKey_XXXX.p8 | pbcopy` is `APPLE_API_KEY_BASE64`.
4. Delete the local `.p12` and `.p8` afterwards. Never commit them.

Optional, to try notarisation by hand on a Mac: `xcrun notarytool store-credentials nocturne-notary --key AuthKey_XXXX.p8
--key-id KEYID --issuer ISSUER` keeps a keychain profile; `tools/notarize.sh` reads the environment instead, so CI does
not need one.

## A release

1. Update `docs/release-notes.md` and the project version in `CMakeLists.txt`, merge.
2. `git tag vX.Y.Z && git push origin vX.Y.Z`. Watch the `Release` run.
3. Open the draft release, check the notes, and on a clean Mac run `spctl -a -vv -t open --context
   context:primary-signature Nocturne-X.Y.Z.dmg`, `xcrun stapler validate` on the DMG, then open the app: it must start
   with no Gatekeeper prompt, and the microphone and local network prompts must appear and work.
4. Publish the draft.

## A notarised build next to a dev build

A Developer ID build and a local "Nocturne Dev" build (`tools/make-signing-identity.sh`) are different identities.
macOS keys privacy grants on the bundle identifier plus the signing identity, so each asks for the microphone and local
network once. The engine's update (`machine update`, `src/engine/bundle_update_apple.cpp`) installs a bundle only when
it satisfies the running bundle's designated requirement, so a release app takes updates signed by the same Developer ID
and a dev build ones signed by "Nocturne Dev"; pushing one kind onto the other is refused.

## Not covered

The Windows installer is built and uploaded by `ci.yml` (`nocturne-windows-installer`), not by this workflow, and is not
code signed.
