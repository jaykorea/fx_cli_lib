# DEVELOPER — 내부 구조 요약

## 구성
- `FxCli` (public): 명령 문자열 생성/전송, 응답 태그 검증, 고수준 API
- `UdpSocket` (internal): UDP 소켓, **RX 스레드**, **링버퍼 큐**, 조건변수 대기
- 파이썬 바인딩: `pybind11`로 `FxCli`를 그대로 노출 + 일부 응답 파싱

## 수신 파이프라인
1. RX 스레드가 `recv()` 블로킹 루프에서 패킷 수신
2. 링버퍼(`std::deque`)에 push (가득 차면 **가장 오래된 것부터 drop**)
3. 조건변수 notify

## 명령 송신 & 태그 대기
- `send_cmd(...)` → UDP `send()`
- `send_cmd_wait_ok_tag(cmd, expect_tag, timeout_ms)`
  1) **flush_queue()**(직전 패킷 제거)
  2) 송신
  3) 큐를 **뒤에서 앞으로** 스캔하며 `OK <TAG>` 일치 검사
  4) 발견 시: 해당 이전 패킷들은 삭제하고 반환
- `req()/status()`는 실시간 특성상 **짧은 타임아웃(기본 2 ms)**

## 기본 타임아웃
- 일반 명령: 200 ms
- 실시간 요청(`req`, `status`): 2 ms
> 필요 시 `fx_client.h` 상수 수정 후 재빌드

## 파이썬 파싱
- 응답 문자열을 세미콜론 단위로 분해 → `dict`로 변환
- `operation_control(groups)`는 딕셔너리 리스트를 받아 내부에서 배열로 분해 후 전송

## 확장 가이드(새 AT 명령 추가)
1. C++: `FxCli::your_cmd(...)` 구현 → 명령 문자열 구성
2. ACK 필요한 경우: `send_cmd_wait_ok_tag(cmd, "TAG", timeout)` 사용
3. Python: `pybind_module.cpp`에 `.def("your_cmd", ...)` 추가(필요 시 파싱 함수 재사용)
4. 예제 코드 업데이트

## 디버그
- `DEBUG` 빌드에서 내부 로그/타이밍 출력
- `utils/elapsed_timer`로 평균/표준편차 통계 출력

## 스레드 주의사항
- `FxCli` 공개 API는 **단일 스레드 호출** 권장(멀티스레드 사용 시 외부 동기화 필요)
