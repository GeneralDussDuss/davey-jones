# ESP32-C6 Security Guidelines — DAVEY JONES Audit Checklist

**Source**: Espressif ESP-IDF stable docs, `/esp32c6/security/*` (11 pages fetched 2026-04-11).
**Scope**: Every guideline, recommendation, warning, and CVE from the upstream tree, plus a verdict on applicability to Davey Jones specifically.

**Context reminder**: Davey Jones is a **single-user offensive/pentesting tool**, not a mass-deployed IoT product. Some of Espressif's production recommendations are overhead or actively counter-productive for this use case (e.g. Secure Boot makes on-device iteration painful). Each item is marked with an applicability verdict — do not blindly apply everything or you will hand-tie yourself in development.

---

## VERDICT LEGEND

| Tag | Meaning |
|---|---|
| **[DEV]** | Apply during development — now, as a default. |
| **[REL]** | Apply only if/when you flip Davey Jones into a "hardened field kit" release. |
| **[SKIP]** | Legitimately not relevant to a personal pentesting tool. Documented for completeness. |
| **[☠ BURN]** | Irreversible eFuse operation. **Do not touch** without a written rollback check — there is no rollback. |
| **[CVE]** | Covered under the version-floor section, not a per-project toggle. |

---

## SECTION 0 — MINIMUM IDF VERSION FLOOR (from CVE audit)

Pin **ESP-IDF v5.3 or later** (5.3+ patches the most recent relevant CVEs). Verify after clone:

```bash
cd $IDF_PATH && git describe --tags
```

CVEs that affect what Davey Jones will actually use (WiFi / BLE scan / lwIP / ESP-NOW / WPS):

| CVE | Affects | Relevant to DJ? | Verdict |
|---|---|---|---|
| CVE-2026-25532 (WPS Enrollee integer underflow, GHSA-m2h2-683f-9mw7) | ESP-IDF | **Yes** — we will scan WPS | **[CVE]** Pin IDF to version with fix |
| CVE-2025-52471 (ESP-NOW integer underflow, GHSA-hqhh-cp47-fv5g) | ESP-IDF | Yes if we add ESP-NOW wardrive mesh | **[CVE]** Pin IDF |
| CVE-2024-28183 (Bootloader TOCTOU anti-rollback) | ESP-IDF | No — we won't enable anti-rollback | **[SKIP]** |
| CVE-2023-52160 (PEAP Phase-2 auth flaw) | ESP-IDF | Maybe — if we attack EAP networks | **[CVE]** Pin IDF |
| CVE-2023-24023 (BT BLUFFS) | ESP-IDF | Only if we pair over BT | **[SKIP]** |
| CVE-2020-26142 (WLAN FragAttacks) | ESP-IDF | **Yes** — WiFi stack | **[CVE]** Pin IDF |
| CVE-2020-12638 (WiFi auth bypass) | ESP-IDF | **Yes** — WiFi stack | **[CVE]** Pin IDF |
| CVE-2020-22283/22284 (lwIP buffer overflows) | ESP-IDF ≥4.4.1 | **Yes** — lwIP is in every WiFi app | **[CVE]** Already fixed in 4.4.1+ |
| CVE-2021-31571/31572 (BadAlloc) | ESP-IDF | Yes | **[CVE]** Already fixed in current IDF |
| CVE-2024-53845 (ESPTouch v2 constant IV) | ESP-IDF | No — not using ESPTouch | **[SKIP]** |
| CVE-2025-55297 (BluFi example overflow) | Example code | No — not using BluFi | **[SKIP]** |
| CVE-2022-24893 (BT Mesh) | ESP-IDF | No — not doing BT Mesh | **[SKIP]** |
| CVE-2023-35818 (EMFI Secure Boot bypass) | ESP32 v3 chips only | No — we're on C6 | **[SKIP]** |

**Action**: `idf_tools.py install-python-env` then `get_idf` to a v5.3.x or newer tag. Subscribe to https://github.com/espressif/esp-idf/security/advisories (or at least `git fetch --tags` before each release build).

---

## SECTION 1 — SECURE BOOT V2

Upstream position: "highly recommended on all production devices." Davey Jones position: **[SKIP] during development, [REL] optional for field hardening**. Secure Boot makes every iteration require re-signing and eliminates the ability to quickly flash experimental payloads from the device itself. For a personal pentesting tool this is a net loss. Document the enablement workflow anyway so it's one command away if the device goes beyond single-user.

### Facts (verbatim-ish)
- Schemes: RSA-3072 (~10.2 ms verify) or ECDSA P-256 (~83.9 ms verify) or P-192. **One scheme per device.**
- Up to **three** public key digests stored in eFuse. Unused slots must have `KEY_REVOKE` burned to permanently disable them.
- Digest is 32-byte SHA-256 of the 776-byte signature block (offset 36 to 812). Write-protected, **must not be read-protected**.
- Key generation quality: "A signing key generated [with idf.py] will use the best random number source available... For production environments, we recommend generating the key pair using OpenSSL."
- "Secure Boot will not be enabled until after a valid partition table and app image have been flashed."
- Key revocation:
  - **Conservative**: revoke old key only after OTA succeeds, via `esp_ota_revoke_secure_boot_public_key()`
  - **Aggressive**: `SECURE_BOOT_AGGRESSIVE_REVOKE` eFuse → revokes on first verify failure
- Flash Encryption interaction: "It is recommended to use both features together." Without FE, a TOCTOU attack is possible where flash swaps after verify.
- Post-enable: "JTAG debugging is disabled via eFuse... USB-OTG USB stack in the ROM [is disabled]" — no more DFU updates over serial.
- "Once a key is revoked, it can never be used for verifying the signature of an image."
- "After Secure Boot is enabled, further read-protection of eFuse keys is not possible" (this is a feature — prevents an attacker from blinding the digest).

### CONFIG_ symbols
- `CONFIG_SECURE_BOOT` — master enable
- `CONFIG_SECURE_BOOT_VERSION` → `SECURE_BOOT_V2_ENABLED`
- `CONFIG_SECURE_BOOT_V2_ALLOW_EFUSE_RD_DIS` — permits later read-protection
- `CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES` — auto-sign during build (disable for HSM flow)
- `CONFIG_SECURE_BOOT_ENABLE_AGGRESSIVE_KEY_REVOKE`
- `CONFIG_SECURE_BOOT_ALLOW_UNUSED_DIGEST_SLOTS`
- `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` — signature verify without hardware SB (useful for dev integrity without the lockdown)

### Davey Jones verdict
- **[DEV]** Leave Secure Boot **off**. Set `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y` only if we want a dev-time integrity check.
- **[REL]** If hardening: RSA-3072 (faster boot → matters for battery), one key, conservative revocation, keys generated with OpenSSL on an air-gapped host, private key stored offline.
- **[☠ BURN]** `SECURE_BOOT_EN`, `KEY_REVOKE0/1/2`, `SOFT_DIS_JTAG`, `DIS_PAD_JTAG`, `DIS_USB_JTAG`, `DIS_DIRECT_BOOT`, `SECURE_BOOT_AGGRESSIVE_REVOKE`.

---

## SECTION 2 — FLASH ENCRYPTION

Upstream position: "enable for confidentiality of stored software/data; release mode for production; unique per device." Davey Jones position: **[SKIP] during development, [REL] optional**. Same iteration cost as Secure Boot. Flash Encryption on a pentesting tool protects... tool config and captured data. Worth doing if we start saving victim data (wardriving CSVs, captured handshakes) to flash and the device may leave your person.

### Facts (verbatim-ish)
- Algorithm: **XTS-AES block cipher mode with 256-bit key size**.
- eFuse blocks used: `BLOCK_KEYN` (N=0–4 on C6 — **not BLOCK9/BLOCK_KEY5**, which can't store XTS-AES keys on C6). `KEY_PURPOSE_N=4` = XTS_AES_128_KEY. `SPI_BOOT_CRYPT_CNT` is 3-bit; encryption is active when an odd number of bits are set.
- Default encrypted partitions: Second Stage Bootloader, Partition Table, NVS Key Partition, Otadata, all `app` partitions, any partition marked `encrypted`.
- **NVS partition cannot itself be encrypted** — "the NVS library is not directly compatible with flash encryption." NVS Key Partition is encrypted instead (then NVS uses that key to encrypt entries).
- Development mode: `SPI_BOOT_CRYPT_CNT=0x1`, re-flashable, `DIS_DOWNLOAD_MANUAL_ENCRYPT` **not** burned. Can re-encrypt via UART bootloader.
- Release mode: `SPI_BOOT_CRYPT_CNT=7`, write-protected, `DIS_DOWNLOAD_MANUAL_ENCRYPT` burned. Plaintext updates only via OTA.
- "For production use, flash encryption should be enabled in the 'Release' mode only."
- "Do not interrupt power to the ESP32-C6 while the first boot encryption pass is running. If power is interrupted, the flash contents will be corrupted." — **the Nesso has a 250 mAh internal LiPo; keep it plugged in AND charged during first encrypt boot.**
- "If flash encryption was enabled accidentally, flashing of plaintext data will soft-brick the ESP32-C6."
- "Enabling flash encryption limits the options for further updates of ESP32-C6."
- "Enabling flash encryption will increase the size of bootloader, which might require updating partition table offset."
- Dev-mode disable: "SPI_BOOT_CRYPT_CNT eFuse. It can only be done one time per chip."

### CONFIG_ symbols
- `CONFIG_SECURE_FLASH_ENC_ENABLED`
- `CONFIG_SECURE_FLASH_ENCRYPTION_MODE` → Development or Release
- `CONFIG_SECURE_UART_ROM_DL_MODE`
- `CONFIG_BOOTLOADER_LOG_LEVEL`

### Davey Jones verdict
- **[DEV]** Off. We'll iterate too fast, and a botched first-encrypt with a dead battery soft-bricks the only board we have.
- **[REL]** If enabling: host-side encryption with unique per-device key, **power the Nesso from USB with a charged battery during first boot**, delete host key immediately after provisioning.
- **[☠ BURN]** `SPI_BOOT_CRYPT_CNT`, `DIS_DOWNLOAD_MANUAL_ENCRYPT`, any `BLOCK_KEYN` burn for XTS-AES purpose.

---

## SECTION 3 — ENABLEMENT WORKFLOW ORDER (when/if we go [REL])

Upstream mandates a **strict order**:

1. **Flash Encryption first**, then **Secure Boot v2 second.**
   Rationale: "Secure Boot v2 write-protects `RD_DIS`, preventing subsequent Flash Encryption key read-protection."

### Complete command order (release mode, both features, for reference)

```bash
# --- FLASH ENCRYPTION ---
esptool --port $PORT erase-flash
espsecure generate-flash-encryption-key fe_key.bin
espefuse --port $PORT burn-key BLOCK_KEY0 fe_key.bin XTS_AES_128_KEY
espefuse --port $PORT --chip esp32c6 burn-efuse SPI_BOOT_CRYPT_CNT 7
espefuse burn-efuse --port $PORT \
    DIS_DOWNLOAD_ICACHE 0x1 \
    DIS_DIRECT_BOOT 0x1 \
    DIS_USB_JTAG 0x1 \
    DIS_PAD_JTAG 0x1 \
    DIS_DOWNLOAD_MANUAL_ENCRYPT 0x1
espefuse --port $PORT write-protect-efuse DIS_ICACHE

# Encrypt binaries on host
espsecure encrypt-flash-data --aes-xts --keyfile fe_key.bin \
    --address 0x0     --output bootloader-enc.bin  build/bootloader/bootloader.bin
espsecure encrypt-flash-data --aes-xts --keyfile fe_key.bin \
    --address 0x8000  --output partition-table-enc.bin build/partition_table/partition-table.bin
espsecure encrypt-flash-data --aes-xts --keyfile fe_key.bin \
    --address 0x10000 --output davey_jones-enc.bin build/davey_jones.bin

# --- SECURE BOOT V2 ---
espsecure generate-signing-key --version 2 --scheme rsa3072 sb_key.pem
espsecure digest-sbv2-public-key --keyfile sb_key.pem --output digest.bin
espefuse --port $PORT --chip esp32c6 burn-key BLOCK_KEY1 digest.bin SECURE_BOOT_DIGEST0
espefuse --port $PORT --chip esp32c6 burn-efuse SECURE_BOOT_EN
espefuse burn-efuse --port $PORT \
    SOFT_DIS_JTAG 0x1 \
    SECURE_BOOT_AGGRESSIVE_REVOKE 0x1
espefuse -p $PORT write-protect-efuse RD_DIS
espefuse --port $PORT --chip esp32c6 burn-efuse SECURE_BOOT_KEY_REVOKE1
espefuse --port $PORT --chip esp32c6 burn-efuse SECURE_BOOT_KEY_REVOKE2

# Sign binaries
espsecure sign-data --version 2 --keyfile sb_key.pem \
    --output bootloader-signed.bin build/bootloader/bootloader.bin
espsecure sign-data --version 2 --keyfile sb_key.pem \
    --output davey_jones-signed.bin build/davey_jones.bin
espsecure signature-info-v2 bootloader-signed.bin   # verify

# --- FINAL LOCK ---
espefuse --port $PORT burn-efuse ENABLE_SECURITY_DOWNLOAD
rm fe_key.bin   # key deletion MUST be the very last step
```

### Davey Jones verdict
**[REL only]**. Record everything in a signed log file if we ever do this. **Absolutely do not run this workflow on the only dev board.** Buy a second Nesso before attempting — the cost of a bricked dev unit exceeds the cost of the board.

---

## SECTION 4 — DEBUG INTERFACES

### JTAG
- Auto-disabled by eFuse if any security feature is enabled.
- Can be disabled independently via eFuse API.
- **Soft disable** supported (re-enable via HMAC + secret key) — worth knowing as an escape hatch.
- Post-Secure-Boot: hard-disabled.

### UART ROM Download Mode
- "Secure UART Download Mode activates if any security features are enabled."
- In secure mode: no arbitrary code, only SPI config updates, basic flash write, `get-security-info`.
- `esptool can only work with the argument --no-stub` in secure mode.
- Permanent disable: `CONFIG_SECURE_UART_ROM_DL_MODE` = "Permanently disable" **or** `esp_efuse_disable_rom_download_mode()` at runtime.

### USB-OTG DFU
- Disabled automatically once Secure Boot or Flash Encryption is on.
- **This kills the Nesso's primary update path** — USB-C programming. Post-enable you can only OTA.

### Davey Jones verdict
- **[DEV]** Leave all debug interfaces on. USB JTAG is the only sane way to debug on the Nesso.
- **[REL]** `DIS_PAD_JTAG`, `DIS_USB_JTAG`, `CONFIG_SECURE_UART_ROM_DL_MODE=permanent_disable`.
- **[DEV]** Keep `CONFIG_ESP_SYSTEM_GDBSTUB_RUNTIME=y` off unless actively debugging — leaks internal state over UART.

---

## SECTION 5 — MEMORY PROTECTION (PMA / PMP)

Upstream position: "Keep `CONFIG_ESP_SYSTEM_MEMPROT` enabled by default."

### Facts
- Enforces R/W on data memories, R/X on instruction memories.
- Raises violation interrupt on breach.
- "Can help prevent remote code injection from software vulnerabilities."
- Monitored attribute: permission violations.

### CONFIG_ symbols
- `CONFIG_ESP_SYSTEM_MEMPROT` — keep on.

### Davey Jones verdict
**[DEV]** **Keep on, always.** Zero iteration cost, real protection against WiFi-stack memory-corruption bugs in the CVE list. This is a free win.

---

## SECTION 6 — SIDE-CHANNEL / DPA PROTECTION

### Facts
- Clock frequency dynamically adjusts during crypto peripheral operations.
- `CONFIG_ESP_CRYPTO_DPA_PROTECTION_LEVEL` — higher = more secure, more perf hit.
- "Hardware RNG must be enabled for DPA protection to function correctly."

### Davey Jones verdict
**[SKIP]** for dev. DPA attacks require physical access + oscilloscope + patience — not a realistic threat model for a tool you carry in a pocket. If Davey Jones ever stores secrets worth side-channel attacks, reconsider. Keep on **default** level (not "off", not "high") — zero-cost middle ground.

---

## SECTION 7 — RANDOM NUMBER GENERATION

### Facts
- Hardware TRNG must be enabled for DPA protection to function.
- RNG is **the** root of trust for key generation — if RNG is weak, every downstream key is weak.
- "A signing key generated this way will use the best random number source available. If this random number source is weak, then the private key will be weak."
- Implicit rule: **do not generate Secure Boot / Flash Encryption keys on the device itself**; generate on a host with proper entropy (OpenSSL on Linux with `/dev/urandom` backed by good entropy).

### Davey Jones verdict
**[DEV]** Use host-generated keys via OpenSSL if we ever touch keys. Do not use `idf.py secure-generate-signing-key` on Windows without good reason.

---

## SECTION 8 — NETWORK SECURITY (TLS, CERTIFICATES, HTTPS)

### TLS Facts
- "It is recommended to use TLS in all external communications."
- Default ESP-IDF protocol components are pre-vetted: "use ... in their default configuration, which has been ensured to be secure."
- Applications should use **ESP-TLS** abstraction, not raw mbedTLS.
- Server verification mandatory: "It is highly recommended to verify the identity of the server based on X.509 certificates."

### Certificate Bundle
- `esp_crt_bundle` API for custom root CAs.
- `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN` for market-share default.
- If enabling `CONFIG_MBEDTLS_HAVE_TIME_DATE`: implement SNTP, maintain trusted certs.
- Root cert updates can break OTA → enable rollback before updating.

### Davey Jones verdict
- **[DEV]** If Davey Jones phones home (to PYTHIA or a wardriving aggregator), **use ESP-TLS with default config and server cert verification**. No HTTPS-disabling shortcuts.
- **[DEV]** Trust the default cert bundle; don't embed custom root certs unless we need mutual TLS.
- **[SKIP]** for pure offline/standalone operation (no backend = no TLS).

---

## SECTION 9 — NVS ENCRYPTION

### Facts
- NVS itself isn't flash-encrypted; its key is.
- "By default, ESP-IDF components writes the device specific data into the default NVS partition, including Wi-Fi credentials too, and it is recommended to protect this data using NVS Encryption."
- Two schemes:
  1. **HMAC-based** (standalone) — key derived via HMAC peripheral from an eFuse block (`HMAC_UP` purpose). Does NOT require Flash Encryption.
  2. **Flash-Encryption-based** — key lives in an encrypted NVS Key Partition; requires Flash Encryption.

### HMAC-based NVS (recommended for DJ if we store creds at all)
```bash
python3 nvs_partition_gen.py generate-key --key_protect_hmac \
    --kp_hmac_keygen --kp_hmac_keyfile hmac_key.bin --keyfile nvs_encr_key.bin
espefuse --port $PORT burn-key BLOCK hmac_key.bin HMAC_UP
python3 nvs_partition_gen.py encrypt input.csv nvs_enc.bin 0x3000 \
    --inputkey nvs_encr_key.bin
```

### CONFIG_ symbols
- `CONFIG_NVS_ENCRYPTION`
- `CONFIG_NVS_SEC_KEY_PROTECTION_SCHEME` → `CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC` or `_USING_FLASH_ENC`
- `CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID`

### Davey Jones verdict
- **[DEV]** Off until we actually write WiFi credentials or API keys to NVS.
- **[REL]** If we start storing target creds (captured WPA handshakes, saved evil-portal harvests), **enable HMAC-based NVS encryption**. It doesn't require Flash Encryption so it's the low-cost win. **[☠ BURN]** one eFuse block (`HMAC_UP`).

---

## SECTION 10 — OTA UPDATES

### Facts
- "OTA Updates must happen over secure transport, e.g., HTTPS."
- Use `esp_https_ota` simplified abstraction.
- Pair with `esp_ota_mark_app_valid_cancel_rollback()` — keep app in "pending verify" until it proves itself post-boot.
- With Secure Boot: host signed images server-side.
- With Flash Encryption: device handles encryption locally; no server-side work.
- **Anti-rollback**: `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=y` — compares `esp_app_desc_t::secure_version` against eFuse `SECURE_VERSION`.

### Davey Jones verdict
- **[DEV]** Don't bother with OTA early. USB reflashing is fine for a personal tool.
- **[REL]** If adding OTA: HTTPS only, signed images, rollback enabled, **no anti-rollback eFuse burn** (we want to downgrade for debugging). Anti-rollback is for fleet deployments.
- **[SKIP]** Anti-rollback: irrelevant for single-device use. CVE-2024-28183 is a moot point.

---

## SECTION 11 — ANTI-ROLLBACK

### Facts
- Managed by 2nd stage bootloader.
- `secure_version` stored in eFuse, incremented monotonically.
- Compared at boot + OTA.
- **Burn-once** — you can raise but never lower the version.

### Davey Jones verdict
**[SKIP]**. Downgrade capability is valuable during dev.

---

## SECTION 12 — WI-FI SECURITY

### Facts
- WPA3 / WPA2-Enterprise / WPA-TKIP / WEP all supported.
- FragAttacks (CVE-2020-26142) patched in current IDF.
- CVE-2023-52160 PEAP Phase-2 — relevant if we attack corporate EAP.
- CVE-2026-25532 WPS — relevant since we WILL probe WPS.

### Davey Jones verdict
- **[CVE]** IDF version floor covers the patches.
- **[DEV]** Standard `esp_wifi_*` API. Use monitor/sniff mode via `esp_wifi_set_promiscuous()` + `esp_wifi_80211_tx()` — both still work on C6 per upstream.
- **[NOTE]** C6 is **WiFi 6 (802.11ax)**. Upstream Ghost ESP / Marauder / Bruce mostly target n-only stacks; expect some packet-crafting helpers to need adjustment for HE frame formats. Save this as a task for the WiFi module.

---

## SECTION 13 — TRUSTED EXECUTION ENVIRONMENT (ESP-TEE)

### What it is
Hardware-backed split between **TEE** (trusted) and **REE** (rich — your normal app) using PMA + APM. Root-of-Trust chain: BootROM → 2nd-stage bootloader → TEE firmware → REE app.

### Services available
- **Secure Storage** — encrypted NVS in `secure_storage` partition, `tee_sec_stg_ns` namespace. Keys: AES-256, ECDSA P-256. Backed by HMAC-derived XTS-AES via eFuse.
- **Attestation** — signed EAT tokens binding device identity (SHA256 of MAC) + firmware measurements (bootloader/TEE/app versions) + Secure Boot status. ECDSA P-256 signature from TEE secure storage key. Challenge-nonce replay protection.
- **TEE OTA** — updates the TEE binary independently of REE app. State machine: NEW → PENDING_VERIFY → VALID/INVALID. Automatic rollback on boot failure.

### Memory impact (this matters for us)
- TEE reserves top SRAM via `CONFIG_SECURE_TEE_IRAM_SIZE`, `_DRAM_SIZE`, `_STACK_SIZE`, `_INTR_STACK_SIZE`. **Reduces REE SRAM budget.** Ghost ESP builds are already SRAM-tight on C-series chips — enabling TEE will cost us headroom for LVGL + WiFi buffers.
- **Cryptographic accelerators (AES, SHA, ECC, HMAC, DS) become inaccessible from REE once TEE is enabled.** The REE falls back to software mbedTLS. Consequence: TLS handshakes get slower, our CPU load goes up. Not a blocker but worth knowing.
- Partition table: at least one `app/tee_0` partition, min 192 KB. The partition following the last TEE partition must be MMU-page aligned or Secure Boot verify will fail.
- Flash SPI1 memory protection optional: `CONFIG_SECURE_TEE_EXT_FLASH_MEMPROT_SPI1` → **+37.33% read latency, +11.32% write latency**. Off by default.
- Multitasking: **not supported in TEE**. Secure service calls serialize.
- MPI/RSA accelerator protection: deferred to future IDF releases (i.e. currently RSA crypto is not isolated by TEE).

### CONFIG_ symbols
- `CONFIG_SECURE_ENABLE_TEE`
- `CONFIG_SECURE_TEE_DEBUG_MODE`
- `CONFIG_SECURE_TEE_LOG_LEVEL`
- `CONFIG_SECURE_TEE_IRAM_SIZE` / `_DRAM_SIZE` / `_STACK_SIZE` / `_INTR_STACK_SIZE`
- `CONFIG_SECURE_TEE_SEC_STG_MODE`
- `CONFIG_SECURE_TEE_SEC_STG_EFUSE_HMAC_KEY_ID` (0–5, **not -1**)
- `CONFIG_SECURE_TEE_PBKDF2_EFUSE_HMAC_KEY_ID` (must differ from SEC_STG ID)
- `CONFIG_SECURE_TEE_ATTESTATION` (default on)
- `CONFIG_SECURE_TEE_EXT_FLASH_MEMPROT_SPI1` (perf hit — leave off)

### Davey Jones verdict
**[SKIP] — HARD SKIP.** TEE is designed for devices that hold persistent secrets (customer data, fleet certs, cloud API keys). Davey Jones holds *transient* captured data and wants *maximum* crypto accelerator throughput + *maximum* SRAM. Enabling TEE would cost us SRAM, crypto HW access, and boot latency for zero defensive benefit against our threat model (physical device theft of a pentesting tool by someone who already has the pentester's laptop).

**One exception**: if we add **Secure Storage for captured handshake data** (so a stolen Nesso can't reveal its capture buffer), TEE Secure Storage *alone* (without attestation/OTA) is a reasonable ask. But even then, HMAC-based NVS Encryption (Section 9) gives 90% of the value for 10% of the cost.

---

## SECTION 14 — DEVICE IDENTITY / DIGITAL SIGNATURE PERIPHERAL

### Facts
- ESP32-C6 has a **DS (Digital Signature) peripheral** — hardware-accelerated RSA signatures.
- "DS peripheral maintains RSA private key inaccessibility to software" — key lives in eFuse, software never sees it.
- Typical use: TLS mutual authentication (client cert with hardware-held key).

### Davey Jones verdict
**[SKIP]** — unless Davey Jones ever needs to authenticate to a backend via mTLS. If we build a "phone home my wardriving log" feature to PYTHIA over mTLS, this is the right way to do it. Note and defer.

---

## SECTION 15 — SECURITY ADVISORIES & UPDATES

### Facts
- "Espressif publishes critical Security Advisories (hardware and software)."
- ESP-IDF advisories: https://github.com/espressif/esp-idf/security/advisories
- "We recommend periodically updating to the latest bugfix version."

### Davey Jones verdict
**[DEV]** Before each significant rebuild: `cd $IDF_PATH && git fetch --tags && git log --oneline HEAD..$(latest-tag)`. Skim release notes for CVE fixes. Pin to a stable tag, not master.

---

## SECTION 16 — HAZARD INDEX (☠ ONE-WAY BURN OPERATIONS)

**Do not execute any command in this table without a second person's sign-off. Every single one is irreversible.**

| eFuse | Where | Consequence of accidental burn |
|---|---|---|
| `SPI_BOOT_CRYPT_CNT = 7` | Flash Encryption release | Board locked to encrypted flash forever. Plaintext flashes soft-brick the board. |
| `SECURE_BOOT_EN` | Secure Boot | Board will only boot signed images from that moment onward. |
| `KEY_REVOKE0/1/2` | Secure Boot | That digest slot is dead forever. |
| `SECURE_BOOT_AGGRESSIVE_REVOKE` | Secure Boot | One bad signature kills a key. |
| `DIS_PAD_JTAG` | Debug | JTAG physically unreachable. |
| `DIS_USB_JTAG` | Debug | USB JTAG physically unreachable. |
| `DIS_DIRECT_BOOT` | Boot | Legacy SPI boot gone. |
| `DIS_DOWNLOAD_ICACHE` | Boot | UART download cache gone. |
| `DIS_DOWNLOAD_MANUAL_ENCRYPT` | Flash Encryption | Bootloader-side encryption gone. |
| `ENABLE_SECURITY_DOWNLOAD` | ROM | UART download mode locked into secure-only. |
| `SOFT_DIS_JTAG` | Debug | JTAG SW-disabled (reversible via HMAC, less catastrophic). |
| `write-protect RD_DIS` | Secure Boot | Read-protection eFuses are write-locked — can't read-protect anything further. |
| `BLOCK_KEYN burn-key` | Any key | That eFuse block is permanently committed to the given purpose. |

**Rule**: Every `espefuse burn-*` command used against Davey Jones must be committed to git with the exact command, the board's MAC, and the date. We should never be unsure which eFuses we've touched.

---

## SECTION 17 — RECOMMENDED DAVEY JONES SECURITY POSTURE (DEV PHASE)

Opinionated summary. This is what I'll set in `sdkconfig.defaults` when we start the BSP component:

```kconfig
# --- VERSION FLOOR ---
# (enforced via top-of-repo IDF version check, not kconfig)

# --- CHEAP WINS ---
CONFIG_ESP_SYSTEM_MEMPROT=y                     # keep PMA on — free
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y         # OTA safety net
CONFIG_ESP_CRYPTO_DPA_PROTECTION_LEVEL_LOW=y    # trivial default

# --- INTEGRITY WITHOUT LOCKDOWN ---
CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y      # dev-time signature check, no hardware lock
# (implies generating a dev signing key that lives in the repo, NEVER committed)

# --- LEAVE OFF DURING DEV ---
# CONFIG_SECURE_BOOT                 -- iteration killer
# CONFIG_SECURE_FLASH_ENC_ENABLED    -- brick risk on 250mAh battery
# CONFIG_SECURE_ENABLE_TEE           -- SRAM / crypto HW tax
# CONFIG_NVS_ENCRYPTION              -- nothing to protect yet
# CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK-- we want downgrade capability

# --- DEBUG STAYS ON ---
CONFIG_ESP_SYSTEM_GDBSTUB_RUNTIME=n             # off unless actively debugging
# JTAG / USB download: default-on
```

And a one-liner rule: **No `espefuse burn-*` commands run until we have a second Nesso board in hand.** This rule lives in the project README from day 1.

---

## SECTION 18 — STEMMA QT / ESP32-S2 QT Py ADDITION NOTES

(Not a docs extract — personal architecture notes for when we wire the S2 to the Nesso.)

- The S2 connects via STEMMA QT = **same I²C bus as the Nesso's internal sensors** (GPIO10/8). Address space is already crowded: 0x38 (touch), 0x43 (E0), 0x44 (E1), 0x68 (IMU). Pick the S2's I²C slave address from the free zones — **avoid 0x00–0x07, 0x78–0x7F (reserved), and anything within ±1 of existing devices**. Reasonable choice: `0x42` or `0x55`.
- Treat the S2 as a **custom secure service endpoint** from Davey Jones's perspective: Nesso is master, S2 is an I²C slave that exposes a defined command set (scan, inject, BadUSB payload, etc.). Document the command protocol like we'd document any RPC.
- The S2 is a **second WiFi radio and a native USB HID device**. From a threat-model standpoint, if the S2 gets plugged into a victim USB port, it becomes an untrusted peripheral on the Nesso's own I²C bus. **Never trust I²C input from the S2 on the Nesso side** — validate lengths, bounds-check every buffer. ESP-IDF I²C slave API doesn't do this for you.
- S2 has **no BLE**. The BLE-advertising gap on the C6 therefore stays open unless we add a third board (nRF52840 Feather or ESP32-S3). Tracked as future work.

---

## APPENDIX — PAGES FETCHED

1. `/security/index.html`
2. `/security/security.html` (Overview)
3. `/security/flash-encryption.html`
4. `/security/secure-boot-v2.html`
5. `/security/tee/index.html`
6. `/security/tee/tee.html` (User Guide)
7. `/security/tee/tee-advanced.html`
8. `/security/tee/tee-sec-storage.html`
9. `/security/tee/tee-ota.html`
10. `/security/tee/tee-attestation.html`
11. `/security/security-features-enablement-workflows.html`
12. `/security/vulnerabilities.html`

Plus CVE cross-reference against features Davey Jones will actually use.
