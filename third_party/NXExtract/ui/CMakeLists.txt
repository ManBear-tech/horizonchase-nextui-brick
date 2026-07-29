# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)
project(nxextract_ui C)

set(NXEXTRACT_GLIBC_GATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools/check-glibc.sh")

add_executable(nxextract-ui nxextract_ui.c)
target_compile_features(nxextract-ui PRIVATE c_std_11)
target_compile_options(nxextract-ui PRIVATE
  -O2
  -Wall
  -Wextra
  -Wformat=2
  -Wshadow
  -Wstrict-prototypes
)
target_link_libraries(nxextract-ui PRIVATE dl)

add_custom_command(TARGET nxextract-ui POST_BUILD
  COMMAND "${NXEXTRACT_GLIBC_GATE}" "$<TARGET_FILE:nxextract-ui>"
  COMMENT "Checking NXExtract AArch64/GLIBC compatibility gate"
)
