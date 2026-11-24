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
foreach(opt STK_N64_SCREEN_LOG STK_N64_CONSOLE STK_N64_LOG_HEAP STK_N64_LOG_DMA STK_N64_PROFILE)
    if(${opt})
        target_compile_definitions(supertuxkart PRIVATE ${opt}=1)
    endif()
endforeach()
foreach(opt STK_N64_LOG_HEAP STK_N64_PROFILE)
    if(${opt})
        set_property(SOURCE ${PROJECT_SOURCE_DIR}/src/main_loop.cpp APPEND
            PROPERTY COMPILE_DEFINITIONS ${opt}=1)
    endif()
endforeach()
target_link_libraries(supertuxkart stk_n64_compat)
# The build is -Oz (code shares the 8 MB with everything else), but at -Oz
# GCC calls even btVector3::operator+= out of line. Physics is most of a
# race's CPU time: Bullet and STK's physics code get -O2.
option(STK_N64_FAST_PHYSICS "Compile Bullet and src/physics with -O2" ON)
if(STK_N64_FAST_PHYSICS)
    foreach(lib bulletmath bulletcollision bulletdynamics)
        target_compile_options(${lib} PRIVATE -O2)
    endforeach()
    file(GLOB STK_N64_PHYSICS_SOURCES ${PROJECT_SOURCE_DIR}/src/physics/*.cpp)
    set_property(SOURCE ${STK_N64_PHYSICS_SOURCES} APPEND PROPERTY
        COMPILE_OPTIONS -O2)
endif()
target_link_options(supertuxkart PRIVATE
    "-Wl,-Map=${CMAKE_BINARY_DIR}/supertuxkart.map")
target_link_options(supertuxkart PRIVATE "-Wl,--wrap=gettimeofday")
target_link_options(supertuxkart PRIVATE "-Wl,--wrap=abort" "-Wl,--wrap=_exit")
target_link_options(supertuxkart PRIVATE "-Wl,--wrap=stat")   # romfs_index.c
if(STK_N64_LOG_DMA)
    target_link_options(supertuxkart PRIVATE "-Wl,--wrap=dma_read")
endif()
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
