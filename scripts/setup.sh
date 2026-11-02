#!/usr/bin/env bash
# Install everything needed to build and test pipe-chunk on Ubuntu 24.04.
set -euo pipefail
sudo apt-get update
# fincore (used by bench/io.sh) is in util-linux-extra on 24.04, in util-linux before that.
extra=$(apt-cache show util-linux-extra >/dev/null 2>&1 && echo util-linux-extra || true)
sudo apt-get install -y build-essential libssl-dev python3 python3-matplotlib strace git $extra
echo "setup done — now run: make && make test"
