#!/bin/bash

set -e
set -x

function cleanup {
	git checkout libfabric.spec.in \
		configure.ac \
		config/distscript.pl
}

trap cleanup EXIT

if [[ "${TARGET_OS}" == "sle15_sp2_cn" || "${TARGET_OS}" == "sle15_sp2_ncn" || "${TARGET_OS}" == "sle15_sp3_cn" || "${TARGET_OS}" == "sle15_sp3_ncn" ]]; then
	ROCM_CONFIG="-c --with-rocr=/opt/rocm \
-c --enable-rocr-dlopen"
else
    ROCM_CONFIG=""
fi

if [[ "${TARGET_OS}" == sle* ]]; then
    CUDA_CONFIG="-c --with-cuda=/usr/local/cuda \
-c --enable-cuda-dlopen"
else
    CUDA_CONFIG=""
fi

LIBFABRIC_VERSION=$(grep AC_INIT configure.ac | \
    awk '{print $2}' | \
    sed -e 's/,//g' -e 's/\[//g' -e 's/\]//g')

VERSION=${LIBFABRIC_VERSION}-$(git rev-parse --short HEAD)
ENABLED_PROVIDERS="-i cxi -i tcp -i udp -i rxm -i rxd"

DISABLED_PROVIDERS="-e psm \
-e psm2 \
-e usnic \
-e mlx \
-e gni \
-e mrail \
-e bgq \
-e rstream \
-e shm \
-e verbs \
-e sockets \
-e psm3"

ORIGINAL_VERSION=$(echo ${LIBFABRIC_VERSION} | \
	sed -e 's/b.*//g' \
            -e 's/a.*//g' \
            -e 's/rc.*//g')
MINOR=$(git tag | egrep ^v${ORIGINAL_VERSION} | wc -l)
NEW_VERSION=${ORIGINAL_VERSION}.${MINOR}
if [[ -z $BUILD_METADATA ]] ; then
	RELEASE=1
else
    SS_VERSION=$(curl https://stash.us.cray.com/projects/SSHOT/repos/slingshot-version/raw/slingshot-version?at=refs%2Fheads%2Fmaster)
	RELEASE="SSHOT${SS_VERSION}_${BUILD_METADATA}"
fi

ORIGINAL_STRING="This file was automatically generated by Libfabric RPM."
APPENDED_INFO="""\n\
#   Git-Commit  : $(git describe) \n\
#   Date        : $(date)  \n\
"""
sed -i -e "s/\[${LIBFABRIC_VERSION}\]/\[${NEW_VERSION}\]/g" configure.ac
sed -i \
        -e "s/Version: .*/Version: ${NEW_VERSION}/g" \
        -e "s/Release: .*/Release: ${RELEASE}\%{?dist}/g" \
	-e "s|Source: .*|Source: libfabric-${NEW_VERSION}.tar.bz2|g" \
        -e "s/${ORIGINAL_STRING}/${ORIGINAL_STRING}${APPENDED_INFO}/g" \
        libfabric.spec.in
sed -i -e \
    's/ die "Refusing to make tarball";/##die "Refusing to make tarball";/g' \
    config/distscript.pl

./autogen.sh
./configure
make dist-bzip2

cleanup

./contrib/buildrpm/buildrpmLibfabric.sh \
    ${ENABLED_PROVIDERS} \
    ${DISABLED_PROVIDERS} \
    -omv \
    -P /opt/cray \
    -M /opt/cray/modulefiles \
    -V ${NEW_VERSION} \
    -c "--disable-memhooks-monitor" \
    ${CUDA_CONFIG} \
    ${ROCM_CONFIG} \
    -D \
    libfabric-${NEW_VERSION}.tar.bz2
