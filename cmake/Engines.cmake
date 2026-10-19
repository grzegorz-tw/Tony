# Optional: build the three inference engines from the exact source versions Tony was developed against.
# This needs the Vulkan SDK (for TONY_GPU=vulkan) or Intel oneAPI (for sycl) and takes a while. Most people should run
# scripts/setup.ps1 instead, which downloads the official prebuilt Windows binaries.
include(ExternalProject)

set(STAGE ${CMAKE_BINARY_DIR}/engines)

if(TONY_GPU STREQUAL "vulkan")
  set(_gpu_llama -DGGML_VULKAN=ON)
  set(_gpu_whisper -DGGML_VULKAN=ON)
  set(_gpu_sd -DSD_VULKAN=ON)
elseif(TONY_GPU STREQUAL "sycl")
  set(_gpu_llama -DGGML_SYCL=ON -DCMAKE_C_COMPILER=icx -DCMAKE_CXX_COMPILER=icpx)
  set(_gpu_whisper -DGGML_SYCL=ON -DCMAKE_C_COMPILER=icx -DCMAKE_CXX_COMPILER=icpx)
  set(_gpu_sd -DSD_SYCL=ON -DCMAKE_C_COMPILER=icx -DCMAKE_CXX_COMPILER=icpx)
else()
  set(_gpu_llama "")
  set(_gpu_whisper "")
  set(_gpu_sd "")
endif()

function(tony_engine name repo tag exes dst)
  ExternalProject_Add(tony_${name}
    GIT_REPOSITORY ${repo}
    GIT_TAG ${tag}
    GIT_SHALLOW TRUE
    GIT_SUBMODULES_RECURSE TRUE
    PREFIX ${CMAKE_BINARY_DIR}/_engines/${name}
    CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF ${ARGN}
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config Release --parallel
    INSTALL_COMMAND ${CMAKE_COMMAND} -DSRC=<BINARY_DIR>/bin "-DEXES=${exes}" -DDST=${STAGE}/${dst} -P ${CMAKE_SOURCE_DIR}/cmake/CopyEngine.cmake
    USES_TERMINAL_BUILD TRUE)
endfunction()

tony_engine(llama https://github.com/ggml-org/llama.cpp.git b11690 "llama-server.exe" llama
  -DLLAMA_BUILD_SERVER=ON -DLLAMA_BUILD_TOOLS=ON -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_APP=OFF
  -DLLAMA_BUILD_UI=OFF -DLLAMA_USE_PREBUILT_UI=OFF -DLLAMA_OPENSSL=OFF -DLLAMA_SUBPROCESS=ON ${_gpu_llama})

tony_engine(whisper https://github.com/ggml-org/whisper.cpp.git v1.9.5 "whisper-server.exe" whisper
  -DWHISPER_BUILD_EXAMPLES=ON -DWHISPER_BUILD_SERVER=ON -DWHISPER_BUILD_TESTS=OFF -DWHISPER_SDL2=OFF ${_gpu_whisper})

tony_engine(sd https://github.com/leejet/stable-diffusion.cpp.git master-ff4fdbb "sd.exe" sd
  -DSD_BUILD_EXAMPLES=ON ${_gpu_sd})

add_custom_target(tony_engines DEPENDS tony_llama tony_whisper tony_sd)
