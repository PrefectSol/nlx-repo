#!/usr/bin/env bash

set -euo pipefail

CERT_URL="https://curl.se/ca/cacert.pem"

download_cert() {
    local dest="$1"

    if command -v curl &>/dev/null; then
        curl \
            --silent \
            --show-error \
            --fail \
            --location \
            --max-time 30 \
            --output "${dest}" \
            "${CERT_URL}"
    elif command -v wget &>/dev/null; then
        wget \
            --quiet \
            --timeout=30 \
            --output-document="${dest}" \
            "${CERT_URL}"
    else
        echo "neither curl nor wget found" >&2
        return 1
    fi
}

verify_cert() {
    local file="$1"

    if ! grep -q "BEGIN CERTIFICATE" "${file}"; then
        echo "downloaded file does not look like a certificate bundle" >&2
        return 1
    fi

    local count
    count=$(grep -c "BEGIN CERTIFICATE" "${file}")
    if (( count < 100 )); then
        echo "too few certificates (${count}), expected 100+" >&2
        return 1
    fi

    echo "${count}"
}

bundle_mozilla_date() {
    local file="$1"
    grep "Certificate data from Mozilla as of:" "${file}" 2>/dev/null \
        | head -1 \
        | sed 's/.*as of: //' \
        || echo "unknown"
}

nearest_expiry() {
    local file="$1"
    local min_date=""
    local in_cert=0
    local cert=""

    while IFS= read -r line; do
        if [[ "${line}" == *"BEGIN CERTIFICATE"* ]]; then
            in_cert=1
            cert="${line}"
        elif [[ "${line}" == *"END CERTIFICATE"* ]] && (( in_cert )); then
            cert+=$'\n'"${line}"
            local expiry
            expiry=$(printf '%s\n' "${cert}" \
                | openssl x509 -noout -enddate 2>/dev/null \
                | cut -d= -f2 \
                || true)
            if [[ -n "${expiry}" ]]; then
                if [[ -z "${min_date}" || "${expiry}" < "${min_date}" ]]; then
                    min_date="${expiry}"
                fi
            fi
            in_cert=0
            cert=""
        elif (( in_cert )); then
            cert+=$'\n'"${line}"
        fi
    done < "${file}"

    echo "${min_date:-unknown}"
}

cmd_update() {
    local dest="$1"
    local tmp="${dest}.tmp"

    mkdir -p "$(dirname "${dest}")"

    download_cert "${tmp}"

    local count
    if ! count=$(verify_cert "${tmp}"); then
        rm -f "${tmp}"
        exit 1
    fi

    mv "${tmp}" "${dest}"

    local size
    size=$(wc -c < "${dest}")

    local mozilla_date
    mozilla_date=$(bundle_mozilla_date "${dest}")

    echo "checking expiry dates (${count} certificates)..."
    local expiry
    expiry=$(nearest_expiry "${dest}")

    echo "updated: ${dest}"
    echo "certificates: ${count}"
    echo "size: ${size} bytes"
    echo "mozilla date: ${mozilla_date}"
    echo "nearest expiry: ${expiry}"
}

[[ -z "${1:-}" ]] && { echo "usage: $0 <path/to/cacert.pem>" >&2; exit 1; }

cmd_update "$1"
