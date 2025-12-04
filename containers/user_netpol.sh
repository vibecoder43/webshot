#!/usr/bin/env bash
set -euo pipefail

# Host firewall helper for crawler networks.
# Run as root on the host (for example on Debian) to scope egress rules
# for a given container subnet.

if [[ "$(id -u)" -ne 0 ]]; then
  echo "ERROR: must be run as root on the host" >&2
  exit 1
fi

if ! command -v iptables >/dev/null 2>&1; then
  echo "ERROR: iptables not found on PATH; install it on the host" >&2
  exit 1
fi

# Default crawler network IPv4 CIDR; can be overridden via environment.
DEFAULT_CRAWLER_V4_CIDR="${DEFAULT_CRAWLER_V4_CIDR:-172.31.0.0/16}"

cmd="${1:-}"
cidr_v4="${2:-}"
cidr_v6="${3:-}"
if [[ -z "${cmd}" ]]; then
  cmd="apply-cidr"
  cidr_v4="${DEFAULT_CRAWLER_V4_CIDR}"
fi

# Chain used for enforcing egress policy. For Podman bridge networks traffic
# traverses FORWARD, so we scope rules there by source CIDR.
CHAIN_V4="FORWARD"
CHAIN_V6="FORWARD"

apply_rules() {
  local cidr_v4="$1"; local cidr_v6="$2"
  # Block DNS/DoT
  if [[ -n "$cidr_v4" ]]; then
    iptables -C "$CHAIN_V4" -s "$cidr_v4" -p udp --dport 53 -j REJECT 2>/dev/null || iptables -I "$CHAIN_V4" -s "$cidr_v4" -p udp --dport 53 -j REJECT
    iptables -C "$CHAIN_V4" -s "$cidr_v4" -p tcp --dport 53 -j REJECT 2>/dev/null || iptables -I "$CHAIN_V4" -s "$cidr_v4" -p tcp --dport 53 -j REJECT
    iptables -C "$CHAIN_V4" -s "$cidr_v4" -p tcp --dport 853 -j REJECT 2>/dev/null || iptables -I "$CHAIN_V4" -s "$cidr_v4" -p tcp --dport 853 -j REJECT
  fi
  # Block IPv4 private/bogon ranges
  for c in \
    0.0.0.0/8 \
    10.0.0.0/8 \
    100.64.0.0/10 \
    127.0.0.0/8 \
    169.254.0.0/16 \
    172.16.0.0/12 \
    192.168.0.0/16 \
    224.0.0.0/4 \
    240.0.0.0/4; do
    if [[ -n "$cidr_v4" ]]; then
      iptables -C "$CHAIN_V4" -s "$cidr_v4" -d "$c" -j REJECT 2>/dev/null || iptables -I "$CHAIN_V4" -s "$cidr_v4" -d "$c" -j REJECT
    fi
  done
  # IPv6 optional (only if ip6tables present)
  if command -v ip6tables >/dev/null 2>&1; then
    for c in \
      ::/128 \
      ::1/128 \
      fc00::/7 \
      fe80::/10 \
      ::ffff:0:0/96 \
      ff00::/8; do
      if [[ -n "$cidr_v6" ]]; then
        ip6tables -C "$CHAIN_V6" -s "$cidr_v6" -d "$c" -j REJECT 2>/dev/null || ip6tables -I "$CHAIN_V6" -s "$cidr_v6" -d "$c" -j REJECT
      fi
    done
  fi
}

clear_rules() {
  local cidr_v4="$1"; local cidr_v6="$2"
  # Best-effort delete; ignore errors
  if [[ -n "$cidr_v4" ]]; then
    iptables -D "$CHAIN_V4" -s "$cidr_v4" -p udp --dport 53 -j REJECT 2>/dev/null || true
    iptables -D "$CHAIN_V4" -s "$cidr_v4" -p tcp --dport 53 -j REJECT 2>/dev/null || true
    iptables -D "$CHAIN_V4" -s "$cidr_v4" -p tcp --dport 853 -j REJECT 2>/dev/null || true
  fi
  for c in 0.0.0.0/8 10.0.0.0/8 100.64.0.0/10 127.0.0.0/8 169.254.0.0/16 172.16.0.0/12 192.168.0.0/16 224.0.0.0/4 240.0.0.0/4; do
    if [[ -n "$cidr_v4" ]]; then
      iptables -D "$CHAIN_V4" -s "$cidr_v4" -d "$c" -j REJECT 2>/dev/null || true
    fi
  done
  if command -v ip6tables >/dev/null 2>&1; then
    for c in ::/128 ::1/128 fc00::/7 fe80::/10 ::ffff:0:0/96 ff00::/8; do
      if [[ -n "$cidr_v6" ]]; then
        ip6tables -D "$CHAIN_V6" -s "$cidr_v6" -d "$c" -j REJECT 2>/dev/null || true
      fi
    done
  fi
}

case "${cmd}" in
  apply-cidr)
    # Usage: apply-cidr <v4cidr> [v6cidr]
    if [[ -z "${cidr_v4}" ]]; then
      echo "IPv4 CIDR is required" >&2
      exit 2
    fi
    apply_rules "${cidr_v4}" "${cidr_v6}"
    echo "Applied egress policy for CIDR(s) ${cidr_v4} ${cidr_v6:-}"
    ;;
  clear-cidr)
    # Usage: clear-cidr <v4cidr> [v6cidr]
    if [[ -z "${cidr_v4}" ]]; then
      echo "IPv4 CIDR is required" >&2
      exit 2
    fi
    clear_rules "${cidr_v4}" "${cidr_v6}"
    echo "Cleared egress policy for CIDR(s) ${cidr_v4} ${cidr_v6:-}"
    ;;
  *)
    echo "Unknown command: ${cmd}" >&2
    exit 2
    ;;
esac
