# SOTN ESP32-S3 — F2 stage 2: the whole game, compiled for Xtensa.
#
# Source lists mirror the root CMakeLists' PC build (SOURCE_FILES_PC + CORE +
# the statically-linked WRP stage) plus psyz's core and the decomp Psy-Q libgpu.
# SDL-coupled platform files are replaced by the esp_shim/soft_esp32 pair.

set(REPO ${CMAKE_CURRENT_LIST_DIR}/../..)
set(PSYZ ${REPO}/tools/psyz/psyz)
set(PSYQ ${REPO}/tools/psyz/decomp)

# ---- game: PC layer (minus nothing - io/main are portable C) ----------------
set(SRC_PC
    ${REPO}/src/pc/main.c
    ${REPO}/src/pc/stubs.c
    ${REPO}/src/pc/sotn.c
    ${REPO}/src/pc/replay.c
    ${REPO}/src/pc/pc.c
    ${REPO}/src/pc/cutscene_pc.c
    ${REPO}/src/pc/io.c
    ${REPO}/src/pc/sim_pc.c
    ${REPO}/src/pc/pl_arc.c
    ${REPO}/src/pc/pl_ric.c
    ${REPO}/src/pc/servant_pc.c
    ${REPO}/src/pc/weapon_pc.c
    ${REPO}/src/pc/stages/stage_loader.c
    ${REPO}/src/pc/stages/overlay.c
    ${REPO}/src/pc/stages/stage_wrp.c
    ${REPO}/src/pc/3rd/cJSON/cJSON.c
)

# ---- game: engine + players + weapons (the PC exe's exact curated lists) ----
include(${CMAKE_CURRENT_LIST_DIR}/game_sources.cmake)
set(SRC_DRA ${GAME_SOURCE_FILES_DRA})
set(SRC_RIC ${GAME_SOURCE_FILES_RIC})
set(SRC_WEAPON ${GAME_SOURCE_FILES_WEAPON})

# ---- game: the WRP stage, statically linked (exact root-CMake list) ---------
set(SRC_WRP ${GAME_SOURCE_FILES_WRP})
set(SRC_WRP_GEN "")
set(SRC_NZ0 ${GAME_SOURCE_FILES_NZ0})
set(SRC_SEL ${GAME_SOURCE_FILES_SEL})
set(SRC_NP3 ${GAME_SOURCE_FILES_NP3})

# ---- psyz core + decomp Psy-Q libgpu ----------------------------------------
file(GLOB SRC_PSYZ ${PSYZ}/src/psyz/*.c)
list(FILTER SRC_PSYZ EXCLUDE REGEX "libgte_hw_sqrt")
include(${CMAKE_CURRENT_LIST_DIR}/psyq_sources.cmake)
set(SRC_LIBGPU ${GAME_SOURCE_FILES_PSYQ})
set(SRC_PLATFORM
    ${PSYZ}/src/platform/psyz.c
)

idf_component_register(
    SRCS
        app_main.c
        esp_shim.c
        esp_fileapi.c
        soft_esp32.c
        lcd_esp32.c
        esp_audio.c
        esp_sd.c
        ${SRC_PC}
        ${SRC_DRA}
        ${SRC_RIC}
        ${SRC_WEAPON}
        ${SRC_WRP}
        ${SRC_WRP_GEN}
        ${SRC_NZ0}
        ${SRC_SEL}
        ${SRC_NP3}
        ${REPO}/src/pc/stages/stage_sel.c
        ${SRC_PSYZ}
        ${SRC_LIBGPU}
        ${SRC_PLATFORM}
    INCLUDE_DIRS
        .
        ${REPO}/include
        ${REPO}/src/dra
        ${REPO}/src/pc/3rd
        ${REPO}/src/pc
        ${PSYZ}/include
        ${PSYZ}/src
    REQUIRES
        esp_timer
        fatfs
        spi_flash
        esp_lcd
        driver
        sdmmc
        esp_adc   # handheld only: the button ladder and stick are analog
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    _USE_MATH_DEFINES
    VERSION_PC
    PERMUTER
    NON_MATCHING
    HARD_LINK
    _internal_version_us
    __psyz
    SOTN_STATIC_WRP
    SOTN_STATIC_NZ0
    SOTN_STATIC_SEL
    SOTN_STATIC_NP3
    # ESP-IDF's VFS ignores chdir(): reroute every fopen through the rebasing
    # wrapper in esp_fileapi.c (which #undefs this to reach the real one)
    fopen=sotn_fopen
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    # -Werror=all promotes every -Wall member to a SPECIFIC error, which a
    # blanket -Wno-error does not demote; the warnings themselves must go
    -Wno-error
    -Wno-char-subscripts
    -Wno-switch
    -Wno-pointer-sign
    -Wno-format
    -Wno-dangling-pointer
    -Wno-format-truncation
    -Wno-uninitialized
    -Wno-maybe-uninitialized
    -Wno-parentheses
    -Wno-misleading-indentation
    -Wno-missing-braces
    -Wno-sign-compare
    -Wno-sequence-point
    -Wno-unused-value
    -Wno-bool-operation
    -Wno-empty-body
    -Wno-type-limits
    -Wno-memset-elt-size
    -Wno-sizeof-pointer-div
    -Wno-unused-label
    -Wno-bool-compare
    -Wno-logical-not-parentheses
    -Wno-stringop-truncation
    -Wno-enum-int-mismatch
    -Wno-enum-conversion
    -Wno-array-bounds
    -Wno-stringop-overflow
    -Wno-address
    -Wno-implicit-function-declaration
    -Wno-incompatible-pointer-types
    -Wno-int-conversion
    -Wno-return-mismatch
    -Wno-return-type
    -Wno-builtin-declaration-mismatch
    -Wno-unused-variable
    -Wno-unused-function
    -funsigned-char
    -include stdio.h
)

# ---- handheld build ---------------------------------------------------------
# build.ps1 xiao sets SOTN_BOARD_XIAO; everything board-specific hangs off it.
if(SOTN_BOARD_XIAO)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        SOTN_BOARD_XIAO
        # the whole game reads from the card here; see partitions_xiao.csv
        SOTN_DATA_ROOT="/sd")
    # Force-include AFTER -include stdio.h above, because it redefines printf
    # and needs stdio's declaration to already be in scope. This covers the
    # whole game tree without editing it; see sotn_printf.h for why a blocking
    # printf is fatal on a console nobody has a terminal attached to.
    target_compile_options(${COMPONENT_LIB} PRIVATE
        "SHELL:-include ${CMAKE_CURRENT_LIST_DIR}/sotn_printf.h")
else()
    # Waveshare board: the assets are flashed into the `storage` partition.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SOTN_DATA_ROOT="/flash")
endif()

# main.c's `main` becomes callable from app_main; stage_wrp's InitStage gets
# the same namespaced rename the PC static build uses
set_source_files_properties(${REPO}/src/pc/main.c PROPERTIES
    COMPILE_DEFINITIONS "main=sotn_main")
# Two static stages in one image: every global that OVL_EXPORT does not
# cover collides between stages (the PSX overlays relied on only one being
# loaded). With plain archives the linker resolves each name to ONE stage
# silently - NZ0 was running WRP's HitDetection. Rename them all per stage;
# references are stage-internal by construction.
include(${CMAKE_CURRENT_LIST_DIR}/stage_dup_syms.cmake)
list(APPEND STAGE_DUP_SYMS cluts gfxBanks rooms_layers spriteBanks)
foreach(src ${SRC_WRP} ${REPO}/src/pc/stages/stage_wrp.c)
    set(defs "")
    foreach(sym ${STAGE_DUP_SYMS})
        list(APPEND defs "${sym}=WRP_${sym}")
    endforeach()
    set_property(SOURCE ${src} APPEND PROPERTY COMPILE_DEFINITIONS ${defs})
endforeach()
foreach(src ${SRC_NZ0})
    set(defs "")
    foreach(sym ${STAGE_DUP_SYMS})
        list(APPEND defs "${sym}=NZ0_${sym}")
    endforeach()
    set_property(SOURCE ${src} APPEND PROPERTY COMPILE_DEFINITIONS ${defs})
endforeach()
foreach(src ${SRC_NP3})
    set(defs "")
    foreach(sym ${STAGE_DUP_SYMS})
        list(APPEND defs "${sym}=NP3_${sym}")
    endforeach()
    set_property(SOURCE ${src} APPEND PROPERTY COMPILE_DEFINITIONS ${defs})
endforeach()
foreach(src ${SRC_SEL} ${REPO}/src/pc/stages/stage_sel.c)
    set(defs "")
    foreach(sym ${STAGE_DUP_SYMS})
        list(APPEND defs "${sym}=SEL_${sym}")
    endforeach()
    set_property(SOURCE ${src} APPEND PROPERTY COMPILE_DEFINITIONS ${defs})
endforeach()

set_property(SOURCE ${REPO}/src/pc/stages/stage_wrp.c APPEND PROPERTY
    COMPILE_DEFINITIONS "InitStage=WRP_StaticInitStage")
set_property(SOURCE ${REPO}/src/pc/stages/stage_nz0.c APPEND PROPERTY
    COMPILE_DEFINITIONS "InitStage=NZ0_StaticInitStage")
set_property(SOURCE ${REPO}/src/pc/stages/stage_sel.c APPEND PROPERTY
    COMPILE_DEFINITIONS "InitStage=SEL_StaticInitStage")
set_property(SOURCE ${REPO}/src/pc/stages/stage_np3.c APPEND PROPERTY
    COMPILE_DEFINITIONS "InitStage=NP3_StaticInitStage")

# WEAPON_ID names each weapon's symbols (w_002_header etc.)
foreach(w 000 002 007 012 034 037 041 045 046 051 052)
    set_source_files_properties(${REPO}/src/weapon/w_${w}.c PROPERTIES
        COMPILE_DEFINITIONS "WEAPON_ID=w_${w}")
endforeach()
