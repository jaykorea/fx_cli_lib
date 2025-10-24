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
```cpp
std::string req(const std::vector<uint8_t>& ids); // 최신 <REQ> 패킷
std::string status();                              // <STATUS> 패킷
```
- 반환: MCU 응답 **원문 문자열**
- 내부 기본 대기시간: 일반 명령 200 ms, 실시간 2 ms

---

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

### 명령 집합
```python
motor_start(ids: list[int]) -> bool
motor_stop(ids: list[int])  -> bool
motor_estop(ids: list[int]) -> bool
motor_setzero(ids: list[int]) -> bool
```
- `ids`: 모터 ID 목록(0–255)
- 브로드캐스트: `[0xFF]`(펌웨어 지원 시)

---

### MIT 제어
```python
operation_control(groups: list[dict]) -> bool
```
- 예시: [{"id":1,"pos":0.0,"vel":0.0,"kp":0.0,"kd":0.1,"tau":0.0}, ...]

---

### 데이터 요청
```python
req(ids: list[int]) -> dict
```
- 반환: MCU 응답을 **dict** 형태로 반환  
- 수신 대기 큐에 패킷이 없을 경우 **빈 dict (`{}`)** 반환  

#### 데이터 프로토콜 (예시: req 호출 시)

```json
{
  "OK <REQ>": true,
  "M1": {"p": 5.149729, "v": 1.217365, "t": -0.021057},
  "M2": {"p": -2.150949, "v": -0.000305, "t": 0.035706},
  "M3": {"p": -1.839284, "v": 0.005188, "t": -0.032043},
  "M4": {"p": 1.951056, "v": 0.047913, "t": -0.065002},
  "IMU": {"r": -0.35, "p": 0.31, "y": -14.61, "gx": 0.0, "gy": 0.0, "gz": 0.0, "pgx": 0.0, "pgy": 0.0,"pgz": 1.00}
  "SEQ_NUM": {"cnt": 183510}
}
```
#### 필드 정의

| 키          | 타입     | 설명 |
|-------------|----------|------|
| `OK <REQ>` | `bool`   | `<REQ>` 명령에 대한 응답 여부 (`true` = 정상 수신) |
| `Mx` | `dict` |   모터 데이터     |
| `IMU` | `dict`  | 관성센서 (IMU) 데이터 |
| `SEQ_NUM`  | `dict`   | 패킷 시퀀스 번호 |

###### 'Mx' 내부 구조 (M1, M2, ...)
| 키   | 타입   | 단위  | 설명      |
|------|--------|-------|-----------|
| `p` | `float` | rad   | 모터 위치 |
| `v` | `float` | rad/s | 모터 속도 |
| `t` | `float` | Nm    | 모터 토크 |

###### `IMU` 내부 구조
| 키  | 타입   | 단위 | 설명 |
|-----|--------|------|------|
| `r` | `float` | deg | Roll |
| `p` | `float` | deg | Pitch |
| `y` | `float` | deg | Yaw |
| `gx` | `float` | rad/s | 자이로 X축 |
| `gy` | `float` | rad/s | 자이로 Y축 |
| `gz` | `float` | rad/s | 자이로 Z축 |
| `pgx` | `float` | g | 투영 중력 x축 |
| `pgy` | `float` | g | 투영 중력 y축 |
| `pgz` | `float` | g | 투영 중력 Z축 |

##### `SEQ_NUM` 내부 구조
| 키   | 타입   | 설명                    |
|------|--------|-------------------------|
| `cnt` | `int` | MCU 내부 시퀀스 카운터 (패킷 순서 추적, 유실 여부 확인용) |

---

### 데이터 상태 호출
```python
status() -> dict
```
- 반환: MCU 응답을 **dict** 형태로 반환  
- 수신 대기 큐에 패킷이 없을 경우 **빈 dict (`{}`)** 반환

### 데이터 프로토콜 (예시: status)
```json
{
  "OK <STATUS>": true,
  "MCU": {"fw": "1.1.0", "proto": "ATv1", "uptime": 81845},
  "NET": {
    "status": "up",
    "ip": "192.168.10.10",
    "gw": "192.168.10.1",
    "mask": "255.255.255.0"
  },
  "QUEUE": {"udp_tx": 0, "motor_ctrl": 0},
  "CAN1": {
    "Err": "0x00000100", "LEC": 3, "ACT": 24,
    "EP": 0, "BO": 0, "REC": 0, "TEC": 62,
    "TXFE": 7, "RX0": 0, "RX1": 0
  },
  "CAN2": {
    "Err": "0x00000300", "LEC": 0, "ACT": 16,
    "EP": 0, "BO": 0, "REC": 0, "TEC": 7,
    "TXFE": 8, "RX0": 0, "RX1": 0
  },
  "M1": {"pattern": 2, "err": "None"},
  "M2": {"pattern": 2, "err": "None"},
  "M3": {"pattern": 2, "err": "None"},
  "M4": {"pattern": 2, "err": "None"},
  "IMU": {"pattern": 2, "err": "None"}
}
```

#### 필드 정의 (status)
| 키             | 타입   | 설명 |
|----------------|--------|------|
| `OK <STATUS>` | `bool` | `<STATUS>` 명령 응답 여부 |
| `MCU`         | `dict` | 로봇 타입, 펌웨어 및 프로토콜 정보, 업타임 |
| `NET`         | `dict` | 네트워크 상태 (IP, GW, MASK 등) |
| `QUEUE`       | `dict` | MCU 내부 큐 상태 |
| `CANx`        | `dict` | CAN 버스 상태 |
| `Mx`          | `dict` | 모터별 상태 정보 |
| `IMU`         | `dict` | 관성센서 (IMU) 데이터 |
| `EMERGENCY`   | `dict` | ESTOP 상태 |


##### `CANx` 내부 구조
| 키     | 타입   | 설명 |
|--------|--------|------|
| `Err`  | `str`  | 에러 코드 (hex) |
| `LEC`  | `int`  | Last Error Code |
| `ACT`  | `int`  | Active Error 상태 |
| `EP`   | `int`  | Error Passive 상태 |
| `BO`   | `int`  | Bus Off 상태 |
| `REC`  | `int`  | Receive Error Counter |
| `TEC`  | `int`  | Transmit Error Counter |
| `TXFE` | `int`  | TX FIFO Empty 상태 |
| `RX0`  | `int`  | RX FIFO0 메시지 수 |
| `RX1`  | `int`  | RX FIFO1 메시지 수 |


##### 'Mx' 내부 구조 (M1, M2, ...)
| 키   | 타입   | 단위  | 설명      |
|------|--------|-------|-----------|
| `pattern` | `int` | code | 0=정지/대기, 1=보정, 2=작동 |
| `err` | `str` | - | 모터 오류 상태 |


##### `IMU` 내부 구조 (수정 및 반영중)
| 키  | 타입   | 단위 | 설명 |
|-----|--------|------|------|
| `pattern` | `int` | code | 0=정지/대기, 1=보정, 2=작동 |
| `err` | `str` | - | 모터 오류 상태 |


##### `IMU` 내부 구조 (수정 및 반영중)
| 키  | 타입   | 단위 | 설명 |
|-----|--------|------|------|
| `EMERGENCY` | `str` | - | "ON", "OFF" |

---

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
