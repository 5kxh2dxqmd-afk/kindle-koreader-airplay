# cross-compile-kindle.cmake
# NOTE: not currently wired into the build — the Makefile + Dockerfile.sf /
# Dockerfile.hf is the actual build path. Kept as a reference toolchain file.
#
# Kindle build ABI is a firmware-version split, not a device-family split:
#   firmware <=5.16.2.1.1  → soft-float (sf)  — PW1-5, Voyage, Oasis 1-3
#   firmware >=5.16.3      → hard-float (hf)  — PW6/PW12, current Scribe,
#                                                and anything updated past sf
# Set ARM_TARGET to "sf" or "hf" (default sf).
#
# Common setups:
#   sf: KOReader's koxtoolchain "kindle" target, or messense's
#       arm-unknown-linux-gnueabi (macOS brew)
#   hf: KOReader's koxtoolchain "kindlehf" release tarball
#       (arm-kindlehf-linux-gnueabihf) — https://github.com/koreader/koxtoolchain

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(NOT DEFINED ARM_TARGET)
    set(ARM_TARGET "sf")
endif()

if(ARM_TARGET STREQUAL "hf")
    set(TC_NAME "arm-kindlehf-linux-gnueabihf-")
else()
    set(TC_NAME "arm-linux-gnueabihf-")
endif()

# ── Toolchain ──────────────────────────────────────────────────────
# Override via: cmake -DARM_TC_PREFIX=/path/to/tc -DARM_TARGET=sf|hf ..
if(DEFINED ARM_TC_PREFIX)
    set(TC_PREFIX "${ARM_TC_PREFIX}/bin/${TC_NAME}")
else()
    # Try common locations
    find_program(CC_FOUND "${TC_NAME}gcc")
    if(CC_FOUND)
        set(TC_PREFIX "${TC_NAME}")
    else()
        message(WARNING "${TC_NAME}gcc not found. Install cross toolchain for ARM_TARGET=${ARM_TARGET}.")
        set(TC_PREFIX "${TC_NAME}")
    endif()
endif()

set(CMAKE_C_COMPILER   "${TC_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${TC_PREFIX}g++")
set(CMAKE_STRIP        "${TC_PREFIX}strip")

# ── Sysroot (optional, improves header/lib resolution) ────────────
if(DEFINED ARM_SYSROOT)
    set(CMAKE_SYSROOT "${ARM_SYSROOT}")
    set(CMAKE_FIND_ROOT_PATH "${ARM_SYSROOT}")
    set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
    set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
endif()

# ── ARM flags ─────────────────────────────────────────────────────
if(ARM_TARGET STREQUAL "hf")
    set(CMAKE_C_FLAGS_INIT "-march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb")
else()
    set(CMAKE_C_FLAGS_INIT "-march=armv7-a -mfloat-abi=softfp -mfpu=neon")
endif()
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,-rpath,/mnt/us/airplay")
