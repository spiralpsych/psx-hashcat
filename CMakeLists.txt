cmake_minimum_required(VERSION 3.21)

project(
    psx-hashcat
    LANGUAGES C ASM
    VERSION 1.1.4
    DESCRIPTION "A PS1 port of GBA-HASHCAT"
)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(SPLASH_SOURCE_DIR "${PROJECT_SOURCE_DIR}/data/assets")
set(SPLASH_BUILD_DIR "${PROJECT_BINARY_DIR}/generated/assets")
set(SPLASH_LOGO_TIM "${SPLASH_BUILD_DIR}/splash_logo.tim")
set(SPLASH_TITLE_TIM "${SPLASH_BUILD_DIR}/splash_title.tim")
set(SPLASH_PREVIEW "${SPLASH_BUILD_DIR}/splash_preview.png")

add_custom_command(
    OUTPUT
        "${SPLASH_LOGO_TIM}"
        "${SPLASH_TITLE_TIM}"
        "${SPLASH_BUILD_DIR}/logo_preview.png"
        "${SPLASH_BUILD_DIR}/title_preview.png"
        "${SPLASH_PREVIEW}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SPLASH_BUILD_DIR}"
    COMMAND
        "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tools/gen_splash_assets.py"
        --assets-dir "${SPLASH_SOURCE_DIR}"
        --output-dir "${SPLASH_BUILD_DIR}"
    DEPENDS
        "${PROJECT_SOURCE_DIR}/tools/gen_splash_assets.py"
        "${SPLASH_SOURCE_DIR}/logo.png"
        "${SPLASH_SOURCE_DIR}/title.png"
    COMMENT "Converting data/assets splash PNGs to embedded PS1 TIM files"
    VERBATIM
)

add_custom_target(
    psx_hashcat_splash_assets
    DEPENDS
        "${SPLASH_LOGO_TIM}"
        "${SPLASH_TITLE_TIM}"
        "${SPLASH_PREVIEW}"
)

set(SOURCES
    src/main.c
    src/sha256.c
    src/cd_wordlist.c
)

psn00bsdk_add_executable(psx_hashcat GPREL ${SOURCES})
add_dependencies(psx_hashcat psx_hashcat_splash_assets)

target_include_directories(psx_hashcat PRIVATE src)
target_compile_options(psx_hashcat PRIVATE -O3 -fomit-frame-pointer -fno-strict-aliasing -Wall -Wextra)

psn00bsdk_target_incbin(psx_hashcat PRIVATE splash_logo_tim "${SPLASH_LOGO_TIM}")
psn00bsdk_target_incbin(psx_hashcat PRIVATE splash_title_tim "${SPLASH_TITLE_TIM}")

psn00bsdk_add_cd_image(
    psx_hashcat_iso
    psx-hashcat
    iso.xml
    DEPENDS psx_hashcat system.cnf ${PROJECT_SOURCE_DIR}/data/WORDLIST.TXT
)
