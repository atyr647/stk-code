# Included at the end of CMakeLists.txt for the N64: the console entry point,
# and the ROM image (ELF -> .z64 the way libdragon's n64.mk does it).
#
# STK's own main() is kept, renamed to stk_main; libdragon's runtime calls
# n64/compat/src/n64_main.cpp's main(), which sets the platform up and then
# hands over to it with a command line.
set_source_files_properties(${PROJECT_SOURCE_DIR}/src/main.cpp PROPERTIES
    COMPILE_DEFINITIONS "main=stk_main")
target_sources(supertuxkart PRIVATE ${STK_N64_DIR}/compat/src/n64_main.cpp
                                    ${STK_N64_DIR}/compat/src/n64_diag.cpp)
# --wrap targets are invisible to LTO's symbol resolution: keep this file out
set_source_files_properties(${STK_N64_DIR}/compat/src/n64_diag.cpp PROPERTIES
    COMPILE_OPTIONS "-fno-lto")
if(STK_N64_SCREEN_LOG)
    target_compile_definitions(supertuxkart PRIVATE STK_N64_SCREEN_LOG=1)
endif()
target_link_libraries(supertuxkart stk_n64_compat)
target_link_options(supertuxkart PRIVATE
    "-Wl,-Map=${CMAKE_BINARY_DIR}/supertuxkart.map")
target_link_options(supertuxkart PRIVATE "-Wl,--wrap=gettimeofday")
target_link_options(supertuxkart PRIVATE "-Wl,--wrap=abort" "-Wl,--wrap=_exit")
# libdragon implements thread-local storage with an assembler macro that
# rewrites `rdhwr` (not a VR4300 instruction) -- injected into every file by
# -include ktls.h. With LTO the macro and the code using it must end up in the
# same assembly output, i.e. a single LTO partition.
if(CMAKE_INTERPROCEDURAL_OPTIMIZATION)
    target_link_options(supertuxkart PRIVATE "-flto-partition=one")
endif()
# see n64/compat/src/n64_diag.cpp, "Exception pointers on the o64 ABI"
target_link_options(supertuxkart PRIVATE
    "-Wl,--wrap=_Unwind_Resume" "-Wl,--wrap=__cxa_begin_catch"
    "-Wl,--wrap=__cxa_get_exception_ptr" "-Wl,--wrap=__cxa_call_unexpected")
