cmake_minimum_required(VERSION 3.17)
project(WhisperGUI LANGUAGES C CXX)

# Windows-only project
if(NOT WIN32)
    message(FATAL_ERROR "This project only supports Windows.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CUDA Support
option(WHISPERGUI_ENABLE_CUDA "Enable CUDA for whisper.cpp" ON)

if(WHISPERGUI_ENABLE_CUDA)
    # Check if CUDA toolkit is available
    find_package(CUDAToolkit QUIET)
    if(CUDAToolkit_FOUND)
        message(STATUS "CUDA Toolkit found: ${CUDAToolkit_VERSION}")
        message(STATUS "CUDA enabled. Enabling GGML CUDA for whisper.cpp")
        enable_language(CUDA)
        set(CMAKE_CUDA_STANDARD 17)
        set(CMAKE_CUDA_STANDARD_REQUIRED ON)
        
        # Set CUDA architectures compatible with modern GPUs (Turing and newer)
        set(CMAKE_CUDA_ARCHITECTURES "75;80;86;89;90" CACHE STRING "CUDA architectures" FORCE)
        set(GGML_CUDA_ARCHITECTURES "75;80;86;89;90" CACHE STRING "GGML CUDA architectures" FORCE)
        
        # Allow unsupported compiler for newer Visual Studio versions with CUDA
        # These flags help with CUDA 13.0 + VS2022 compatibility
        set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -allow-unsupported-compiler --expt-relaxed-constexpr" CACHE STRING "" FORCE)
        
        # Build CUDA as static library to avoid atexit linking issues with DLLs
        set(GGML_CUDA ON CACHE BOOL "Enable GGML CUDA" FORCE)
        set(GGML_STATIC ON CACHE BOOL "Build ggml as static library" FORCE)
        set(WHISPERGUI_CUDA_FOUND TRUE)
    else()
        message(WARNING "CUDA requested but CUDAToolkit not found. Building without CUDA.")
        set(GGML_CUDA OFF CACHE BOOL "Enable GGML CUDA" FORCE)
        set(WHISPERGUI_CUDA_FOUND FALSE)
    endif()
else()
    message(STATUS "CUDA disabled. whisper.cpp will run on CPU.")
    set(GGML_CUDA OFF CACHE BOOL "Enable GGML CUDA" FORCE)
    set(WHISPERGUI_CUDA_FOUND FALSE)
endif()

include(FetchContent)

# Whisper.cpp - Use latest stable version for CUDA compatibility
FetchContent_Declare(
    whisper
    GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp.git
    GIT_TAG master
)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(whisper)

# SDL2
FetchContent_Declare(
    SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-2.30.0
)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL2)

# ImGui
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG docking
)
FetchContent_MakeAvailable(imgui)

# nlohmann_json
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)

# sherpa-onnx - Speaker diarization (optional, 10k+ stars, production-grade)
# https://github.com/k2-fsa/sherpa-onnx
# Uses pre-built Windows binaries (auto-downloaded or manually specified)
option(WHISPERGUI_USE_SHERPA_ONNX "Use sherpa-onnx for speaker diarization" OFF)

# sherpa-onnx version for pre-built binaries
set(SHERPA_ONNX_VERSION "1.10.40" CACHE STRING "sherpa-onnx version to download")

if(WHISPERGUI_USE_SHERPA_ONNX)
# Force x64 architecture for Windows (this project doesn't support x86)
set(SHERPA_ONNX_ARCH "x64")
    
# Check if user provided pre-built library path
if(DEFINED SHERPA_ONNX_DIR AND EXISTS "${SHERPA_ONNX_DIR}")
    message(STATUS "Using user-provided sherpa-onnx from: ${SHERPA_ONNX_DIR}")
    set(SHERPA_ONNX_ROOT "${SHERPA_ONNX_DIR}")
else()
    # Auto-download pre-built Windows binaries (shared/DLL version)
    set(SHERPA_ONNX_ARCHIVE_NAME "sherpa-onnx-v${SHERPA_ONNX_VERSION}-win-${SHERPA_ONNX_ARCH}-shared")
    set(SHERPA_ONNX_DOWNLOAD_URL "https://github.com/k2-fsa/sherpa-onnx/releases/download/v${SHERPA_ONNX_VERSION}/${SHERPA_ONNX_ARCHIVE_NAME}.tar.bz2")
    set(SHERPA_ONNX_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/_deps/sherpa-onnx-prebuilt")
    set(SHERPA_ONNX_ARCHIVE_PATH "${SHERPA_ONNX_DOWNLOAD_DIR}/${SHERPA_ONNX_ARCHIVE_NAME}.tar.bz2")
    set(SHERPA_ONNX_ROOT "${SHERPA_ONNX_DOWNLOAD_DIR}/${SHERPA_ONNX_ARCHIVE_NAME}")
        
        if(NOT EXISTS "${SHERPA_ONNX_ROOT}/lib/sherpa-onnx-c-api.lib")
            message(STATUS "Downloading pre-built sherpa-onnx v${SHERPA_ONNX_VERSION} for Windows ${SHERPA_ONNX_ARCH}...")
            message(STATUS "  URL: ${SHERPA_ONNX_DOWNLOAD_URL}")
            
            # Create download directory
            file(MAKE_DIRECTORY "${SHERPA_ONNX_DOWNLOAD_DIR}")
            
            # Download the archive
            if(NOT EXISTS "${SHERPA_ONNX_ARCHIVE_PATH}")
                file(DOWNLOAD
                    "${SHERPA_ONNX_DOWNLOAD_URL}"
                    "${SHERPA_ONNX_ARCHIVE_PATH}"
                    STATUS DOWNLOAD_STATUS
                    SHOW_PROGRESS
                )
                list(GET DOWNLOAD_STATUS 0 DOWNLOAD_RESULT)
                if(NOT DOWNLOAD_RESULT EQUAL 0)
                    list(GET DOWNLOAD_STATUS 1 DOWNLOAD_ERROR)
                    message(FATAL_ERROR "Failed to download sherpa-onnx: ${DOWNLOAD_ERROR}\n"
                        "Please download manually from: ${SHERPA_ONNX_DOWNLOAD_URL}\n"
                        "Extract to: ${SHERPA_ONNX_ROOT}\n"
                        "Or set SHERPA_ONNX_DIR to your extracted location.")
                endif()
            endif()
            
            # Extract the archive
            message(STATUS "Extracting sherpa-onnx...")
            execute_process(
                COMMAND ${CMAKE_COMMAND} -E tar xjf "${SHERPA_ONNX_ARCHIVE_PATH}"
                WORKING_DIRECTORY "${SHERPA_ONNX_DOWNLOAD_DIR}"
                RESULT_VARIABLE EXTRACT_RESULT
            )
            if(NOT EXTRACT_RESULT EQUAL 0)
                message(FATAL_ERROR "Failed to extract sherpa-onnx archive. Please extract manually.")
            endif()
        endif()
    endif()
    
    # Verify the pre-built library exists
    if(NOT EXISTS "${SHERPA_ONNX_ROOT}/lib/sherpa-onnx-c-api.lib")
        message(FATAL_ERROR "sherpa-onnx library not found at ${SHERPA_ONNX_ROOT}/lib/sherpa-onnx-c-api.lib\n"
            "Please ensure the pre-built library is correctly extracted.")
    endif()
    
    # Set up include and library paths
    set(SHERPA_ONNX_INCLUDE_DIR "${SHERPA_ONNX_ROOT}/include")
    set(SHERPA_ONNX_LIB_DIR "${SHERPA_ONNX_ROOT}/lib")
    set(SHERPA_ONNX_BIN_DIR "${SHERPA_ONNX_ROOT}/bin")
    
    # Find the import library
    find_library(SHERPA_ONNX_C_API_LIB
        NAMES sherpa-onnx-c-api
        PATHS "${SHERPA_ONNX_LIB_DIR}"
        NO_DEFAULT_PATH
        REQUIRED
    )
    
    # Find all DLLs that need to be copied (check both bin and lib directories)
    file(GLOB SHERPA_ONNX_DLLS_BIN "${SHERPA_ONNX_BIN_DIR}/*.dll")
    file(GLOB SHERPA_ONNX_DLLS_LIB "${SHERPA_ONNX_LIB_DIR}/*.dll")
    list(APPEND SHERPA_ONNX_DLLS ${SHERPA_ONNX_DLLS_BIN} ${SHERPA_ONNX_DLLS_LIB})
    list(REMOVE_DUPLICATES SHERPA_ONNX_DLLS)
    
    message(STATUS "sherpa-onnx configured successfully")
    message(STATUS "  Include: ${SHERPA_ONNX_INCLUDE_DIR}")
    message(STATUS "  Library: ${SHERPA_ONNX_C_API_LIB}")
    message(STATUS "  DLLs: ${SHERPA_ONNX_DLLS}")
    
    set(WHISPERGUI_HAS_SHERPA_ONNX TRUE)
else()
    message(STATUS "sherpa-onnx disabled. Using basic speaker detection fallback.")
    message(STATUS "To enable: -DWHISPERGUI_USE_SHERPA_ONNX=ON")
    set(WHISPERGUI_HAS_SHERPA_ONNX FALSE)
endif()

find_package(Threads REQUIRED)

# ImGui Source files
set(IMGUI_DIR ${imgui_SOURCE_DIR})
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp
)

# Windows executable (WIN32 to hide console)
# Include Windows resource file for embedded icon
set(WIN_RESOURCES "")
if(WIN32 AND EXISTS "${CMAKE_SOURCE_DIR}/resources/app.rc")
    set(WIN_RESOURCES "${CMAKE_SOURCE_DIR}/resources/app.rc")
    # Set resource compiler include path so it can find app.ico relative to app.rc
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} /I\"${CMAKE_SOURCE_DIR}/resources\"")
endif()

add_executable(WhisperGUI WIN32
    src/main.cpp
    src/AudioRecorder.cpp
    src/WhisperEngine.cpp
    src/SpeakerDiarizer.cpp
    src/ModelManager.cpp
    src/InputManager.cpp
    src/Gui.cpp
    ${IMGUI_SOURCES}
    ${WIN_RESOURCES}
)

target_include_directories(WhisperGUI PRIVATE
    src
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${SDL2_SOURCE_DIR}/include
    ${SDL2_BINARY_DIR}/include
    ${SDL2_BINARY_DIR}/include-config-$<LOWER_CASE:$<CONFIG>>
    ${whisper_SOURCE_DIR}/include
)

target_link_libraries(WhisperGUI PRIVATE
    whisper
    SDL2::SDL2
    SDL2::SDL2main
    nlohmann_json::nlohmann_json
    Threads::Threads
    winhttp
    advapi32
    crypt32
    ws2_32
)

# Add sherpa-onnx if enabled
if(WHISPERGUI_HAS_SHERPA_ONNX)
    target_include_directories(WhisperGUI PRIVATE ${SHERPA_ONNX_INCLUDE_DIR})
    target_link_libraries(WhisperGUI PRIVATE ${SHERPA_ONNX_C_API_LIB})
    target_compile_definitions(WhisperGUI PRIVATE WHISPERGUI_HAS_SHERPA_ONNX=1)
else()
    target_compile_definitions(WhisperGUI PRIVATE WHISPERGUI_HAS_SHERPA_ONNX=0)
endif()

# Pass CUDA status to the code via compile definition
if(WHISPERGUI_CUDA_FOUND AND GGML_CUDA)
    target_compile_definitions(WhisperGUI PRIVATE WHISPERGUI_CUDA_ENABLED=1)
else()
    target_compile_definitions(WhisperGUI PRIVATE WHISPERGUI_CUDA_ENABLED=0)
endif()

# Copy whisper.dll
add_custom_command(TARGET WhisperGUI POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:whisper>
        $<TARGET_FILE_DIR:WhisperGUI>
)

# Copy all ggml DLLs - ggml-base, ggml-cpu, ggml, and ggml-cuda if available
if(TARGET ggml-base)
    add_custom_command(TARGET WhisperGUI POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:ggml-base>
            $<TARGET_FILE_DIR:WhisperGUI>
    )
endif()

if(TARGET ggml-cpu)
    add_custom_command(TARGET WhisperGUI POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:ggml-cpu>
            $<TARGET_FILE_DIR:WhisperGUI>
    )
endif()

if(TARGET ggml)
    add_custom_command(TARGET WhisperGUI POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:ggml>
            $<TARGET_FILE_DIR:WhisperGUI>
    )
endif()

if(TARGET ggml-cuda)
    add_custom_command(TARGET WhisperGUI POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:ggml-cuda>
            $<TARGET_FILE_DIR:WhisperGUI>
    )
endif()

# Copy SDL2.dll
add_custom_command(TARGET WhisperGUI POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:SDL2::SDL2>
        $<TARGET_FILE_DIR:WhisperGUI>
)

# Copy sherpa-onnx DLLs if enabled (from pre-built package)
if(WHISPERGUI_HAS_SHERPA_ONNX)
    # Copy all sherpa-onnx DLLs (includes onnxruntime, sherpa-onnx-c-api, etc.)
    foreach(DLL_FILE ${SHERPA_ONNX_DLLS})
        add_custom_command(TARGET WhisperGUI POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DLL_FILE}"
                $<TARGET_FILE_DIR:WhisperGUI>
        )
    endforeach()
endif()

# Copy resources folder (contains app icon)
if(EXISTS "${CMAKE_SOURCE_DIR}/resources")
    add_custom_command(TARGET WhisperGUI POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/resources"
            "$<TARGET_FILE_DIR:WhisperGUI>/resources"
    )
endif()
