#!/bin/bash

function _help(){
    echo
    echo "Build EDK2 for Qualcomm platforms."
    echo
    echo "Usage: edk2-qualcomm/build.sh [options]"
    echo
    echo "Options:"
    echo "  -d, --device DEV            Build for DEV, or 'all'."
    echo "  -r, --release MODE          Release mode for building, default is 'DEBUG', 'RELEASE' alternatively."
    echo "  -t, --toolchain TOOLCHAIN   Set toolchain, default is 'GCC'."
    echo "  --edk2-flags \"FLAGS\"        Flags appended to the EDK2 build process."
    echo "  --skip-patchsets            Skip applying upstream submodule patchsets during development."
    echo "  -C, --clean                 Clean the build output of DEV, or of all devices."
    echo "  -h, --help                  Show this help."
    echo
    echo "Devices:"
    for conf in "${QCOMDIR}"/configs/*.conf; do
        echo "  $(basename "${conf}" .conf)"
    done
    echo
    exit "${1}"
}

function _error() { echo "${@}" >&2; exit 1; }

function apply_patchset() {
    ${SKIP_PATCHSETS} && return 0

    local patches_dir="$1"
    local target_dir="$2"

    [ ! -d "${patches_dir}" ] && return 0

    if [ ! -d "${target_dir}" ]; then
        echo "Patchset target directory does not exist: ${target_dir}"
        return 1
    fi

    echo "Checking patchset ${patches_dir} for ${target_dir}"

    local patchset_name=$(basename "${patches_dir}")
    local patchset_marker="${target_dir}/.patchset_${patchset_name}"

    if [ ! -f "${patchset_marker}" ] || [ "${patches_dir}" -nt "${patchset_marker}" ]; then
        echo "Patchset needs to be (re)applied"
        if ! git -C "${target_dir}" reset --hard || ! git -C "${target_dir}" clean -xfd; then
            echo "Failed to reset git repository - aborting"
            return 1
        fi
    else
        echo "Patchset already applied - skipping"
        return 0
    fi

    local patch_file
    local patch_count=0

    for patch_file in "${patches_dir}"/*.patch; do
        [ -f "${patch_file}" ] || continue

        local patch_name=$(basename "${patch_file}")
        echo "Patch ${patch_count}: ${patch_name}"

        if patch -p1 -d "${target_dir}" < "${patch_file}"; then
            echo "  Successfully applied"
            ((patch_count++))
        else
            echo "  Failed to apply - aborting"
            return 1
        fi
    done

    touch "${patchset_marker}"

    echo "Patchset summary: ${patch_count} applied"
    return 0
}

#
# Same check as the Rockchip build.sh: a string that a translated UNI file
# lacks in one of its languages shows up as "!", or not at all, in that
# language.
#
function _check_translations() {
    local unis

    unis=$(grep -rl --include='*.uni' 'zh-Hans' \
        "${ROOTDIR}/edk2" "${ROOTDIR}/edk2-platforms" "${QCOMDIR}") || true
    [ -n "${unis}" ] || return 0

    python3 "${ROOTDIR}/edk2-rockchip/Silicon/Rockchip/Drivers/CjkFontDxe/Tools/UniWide.py" \
        --check --edk2 "${ROOTDIR}/edk2" ${unis} ||
        _error "The zh-Hans strings above need attention." \
               "Running UniWide.py on the files without --check fixes the markup and copies missing strings."
}

#
# XBL loads UEFI as a signed ELF: wrap the FD in an ELF whose single segment
# is loaded, and entered, at the FD base address, then append the hash
# segment with a test signature. Boards without secure boot accept it.
#
function _pack_image() {
    local fd="${WORKSPACE}/Build/${PLATFORM_NAME}/${RELEASE_TYPE}_${TOOLCHAIN}/FV/${FD_NAME}.fd"
    local elf="${WORKSPACE}/${PLATFORM_NAME}_unsigned.elf"
    local load_addr
    local size

    echo " => Building ${OUTPUT_NAME}"

    load_addr=$(sed -n 's/^BaseAddress[[:space:]]*=[[:space:]]*\(0x[0-9a-fA-F]*\).*/\1/p' "${ROOTDIR}/${FDF_FILE}")
    [ -n "${load_addr}" ] || _error "No FD BaseAddress in ${FDF_FILE}"

    ${CROSS_COMPILE}objcopy -I binary -B aarch64 -O elf64-littleaarch64 \
        "${fd}" "${WORKSPACE}/${PLATFORM_NAME}_fd.o"
    ${CROSS_COMPILE}ld "${WORKSPACE}/${PLATFORM_NAME}_fd.o" -o "${elf}" -EL \
        -T "${ROOTDIR}/edk2-platforms/Silicon/Qualcomm/ElfPayload.lds" \
        --defsym=ELFENTRY=${load_addr} -Ttext=${load_addr}
    rm -f "${WORKSPACE}/${PLATFORM_NAME}_fd.o"

    python3 "${QCOMDIR}/misc/qtestsign/qtestsign.py" -v${MBN_VERSION} uefi \
        -o "${WORKSPACE}/${OUTPUT_NAME}" "${elf}"

    size=$(stat -c %s "${WORKSPACE}/${OUTPUT_NAME}")
    [ "${size}" -le $((UEFI_PARTITION_SIZE)) ] ||
        _error "${OUTPUT_NAME} is ${size} bytes, larger than the UEFI partition ($((UEFI_PARTITION_SIZE)))"

    cp "${WORKSPACE}/${OUTPUT_NAME}" "${ROOTDIR}/"
    echo " => ${OUTPUT_NAME}: entry ${load_addr}, ${size} bytes"
}

function _build(){
    local DEVICE="${1}"; shift

    #
    # Grab platform parameters
    #
    if [ -f "${QCOMDIR}/configs/${DEVICE}.conf" ]
    then source "${QCOMDIR}/configs/${DEVICE}.conf"
    else _error "Device configuration not found"
    fi

    [ -f "${QCOMDIR}/misc/qtestsign/qtestsign.py" ] ||
        _error "qtestsign is missing. Run: git submodule update --init edk2-qualcomm/misc/qtestsign"
    python3 -c 'import cryptography' 2>/dev/null ||
        _error "qtestsign needs the Python cryptography module (python3-cryptography)"

    rm -f "${ROOTDIR}/${OUTPUT_NAME}"

    #
    # Build EDK2
    #
    apply_patchset "${ROOTDIR}/edk2-patches" "${ROOTDIR}/edk2" || exit 1
    apply_patchset "${QCOMDIR}/edk2-platforms-patches" "${ROOTDIR}/edk2-platforms" || exit 1
    apply_patchset "${ROOTDIR}/devicetree/mainline/patches" "${ROOTDIR}/devicetree/mainline/upstream" || exit 1
    _check_translations

    [ -d "${WORKSPACE}/Conf" ] || mkdir -p "${WORKSPACE}/Conf"

    export GCC_AARCH64_PREFIX="${CROSS_COMPILE}"
    export CLANG38_AARCH64_PREFIX="${CROSS_COMPILE}"
    #
    # The upstream Qualcomm libraries name their packages relative to
    # Silicon/Qualcomm and Platform/Qualcomm.
    #
    PACKAGES_PATH="${ROOTDIR}"
    PACKAGES_PATH+=":${ROOTDIR}/edk2"
    PACKAGES_PATH+=":${ROOTDIR}/edk2-platforms"
    PACKAGES_PATH+=":${ROOTDIR}/edk2-platforms/Silicon/Qualcomm"
    PACKAGES_PATH+=":${ROOTDIR}/edk2-platforms/Platform/Qualcomm"
    PACKAGES_PATH+=":${QCOMDIR}"
    export PACKAGES_PATH

    make -C "${ROOTDIR}/edk2/BaseTools"
    source "${ROOTDIR}/edk2/edksetup.sh" --reconfig

    build \
        -s \
        -n 0 \
        -a AARCH64 \
        -t "${TOOLCHAIN}" \
        -p "${ROOTDIR}/${DSC_FILE}" \
        -b "${RELEASE_TYPE}" \
        -D FIRMWARE_VER="${GIT_COMMIT}" \
        ${EDK2_FLAGS}

    #
    # Compile final image
    #
    _pack_image

    echo "Build done: ${OUTPUT_NAME}"
}

function _clean() {
    local conf

    for conf in "${QCOMDIR}"/configs/*.conf; do
        [ "${DEVICE}" == "all" ] || [ "$(basename "${conf}" .conf)" == "${DEVICE}" ] || continue
        (
            source "${conf}"
            rm --one-file-system --recursive --force \
                "${WORKSPACE}/Build/${PLATFORM_NAME}" \
                "${WORKSPACE}/${PLATFORM_NAME}_unsigned.elf" \
                "${WORKSPACE}/${OUTPUT_NAME}" \
                "${ROOTDIR}/${OUTPUT_NAME}"
        )
    done
}

#
# Default variables
#
typeset -l DEVICE
typeset -u RELEASE_TYPE
DEVICE=""
RELEASE_TYPE=DEBUG
TOOLCHAIN=GCC
EDK2_FLAGS=""
SKIP_PATCHSETS=false
CLEAN=false
OUTDIR="${PWD}"

QCOMDIR="$(realpath "$(dirname "$0")")"
ROOTDIR="$(realpath "${QCOMDIR}/..")"

#
# Get options
#
OPTS=$(getopt -o "d:r:t:Ch" -l "device:,release:,toolchain:,edk2-flags:,skip-patchsets,clean,help" -n build.sh -- "${@}") || _help $?
eval set -- "${OPTS}"
while true; do
    case "${1}" in
        -d|--device) DEVICE="${2}"; shift 2 ;;
        -r|--release) RELEASE_TYPE="${2}"; shift 2 ;;
        -t|--toolchain) TOOLCHAIN="${2}"; shift 2 ;;
        --edk2-flags) EDK2_FLAGS="${2}"; shift 2 ;;
        --skip-patchsets) SKIP_PATCHSETS=true; shift ;;
        -C|--clean) CLEAN=true; shift ;;
        -h|--help) _help 0; shift ;;
        --) shift; break ;;
        *) break ;;
    esac
done
if [[ -n "${@}" ]]; then
    echo "Invalid additional arguments '${@}'"
    _help 1
fi

[ -z "${DEVICE}" ] && _help 1
[ -f "${QCOMDIR}/configs/${DEVICE}.conf" ] || [ "${DEVICE}" == "all" ] || _error "Device configuration not found"

export WORKSPACE="${OUTDIR}/workspace"

if "${CLEAN}"; then _clean; exit "$?"; fi

#
# Get machine architecture
#
MACHINE_TYPE=$(uname -m)

# Fix-up possible differences in reported arch
if [ ${MACHINE_TYPE} == 'arm64' ]; then
    MACHINE_TYPE='aarch64'
elif [ ${MACHINE_TYPE} == 'amd64' ]; then
    MACHINE_TYPE='x86_64'
fi

if [ ${MACHINE_TYPE} != 'aarch64' ]; then
    export CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
fi

GIT_COMMIT="$(git -C "${ROOTDIR}" describe --tags --always)" || GIT_COMMIT="unknown"

[ -d "${WORKSPACE}" ] || mkdir "${WORKSPACE}"

cd "${ROOTDIR}" || exit 1

# Exit on first error
set -e

if [ "${DEVICE}" == "all" ]
then
    for i in "${QCOMDIR}"/configs/*.conf; do
        DEV="$(basename "$i" .conf)"
        echo "Building ${DEV}"
        _build "${DEV}"
    done
else
    _build "${DEVICE}"
fi
