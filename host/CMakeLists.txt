cmake_minimum_required(VERSION 3.16)
project(pdui_host LANGUAGES C CXX)
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
# Single host archive; no GoogleTest or device-only dependencies.
add_library(pdui_host STATIC
  PocketUIHost.cpp HostChecksums.cpp
  ${REPO_ROOT}/src/pocket_daily/ContentCard.cpp
  ${REPO_ROOT}/src/pocket_daily/ContentManifest.cpp
  ${REPO_ROOT}/src/pocket_daily/ContentPageRenderer.cpp
  ${REPO_ROOT}/src/pocket_daily/ContentImageRenderer.cpp
  ${REPO_ROOT}/src/pocket_daily/ContentImage.cpp
  ${REPO_ROOT}/src/pocket_daily/ContentTextLayout.cpp
  ${REPO_ROOT}/lib/GfxRenderer/GfxRenderer.cpp
  ${REPO_ROOT}/lib/GfxRenderer/FontCacheManager.cpp
  ${REPO_ROOT}/lib/GfxRenderer/Bitmap.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFont.cpp
  ${REPO_ROOT}/lib/EpdFont/FontDecompressor.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c)
target_compile_features(pdui_host PUBLIC cxx_std_20)
set_target_properties(pdui_host PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
target_include_directories(pdui_host PUBLIC include hal ${REPO_ROOT} ${REPO_ROOT}/src ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/GfxRenderer ${REPO_ROOT}/lib/EpdFont ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/lib/MiniBidi ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/uzlib/src)
find_package(ZLIB REQUIRED)
target_compile_options(pdui_host PRIVATE -Wall -Wextra -pedantic)
target_link_libraries(pdui_host PUBLIC ZLIB::ZLIB)
