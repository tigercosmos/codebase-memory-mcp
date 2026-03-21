# Security Policy

## Reporting a Vulnerability

If you discover a security vulnerability, please report it responsibly:

1. **Do NOT open a public issue** for security vulnerabilities
2. Email: martin.vogel.tech@gmail.com
3. Include: description, reproduction steps, affected version, potential impact

We will acknowledge your report within 48 hours and provide a fix timeline within 7 days.

## Security Measures

This project implements multiple layers of security verification:

### Build-Time (CI)

- **8-layer security audit suite** runs on every build (static analysis, binary string audit, network egress monitoring, install path validation, MCP robustness testing, UI security audit, vendored dependency integrity, smoke test hardening)
- **All dangerous function calls** (`system()`, `popen()`, `fork()`, `connect()`) require a reviewed entry in `scripts/security-allowlist.txt`
- **Vendored dependency checksums** verified on every build (72 files, SHA-256)

### Release-Time

- **VirusTotal scanning** — all release binaries scanned by 70+ antivirus engines, reports linked in release notes
- **SLSA build provenance** — cryptographic attestation proving each binary was built by GitHub Actions from this repository
- **Sigstore cosign signing** — keyless signatures verifiable by anyone
- **SBOM** — Software Bill of Materials listing all vendored dependencies
- **SHA-256 checksums** — published with every release

### Code-Level Defenses

- **Shell injection prevention** — `cbm_validate_shell_arg()` rejects metacharacters before all `popen()`/`system()` calls
- **SQLite authorizer** — blocks `ATTACH`/`DETACH` at engine level
- **CORS locked to localhost** — graph UI only accessible from localhost origins
- **Path containment** — `realpath()` check prevents reading files outside project root
- **Process-kill restriction** — only server-spawned PIDs can be terminated

### Verification

Users can independently verify any release binary:

```bash
# SLSA provenance (proves binary came from this repo's CI)
gh attestation verify <downloaded-file> --repo DeusData/codebase-memory-mcp

# Sigstore cosign (keyless signature)
cosign verify-blob --bundle <file>.bundle <file>

# SHA-256 checksum
sha256sum -c checksums.txt
```

## Supported Versions

| Version | Supported |
|---------|-----------|
| 0.5.x   | Yes       |
| < 0.5   | No (Go codebase, superseded by C rewrite) |
