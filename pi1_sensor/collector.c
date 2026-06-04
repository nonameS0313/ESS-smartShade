/* 파이 1 데이터 취합 + MQTT 전송 유저 앱

빌드: arm-linux-gnueabi-gcc -o collector collector.c -lmosquitto -lpthread
실행: sudo ./collector
의존성: sudo apt install libmosquitto-dev
 
MQTT 메시지: {"light_a": 2800, "light_b": 1500, "intruder": 0}

*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <mosquitto.h>

// ioctl 명령 정의 (spi_mcp3208_light 드라이버와 동일하게 유지)
#define LIGHT_IOC_MAGIC 'L'
#define LIGHT_SET_CHANNEL _IOW(LIGHT_IOC_MAGIC, 1, int)

#define LIGHT_DEV		"/dev/light_sensor"
#define PIR_DEV			"/dev/pir_sensor"
#define MQTT_HOST		"10.10.10.12"  // 파이 2 브로커 IP
#define MQTT_PORT		1883 // MQTT 기본 포트
#define MQTT_TOPIC		"smartshade/sensor/data"
#define MQTT_CLIENT_ID		"pi1_collector"
#define MQTT_KEEPALIVE		60 // 해당 초마다 연결 유지 확인
#define PUBLISH_INTERVAL_SEC	1 // 해당 초마다 센서값 전송
#define RECONNECT_DELAY_SEC	3 // 연결 실패 시 해당 초 이후 재시도
#define BUF_SIZE		32

// 조도 드라이버에서 read() -> 문자열 수신 -> atoi() 변환 후 반환
// 캐릭터 디바이스는 lseek 불가, 매번 open/close로 ppos 리셋
static int read_light(int channel)
{
	int fd;
	char buf[BUF_SIZE];
	int ch = channel;

	fd = open(LIGHT_DEV, O_RDWR);
	if (fd < 0) {
		perror("[collector] open /dev/light_sensor failed");

		return -1;
	}

	if (ioctl(fd, LIGHT_SET_CHANNEL, &ch) < 0) {
		perror("[collector] ioctl LIGHT_SET_CHANNEL failed");
		close(fd);

		return -1;
	}

	memset(buf, 0, sizeof(buf));
	if (read(fd, buf, BUF_SIZE - 1) < 0) {
		perror("[collector] read light_sensor failed");
		close(fd);

		return -1;
	}

	close(fd);

	return atoi(buf);
}

// PIR 드라이버에서 read() -> 문자열 수신 -> atoi() 변환 후 반환
static int read_pir(void)
{
	int fd;
	char buf[BUF_SIZE];

	fd = open(PIR_DEV, O_RDONLY);
	if (fd < 0) {
		perror("[collector] open /dev/pir_sensor failed");

		return -1;
	}

	memset(buf, 0, sizeof(buf));
	if (read(fd, buf, BUF_SIZE - 1) < 0) {
		perror("[collector] read pir_sensor failed");
		close(fd);

		return -1;
	}

	close(fd);

	return atoi(buf);
}

static void on_connect(struct mosquitto *mosq, void *obj, int rc)
{
	if (rc == 0)
		printf("[collector] MQTT connected to %s:%d\n", MQTT_HOST, MQTT_PORT);
	else
		fprintf(stderr, "[collector] MQTT connect failed: rc=%d\n", rc);
}

static void on_disconnect(struct mosquitto *mosq, void *obj, int rc)
{
	if (rc != 0)
		fprintf(stderr, "[collector] MQTT unexpected disconnect (rc=%d)\n", rc);
}

// 브로커 연결 실패 시 재시도
static int mqtt_connect(struct mosquitto *mosq)
{
	int ret;
	
	while (1) {
		ret = mosquitto_connect(mosq, MQTT_HOST, MQTT_PORT, MQTT_KEEPALIVE);
		if (ret == MOSQ_ERR_SUCCESS)
			return 0;

		fprintf(stderr, "[collector] mosquitto_connect failed: %s - retrying in %ds\n",
				mosquitto_strerror(ret), RECONNECT_DELAY_SEC);
		sleep(RECONNECT_DELAY_SEC);
	}
}

int main(void)
{
	int light_a, light_b, intruder;
	char payload[128];
	struct mosquitto *mosq = NULL;
	int ret;

	printf("[collector] Smart Window Shade - Pi1 Collector starting...\n");

	mosquitto_lib_init();
	mosq = mosquitto_new(MQTT_CLIENT_ID, true, NULL);
	if (!mosq) {
		fprintf(stderr, "[collector] mosquitto_new failed\n");
		mosquitto_lib_cleanup();

		return 1;
	}

	mosquitto_connect_callback_set(mosq, on_connect);
	mosquitto_disconnect_callback_set(mosq, on_disconnect);

	mqtt_connect(mosq);

	// MQTT 네트워크 처리 백그라운드 스레드 시작
	ret = mosquitto_loop_start(mosq);
	if (ret != MOSQ_ERR_SUCCESS) {
		fprintf(stderr, "[collector] mosquitto_loop_start failed: %s\n", mosquitto_strerror(ret));

		goto cleanup;
	}

	printf("[collector] Publishing to topic: %s (interval: %ds)\n", MQTT_TOPIC, PUBLISH_INTERVAL_SEC);

	while (1) {
		light_a = read_light(0); // CH0: 블라인드 A 방향
		if (light_a < 0) light_a = 0;

		light_b = read_light(1); // CH1: 블라인드 B 방향
		if (light_b < 0) light_b = 0;

		intruder = read_pir();
		if (intruder < 0) intruder = 0;

		snprintf(payload, sizeof(payload),
				"{\"light_a\": %d, \"light_b\": %d, \"intruder\": %d}",
				light_a, light_b, intruder);

		ret = mosquitto_publish(mosq, NULL, MQTT_TOPIC, (int)strlen(payload), payload, 0, false);
		if (ret == MOSQ_ERR_NO_CONN) {
			fprintf(stderr, "[collector] MQTT disconnected, reconnecting...\n");
			mosquitto_reconnect(mosq);
		}
		else if (ret != MOSQ_ERR_SUCCESS) {
			fprintf(stderr, "[collector] publish failed: %s\n", mosquitto_strerror(ret));
		}
		else {
			printf("[collector] Published: %s\n", payload);
		}

		sleep(PUBLISH_INTERVAL_SEC);
	}

cleanup:
	mosquitto_loop_stop(mosq, true);
	mosquitto_disconnect(mosq);
	mosquitto_destroy(mosq);
	mosquitto_lib_cleanup();

	return 0;
}
