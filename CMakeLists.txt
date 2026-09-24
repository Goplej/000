# ============================================================================
#  AI Agent Studio (C++) — сборка проекта
#
#  Windows (MSVC):   cmake -S . -B build -G "Visual Studio 17 2022" -A x64
#                    cmake --build build --config Release -j
#  Windows (MinGW):  cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
#                    cmake --build build -j
#  Linux/macOS:      cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
#                    cmake --build build -j && ctest --test-dir build --output-on-failure
#
#  Зависимостей нет: только стандартная библиотека C++20 и системные API.
# ============================================================================
cmake_minimum_required(VERSION 3.20)

project(aiagent_studio_cpp
    VERSION 1.0.0
    DESCRIPTION "Полностью автономный ИИ-агент на C++: свой инференс-движок, планировщик, инструменты"
    LANGUAGES CXX)

# --- Общие настройки ---------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Тип сборки" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# --- Опции -------------------------------------------------------------------
option(AIA_BUILD_TESTS      "Собирать тесты"                       ON)
option(AIA_NATIVE_ARCH      "Оптимизировать под текущий процессор" OFF)
option(AIA_ENABLE_AVX2      "Разрешить AVX2-ядра (x86-64)"         ON)
option(AIA_WITH_ASAN        "Сборка с AddressSanitizer"            OFF)
option(AIA_WITH_UBSAN       "Сборка с UndefinedBehaviorSanitizer"  OFF)
option(AIA_STATIC_RUNTIME   "MSVC: статическая линковка CRT"       OFF)
option(AIA_WITH_CUDA        "Опциональный CUDA-бэкенд"             OFF)
option(AIA_WITH_VULKAN      "Опциональный Vulkan-бэкенд"           OFF)

# --- Версия и платформа ------------------------------------------------------
# Версия попадает в код через define, чтобы `aia version` не расходился со сборкой.
add_compile_definitions(
    AIA_VERSION="${PROJECT_VERSION}"
    AIA_PRODUCT="AI Agent Studio"
)

if(WIN32)
    add_compile_definitions(
        AIA_PLATFORM_WINDOWS=1
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )
    set(AIA_PLATFORM_NAME "windows")
elseif(APPLE)
    add_compile_definitions(AIA_PLATFORM_MACOS=1)
    set(AIA_PLATFORM_NAME "macos")
else()
    add_compile_definitions(AIA_PLATFORM_LINUX=1)
    set(AIA_PLATFORM_NAME "linux")
endif()

if(AIA_ENABLE_AVX2 AND (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64"))
    add_compile_definitions(AIA_HAVE_AVX2=1)
endif()

# --- Предупреждения ----------------------------------------------------------
function(aia_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /EHsc /MP
            /wd4127   # «условное выражение — константа» (норма для if constexpr-обвязок)
        )
        target_compile_options(${target} PRIVATE /bigobj)
        if(AIA_STATIC_RUNTIME)
            set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast
            -Wcast-align -Wunused -Woverloaded-virtual -Wnull-dereference
            -Wdouble-promotion -Wformat=2 -Wno-format-nonliteral
        )
    endif()
    if(AIA_NATIVE_ARCH)
        if(MSVC)
            target_compile_options(${target} PRIVATE /arch:AVX2)
        else()
            target_compile_options(${target} PRIVATE -march=native)
        endif()
    endif()
    if(AIA_WITH_ASAN AND NOT MSVC)
        target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address)
    endif()
    if(AIA_WITH_UBSAN AND NOT MSVC)
        target_compile_options(${target} PRIVATE -fsanitize=undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=undefined)
    endif()
endfunction()

# --- Ядро ---------------------------------------------------------------------
# aia_core: строки, JSON, файлы, логи, математика, потоки — от него зависят все.
add_library(aia_core STATIC
    # Текстовое ядро: строки, Unicode, кодировки, хеши, сравнение, рендер, diff.
    # Один крупный файл-подсистема (см. docs/ARCHITECTURE.md): разделы внутри
    # переиспользуют друг друга, а разделение на части дало бы больше «шапок».
    src/core/strings.cpp
)

target_include_directories(aia_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
aia_set_warnings(aia_core)

if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(aia_core PUBLIC Threads::Threads)
endif()

# --- Исполняемый файл ---------------------------------------------------------
add_executable(aia
    src/cli/main.cpp
)
target_link_libraries(aia PRIVATE aia_core)
aia_set_warnings(aia)

# --- Тесты --------------------------------------------------------------------
if(AIA_BUILD_TESTS)
    enable_testing()
    add_executable(aia_tests
        tests/test_strings.cpp
    )
    target_link_libraries(aia_tests PRIVATE aia_core)
    aia_set_warnings(aia_tests)

    add_test(NAME strings COMMAND aia_tests)
    set_tests_properties(strings PROPERTIES TIMEOUT 120)

    add_test(NAME cli_smoke COMMAND aia version)
    set_tests_properties(cli_smoke PROPERTIES TIMEOUT 30)

    # Самопроверка ядра — быстрый способ убедиться, что сборка корректна
    # (особенно полезно на Windows: `ctest` сразу покажет, всё ли на месте).
    add_test(NAME core_selftest COMMAND aia selftest)
    set_tests_properties(core_selftest PROPERTIES TIMEOUT 60)
endif()

# --- Установка ----------------------------------------------------------------
install(TARGETS aia RUNTIME DESTINATION bin)

message(STATUS "AI Agent Studio ${PROJECT_VERSION} | платформа: ${AIA_PLATFORM_NAME} | тип: ${CMAKE_BUILD_TYPE}")
