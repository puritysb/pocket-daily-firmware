cmake_minimum_required(VERSION 3.16)
project(crosspoint_reader_tests C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Same header-only JSON library and version the firmware links (platformio.ini).
FetchContent_Declare(
  ArduinoJson
  GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
  GIT_TAG v7.4.2
)
FetchContent_MakeAvailable(ArduinoJson)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_library(crosspoint_test_common INTERFACE)
target_include_directories(crosspoint_test_common INTERFACE
  ${REPO_ROOT}
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib
)
target_compile_options(crosspoint_test_common INTERFACE
  -Wall
  -Wextra
  -pedantic
)

enable_testing()
include(GoogleTest)

add_subdirectory(streaming_json_parser)
add_subdirectory(release_json_parser)
add_subdirectory(differential_rounding)
add_subdirectory(hyphenation_eval)
add_subdirectory(utf8_compose)
add_subdirectory(content_manifest)
add_subdirectory(content_store)
add_subdirectory(content_presentation)
add_subdirectory(screen_presentation)
add_subdirectory(sd_font)
add_subdirectory(gfx_host)
add_subdirectory(hal_storage)
add_subdirectory(pocket_glance)
add_subdirectory(pocket_daily_identity)
add_subdirectory(pocket_daily_settings)
add_subdirectory(pocket_daily_learning_pack)
add_subdirectory(pocket_daily_upload_stream)
add_subdirectory(pocket_daily_staged_firmware)
add_subdirectory(publication_receipt)
add_subdirectory(ota_failure)
add_subdirectory(live_studio)
add_subdirectory(pocket_profile)
add_subdirectory(reading_progress)
add_subdirectory(reading_sync_ble)
add_subdirectory(build_failure)
add_subdirectory(reader_perf)
add_subdirectory(reader_layout)

add_subdirectory(opds_parser)

add_subdirectory(articles)
add_subdirectory(opds_filename)
add_subdirectory(html_to_plain_text)
add_subdirectory(library_text)
add_subdirectory(library_format)
add_subdirectory(library_index_file)
add_subdirectory(library_builder)
add_subdirectory(minibidi_arabic)
add_subdirectory(combining_marks)
add_subdirectory(credential_integrity)
add_subdirectory(xml_parser_utils)
add_subdirectory(token_boundary)
add_subdirectory(koreader_xpath_resolver)
add_subdirectory(progress_comparison)
add_subdirectory(font_cache_manager)
add_subdirectory(css_parser)
add_subdirectory(chapter_html_slim_parser)
add_subdirectory(korean_line_breaking)
add_subdirectory(ligature_guard)
add_subdirectory(page_link)
add_subdirectory(content_opf_parser)
add_subdirectory(chapter_position)
add_subdirectory(home_button)

add_subdirectory(inflate_stream)
add_subdirectory(fs_helpers)

add_subdirectory(absolute_grayscale)

add_subdirectory(sd_card_font)
add_subdirectory(glyph_bitmap)

target_include_directories(crosspoint_test_common INTERFACE
  ${REPO_ROOT}/freeink-sdk/libs/display/FreeInkDisplay/include
  ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/include)
foreach(target reader_layout_test reading_position_layout_test)
  target_sources(${target} PRIVATE
    ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/src/FontAlloc.c
    ${REPO_ROOT}/lib/Memory/BuildScratch.cpp
    ${REPO_ROOT}/lib/miniz/src/InflateStream.cpp
    ${REPO_ROOT}/lib/miniz/src/miniz_impl.c)
  target_include_directories(${target} PRIVATE ${REPO_ROOT}/lib/miniz/src ${REPO_ROOT}/lib/miniz/third_party)
endforeach()
