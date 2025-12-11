#!/usr/bin/env bash
# Initialize MIT Kerberos KDC for devenv
# This script creates the Kerberos database and initial principals

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
KRB_DIR="$PROJECT_ROOT/.devenv/kerberos"
KEYTAB_DIR="$KRB_DIR/keytabs"

# Load configuration from .env
if [ -f "$PROJECT_ROOT/.env" ]; then
    source "$PROJECT_ROOT/.env"
fi

REALM="${FREEIPA_REALM:-VISTA.ZNDX.ORG}"
DOMAIN="${FREEIPA_DOMAIN:-vista.zndx.lan}"
SERVER_IP="${FREEIPA_SERVER_IP:-127.0.0.1}"

echo "========================================="
echo "Initializing MIT Kerberos KDC"
echo "========================================="
echo ""
echo "  Realm: $REALM"
echo "  Domain: $DOMAIN"
echo "  Server IP: $SERVER_IP"
echo ""

# Create directories
mkdir -p "$KRB_DIR" "$KEYTAB_DIR"

# Create krb5.conf
cat > "$KRB_DIR/krb5.conf" <<EOF
[libdefaults]
    default_realm = $REALM
    kdc_timesync = 1
    ccache_type = 4
    forwardable = true
    proxiable = true
    dns_lookup_realm = false
    dns_lookup_kdc = false
    ticket_lifetime = 24h
    renew_lifetime = 7d
    rdns = false

[realms]
    $REALM = {
        kdc = 127.0.0.1:10088
        admin_server = 127.0.0.1:10749
        default_domain = $DOMAIN
    }

[domain_realm]
    .$DOMAIN = $REALM
    $DOMAIN = $REALM

[logging]
    kdc = FILE:$KRB_DIR/kdc.log
    admin_server = FILE:$KRB_DIR/kadmin.log
    default = FILE:$KRB_DIR/krb5lib.log
EOF

# Create kdc.conf
cat > "$KRB_DIR/kdc.conf" <<EOF
[kdcdefaults]
    kdc_listen = 10088
    kdc_tcp_listen = 10088

[realms]
    $REALM = {
        database_name = $KRB_DIR/principal
        admin_keytab = $KRB_DIR/kadm5.keytab
        acl_file = $KRB_DIR/kadm5.acl
        key_stash_file = $KRB_DIR/stash
        max_life = 24h 0m 0s
        max_renewable_life = 7d 0h 0m 0s
        master_key_type = aes256-cts-hmac-sha1-96
        supported_enctypes = aes256-cts-hmac-sha1-96:normal aes128-cts-hmac-sha1-96:normal
        default_principal_flags = +preauth
    }
EOF

# Create ACL
cat > "$KRB_DIR/kadm5.acl" <<EOF
*/admin@$REALM *
kudu/*@$REALM *
EOF

# Set environment for Kerberos commands
export KRB5_CONFIG="$KRB_DIR/krb5.conf"
export KRB5_KDC_PROFILE="$KRB_DIR/kdc.conf"

# Create KDC database
echo "Creating Kerberos database..."
kdb5_util create -s -P "devenv-master-key" -r "$REALM" -d "$KRB_DIR/principal"

# Create admin principal
echo "Creating admin principal..."
kadmin.local -c "$KRB_DIR/krb5.conf" -r "$REALM" <<EOF
addprinc -pw admin admin/admin@$REALM
quit
EOF

# Create Kudu service principals
echo "Creating Kudu service principals..."
kadmin.local -c "$KRB_DIR/krb5.conf" -r "$REALM" <<EOF
addprinc -randkey kudu/kudu-master.$DOMAIN@$REALM
addprinc -randkey kudu/kudu-tserver-1.$DOMAIN@$REALM
addprinc -randkey kudu/kudu-tserver-2.$DOMAIN@$REALM
addprinc -randkey kudu/kudu-tserver-3.$DOMAIN@$REALM

ktadd -k $KEYTAB_DIR/kudu-master.keytab kudu/kudu-master.$DOMAIN@$REALM
ktadd -k $KEYTAB_DIR/kudu-tserver-1.keytab kudu/kudu-tserver-1.$DOMAIN@$REALM
ktadd -k $KEYTAB_DIR/kudu-tserver-2.keytab kudu/kudu-tserver-2.$DOMAIN@$REALM
ktadd -k $KEYTAB_DIR/kudu-tserver-3.keytab kudu/kudu-tserver-3.$DOMAIN@$REALM
quit
EOF

chmod 600 "$KEYTAB_DIR"/*.keytab

echo ""
echo "========================================="
echo "Kerberos Initialization Complete"
echo "========================================="
echo ""
echo "Configuration:"
echo "  krb5.conf: $KRB_DIR/krb5.conf"
echo "  Keytabs: $KEYTAB_DIR/"
echo ""
echo "To use:"
echo "  export KRB5_CONFIG=$KRB_DIR/krb5.conf"
echo "  kinit admin/admin@$REALM"
echo "  (password: admin)"
echo ""
echo "Keytabs for Kudu services:"
echo "  kudu-master: $KEYTAB_DIR/kudu-master.keytab"
echo "  kudu-tserver-1: $KEYTAB_DIR/kudu-tserver-1.keytab"
echo "  kudu-tserver-2: $KEYTAB_DIR/kudu-tserver-2.keytab"
echo "  kudu-tserver-3: $KEYTAB_DIR/kudu-tserver-3.keytab"
echo ""
