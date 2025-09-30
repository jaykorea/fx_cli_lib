# Install — fx_cli 설치 가이드

## 요구 사항
- **OS:** Linux (POSIX 소켓)
- **툴체인:** CMake ≥ 3.15, C++17 컴파일러
- **Python(선택):** Python 3.10, `pybind11` (Python 바인딩 빌드 시)

## 의존성 설치(예: Ubuntu)
```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake python3-dev pybind11-dev
```
## 간편설치 (스크립트)
```bash
chmod +x install.sh
./install.sh   # 빌드 후 설치까지
```

## 빌드
```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DPython3_EXECUTABLE=$(which python3)
cmake --build . -- -j$(nproc)
```

## 설치(선택)
```bash
sudo cmake --install .
```
- C++ 헤더: `/usr/local/include/fx_cli/`
- 라이브러리: `/usr/local/lib/`
- Python 모듈: `site-packages/fx_cli/`
