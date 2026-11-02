#!/usr/bin/env bash
# Install everything needed to build and test pipe-chunk on Ubuntu 24.04.
set -euo pipefail
sudo apt-get update
sudo apt-get install -y build-essential libssl-dev python3 python3-matplotlib git
echo "setup done — now run: make && make test"
