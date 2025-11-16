# Kudu Development Scripts

This directory contains utility scripts for Kudu development environment configuration and management.

## FreeIPA Dynamic Configuration

### Overview

These scripts enable dynamic FreeIPA configuration that automatically adapts to your network environment. This eliminates hardcoded domain names and supports multiple deployment scenarios including Cloudflare Workers integration.

### Scripts

#### `discover-network-config.sh`

**Purpose:** Automatically discover network configuration from the system.

**Features:**
- Detects hostname, FQDN, and domain
- Discovers network interfaces and IP addresses (IPv4 and IPv6)
- Identifies DNS servers and search domains
- Detects Cloudflare WARP if installed
- Recommends configuration mode based on environment
- Outputs in multiple formats: env, yaml, json

**Usage:**
```bash
# Default output (env format)
./scripts/discover-network-config.sh

# YAML format (for Ansible)
./scripts/discover-network-config.sh yaml

# JSON format (for programmatic use)
./scripts/discover-network-config.sh json

# Save to file
./scripts/discover-network-config.sh > discovered.env
./scripts/discover-network-config.sh yaml > discovered.yml
```

**Example Output:**
```bash
$ ./scripts/discover-network-config.sh

=========================================
Network Configuration Discovery
=========================================

Hostname Discovery:
  Short name: tinybox
  FQDN: tinybox.lan
  Domain: lan

Network Interface Discovery:
  Default interface: enp5s0
  IPv4: 192.168.1.55
  IPv4 CIDR: 192.168.1.55/24
  IPv4 Network: 192.168.1.0/24
  IPv6 (GUA): 2605:59c8:52e2:d508:9e6b:ff:fe4c:c6e6

DNS Configuration Discovery:
  systemd-resolved: Active
  DNS Servers: 192.168.1.1
  Search Domains: lan

Cloudflare WARP Detection:
  WARP Status: Connected
  Organization: weathership
  DNS Gateway: 35b2c9c278d25bdc527db86a2a7ba17b.cloudflare-gateway.com

Recommendations:
  Local Domain: lan (from FQDN)
  Kerberos Realm: WEATHERSHIP.IO (HYBRID mode)
  FreeIPA Domain: kudu.lan
  Public Domain: kudu.weathership.io (for Cloudflare Workers)
  Recommended Mode: HYBRID (Cloudflare WARP detected)

=========================================
Configuration Output
=========================================

# Network Configuration (discovered 2025-11-15)
# Copy to .env or source directly

DISCOVERED_HOSTNAME_SHORT="tinybox"
DISCOVERED_HOSTNAME_FQDN="tinybox.lan"
DISCOVERED_HOSTNAME_DOMAIN="lan"
...
```

#### `configure-freeipa-dynamic.sh`

**Purpose:** Generate FreeIPA configuration based on discovered network properties and selected mode.

**Features:**
- Supports three configuration modes: AUTO, STATIC, HYBRID
- Generates Ansible variables file
- Generates .env configuration file
- Configures Kudu server DNS records
- Configures Knative wildcard DNS
- HYBRID mode: Cloudflare Tunnel integration

**Usage:**
```bash
# Use AUTO mode (default)
./scripts/configure-freeipa-dynamic.sh

# Use mode from .env
export NETWORK_CONFIG_MODE=HYBRID
./scripts/configure-freeipa-dynamic.sh

# Review generated files
cat .env.freeipa-discovered
cat playbooks/group_vars/discovered.yml
```

**Generated Files:**
1. `playbooks/group_vars/discovered.yml` - Ansible variables
2. `.env.freeipa-discovered` - Environment configuration

**Example Output:**
```bash
$ ./scripts/configure-freeipa-dynamic.sh

=========================================
FreeIPA Dynamic Configuration
=========================================

Loading configuration from .env

Configuration Mode: HYBRID

Discovering network configuration...
Network discovery complete

Determining final configuration...

Mode: HYBRID - Local domain + public domain via Cloudflare Tunnel
  Local FreeIPA Domain: kudu.lan
  Public Domain: kudu.weathership.io
  Kerberos Realm: WEATHERSHIP.IO
  Server Hostname: ipa.kudu.lan
  Server IP: 192.168.1.55
  Cloudflare Tunnel: Enabled

Generating Ansible configuration...
Ansible variables written to: playbooks/group_vars/discovered.yml

Generating .env configuration...
.env configuration written to: .env.freeipa-discovered

=========================================
Configuration Summary
=========================================

Configuration Mode: HYBRID

FreeIPA Configuration:
  Domain: kudu.lan
  Realm: WEATHERSHIP.IO
  Server: ipa.kudu.lan (192.168.1.55)

Public Domain: kudu.weathership.io
  Master UI: https://kudu-master.kudu.weathership.io
  Tablet Server UI: https://kudu-tserver.kudu.weathership.io
  FreeIPA UI: https://ipa.kudu.weathership.io

Next Steps:

  1. Review discovered configuration:
     cat .env.freeipa-discovered
     # Merge with existing .env if needed

  2. Run Ansible playbook to install FreeIPA:
     cd playbooks
     ./quickstart.sh
     # Or manually:
     ansible-playbook -i inventory/hosts freeipa-install.yml

  3. Configure Cloudflare Tunnel:
     cd infrastructure
     ./scripts/setup-cloudflare-tunnel.sh

  4. Test FreeIPA installation:
     kinit admin@WEATHERSHIP.IO
     ipa user-show admin

  5. Configure Kudu development environment:
     devenv shell
     # FreeIPA client tools are available

=========================================
Configuration Complete
=========================================
```

## Configuration Modes

### Mode 1: AUTO (Recommended for Solo Developers)

**Use Case:** Simple, automatic configuration

**Configuration:**
```bash
# .env
NETWORK_CONFIG_MODE=AUTO
```

**Behavior:**
- Automatically discovers network properties
- Uses actual domain from hostname (e.g., `kudu.lan` from `tinybox.lan`)
- Generates realm from domain (e.g., `LAN`)
- Zero manual configuration required

**Pros:**
- Simplest setup
- Works immediately
- No hardcoded values

**Cons:**
- Not suitable for Cloudflare Workers
- Domain may change if hostname changes

### Mode 2: STATIC (Recommended for Custom Setups)

**Use Case:** Full control over configuration

**Configuration:**
```bash
# .env
NETWORK_CONFIG_MODE=STATIC
FREEIPA_DOMAIN=kudu.dev
FREEIPA_REALM=KUDU.DEV
FREEIPA_SERVER_HOSTNAME=ipa.kudu.dev
FREEIPA_SERVER_IP=192.168.1.100
```

**Behavior:**
- Uses values from `.env`
- No automatic discovery
- Predictable configuration

**Pros:**
- Complete control
- Stable configuration
- Custom domain names

**Cons:**
- Requires manual setup
- No automatic adaptation

### Mode 3: HYBRID (Recommended for Cloudflare Workers)

**Use Case:** Local development with public access for Workers

**Configuration:**
```bash
# .env
NETWORK_CONFIG_MODE=HYBRID
FREEIPA_DOMAIN=kudu.lan              # Auto-discovered or manual
PUBLIC_DOMAIN=kudu.weathership.io    # Your Cloudflare domain
FREEIPA_REALM=WEATHERSHIP.IO         # Shared realm
CLOUDFLARE_TUNNEL_ENABLED=true
CF_API_TOKEN=your-cloudflare-token
```

**Behavior:**
- Local domain for fast development
- Public domain via Cloudflare Tunnel
- Single Kerberos realm for both
- Workers access via public endpoints

**Pros:**
- Best of both worlds
- Cloudflare Workers support
- Secure public access
- Fast local access

**Cons:**
- More complex setup
- Requires Cloudflare account
- Need to run tunnel

## Complete Workflow

### Quick Start (AUTO mode)

```bash
# 1. Discover network
./scripts/discover-network-config.sh

# 2. Generate configuration
./scripts/configure-freeipa-dynamic.sh

# 3. Review and merge
cat .env.freeipa-discovered >> .env

# 4. Install FreeIPA
cd playbooks
./quickstart.sh

# 5. Configure DNS
ansible-playbook freeipa-configure-dns.yml

# 6. Test
kinit admin@LAN
host ipa.kudu.lan
```

### HYBRID Mode Setup

```bash
# 1. Discover network
./scripts/discover-network-config.sh

# 2. Configure HYBRID mode
cat >> .env <<EOF
NETWORK_CONFIG_MODE=HYBRID
PUBLIC_DOMAIN=kudu.weathership.io
CF_API_TOKEN=your-cloudflare-token
EOF

# 3. Generate configuration
./scripts/configure-freeipa-dynamic.sh

# 4. Install FreeIPA
cd playbooks
./quickstart.sh

# 5. Configure DNS
ansible-playbook freeipa-configure-dns.yml

# 6. Setup Cloudflare Tunnel
cd ../infrastructure/scripts
./setup-cloudflare-tunnel.sh

# 7. Start tunnel
cloudflared tunnel run kudu-dev-tunnel

# 8. Test local
curl http://kudu-master.kudu.lan:8051

# 9. Test public
curl https://kudu-master.kudu.weathership.io

# 10. Use in Workers
# Access https://kudu-master.kudu.weathership.io from Workers
```

## Environment Variables

### Discovery Output Variables

Set by `discover-network-config.sh`:

```bash
# Hostname
DISCOVERED_HOSTNAME_SHORT        # Short hostname (e.g., "tinybox")
DISCOVERED_HOSTNAME_FQDN         # Fully qualified (e.g., "tinybox.lan")
DISCOVERED_HOSTNAME_DOMAIN       # Domain part (e.g., "lan")

# Network
DISCOVERED_DEFAULT_IFACE         # Primary network interface
DISCOVERED_IPV4_ADDR             # IPv4 address
DISCOVERED_IPV4_CIDR             # IPv4 with netmask
DISCOVERED_IPV4_NETWORK          # Network address
DISCOVERED_IPV6_GUA              # IPv6 global address
DISCOVERED_IPV6_LLA              # IPv6 link-local

# DNS
DISCOVERED_DNS_SERVERS           # DNS server addresses
DISCOVERED_SEARCH_DOMAINS        # DNS search domains

# Cloudflare WARP
DISCOVERED_WARP_ENABLED          # true/false
DISCOVERED_WARP_ORGANIZATION     # Organization name
DISCOVERED_WARP_DNS_GATEWAY      # Gateway ID

# Recommendations
RECOMMENDED_LOCAL_DOMAIN         # Suggested domain
RECOMMENDED_REALM                # Suggested realm
RECOMMENDED_FREEIPA_DOMAIN       # Suggested FreeIPA domain
RECOMMENDED_MODE                 # Suggested mode (AUTO/HYBRID)
RECOMMENDED_PUBLIC_DOMAIN        # Suggested public domain (if WARP)
```

### Configuration Input Variables

Used by `configure-freeipa-dynamic.sh`:

```bash
# Mode selection
NETWORK_CONFIG_MODE              # AUTO, STATIC, or HYBRID

# FreeIPA configuration
FREEIPA_DOMAIN                   # DNS domain for FreeIPA
FREEIPA_REALM                    # Kerberos realm
FREEIPA_SERVER_HOSTNAME          # Server FQDN
FREEIPA_SERVER_IP                # Server IP address

# HYBRID mode
PUBLIC_DOMAIN                    # Public domain for Cloudflare
CLOUDFLARE_TUNNEL_ENABLED        # Enable tunnel setup
CF_API_TOKEN                     # Cloudflare API token
CF_ACCOUNT_ID                    # Cloudflare account ID
```

## Troubleshooting

### Discovery Issues

**Problem:** Discovery doesn't find domain

**Solution:**
```bash
# Check hostname configuration
hostname -f

# Set FQDN if needed
sudo hostnamectl set-hostname $(hostname).lan

# Verify
hostname -f
hostname -d

# Re-run discovery
./scripts/discover-network-config.sh
```

**Problem:** Wrong network interface detected

**Solution:**
```bash
# Check interfaces
ip addr show

# Override in .env (STATIC mode)
NETWORK_CONFIG_MODE=STATIC
FREEIPA_SERVER_IP=192.168.1.100  # Use specific IP
```

### Configuration Issues

**Problem:** Configuration script fails

**Solution:**
```bash
# Check .env exists
ls -la .env

# Check for syntax errors
bash -n ./scripts/configure-freeipa-dynamic.sh

# Run with debug
bash -x ./scripts/configure-freeipa-dynamic.sh
```

**Problem:** Generated configuration is wrong

**Solution:**
```bash
# Use STATIC mode for full control
cat > .env <<EOF
NETWORK_CONFIG_MODE=STATIC
FREEIPA_DOMAIN=your.domain
FREEIPA_REALM=YOUR.REALM
FREEIPA_SERVER_IP=your.ip
EOF

./scripts/configure-freeipa-dynamic.sh
```

## Documentation

- **Full Setup Guide:** `docs/notes/2025-11-15/COMPLETE-SETUP-SUMMARY.md`
- **Implementation Details:** `docs/notes/2025-11-15/IMPLEMENTATION-SUMMARY.md`
- **Quick Reference:** `docs/notes/2025-11-15/QUICK-REFERENCE.md`
- **Design Document:** `docs/notes/2025-11-15/dynamic-dns-realm-configuration.md`
- **Cloudflare Integration:** `docs/notes/2025-11-15/cloudflare-zerotrust-freeipa-integration.md`

## Examples

See `.env.example` for complete configuration examples for all three modes.

## Support

For issues or questions:
1. Check the documentation in `docs/notes/2025-11-15/`
2. Review `.env.example` for configuration examples
3. Run discovery with `./scripts/discover-network-config.sh` to understand your environment
4. Check Ansible playbook logs in `playbooks/`
