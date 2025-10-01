# API — 공개 함수 명세 및 사용

---

## C++ API (`FxCli`)

### 생성자
```cpp
FxCli(const std::string& ip, uint16_t port);
```

---

### 명령 집합
```cpp
bool motor_start(const std::vector<uint8_t>& ids);
bool motor_stop (const std::vector<uint8_t>& ids);
bool motor_estop(const std::vector<uint8_t>& ids);
bool motor_setzero(const std::vector<uint8_t>& ids);
```
- `ids`: 제어 대상 모터 ID 목록
  - 브로드캐스트: `0xFF` 사용
- 반환: 기대 `<TAG>` ACK 수신 시 `true`

---

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

---

### 데이터 요청/상태
```python
req(ids: list[int]) -> dict
status() -> dict
```
- 반환: MCU 응답을 **dict** 형태로 반환  
- 수신 대기 큐에 패킷이 없을 경우 **빈 dict (`{}`)** 반환  

#### 데이터 프로토콜 (예시)

```json
{
  "OK <REQ>": true,
  "OBS": {
    "M1": {"pos": 5.149729, "vel": 1.217365, "tau": -0.021057},
    "M2": {"pos": -2.150949, "vel": -0.000305, "tau": 0.035706},
    "M3": {"pos": -1.839284, "vel": 0.005188, "tau": -0.032043},
    "M4": {"pos": 1.951056, "vel": 0.047913, "tau": -0.065002}
  },
  "SEQ_NUM": {"cnt": 183510}
}
```

---

#### 필드 정의

| 키          | 타입     | 설명 |
|-------------|----------|------|
| `OK <REQ>` | `bool`   | `<REQ>` 명령에 대한 응답 여부 (`true` = 정상 수신) |
| `OBS`      | `dict`   | 모터별 관측 데이터 집합 |
| `SEQ_NUM`  | `dict`   | 패킷 시퀀스 번호 |

#### `OBS` 내부 구조 (모터별 상태)
| 키   | 타입   | 단위  | 설명      |
|------|--------|-------|-----------|
| `pos` | `float` | rad   | 모터 위치 |
| `vel` | `float` | rad/s | 모터 속도 |
| `tau` | `float` | Nm    | 모터 토크 |

#### `SEQ_NUM` 내부 구조
| 키   | 타입   | 설명                    |
|------|--------|-------------------------|
| `cnt` | `int` | MCU 내부 시퀀스 카운터 (패킷 순서 추적, 유실 여부 확인용) |

### 기타
```python
mcu_ping()   -> dict
mcu_whoami() -> dict
```
- 반환: MCU 응답을 dict 형태로 반환
- 단, 수신 대기 큐에 패킷이 없을 경우 빈 dict ({}) 반환

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
