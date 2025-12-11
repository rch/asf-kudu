#!/usr/bin/env bash
# Initialize BIND9 DNS server for devenv
# This script creates DNS zone files and configuration

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BIND_DIR="$PROJECT_ROOT/.devenv/bind"
ZONES_DIR="$BIND_DIR/zones"
CACHE_DIR="$BIND_DIR/cache"

# Load configuration from .env
if [ -f "$PROJECT_ROOT/.env" ]; then
    source "$PROJECT_ROOT/.env"
fi

DOMAIN="${FREEIPA_DOMAIN:-vista.zndx.lan}"
SERVER_IP="${FREEIPA_SERVER_IP:-192.168.1.55}"
REALM="${FREEIPA_REALM:-VISTA.ZNDX.ORG}"

echo "========================================="
echo "Initializing BIND DNS Server"
echo "========================================="
echo ""
echo "  Domain: $DOMAIN"
echo "  Server IP: $SERVER_IP"
echo "  Realm: $REALM"
echo ""

# Create directories
mkdir -p "$ZONES_DIR" "$CACHE_DIR"

# Create named.conf
cat > "$BIND_DIR/named.conf" <<EOF
options {
    directory "$CACHE_DIR";
    listen-on port 5353 { 127.0.0.1; $SERVER_IP; };
    listen-on-v6 { none; };
    allow-query { any; };
    recursion yes;
    forwarders { 8.8.8.8; 8.8.4.4; };
    dnssec-validation no;
};

zone "$DOMAIN" {
    type master;
    file "$ZONES_DIR/db.$DOMAIN";
};

zone "1.168.192.in-addr.arpa" {
    type master;
    file "$ZONES_DIR/db.192.168.1";
};
EOF

# Create forward zone
SERIAL=$(date +%Y%m%d01)

cat > "$ZONES_DIR/db.$DOMAIN" <<EOF
\$TTL    604800
@       IN      SOA     ipa.$DOMAIN. admin.$DOMAIN. (
                              $SERIAL        ; Serial
                              604800         ; Refresh
                              86400          ; Retry
                              2419200        ; Expire
                              604800 )       ; Negative Cache TTL

; Name servers
@       IN      NS      ipa.$DOMAIN.

; Kerberos SRV records
_kerberos               IN      TXT     "$REALM"
_kerberos._tcp          IN      SRV     0 0 88 ipa.$DOMAIN.
_kerberos._udp          IN      SRV     0 0 88 ipa.$DOMAIN.
_kerberos-master._tcp   IN      SRV     0 0 88 ipa.$DOMAIN.
_kerberos-master._udp   IN      SRV     0 0 88 ipa.$DOMAIN.
_kerberos-adm._tcp      IN      SRV     0 0 749 ipa.$DOMAIN.
_kpasswd._udp           IN      SRV     0 0 464 ipa.$DOMAIN.

; A Records
ipa                     IN      A       $SERVER_IP
kudu-master             IN      A       $SERVER_IP
kudu-tserver-1          IN      A       $SERVER_IP
kudu-tserver-2          IN      A       $SERVER_IP
kudu-tserver-3          IN      A       $SERVER_IP

; Knative wildcard for dynamic apps
*.knative               IN      A       $SERVER_IP
EOF

# Create reverse zone
REV_OCTET="${SERVER_IP##*.}"

cat > "$ZONES_DIR/db.192.168.1" <<EOF
\$TTL    604800
@       IN      SOA     ipa.$DOMAIN. admin.$DOMAIN. (
                              $SERIAL        ; Serial
                              604800         ; Refresh
                              86400          ; Retry
                              2419200        ; Expire
                              604800 )       ; Negative Cache TTL

; Name servers
@       IN      NS      ipa.$DOMAIN.

; PTR Records
$REV_OCTET      IN      PTR     ipa.$DOMAIN.
EOF

echo "========================================="
echo "BIND DNS Initialization Complete"
echo "========================================="
echo ""
echo "Configuration:"
echo "  named.conf: $BIND_DIR/named.conf"
echo "  Zones: $ZONES_DIR/"
echo ""
echo "DNS Server Info:"
echo "  Listen port: 5353 (non-privileged)"
echo "  Domain: $DOMAIN"
echo "  Nameserver: ipa.$DOMAIN ($SERVER_IP)"
echo ""
echo "To use:"
echo "  dig @127.0.0.1 -p 5353 ipa.$DOMAIN"
echo "  dig @127.0.0.1 -p 5353 kudu-master.$DOMAIN"
echo "  dig @127.0.0.1 -p 5353 myapp.knative.$DOMAIN"
echo ""
echo "Note: Knative apps get wildcard *.knative.$DOMAIN -> $SERVER_IP"
echo "      CF Workers should use Cloudflare DNS (canonical source)"
echo ""
