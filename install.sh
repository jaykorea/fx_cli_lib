#!/bin/bash
set -e

# 프로젝트 루트로 이동 (이 스크립트가 있는 위치 기준)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# 빌드 디렉토리 생성 및 이동
mkdir -p build
cd build

# CMake 설정 (Python3 경로 자동 지정)
cmake .. -DCMAKE_BUILD_TYPE=DEBUG -DPython3_EXECUTABLE=$(which python3)

# 병렬 빌드
cmake --build . -j$(nproc) -- VERBOSE=0

# 설치 (루트 권한 필요)
sudo cmake --install .

