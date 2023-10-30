#!/bin/bash

set -e
set -x

function cleanup {
	git checkout libfabric.spec.in \
		configure.ac \
		config/distscript.pl
}

trap cleanup EXIT

if [[ "${TARGET_OS}" == sle*  || "${TARGET_OS}" == rhel_8_6* ]]; then

    if [[ "${TARGET_ARCH}" == x86_64 ]]; then
        ROCM_CONFIG="-c --with-rocr=/opt/rocm -c --enable-rocr-dlopen"
    else
        ROCM_CONFIG=""
    fi
    CUDA_CONFIG="-c --with-cuda=/usr/local/cuda -c --enable-cuda-dlopen"
    if [[ "${OBS_TARGET_OS}" == cos* ]]; then
        GDRCOPY_CONFIG="-c --enable-gdrcopy-dlopen"
    else
        GDRCOPY_CONFIG=""
    fi
else
    ROCM_CONFIG=""
    CUDA_CONFIG=""
    GDRCOPY_CONFIG=""
fi

if [[ ( ${TARGET_OS} == sle15_sp4* || ${TARGET_OS} == sle15_sp5* ) \
        && ${TARGET_ARCH} == x86_64 ]]; then
    ZE_CONFIG="-c --with-ze=/usr -c --enable-ze-dlopen"
else
    ZE_CONFIG=""
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
-e psm3 \
-e opx"

ORIGINAL_VERSION=$(echo ${LIBFABRIC_VERSION} | \
	sed -e 's/b.*//g' \
            -e 's/a.*//g' \
            -e 's/rc.*//g')
MINOR=$(git tag | egrep ^v${ORIGINAL_VERSION} | wc -l)
NEW_VERSION=${ORIGINAL_VERSION}.${MINOR}
if [[ -z $BUILD_METADATA ]] ; then
	RELEASE=1
else
	BRANCH=`git branch --show-current`
	git clone https://$HPE_GITHUB_TOKEN@github.hpe.com/hpe/hpc-sshot-slingshot-version.git

	cd hpc-sshot-slingshot-version

	if ! git checkout $BRANCH_NAME ; then
	echo "INFO: Branch '$BRANCH_NAME' does not exist in hpc-sshot-slingshot-version repo, using version string from master branch"
	else
	echo "INFO: Using Slingshot release version from '$BRANCH_NAME'"
	fi

	cd -

	PRODUCT_VERSION=$(cat hpc-sshot-slingshot-version/slingshot-version|| echo "0.0.0")
	echo "INFO: Slingshot release version '$PRODUCT_VERSION'"
	RELEASE="SSHOT${PRODUCT_VERSION}_${BUILD_METADATA}"
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
    -c "--enable-restricted-dl" \
    -omv \
    -P /opt/cray \
    -M /opt/cray/modulefiles \
    -V ${NEW_VERSION} \
    ${CUDA_CONFIG} \
    ${GDRCOPY_CONFIG} \
    ${ROCM_CONFIG} \
    ${ZE_CONFIG} \
    -D \
    libfabric-${NEW_VERSION}.tar.bz2

# Move the RPMs and SRPMS to where the "Publish" stage expects to find them
mkdir RPMS
mv `find rpmbuild/RPMS | grep rpm$` `find rpmbuild/SRPMS | grep rpm$` RPMS
chmod a+rX -R RPMS

# Finish up rpmlint to check for warnings and errors.
rpmlint RPMS/*.rpm ||:

# Return codes from rpmlint:
#  0: OK
#  1: Unspecified error
#  2: Interrupted
# 64: One or more error messages
# 66: Badness level exceeded

# Let's not fail builds for (for example) using static linking.
if [[ $? != 0 && $? != 64 ]]; then
    echo "rpmlint failure!"
    exit 1
fi

exit 0
