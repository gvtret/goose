# FindGooseLLVM: locates LLVM >= 15 and its tools.
#
# On success sets:
#   GooseLLVM_FOUND        TRUE
#   LLVM_LIBS              link libraries (e.g. -lLLVM-15)
#   LLVM_INCLUDE_DIRS      include dirs for llvm headers
#   LLVM_BIN_DIR           directory containing llc / clang (if found)
#   LLC_BIN                path to llc (may be empty)
#   CLANG_BIN              path to clang (may be empty)
#
# The libgoose_rt.a archive is built by the CMakeLists.txt goose_rt
# target, not by this module.
#
# Requires the caller to have enabled_language(C).
# This module is called from the root CMakeLists.txt when
# GOOSE_LLVM is ON.

if(NOT GOOSE_LLVM_FOUND)

    # LLVM minimum version. llvm-config reports the major.minor.patch;
    # we accept >= 15.
    find_program(LLVM_CONFIG_BIN llvm-config
        HINTS ENV LLVM_CONFIG
        PATHS /usr/bin /usr/local/bin
    )
    if(NOT LLVM_CONFIG_BIN)
        message(WARNING "FindGooseLLVM: llvm-config not found; "
                        "install LLVM 15+ (e.g. apt install llvm-15-dev)")
        set(GooseLLVM_FOUND FALSE)
        return()
    endif()

    execute_process(
        COMMAND ${LLVM_CONFIG_BIN} --version
        RESULT_VARIABLE _llvmcfg_result
        OUTPUT_VARIABLE _llvm_version
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT _llvmcfg_result EQUAL 0)
        message(WARNING "FindGooseLLVM: llvm-config --version failed")
        set(GooseLLVM_FOUND FALSE)
        return()
    endif()

    # Parse "15.0.1" into major = 15
    string(REGEX MATCH "^([0-9]+)" _llvm_major_match "${_llvm_version}")
    set(_llvm_major "${_llvm_major_match}")
    if(_llvm_major LESS 15)
        message(WARNING
            "FindGooseLLVM: LLVM ${_llvm_version} found, but version >= 15 is required. "
            "Install a newer LLVM (apt install llvm-${_llvm_major}-dev or later).")
        set(GooseLLVM_FOUND FALSE)
        return()
    endif()

    # LLVM include dir and lib files
    execute_process(
        COMMAND ${LLVM_CONFIG_BIN} --includedir
        OUTPUT_VARIABLE LLVM_INCLUDE_DIRS
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
        COMMAND ${LLVM_CONFIG_BIN} --libdir
        OUTPUT_VARIABLE LLVM_LIBDIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
        COMMAND ${LLVM_CONFIG_BIN} --libfiles
        OUTPUT_VARIABLE _llvm_libfiles
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    # On most systems LLVM ships as a shared library; we link it by path.
    # llvm-config --link-static returns a non-zero exit code when static
    # libraries are not installed; fall back to --libs (shared) in that case.
    execute_process(
        COMMAND ${LLVM_CONFIG_BIN} --link-static
        RESULT_VARIABLE _llvm_static_result
        OUTPUT_VARIABLE _llvm_static_libs
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(_llvm_static_result EQUAL 0 AND NOT _llvm_static_libs STREQUAL "")
        string(REPLACE " " ";" _llvm_static_list "${_llvm_static_libs}")
        set(LLVM_LIBS "${_llvm_static_list}")
        set(GOOSE_LLVM_STATIC TRUE)
    else()
        # llvm-config --libs returns full paths (e.g. /usr/lib/llvm-20/lib/libLLVM-20.so).
        # Use them as-is; CMake treats a path as a library to link.
        string(REPLACE " " ";" _llvm_lib_list "${_llvm_libfiles}")
        set(LLVM_LIBS "${_llvm_lib_list}")
        set(GOOSE_LLVM_STATIC FALSE)
    endif()

    # Find llc and clang (the tools the test runner needs).
    find_program(LLC_BIN llc
        HINTS ${LLVM_LIBDIR}/../bin
    )
    find_program(CLANG_BIN clang
        HINTS ${LLVM_LIBDIR}/../bin
    )
    # LLVM_BIN_DIR: the bin directory containing llc/clang, if found
    if(LLC_BIN)
        get_filename_component(LLVM_BIN_DIR "${LLC_BIN}" DIRECTORY)
    elseif(CLANG_BIN)
        get_filename_component(LLVM_BIN_DIR "${CLANG_BIN}" DIRECTORY)
    endif()

    set(GooseLLVM_FOUND TRUE)

    message(STATUS "FindGooseLLVM: LLVM ${_llvm_version} found")

endif()
