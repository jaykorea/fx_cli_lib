# API — 공개 함수 명세 및 사용

---

## C++ API (`FxCli`)

### 생성자
```cpp
FxCli(const std::string& ip, uint16_t port);
```

### 제어 명령
```cpp
bool motor_start(const std::vector<uint8_t>& ids);
bool motor_stop (const std::vector<uint8_t>& ids);
bool motor_estop(const std::vector<uint8_t>& ids);
bool motor_setzero(const std::vector<uint8_t>& ids);
```
- `ids`: 제어 대상 모터 ID 목록
  - 브로드캐스트: `0xFF` 사용
- 반환: 기대 `<TAG>` ACK 수신 시 `true`

### MIT 제어
```cpp
void operation_control(const std::vector<uint8_t>& ids,
                       const std::vector<float>& pos,
                       const std::vector<float>& vel,
                       const std::vector<float>& kp,
                       const std::vector<float>& kd,
                       const std::vector<float>& tau);
```
- 각 배열의 길이는 `ids.size()`와 같아야 함

### 데이터 요청/상태
```cpp
std::string req(const std::vector<uint8_t>& ids); // 최신 <REQ> 패킷
std::string status();                              // <STATUS> 패킷
```
- 반환: MCU 응답 **원문 문자열**
- 내부 기본 대기시간: 일반 명령 200 ms, 실시간 2 ms

### 기타
```cpp
std::string mcu_ping();
std::string mcu_whoami();
void flush();  // 수신 큐 즉시 비우기
```

---

## Python API (`fx_cli.FxCli`)

### 생성자
```python
FxCli(ip: str, port: int)
```

### 제어 명령
```python
motor_start(ids: list[int]) -> bool
motor_stop(ids: list[int])  -> bool
motor_estop(ids: list[int]) -> bool
motor_setzero(ids: list[int]) -> bool
```
- `ids`: 모터 ID 목록(0–255)
- 브로드캐스트: 빈 리스트 `[]`(권장) 또는 `[0xFF]`(펌웨어 지원 시)

### MIT 제어
```python
operation_control(groups: list[dict]) -> None
# 예시: [{"id":1,"pos":0.0,"vel":0.0,"kp":0.0,"kd":0.1,"tau":0.0}, ...]
```

### 데이터 요청/상태
```python
req(ids: list[int]) -> dict
status() -> dict
```
- 반환: MCU 응답을 dict 형태로 반환
- 단, 수신 대기 큐에 패킷이 없을 경우 빈 dict ({}) 반환

### 기타
```python
mcu_ping()   -> dict
mcu_whoami() -> dict
```

---

## 예시

### C++
```cpp
FxCli cli("192.168.10.10", 5101);
std::vector<uint8_t> ids = {1,2};
cli.motor_start(ids);
auto s = cli.req(ids);
cli.motor_stop(ids);
```

### Python
```python
cli = fx_cli.FxCli("192.168.10.10", 5101)
ids = [1,2]
cli.motor_start(ids)
obs = cli.req(ids)
cli.motor_stop(ids)
```
