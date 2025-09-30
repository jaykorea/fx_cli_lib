cmake_minimum_required(VERSION 3.15)
project(fx_cli LANGUAGES CXX)

# pybind11 찾기 (사전 설치 필요)
include(FetchContent)
find_package(pybind11 CONFIG QUIET)
if(NOT pybind11_FOUND)
  message(FATAL_ERROR "pybind11를 찾을 수 없습니다. 설치 후 다시 시도하세요.\n"
                      "  - pip3 install pybind11\n"
                      "  - 또는: sudo apt-get install pybind11-dev")
endif()

# Python (빌드 및 설치 경로 계산에 필요)
find_package(Python3 3.10 COMPONENTS Interpreter Development.Module REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# -----------------------------------------------------------------------------
# C++ library
add_library(fx_cli_cpp STATIC
  fx_client.cpp
  utils/elapsed_timer.cpp
)

target_include_directories(fx_cli_cpp PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/utils>
)

# -----------------------------------------------------------------------------
# Python module (pybind11)
# -> 모듈 이름을 fx_cli 로 통일
pybind11_add_module(fx_cli
  pybind_module.cpp
)

target_link_libraries(fx_cli PRIVATE fx_cli_cpp)

# -----------------------------------------------------------------------------
# Install
install(TARGETS fx_cli_cpp
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
)

# site-packages 경로 계산
# Conda 환경 변수가 있는지 확인
if(DEFINED ENV{CONDA_PREFIX})
  # CONDA_PREFIX를 Python 검색 경로의 최우선 순위로 지정
  set(Python3_ROOT_DIR $ENV{CONDA_PREFIX})
  message(STATUS "Conda environment detected. Using Python from: ${Python3_ROOT_DIR}")
endif()

# 기존의 Python 검색 및 사용 코드
find_package(Python3 COMPONENTS Interpreter REQUIRED)

if(Python3_FOUND)
  message(STATUS "Found Python interpreter: ${Python3_EXECUTABLE}")
  
  execute_process(
    COMMAND ${Python3_EXECUTABLE} -c "import sysconfig; print(sysconfig.get_paths()['platlib'])"
    OUTPUT_VARIABLE PY_SITE_PACKAGES
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  
  message(STATUS "Python site-packages found at: ${PY_SITE_PACKAGES}")
endif()

# 파이썬 확장 모듈을 fx_cli 패키지 디렉터리에 설치
install(TARGETS fx_cli
  LIBRARY DESTINATION ${PY_SITE_PACKAGES}/fx_cli
)

# 패키지 초기화 파일 설치 (바인딩 re-export 등)
install(FILES
  __init__.py
  DESTINATION ${PY_SITE_PACKAGES}/fx_cli
)

# 공개 헤더 설치 (fx_client.h만)
install(FILES
  fx_client.h
  utils/elapsed_timer.h
  DESTINATION include/fx_cli
)

# Debug 빌드에서만 DEBUG 매크로를 켜도록 변경
target_compile_definitions(fx_cli_cpp
    PRIVATE $<$<CONFIG:Debug>:DEBUG>
)