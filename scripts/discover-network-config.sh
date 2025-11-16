#!/bin/bash
# Discover network configuration for dynamic FreeIPA setup
# This script auto-detects hostname, domain, IP addresses, and DNS settings

set -euo pipefail

echo "========================================="
echo "Network Configuration Discovery"
echo "========================================="
echo ""

# Determine output format
OUTPUT_FORMAT="${1:-env}"  # env, yaml, or json

# Discover hostname
HOSTNAME=$(hostname)
HOSTNAME_FQDN=$(hostname -f)
HOSTNAME_SHORT=$(hostname -s)
HOSTNAME_DOMAIN=$(hostname -d || echo "")

echo "Hostname Discovery:"
echo "  Short name: $HOSTNAME_SHORT"
echo "  FQDN: $HOSTNAME_FQDN"
echo "  Domain: ${HOSTNAME_DOMAIN:-<none>}"
echo ""

# Discover primary network interface and IPs
echo "Network Interface Discovery:"

# Get default route interface
DEFAULT_IFACE=$(ip route show default | awk '/default/ {print $5; exit}')
echo "  Default interface: $DEFAULT_IFACE"

# Get IPv4 address
IPV4_ADDR=$(ip -4 addr show "$DEFAULT_IFACE" | awk '/inet / {print $2}' | cut -d/ -f1 | head -1)
IPV4_CIDR=$(ip -4 addr show "$DEFAULT_IFACE" | awk '/inet / {print $2}' | head -1)
IPV4_NETWORK=$(echo "$IPV4_CIDR" | cut -d. -f1-3).0/24

echo "  IPv4: $IPV4_ADDR"
echo "  IPv4 CIDR: $IPV4_CIDR"
echo "  IPv4 Network: $IPV4_NETWORK"

# Get IPv6 address (GUA - Global Unicast Address)
IPV6_GUA=$(ip -6 addr show "$DEFAULT_IFACE" scope global | awk '/inet6/ && !/deprecated/ && !/temporary/ {print $2}' | cut -d/ -f1 | head -1 || echo "")
IPV6_LLA=$(ip -6 addr show "$DEFAULT_IFACE" scope link | awk '/inet6/ {print $2}' | cut -d/ -f1 | head -1 || echo "")

if [ -n "$IPV6_GUA" ]; then
    echo "  IPv6 (GUA): $IPV6_GUA"
fi
if [ -n "$IPV6_LLA" ]; then
    echo "  IPv6 (Link-Local): $IPV6_LLA"
fi
echo ""

# Discover DNS configuration
echo "DNS Configuration Discovery:"

# Check if systemd-resolved is running
if systemctl is-active systemd-resolved >/dev/null 2>&1; then
    echo "  systemd-resolved: Active"

    # Get DNS servers from resolvectl
    DNS_SERVERS=$(resolvectl status "$DEFAULT_IFACE" 2>/dev/null | awk '/DNS Servers:/ {getline; print}' | sed 's/^[[:space:]]*//' || echo "")
    SEARCH_DOMAINS=$(resolvectl status "$DEFAULT_IFACE" 2>/dev/null | awk '/DNS Domain:/ {print $3}' || echo "")

    if [ -n "$DNS_SERVERS" ]; then
        echo "  DNS Servers: $DNS_SERVERS"
    fi
    if [ -n "$SEARCH_DOMAINS" ]; then
        echo "  Search Domains: $SEARCH_DOMAINS"
    fi
else
    echo "  systemd-resolved: Inactive"

    # Fallback to /etc/resolv.conf
    if [ -f /etc/resolv.conf ]; then
        DNS_SERVERS=$(grep '^nameserver' /etc/resolv.conf | awk '{print $2}' | tr '\n' ' ' | sed 's/ $//')
        SEARCH_DOMAINS=$(grep '^search' /etc/resolv.conf | awk '{print $2}')

        if [ -n "$DNS_SERVERS" ]; then
            echo "  DNS Servers: $DNS_SERVERS"
        fi
        if [ -n "$SEARCH_DOMAINS" ]; then
            echo "  Search Domains: $SEARCH_DOMAINS"
        fi
    fi
fi
echo ""

# Detect Cloudflare WARP
echo "Cloudflare WARP Detection:"
WARP_ENABLED=false
WARP_ORGANIZATION=""
WARP_DNS_GATEWAY=""

if command -v warp-cli >/dev/null 2>&1; then
    if warp-cli status 2>/dev/null | grep -q "Status update: Connected"; then
        WARP_ENABLED=true
        WARP_ORGANIZATION=$(warp-cli settings 2>/dev/null | awk '/Organization:/ {print $2}' || echo "")
        WARP_DNS_GATEWAY=$(warp-cli settings 2>/dev/null | awk '/Gateway unique id:/ {print $4}' || echo "")

        echo "  WARP Status: Connected"
        if [ -n "$WARP_ORGANIZATION" ]; then
            echo "  Organization: $WARP_ORGANIZATION"
        fi
        if [ -n "$WARP_DNS_GATEWAY" ]; then
            echo "  DNS Gateway: $WARP_DNS_GATEWAY"
        fi
    else
        echo "  WARP Status: Not connected"
    fi
else
    echo "  WARP Client: Not installed"
fi
echo ""

# Infer recommended domain and realm
echo "Recommendations:"

# Determine recommended local domain
if [ -n "$HOSTNAME_DOMAIN" ] && [ "$HOSTNAME_DOMAIN" != "localdomain" ]; then
    RECOMMENDED_LOCAL_DOMAIN="$HOSTNAME_DOMAIN"
    echo "  Local Domain: $RECOMMENDED_LOCAL_DOMAIN (from FQDN)"
else
    # Use .lan as default for local networks
    RECOMMENDED_LOCAL_DOMAIN="lan"
    echo "  Local Domain: $RECOMMENDED_LOCAL_DOMAIN (default)"
fi

# Determine recommended realm
if [ "$WARP_ENABLED" = true ] && [ -n "$WARP_ORGANIZATION" ]; then
    # For HYBRID mode, use organization name in uppercase
    RECOMMENDED_REALM=$(echo "$WARP_ORGANIZATION" | tr '[:lower:]' '[:upper:]').IO
    echo "  Kerberos Realm: $RECOMMENDED_REALM (HYBRID mode)"
    echo "  Public Domain: kudu.$WARP_ORGANIZATION.io (for Cloudflare Workers)"
else
    # For AUTO/STATIC mode, derive from local domain
    RECOMMENDED_REALM=$(echo "$RECOMMENDED_LOCAL_DOMAIN" | tr '[:lower:]' '[:upper:]')
    echo "  Kerberos Realm: $RECOMMENDED_REALM (AUTO mode)"
fi

# Suggested FreeIPA domain
SUGGESTED_FREEIPA_DOMAIN="kudu.$RECOMMENDED_LOCAL_DOMAIN"
echo "  FreeIPA Domain: $SUGGESTED_FREEIPA_DOMAIN"

# Configuration mode recommendation
if [ "$WARP_ENABLED" = true ]; then
    echo "  Recommended Mode: HYBRID (Cloudflare WARP detected)"
else
    echo "  Recommended Mode: AUTO (no Zero Trust detected)"
fi
echo ""

# Output configuration
echo "========================================="
echo "Configuration Output"
echo "========================================="
echo ""

case "$OUTPUT_FORMAT" in
    env)
        cat <<EOF
# Network Configuration (discovered $(date))
# Copy to .env or source directly

# Host Information
DISCOVERED_HOSTNAME_SHORT="$HOSTNAME_SHORT"
DISCOVERED_HOSTNAME_FQDN="$HOSTNAME_FQDN"
DISCOVERED_HOSTNAME_DOMAIN="$HOSTNAME_DOMAIN"

# Network Information
DISCOVERED_DEFAULT_IFACE="$DEFAULT_IFACE"
DISCOVERED_IPV4_ADDR="$IPV4_ADDR"
DISCOVERED_IPV4_CIDR="$IPV4_CIDR"
DISCOVERED_IPV4_NETWORK="$IPV4_NETWORK"
DISCOVERED_IPV6_GUA="$IPV6_GUA"
DISCOVERED_IPV6_LLA="$IPV6_LLA"

# DNS Configuration
DISCOVERED_DNS_SERVERS="$DNS_SERVERS"
DISCOVERED_SEARCH_DOMAINS="$SEARCH_DOMAINS"

# Cloudflare WARP
DISCOVERED_WARP_ENABLED="$WARP_ENABLED"
DISCOVERED_WARP_ORGANIZATION="$WARP_ORGANIZATION"
DISCOVERED_WARP_DNS_GATEWAY="$WARP_DNS_GATEWAY"

# Recommendations
RECOMMENDED_LOCAL_DOMAIN="$RECOMMENDED_LOCAL_DOMAIN"
RECOMMENDED_REALM="$RECOMMENDED_REALM"
RECOMMENDED_FREEIPA_DOMAIN="$SUGGESTED_FREEIPA_DOMAIN"
RECOMMENDED_MODE="$([ "$WARP_ENABLED" = true ] && echo "HYBRID" || echo "AUTO")"

# For HYBRID mode (when WARP is enabled)
$(if [ "$WARP_ENABLED" = true ]; then
    echo "RECOMMENDED_PUBLIC_DOMAIN=\"kudu.$WARP_ORGANIZATION.io\""
fi)
EOF
        ;;

    yaml)
        cat <<EOF
# Network Configuration (discovered $(date))
# For use with Ansible playbooks

discovered_network:
  hostname:
    short: "$HOSTNAME_SHORT"
    fqdn: "$HOSTNAME_FQDN"
    domain: "$HOSTNAME_DOMAIN"

  network:
    default_interface: "$DEFAULT_IFACE"
    ipv4:
      address: "$IPV4_ADDR"
      cidr: "$IPV4_CIDR"
      network: "$IPV4_NETWORK"
    ipv6:
      gua: "$IPV6_GUA"
      lla: "$IPV6_LLA"

  dns:
    servers: "$DNS_SERVERS"
    search_domains: "$SEARCH_DOMAINS"

  cloudflare_warp:
    enabled: $WARP_ENABLED
    organization: "$WARP_ORGANIZATION"
    dns_gateway: "$WARP_DNS_GATEWAY"

recommendations:
  local_domain: "$RECOMMENDED_LOCAL_DOMAIN"
  realm: "$RECOMMENDED_REALM"
  freeipa_domain: "$SUGGESTED_FREEIPA_DOMAIN"
  mode: "$([ "$WARP_ENABLED" = true ] && echo "HYBRID" || echo "AUTO")"
  $(if [ "$WARP_ENABLED" = true ]; then
      echo "public_domain: \"kudu.$WARP_ORGANIZATION.io\""
  fi)
EOF
        ;;

    json)
        # Construct JSON using jq
        jq -n \
            --arg hostname_short "$HOSTNAME_SHORT" \
            --arg hostname_fqdn "$HOSTNAME_FQDN" \
            --arg hostname_domain "$HOSTNAME_DOMAIN" \
            --arg default_iface "$DEFAULT_IFACE" \
            --arg ipv4_addr "$IPV4_ADDR" \
            --arg ipv4_cidr "$IPV4_CIDR" \
            --arg ipv4_network "$IPV4_NETWORK" \
            --arg ipv6_gua "$IPV6_GUA" \
            --arg ipv6_lla "$IPV6_LLA" \
            --arg dns_servers "$DNS_SERVERS" \
            --arg search_domains "$SEARCH_DOMAINS" \
            --argjson warp_enabled "$WARP_ENABLED" \
            --arg warp_org "$WARP_ORGANIZATION" \
            --arg warp_gateway "$WARP_DNS_GATEWAY" \
            --arg rec_domain "$RECOMMENDED_LOCAL_DOMAIN" \
            --arg rec_realm "$RECOMMENDED_REALM" \
            --arg rec_freeipa "$SUGGESTED_FREEIPA_DOMAIN" \
            --arg rec_mode "$([ "$WARP_ENABLED" = true ] && echo "HYBRID" || echo "AUTO")" \
            --arg rec_public "kudu.$WARP_ORGANIZATION.io" \
            '{
                discovered_network: {
                    hostname: {
                        short: $hostname_short,
                        fqdn: $hostname_fqdn,
                        domain: $hostname_domain
                    },
                    network: {
                        default_interface: $default_iface,
                        ipv4: {
                            address: $ipv4_addr,
                            cidr: $ipv4_cidr,
                            network: $ipv4_network
                        },
                        ipv6: {
                            gua: $ipv6_gua,
                            lla: $ipv6_lla
                        }
                    },
                    dns: {
                        servers: $dns_servers,
                        search_domains: $search_domains
                    },
                    cloudflare_warp: {
                        enabled: $warp_enabled,
                        organization: $warp_org,
                        dns_gateway: $warp_gateway
                    }
                },
                recommendations: {
                    local_domain: $rec_domain,
                    realm: $rec_realm,
                    freeipa_domain: $rec_freeipa,
                    mode: $rec_mode,
                    public_domain: (if $warp_enabled then $rec_public else null end)
                }
            }'
        ;;

    *)
        echo "Error: Unknown output format '$OUTPUT_FORMAT'" '$OUTPUT_FORMAT'
        echo "Usage: $0 [env|yaml|json]"
        exit 1
        ;;
esac
