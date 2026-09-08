cmake_minimum_required(VERSION 3.16)
project(aera_telegram LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_executable(aera-telegram telegram_worker.cpp)
target_include_directories(aera-telegram PRIVATE
  "${TDLIB_SOURCE}"
  "${TDLIB_SOURCE}/td/generate/auto"
  "${TDLIB_SOURCE}/tdutils")
target_link_libraries(aera-telegram PRIVATE
  "${TDLIB_BUILD}/libtdclient.a"
  "${TDLIB_BUILD}/libtdcore.a"
  "${TDLIB_BUILD}/libtdmtproto.a"
  "${TDLIB_BUILD}/tde2e/libtde2e.a"
  "${TDLIB_BUILD}/tddb/libtddb.a"
  "${TDLIB_BUILD}/tdnet/libtdnet.a"
  "${TDLIB_BUILD}/tdactor/libtdactor.a"
  "${TDLIB_BUILD}/libtdapi.a"
  "${TDLIB_BUILD}/tdutils/libtdutils.a"
  "${TDLIB_BUILD}/sqlite/libtdsqlite.a"
  ssl crypto z dl pthread)
target_compile_options(aera-telegram PRIVATE -Os -ffunction-sections -fdata-sections)
target_link_options(aera-telegram PRIVATE -fuse-ld=lld -Wl,--gc-sections)
