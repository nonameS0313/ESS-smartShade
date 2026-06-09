# ESS Smart Shade

건국대학교 ESS 수업 Smart Shade 프로젝트

스마트 창문 셰이드: **Pi1**에서 조도·PIR 수집 후 MQTT로 발행하고, **Pi2**에서 구독해 스테퍼 모터·시스템 상태(초음파/스위치/ALERT)를 제어합니다.

- 저장소: https://github.com/nonameS0313/ESS-smartShade.git

## 시스템 구성

```
[ Pi1 — pi1_sensor ]                    [ Pi2 ]
  MCP3208 (조도 CH0/CH1)                  Mosquitto broker :1883
  PIR (GPIO17)                            pi2_sensor (초음파, 스위치, LED)
       │                                  pi2_motor (듀얼 스테퍼)
       │  MQTT publish                         ▲
       └──────── smartshade/sensor/data ───────┘ subscribe
```

| 보드 | 역할 | 주요 경로 |
|------|------|-----------|
| Pi1 | 센서 수집·발행 | `pi1_sensor/` |
| Pi2 | 브로커·구독·모터·로컬 IO | `broker/`, `pi2_motor/`, `pi2_sensor/` |

기본 IP는 코드에 **`10.10.10.12`(Pi2)** 로 고정되어 있습니다. 실제 네트워크에 맞게 `collector.c`, `smartshade_mqtt.c`의 `MQTT_HOST`를 수정하세요.

## Make 명령어
아래의 순서대로 폴더마다 make 실시

1. Makefile 내의 \M 문자 지우는 명령어
```
sed -i 's/\r$//' Makefile
```
2. Make 명령어
```
make ARCH=arm
```


## MQTT 메시지

- **토픽:** `smartshade/sensor/data`
- **페이로드 예:**

```json
{"light_a": 2800, "light_b": 1500, "intruder": 0}
```

| 필드 | 의미 |
|------|------|
| `light_a`, `light_b` | 조도 ADC 값 (채널 0/1) |
| `intruder` | PIR: `0` 평시, `1` 침입 |

Pi2 동작 요약:

- `intruder == 1` → `/dev/sys_state`에 `STATE_ALERT` 설정 → `poll_ultrasonic()`에서 NORMAL→ALERT 전이 시 스테퍼 **0도(ZERO)**
- `sys_state == STATE_NORMAL`일 때만 조도 임계값으로 스테퍼 회전
- `intruder == 1`인 메시지에서는 조도 기반 회전 **스킵**

조도 임계값 (`smartshade_mqtt.c`): `TOP=2100`, `BOTTOM=1400`, 회전량 `15°`.

---

## 사전 준비 (공통)

### 네트워크

- Pi1, Pi2가 같은 LAN에 있고 Pi1 → Pi2 `1883` 접근 가능
- Pi2 IP를 `10.10.10.12`로 쓸 경우: Pi2에 고정 IP 또는 hosts 등록

### 패키지

**Pi1**

```bash
sudo apt update
sudo apt install -y libmosquitto-dev mosquitto-clients
# 커널 모듈 빌드용 (Pi OS에 맞게)
sudo apt install -y raspberrypi-kernel-headers build-essential
```

**Pi2**

```bash
sudo apt update
sudo apt install -y mosquitto libmosquitto-dev mosquitto-clients \
  raspberrypi-kernel-headers build-essential
```

**Pi1 SPI (조도 ADC)**

```bash
sudo raspi-config
# Interface Options → SPI → Enable
ls /dev/spidev0.0   # 존재 확인
```

---

## Pi2 — 브로커·드라이버·앱

### 1) MQTT 브로커

```bash
cd broker
chmod +x start_broker.sh
./start_broker.sh
# 또는: mosquitto -c mosquitto.conf -v
```

다른 터미널에서 확인:

```bash
mosquitto_sub -h 127.0.0.1 -t 'smartshade/sensor/data' -v
```

### 2) 커널 모듈 — 시스템 IO (`pi2_sensor`)

```bash
cd pi2_sensor
make KDIR=/lib/modules/$(uname -r)/build
# Makefile의 KDIR/크로스 컴파일은 환경에 맞게 조정

sudo insmod ultra_switch.ko
# ultra_switch는 device_create 없음 → 노드 수동 생성
sudo sh mknod.sh   # /dev/sys_state 생성
ls -l /dev/sys_state
```

### 3) 커널 모듈 — 스테퍼 (`pi2_motor/driver`)

```bash
cd pi2_motor/driver
make
sudo insmod stepper_motor.ko
ls -l /dev/stepper_a /dev/stepper_b
```

### 4) 유저 앱 `smartshade_mqtt`

```bash
cd pi2_motor
make
sudo ./smartshade_mqtt
```

정상 시 로그 예: MQTT 연결, `Subscribed: smartshade/sensor/data`, stepper/sys_state 디바이스 open.

---

## Pi1 — 드라이버·collector

### 1) 커널 모듈

Pi1에서 소스 빌드 (크로스/네이티브는 보드 OS에 맞게):

```bash
cd pi1_sensor
# 예: 네이티브 Pi 빌드
make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
# 또는 각 .c를 별도 Makefile로 빌드하는 경우 환경에 맞게 insmod
```

로드 예:

```bash
sudo insmod spi_mcp3208_light.ko
sudo insmod gpio_pir.ko
ls -l /dev/light_sensor /dev/pir_sensor
```

### 2) collector 빌드·실행

```bash
cd pi1_sensor
gcc -o collector collector.c -lmosquitto -lpthread
# 32/64비트·크로스 툴체인은 Pi OS에 맞게 선택

sudo ./collector
```

1초마다 `Published: {"light_a": ...}` 로그가 보이면 Pi2 브로커까지 publish 성공입니다.

---

## 라즈베리파이에서 동작 재현·검증

### A. 전체 연동 (하드웨어 연결 후)

1. Pi2: 브로커 → `ultra_switch` + `mknod` → `stepper_motor` → `smartshade_mqtt`
2. Pi1: 조도/PIR 모듈 → `collector`
3. Pi2에서 구독 확인:

```bash
mosquitto_sub -h 10.10.10.12 -t 'smartshade/sensor/data' -v
```

4. 조도 변화 시 Pi2 로그에 `light_a=...` 및 stepper CW/CCW 메시지 확인
5. PIR 감지(또는 Pi1에서 `/dev/pir_sensor` read가 `1`) 시:
   - Pi1 payload에 `"intruder": 1`
   - Pi2: `intruder detected → STATE_ALERT`
   - 이후 폴링에서 `stepper_zero_all` 동작(창 닫기)

### B. Pi2만 MQTT로 재현 (센서·Pi1 없이)

브로커와 `smartshade_mqtt`만 띄운 뒤:

**조도 — 스테퍼 (NORMAL 가정, 초음파 장애물 없음)**

```bash
# light_a 높음 → A축 CW 15°
mosquitto_pub -h 10.10.10.12 -t smartshade/sensor/data \
  -m '{"light_a": 2800, "light_b": 1500, "intruder": 0}'

# light_a 낮음 → A축 CCW 15°
mosquitto_pub -h 10.10.10.12 -t smartshade/sensor/data \
  -m '{"light_a": 1000, "light_b": 1500, "intruder": 0}'

# deadband — 회전 없음
mosquitto_pub -h 10.10.10.12 -t smartshade/sensor/data \
  -m '{"light_a": 1800, "light_b": 1500, "intruder": 0}'
```

**침입 — ALERT + ZERO (NORMAL에서 시작)**

```bash
mosquitto_pub -h 10.10.10.12 -t smartshade/sensor/data \
  -m '{"light_a": 1800, "light_b": 1500, "intruder": 1}'
```

기대 로그: `intruder detected → STATE_ALERT`, 조도 stepper 로그 없음, 곧 이어 zero 관련 동작.

같은 메시지에 `light_a`가 극단값이어도 `intruder==1`이면 **조도 모터 제어는 하지 않음**.

### C. Pi1 디바이스만 로컬 확인

```bash
# 조도 (채널 0)
sudo sh -c 'echo 0 > /dev/null'  # ioctl은 작은 C 스니펫 또는 collector 로그로 확인
cat /dev/light_sensor   # 드라이버: ioctl CH 설정 후 read

# PIR
cat /dev/pir_sensor     # 0 또는 1
```

### D. 수동 모드·초음파 (Pi2 로컬)

- 물리 스위치: `STATE_MANUAL` ↔ `STATE_NORMAL` (드라이버 `GPIO_SW`)
- 초음파 10cm 미만: `STATE_DISABLED` (자동 조도 MQTT 차단)
- MANUAL/ALERT 진입 시에도 스테퍼 ZERO / NORMAL 복귀 시 RESTORE (`smartshade_mqtt` 폴링)

---

## 디렉터리 구조

```
.
├── README.md
├── broker/
│   ├── mosquitto.conf
│   └── start_broker.sh
├── pi1_sensor/
│   ├── collector.c
│   ├── spi_mcp3208_light.c
│   └── gpio_pir.c
├── pi2_sensor/
│   ├── ultra_switch.c
│   ├── io_common.h
│   ├── mknod.sh
│   └── Makefile
└── pi2_motor/
    ├── smartshade_mqtt.c
    ├── Makefile
    └── driver/
        ├── stepper_motor.c
        ├── stepper_ioctl.h
        └── Makefile
```

---

## 문제 해결

| 증상 | 확인 |
|------|------|
| MQTT 연결 실패 | Pi2 IP, `mosquitto` 실행, 방화벽 1883 |
| `/dev/sys_state` 없음 | `insmod ultra_switch.ko` 후 `mknod.sh` |
| 조도 항상 0 | Pi1 SPI, `/dev/spidev0.0`, `light_sensor` 모듈 |
| publish는 되는데 모터 무반응 | `sys_state`가 NORMAL인지, payload 공백 형식, `intruder==1` 여부 |
| 모듈 빌드 실패 | `uname -r`와 동일한 kernel headers (`KDIR`) |

---

## 알려진 제한 (참고)

- `intruder==0`으로 ALERT 자동 해제는 미구현
- `pi2_sensor/Makefile`의 `test_app` 타겟은 저장소에 소스 없음 — 모듈 빌드만 사용
- 조도 커널 드라이버의 spidev 접근 방식은 최신 커널에서 빌드 이슈 가능
- 스테퍼 드라이버 `backward()` 인덱스 등은 추후 수정 권장

---

## 라이선스

GPL 커널 모듈(`MODULE_LICENSE("GPL")`) 및 프로젝트 정책에 따릅니다.
