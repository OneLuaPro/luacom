# ------------------------------------------------------------------------------
# General definitions
cmake_minimum_required(VERSION 3.31 FATAL_ERROR)

# ------------------------------------------------------------------------------
# find liblua installation and version info
if(NOT LUA_HINTS)
  if(WIN32)
    set(LUA_HINTS "c:/Apps")
  endif()
endif()
find_package(liblua REQUIRED CONFIG HINTS ${LUA_HINTS})
if(liblua_FOUND)
  message(STATUS "liblua version        : ${liblua_VERSION}")
  message(STATUS "liblua install prefix : ${LIBLUA_INSTALLDIR}")
  message(STATUS "liblua include dir    : ${LIBLUA_INCLUDEDIR}")
  message(STATUS "liblua lib dir        : ${LIBLUA_LIBDIR}")
else()
  message(FATAL_ERROR "Unable to find liblua version ${liblua_VERSION}.")
endif()
# Note: liblua_VERSION is set by find_package() directly. LIBLUA_INSTALLDIR,
# LIBLUA_INCLUDEDIR and LIBLUA_LIBDIR are set by libluaConfigVersion.cmake in
# <prefix>/share/cmake/liblua.

# ------------------------------------------------------------------------------
# Project defintion
project(luacom C CXX )

# ------------------------------------------------------------------------------
include(CheckCSourceCompiles)
include(CheckSymbolExists)

# ------------------------------------------------------------------------------
# Setup GNU-alike installatin directories
include (GNUInstallDirs)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/${CMAKE_INSTALL_LIBDIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/${CMAKE_INSTALL_LIBDIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/${CMAKE_INSTALL_BINDIR})
set(INSTALL_LIBDIR
  ${CMAKE_INSTALL_LIBDIR} CACHE PATH "Installation directory for libraries")
set(INSTALL_BINDIR
  ${CMAKE_INSTALL_BINDIR} CACHE PATH "Installation directory for executables")
set(INSTALL_INCLUDEDIR
  ${CMAKE_INSTALL_INCLUDEDIR} CACHE PATH "Installation directory for header files")
set(INSTALL_DOCDIR
  ${CMAKE_INSTALL_DOCDIR} CACHE PATH "Installation directory for documentation")
set(INSTALL_MANDIR
  ${CMAKE_INSTALL_MANDIR} CACHE PATH "Installation directory for manpages")
set(INSTALL_DATAROOTDIR
  ${CMAKE_INSTALL_DATAROOTDIR} CACHE PATH "Installation directory for data")
# Lua-specific installation dirs
set(INSTALL_TOP_CDIR
  ${INSTALL_LIBDIR}/lua/${liblua_VERSION_MAJOR}.${liblua_VERSION_MINOR})
set(INSTALL_TOP_LDIR
  ${INSTALL_DATAROOTDIR}/lua/${liblua_VERSION_MAJOR}.${liblua_VERSION_MINOR})

# ------------------------------------------------------------------------------
# _stricmp (is there a simpler way of doing this?)
check_symbol_exists( _stricmp string.h HAVE__STRICMP)
if(HAVE__STRICMP)
  set(STRICMP _stricmp)
endif()
if(NOT STRICMP)
  check_symbol_exists(stricmp string.h HAVE_STRICMP)
  if(HAVE_STRICMP)
    set(STRICMP stricmp)
  endif()
endif ( )
if(NOT STRICMP)
  check_symbol_exists(strcasecmp string.h HAVE_STRCASECMP)
  if(HAVE_STRCASECMP)
    set(STRICMP strcasecmp)
  endif()
endif()
if(NOT STRICMP)
  message(FATAL_ERROR "_stricmp, stricmp, or strcasecmp not found")
endif()

# ------------------------------------------------------------------------------
# luacom core DLL
add_library(luacom SHARED)
# setup lua include directory
target_include_directories(luacom PRIVATE
  ${LIBLUA_INCLUDEDIR} ../src/library ../src/dll ../include
)
# plattform-independend sources
target_sources(luacom PRIVATE
  ../src/library/LuaAux.cpp ../src/library/luabeans.cpp ../src/library/luacom.cpp
  ../src/library/tLuaCOM.cpp ../src/library/tLuaCOMException.cpp
  ../src/library/tLuaCOMTypeHandler.cpp ../src/library/tLuaDispatch.cpp
  ../src/library/tLuaObjList.cpp ../src/library/tLuaVector.cpp
  ../src/library/tStringBuffer.cpp ../src/library/tUtil.cpp ../src/library/tCOMUtil.cpp
  ../src/library/tLuaCOMClassFactory.cpp ../src/library/tLuaCOMConnPoints.cpp
  ../src/library/LuaCompat.cpp ../src/library/tLuaCOMEnumerator.cpp
  ../src/library/tLuaObject.cpp ../src/library/tLuaControl.cpp ../src/library/tLuaTLB.cpp
  ../src/dll/luacom_dll.cpp ../src/dll/luacom_dll.def
)
# setup platform-specific sources, compile and linker options
if(WIN32 AND NOT MinGW)
  # Important definitions for DLL and symbol export
  target_compile_definitions(luacom PRIVATE
    NO_HTMLHELP _stricmp=${STRICMP}
    LUACOM_DLL="luacom.dll"
    luacom_EXPORTS
    _WINDLL _WIN32 _CRT_SECURE_NO_WARNINGS
  )
  # We silently assume all libs are available with headers and static lib
  target_link_directories(luacom PRIVATE ${LIBLUA_LIBDIR})
  target_link_libraries(luacom PRIVATE
    liblua gdi32 shell32 advapi32 ole32 winspool uuid oleaut32 shlwapi
  )
else()
  message(FATAL_ERROR "Not yet fully implemented.")
endif()
# Install
install(TARGETS luacom RUNTIME DESTINATION ${INSTALL_TOP_CDIR})

# ------------------------------------------------------------------------------
# Documentation Generation (Pandoc)
find_program(PANDOC_EXECUTABLE pandoc DOC "Path to pandoc executable")
if(PANDOC_EXECUTABLE)
  # Define build and output paths
  set(DOC_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen-doc")
  set(DOC_HTML_OUTPUT "${DOC_BUILD_DIR}/luacom.html")
  # Add command to merge and convert RST files to HTML
  add_custom_command(
    OUTPUT "${DOC_HTML_OUTPUT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DOC_BUILD_DIR}"
    COMMAND ${PANDOC_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../doc/luacom.tex
    -f latex -t html -s
    --toc --mathjax
    --number-sections
    -c lua.css -c manual.css -c index.css luacom.tex
    --embed-resources
    -o "${DOC_HTML_OUTPUT}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../doc"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../doc/luacom.tex"
    COMMENT "Converting luacom tex manual to HTML"
    VERBATIM
  )
  # Create a target that is built by default
  add_custom_target(luacom_doc ALL DEPENDS "${DOC_HTML_OUTPUT}")
  # Install the generated HTML documentation
  install(FILES "${DOC_HTML_OUTPUT}" DESTINATION "${INSTALL_DOCDIR}")
else()
  message(FATAL_ERROR "Pandoc not found. Documentation will not be generated.")
endif()

# ------------------------------------------------------------------------------
# Install tests and examples
install(DIRECTORY ../src/test DESTINATION ${INSTALL_DOCDIR}/src)
install(DIRECTORY ../demo/ DESTINATION ${INSTALL_DOCDIR}/examples)

# ------------------------------------------------------------------------------
# Create and install README.txt
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/README.txt"
"Run the luacom test suite from here with:
lua src\\test\\luacom_tests5.lua

Note: The examples provided in this directory are legacy code. While they may
not be fully compatible with this OneLuaPro distribution, they remain a valuable
reference for future development and implementation.
"
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/README.txt" DESTINATION ${INSTALL_DOCDIR})
