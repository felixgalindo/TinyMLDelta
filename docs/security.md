# Security: Integrity, Authenticity, and the Standards Track

TinyMLDelta separates **integrity** (is the patch intact / for the right base?)
from **authenticity** (is the patch from a trusted publisher?). Integrity is on
by default; authenticity is **opt-in and pluggable**, and designed to be
**forward-compatible with the IETF SUIT/COSE standards track**.

## What's on by default (integrity)

| Mechanism | Catches |
|-----------|---------|
| CRC32 per chunk | accidental corruption of a chunk in transit |
| Base/target digests in the header | wrong/garbled model lengths |
| **Base-slot digest verify** (`TMD_FEAT_VERIFY_BASE`, default on) | applying a patch built for a *different* base model |

> **CRC32 is integrity, not security.** It detects accidental corruption, not
> tampering — an attacker who can modify the OTA stream can forge a patch with a
> valid CRC. Production deployments that accept patches over an untrusted channel
> **must enable authenticity** (below).

## Authenticity (opt-in) — `TMD_FEAT_VERIFY_SIG`

When `TMD_FEAT_VERIFY_SIG=1`, the core verifies patch authenticity **first** —
before parsing metadata, running guardrails, or touching flash — and is
**fail-closed**: if no verifier is configured, or verification fails, the patch
is rejected (`TMD_STATUS_ERR_SIGNATURE`).

The core is **crypto-agnostic**. It calls one platform hook:

```c
bool (*verify_patch)(const uint8_t* patch, size_t patch_len);
```

The platform implements the scheme and locates the signature/key itself. This is
the key design choice that keeps TinyMLDelta **forward-compatible with standards**:
the same hook can wrap SHA-256 + Ed25519, a secure element, or a full
**SUIT manifest + COSE_Sign1** verifier — without changing the core.

```
TMD_FEAT_VERIFY_SIG = 0   -> integrity only (CRC32 + base digest)
TMD_FEAT_VERIFY_SIG = 1   -> require P->verify_patch() to pass (fail-closed)
```

## Threat model

An untrusted OTA channel (BLE/LoRa/Wi-Fi/cellular) that can intercept, modify, or
replay patches. A forged or downgraded model patch could brick the device or run
attacker-controlled inference. Production needs the patch to be:

1. **Authentic** — provably from the publisher (signature).
2. **Integrity-protected** — unaltered (hash, covered by the signature).
3. **Anti-rollback** — not downgradable to a vulnerable version (monotonic counter).
4. **Confidential** *(optional)* — weights encrypted, if model IP is sensitive.

## Industry-standard stack (what real products enable)

| Concern | Standard mechanism |
|---------|--------------------|
| Signature | **Ed25519** (64-byte sig, fast/tiny — ideal MCU) or **ECDSA P-256 / ES256** (NIST, HW-accelerated). Not RSA (too heavy). |
| Hash | **SHA-256** over header+meta+body |
| Signed envelope | **COSE_Sign1** (RFC 9052) |
| Update manifest | **SUIT** (RFC 9019) — signed manifest: digest, version, dependencies, install steps. Pairs with MCUboot. |
| Anti-rollback | monotonic version/sequence number ≥ stored min-version (OTP/secure storage) |
| Key storage | device **public** key (or its hash) in OTP/fuses or a secure element (ATECC608, SE050) / TrustZone, rooted in secure boot |
| Confidentiality | **AES-GCM** payload encryption (COSE_Encrypt), per-device key |

Asymmetric signing is preferred: devices hold only a **public** key, so a single
device compromise does not forge patches for the fleet (unlike a shared CMAC key).

## How a SUIT/COSE deployment maps onto TinyMLDelta

1. Publisher builds the `.tmd` patch, then wraps it in a **SUIT manifest**
   (digest of the `.tmd`, version, target) signed with **COSE_Sign1** (Ed25519/ES256).
2. The device's `verify_patch()` (mbedTLS / secure element / a SUIT processor)
   verifies the COSE signature against the provisioned public key, checks the
   SUIT digest against the received `.tmd`, and enforces the anti-rollback version.
3. On success the core proceeds to guardrails → base-digest → apply.

TinyMLDelta is the **delta payload + apply mechanism**; SUIT/COSE is the
**security envelope**, so it composes with the existing IoT secure-update
ecosystem instead of inventing crypto.

## Status & roadmap

| Item | State |
|------|-------|
| CRC32 integrity + base-digest verify | **implemented** |
| `verify_patch` hook + fail-closed gating (`TMD_FEAT_VERIFY_SIG`) | **implemented** (platform supplies the verifier) |
| SHA-256 digests (use the 32-byte header fields) | wire-format ready; pipeline planned |
| Reference Ed25519 / COSE_Sign1 verifier example | planned |
| SUIT manifest wrapping + anti-rollback version | planned |
| COSE_Encrypt confidentiality | planned |

The `algo` enum (`2=SHA256`, `3=CMAC+CRC`), 32-byte digest fields, and `sha256_*`
/ `cmac_verify` ports hooks already reserve space for these, so adoption is
incremental, not a redesign.
