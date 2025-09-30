# fx_cli — FX 모터 컨트롤 클라이언트

`fx_cli`는 FX 계열 모터 제어기와 UDP 기반 AT-명령 프로토콜로 통신하는 **C++/Python 클라이언트**입니다. 내부적으로 **수신 전용 스레드 + 링버퍼**로 응답 지연 편차를 줄입니다. 간단히 쓰고, 빠르게 동작하도록 설계되었습니다.

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
프로젝트 정책에 맞게 적용하십시오(별도 라이선스 파일 미포함).
