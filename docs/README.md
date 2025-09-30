# fx_cli — fx client library

- `fx_cli`는 Fx Controller UDP 기반 통신 **C++/Python 클라이언트** 라이브러리 입니다. 내부적으로 **수신 전용 스레드 + 링버퍼**로 응답 지연 편차를 줄입니다. 간단히 쓰고, 빠르게 동작하도록 설계되었습니다.
- ```
========================================
📈 테스트 결과 요약
========================================
  - 총 실행 시간: 3786.49 초
  - 총 요청 횟수 (수신 시도): 180000 회
  - 놓친 패킷 총계: 28 개
  - 패킷 손실률: 0.0156 %
========================================
```
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
