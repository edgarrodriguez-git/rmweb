"""Test CA and server certificates for the fake captive portal.

Drives the OpenSSL 3 command line (standard library only). The binary is resolved in this order:
the OPENSSL environment variable, `$(brew --prefix openssl@3)/bin/openssl`, then `openssl` on PATH.
Only a binary whose `version` output starts with "OpenSSL 3." is accepted (macOS ships LibreSSL).

Everything used here exists in OpenSSL 3.0 (GitHub's ubuntu-latest) and 3.5 (Homebrew). In particular the
expired and not-yet-valid leaves are issued with `openssl ca -startdate/-enddate`, because `x509 -not_before`
and `-not_after` only exist from 3.4.
"""
from __future__ import annotations

import datetime
import ipaddress
import os
import re
import shutil
import subprocess
import tempfile

_DNS_NAME = re.compile(r"^[A-Za-z0-9*._-]+$")
_CA_MIN_REMAINING_S = 30 * 86400
_cached_openssl: str | None = None


class OpenSSLUnavailable(RuntimeError):
    """No OpenSSL 3.x command line binary was found."""


class CertError(RuntimeError):
    """An openssl command failed."""


def _version_banner(path: str) -> str:
    try:
        out = subprocess.run([path, "version"], capture_output=True, text=True, timeout=15)
    except (OSError, subprocess.SubprocessError):
        return ""
    return out.stdout.strip()


def _candidates():
    """Yield (label, path-or-None) in resolution order."""
    env = os.environ.get("OPENSSL")
    if env:
        yield "$OPENSSL", shutil.which(env)
    brew = shutil.which("brew")
    if brew:
        try:
            prefix = subprocess.run([brew, "--prefix", "openssl@3"], capture_output=True, text=True,
                                    timeout=60).stdout.strip()
        except (OSError, subprocess.SubprocessError):
            prefix = ""
        yield "brew openssl@3", os.path.join(prefix, "bin", "openssl") if prefix else None
    yield "openssl on PATH", shutil.which("openssl")


def find_openssl(use_cache: bool = True) -> str:
    """Return the path of an OpenSSL 3.x binary, or raise OpenSSLUnavailable saying what was tried."""
    global _cached_openssl
    if use_cache and _cached_openssl:
        return _cached_openssl
    tried = []
    for label, path in _candidates():
        if not path or not os.access(path, os.X_OK):
            tried.append(f"{label}: not found")
            continue
        banner = _version_banner(path)
        if banner.startswith("OpenSSL 3."):
            if use_cache:
                _cached_openssl = path
            return path
        tried.append(f"{label}: {path} reports {banner or 'nothing'!r}, need OpenSSL 3.x")
    raise OpenSSLUnavailable("no OpenSSL 3.x binary found (" + "; ".join(tried) + ")")


def _san_extension(san) -> str:
    entries = []
    for raw in san:
        name = str(raw).strip("[]")
        try:
            ipaddress.ip_address(name)
            entries.append(f"IP:{name}")
        except ValueError:
            if not _DNS_NAME.match(name):
                raise ValueError(f"unusable --san entry: {raw!r}") from None
            entries.append(f"DNS:{name}")
    if not entries:
        raise ValueError("at least one SAN entry is required")
    return ",".join(entries)


def _stamp(when: datetime.datetime) -> str:
    return when.strftime("%Y%m%d%H%M%SZ")


_CONFIG = """\
[req]
distinguished_name = dn
prompt = no
default_md = sha256
[dn]
CN = fake-portal
[ca]
default_ca = CA_default
[CA_default]
database = {work}/index.txt
new_certs_dir = {work}/newcerts
serial = {work}/serial
default_md = sha256
unique_subject = no
policy = policy_any
[policy_any]
commonName = supplied
[ca_ext]
basicConstraints = critical,CA:TRUE
keyUsage = critical,keyCertSign,cRLSign
subjectKeyIdentifier = hash
[leaf_ext]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature
extendedKeyUsage = serverAuth
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always
subjectAltName = {san}
[self_ext]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature
extendedKeyUsage = serverAuth
subjectKeyIdentifier = hash
subjectAltName = {san}
"""


def make_certs(cert_dir, san) -> dict[str, tuple[str, str]]:
    """Write a test CA and four server certificates into `cert_dir`.

    Returns {"ca", "valid", "selfsigned", "expired", "notyet"} -> (cert_path, key_path).
      valid       signed by the CA, valid now
      selfsigned  signed by its own key, so it never chains to the CA
      expired     signed by the CA, not_after one day ago
      notyet      signed by the CA, not_before one year ahead
    Every leaf carries every `san` entry (IP: for addresses, DNS: for names). An existing CA in `cert_dir`
    is kept while it has more than 30 days left, so ca.pem stays stable across portal restarts and a copy
    pushed to the tablet keeps working; the leaves are always reissued.
    """
    san_ext = _san_extension(san)  # validate before looking for a binary
    openssl = find_openssl()
    cert_dir = os.fspath(cert_dir)
    os.makedirs(cert_dir, exist_ok=True)
    now = datetime.datetime.now(datetime.timezone.utc)
    ca_cert, ca_key = os.path.join(cert_dir, "ca.pem"), os.path.join(cert_dir, "ca.key")
    key_args = ["-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes"]

    with tempfile.TemporaryDirectory(prefix="fake-portal-ca-") as work:
        os.mkdir(os.path.join(work, "newcerts"))
        with open(os.path.join(work, "index.txt"), "w"):
            pass
        with open(os.path.join(work, "serial"), "w") as f:
            f.write("01\n")
        conf = os.path.join(work, "openssl.cnf")
        with open(conf, "w") as f:
            f.write(_CONFIG.format(work=work, san=san_ext))
        env = dict(os.environ, OPENSSL_CONF=conf)  # keep the system openssl.cnf out of it

        def run(*args):
            try:
                subprocess.run([openssl, *args], check=True, capture_output=True, text=True, timeout=60, env=env)
            except subprocess.CalledProcessError as exc:
                raise CertError(f"openssl {args[0]} failed: {exc.stderr.strip()}") from exc

        def ca_is_usable():
            if not (os.path.exists(ca_cert) and os.path.exists(ca_key)):
                return False
            check = subprocess.run([openssl, "x509", "-in", ca_cert, "-noout", "-checkend", str(_CA_MIN_REMAINING_S)],
                                   capture_output=True, env=env)
            return check.returncode == 0

        if not ca_is_usable():
            run("req", "-x509", "-config", conf, "-extensions", "ca_ext", *key_args, "-keyout", ca_key,
                "-out", ca_cert, "-subj", "/CN=rmweb fake portal test CA", "-days", "3650")

        def signed_leaf(name, not_before, not_after):
            cert, key = os.path.join(cert_dir, f"{name}.pem"), os.path.join(cert_dir, f"{name}.key")
            csr = os.path.join(work, f"{name}.csr")
            run("req", "-new", "-config", conf, *key_args, "-keyout", key, "-out", csr,
                "-subj", f"/CN=fake-portal {name}")
            run("ca", "-batch", "-config", conf, "-cert", ca_cert, "-keyfile", ca_key, "-in", csr, "-out", cert,
                "-extensions", "leaf_ext", "-notext", "-startdate", _stamp(not_before), "-enddate", _stamp(not_after))
            return cert, key

        day = datetime.timedelta(days=1)
        leaves = {
            "valid": signed_leaf("valid", now - day, now + 365 * day),
            "expired": signed_leaf("expired", now - 30 * day, now - day),
            "notyet": signed_leaf("notyet", now + 365 * day, now + 730 * day),
        }
        self_cert, self_key = os.path.join(cert_dir, "selfsigned.pem"), os.path.join(cert_dir, "selfsigned.key")
        run("req", "-x509", "-config", conf, "-extensions", "self_ext", *key_args, "-keyout", self_key,
            "-out", self_cert, "-subj", "/CN=fake-portal selfsigned", "-days", "365")

    for path in [ca_key, self_key, *(key for _, key in leaves.values())]:
        os.chmod(path, 0o600)
    return {"ca": (ca_cert, ca_key), "valid": leaves["valid"], "selfsigned": (self_cert, self_key),
            "expired": leaves["expired"], "notyet": leaves["notyet"]}
