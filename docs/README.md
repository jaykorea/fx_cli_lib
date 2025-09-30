# fx_cli — fx client library

- `fx_cli`는 FX 계열 모터 제어기와 UDP 기반 통신 **C++/Python 클라이언트** 라이브러리 입니다. 내부적으로 **수신 전용 스레드 + 링버퍼**로 응답 지연 편차를 줄입니다. 간단히 쓰고, 빠르게 동작하도록 설계되었습니다.
<img width="1536" height="1024" alt="image" src="https://github.com/user-attachments/assets/26871cb1-bbee-48e7-aff1-75a4214796ae" />

## 빠른 시작

### C++
```cpp
#include "fx_client.h"
#include <vector>
int main() {
  FxCli cli("192.168.10.10", 5101);
  std::vector<uint8_t> ids = {1,2};
  cli.motor_start(ids);
  auto obs = cli.req(ids);   // MCU 응답 원문 문자열
  cli.motor_stop(ids);
}
```

### Python
```python
import fx_cli
cli = fx_cli.FxCli("192.168.10.10", 5101)
ids = [1, 2]
cli.motor_start(ids)
obs = cli.req(ids)   # dict로 파싱된 응답
cli.motor_stop(ids)
```

## 문서
- 설치: [Install.md](Install.md)
- 공개 API 명세/사용: [API.md](API.md)
- 개발자 내부 구조: [DEVELOPER.md](DEVELOPER.md)

## 라이선스
Copyright (c) <year>, <COCELO>
All rights reserved.
 
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
 
* Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.
* Neither the name of the <organization> nor the
names of its contributors may be used to endorse or promote products
derived from this software without specific prior written permission.
 
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL <COCELO> BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
