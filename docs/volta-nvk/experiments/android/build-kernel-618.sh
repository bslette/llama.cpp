#!/bin/bash
# Runs build-kernel-618.inner.sh in the lineage-build container (host lacks openssl headers).
H=/home/slettela
podman run --rm --name nxkernel618 --userns=keep-id -v $H:$H -e HOME=$H -w $H lineage-build:22.04-xm bash $H/nouveau-l2/build-kernel-618.inner.sh
