include_directories("${SDL2_INCLUDEDIR}")
include_directories("${PROJECT_SOURCE_DIR}/lib/mojoal")
if(N64)
    # OpenAL on libdragon's mixer, streaming .wav64 files from the cartridge
    # (n64/compat/src/al_n64.c explains why not MojoAL there)
    add_library(mojoal STATIC ${PROJECT_SOURCE_DIR}/n64/compat/src/al_n64.c)
else()
add_library(mojoal STATIC
    mojoal.c
)
endif()
