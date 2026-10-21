#!/usr/bin/env bash

set -euo pipefail

CERT_URL="https://curl.se/ca/cacert.pem"
CHECKSUM_URL="${CERT_URL}.sha256"

TMP_CERT=""
TMP_CHECKSUM=""

cleanup() {
    if [[ -n "${TMP_CERT}" ]]; then
        rm -f -- "${TMP_CERT}"
    fi

    if [[ -n "${TMP_CHECKSUM}" ]]; then
        rm -f -- "${TMP_CHECKSUM}"
    fi
}

download() {
    local url="$1"
    local dest="$2"

    if command -v curl &>/dev/null; then
        curl \
            --silent \
            --show-error \
            --fail \
            --location \
            --max-time 30 \
            --output "${dest}" \
            "${url}"
    elif command -v wget &>/dev/null; then
        wget \
            --quiet \
            --timeout=30 \
            --output-document="${dest}" \
            "${url}"
    else
        echo "neither curl nor wget found" >&2
        return 1
    fi
}

verify_checksum() {
    local file="$1"
    local checksum="$2"

    local expected
    expected=$(awk 'NR == 1 { print $1 }' "${checksum}")

    if [[ ! "${expected}" =~ ^[0-9a-f]{64}$ ]]; then
        echo "invalid checksum file" >&2
        return 1
    fi

    local actual
    actual=$(sha256sum "${file}" | awk '{ print $1 }')

    if [[ "${actual}" != "${expected}" ]]; then
        echo "checksum mismatch: expected ${expected}, got ${actual}" >&2
        return 1
    fi
}

verify_cert() {
    local file="$1"

    local count
    count=$(grep -c "BEGIN CERTIFICATE" "${file}" || true)
    if (( count < 100 )); then
        echo "too few certificates (${count}), expected 100+" >&2
        return 1
    fi

    if ! openssl storeutl -noout -certs "${file}" &>/dev/null; then
        echo "downloaded file is not a valid certificate bundle" >&2
        return 1
    fi

    echo "${count}"
}

bundle_mozilla_date() {
    local file="$1"

    local stamp
    stamp=$(sed -n 's/^## Certificate data from Mozilla as of: //p' "${file}" | awk 'NR == 1')

    echo "${stamp:-unknown}"
}

nearest_expiry() {
    local file="$1"

    local epoch
    epoch=$(openssl storeutl -noout -text -certs "${file}" \
        | sed -n 's/^ *Not After *: *//p' \
        | date -u -f - +%s \
        | awk 'NR == 1 || $1 < min { min = $1 } END { if (NR > 0) print min }')

    if [[ -z "${epoch}" ]]; then
        echo "unknown"
        return
    fi

    local days
    days=$(( (epoch - $(date -u +%s)) / 86400 ))

    echo "$(date -u -d "@${epoch}" '+%Y-%m-%d %H:%M:%S UTC') (${days} days left)"
}

cmd_update() {
    local dest="$1"

    mkdir -p "$(dirname "${dest}")"

    trap cleanup EXIT

    TMP_CERT=$(mktemp "${dest}.XXXXXX")
    TMP_CHECKSUM=$(mktemp)

    download "${CERT_URL}" "${TMP_CERT}"
    download "${CHECKSUM_URL}" "${TMP_CHECKSUM}"

    verify_checksum "${TMP_CERT}" "${TMP_CHECKSUM}"

    local count
    count=$(verify_cert "${TMP_CERT}")

    chmod 0644 "${TMP_CERT}"
    mv -f -- "${TMP_CERT}" "${dest}"
    TMP_CERT=""

    local size
    size=$(wc -c < "${dest}")

    echo "updated: ${dest}"
    echo "certificates: ${count}"
    echo "size: ${size} bytes"
    echo "mozilla date: $(bundle_mozilla_date "${dest}")"
    echo "nearest expiry: $(nearest_expiry "${dest}")"
}

[[ -z "${1:-}" ]] && { echo "usage: $0 <path/to/cacert.pem>" >&2; exit 1; }

cmd_update "$1"
