# Security Policy

## Reporting a Vulnerability

If you discover a security vulnerability in libcs, please report it
responsibly:

1. **Do NOT open a public GitHub issue** for security vulnerabilities
2. **Use GitHub Private Security Advisories**:
   - Go to the repository's Security tab
   - Click "Report a vulnerability"
   - Provide a description and reproduction steps

## Response Timeline

| Step | Target |
|------|--------|
| Acknowledge receipt | Within 48 hours |
| Initial assessment | Within 5 business days |
| Fix or mitigation | Within 30 days (severity-dependent) |
| Public disclosure | After fix is released, coordinated with reporter |

## Supported Versions

| Version | Supported |
|---------|-----------|
| 1.x.y | Yes |
| < 1.0 | No |

## Scope

- Vulnerabilities in libcs source code (`src/`, `include/`)
- Vulnerabilities in the simulator backend that could mask real bugs
- API misuse patterns that could lead to memory corruption or undefined behavior

## Out of Scope

- Vulnerabilities in dependencies (xNVMe, SPDK, liburing, libnvme) — report to
  their respective security teams
- Vulnerabilities requiring physical access to NVMe hardware
- Denial of service requiring root privileges
