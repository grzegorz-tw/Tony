cmake_minimum_required(VERSION 3.24)
project(Tony VERSION 1.0.0 LANGUAGES C CXX)

# ---------------------------------------------------------------- options
option(TONY_BUILD_APP     "Build tony.exe (Windows only)"                                  ON)
option(TONY_BUILD_TESTS   "Build the unit/integration tests (portable, run on any OS)"     ON)
option(TONY_BUILD_ENGINES "Also build llama.cpp, whisper.cpp and stable-diffusion.cpp from source instead of using scripts/setup.ps1 (advanced)" OFF)
option(TONY_SYSTEM_CURL   "Use the system libcurl instead of building curl (Linux/macOS dev builds)" OFF)
set(TONY_GPU "vulkan" CACHE STRING "GPU backend when building engines from source: vulkan | sycl | cpu")
set_property(CACHE TONY_GPU PROPERTY STRINGS vulkan sycl cpu)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include(cmake/Deps.cmake)

# ---------------------------------------------------------------- portable libraries (no Windows headers)
function(tony_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W3 /utf-8 /permissive- /Zc:__cplusplus /EHsc)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter)
  endif()
endfunction()

add_library(tony_core STATIC
  src/core/strutil.cpp src/core/pathguard.cpp src/core/cmdrisk.cpp src/core/settings.cpp
  src/core/agent.cpp src/core/chatstore.cpp)
target_include_directories(tony_core PUBLIC src ${nlohmann_json_SOURCE_DIR}/single_include)
tony_warnings(tony_core)

add_library(tony_net STATIC src/net/http.cpp src/net/netpolicy.cpp)
target_link_libraries(tony_net PUBLIC tony_core CURL::libcurl)
tony_warnings(tony_net)

add_library(tony_logic STATIC
  src/engines/openai_backend.cpp src/engines/llama_preset.cpp src/engines/image_args.cpp
  src/engines/whisper_util.cpp src/engines/hf.cpp src/tools/web_parse.cpp)
target_link_libraries(tony_logic PUBLIC tony_net)
tony_warnings(tony_logic)

# ---------------------------------------------------------------- tests
if(TONY_BUILD_TESTS)
  enable_testing()
  foreach(t test_core test_agent test_engines)
    add_executable(${t} tests/${t}.cpp)
    target_link_libraries(${t} PRIVATE tony_logic)
    add_test(NAME ${t} COMMAND ${t})
  endforeach()
  add_executable(test_llm_backend tests/test_llm_backend.cpp)
  target_link_libraries(test_llm_backend PRIVATE tony_logic)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    add_test(NAME llm_backend_stream COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_with_mock.py $<TARGET_FILE:test_llm_backend>)
  endif()
endif()

# ---------------------------------------------------------------- the Windows application
if(WIN32 AND TONY_BUILD_APP)
  set(IMGUI_SRC
    ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp)

  add_executable(tony WIN32
    src/platform/winutil.cpp src/platform/process.cpp src/platform/audio.cpp src/platform/screen.cpp src/platform/dialogs.cpp src/platform/stb_impl.cpp
    src/engines/llama_service.cpp src/engines/whisper_service.cpp src/engines/image_service.cpp
    src/tools/tools_util.cpp src/tools/tools_files.cpp src/tools/tools_shell.cpp src/tools/tools_system.cpp src/tools/tools_web.cpp src/tools/tools_media.cpp
    src/ui/theme.cpp src/ui/app.cpp src/ui/ui_chat.cpp src/ui/ui_settings.cpp src/ui/main.cpp
    ${IMGUI_SRC} resources/app.rc)
  target_include_directories(tony PRIVATE src ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${miniaudio_SOURCE_DIR} ${stb_SOURCE_DIR})
  target_compile_definitions(tony PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
  target_link_libraries(tony PRIVATE tony_logic
    d3d11 dxgi d3dcompiler dwmapi shcore comdlg32 shell32 ole32 oleaut32 uuid user32 gdi32 advapi32 crypt32 bcrypt iphlpapi ws2_32 winmm)
  if(MSVC)
    target_compile_options(tony PRIVATE /W3 /utf-8 /permissive- /Zc:__cplusplus /EHsc /MP)
    target_link_options(tony PRIVATE /MANIFEST:NO)   # the manifest lives in resources/app.rc
  else()
    target_compile_options(tony PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_options(tony PRIVATE -static-libgcc -static-libstdc++ -static -mwindows)
  endif()
  set_source_files_properties(src/platform/stb_impl.cpp PROPERTIES COMPILE_OPTIONS "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w>")
  set_target_properties(tony PROPERTIES OUTPUT_NAME "tony")

  if(TONY_BUILD_ENGINES)
    include(cmake/Engines.cmake)
    add_dependencies(tony tony_engines)
  endif()

  install(TARGETS tony RUNTIME DESTINATION .)
  install(DIRECTORY scripts/ DESTINATION scripts)
  install(FILES README.md LICENSE DESTINATION .)
  if(TONY_BUILD_ENGINES)
    install(DIRECTORY ${CMAKE_BINARY_DIR}/engines/ DESTINATION engines)
  endif()
endif()
