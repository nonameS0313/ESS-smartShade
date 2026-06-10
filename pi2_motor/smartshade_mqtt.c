/* Smart Shade - Pi2 stepper + ultrasonic + MQTT
 *
 * Native build:  make -C app
 * Run:           sudo ./smartshade_mqtt
 */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <mosquitto.h>

#include "stepper_ioctl.h"
#include "io_common.h"

#define MQTT_HOST             "10.10.10.13"
#define MQTT_PORT             1883
#define MQTT_TOPIC            "smartshade/sensor/data"
#define MQTT_CLIENT_ID        "pi2_smartshade"
#define MQTT_KEEPALIVE        60
#define RECONNECT_DELAY_SEC   3
#define ROTATE_DEGREE         15

#define THRESHOLD_TOP         2100
#define THRESHOLD_BOTTOM      1400

#define DEV_STEPPER_A         "/dev/stepper_a"
#define DEV_STEPPER_B         "/dev/stepper_b"
#define DEV_SYS_STATE         "/dev/sys_state"
#define ULTRA_WAIT_US         60000

static int fd_a = -1;
static int fd_b = -1;
static int fd_sys = -1;
static volatile int sys_state = STATE_NORMAL;
static int prev_sys_state = STATE_NORMAL;

//stepper_motor.c 사용
static int stepper_move(int fd, int degree, int direction)
{
	struct stepper_move move;

	if (fd < 0)
		return -1;

	move.degree = degree;
	move.direction = direction;

	if (ioctl(fd, STEPPER_IOC_MOVE, &move) < 0) {
		perror("[smartshade] ioctl STEPPER_IOC_MOVE failed");
		return -1;
	}

	return 0;
}

static void stepper_zero_all(void)
{
	if (fd_a >= 0)
		ioctl(fd_a, STEPPER_IOC_ZERO);
	if (fd_b >= 0)
		ioctl(fd_b, STEPPER_IOC_ZERO);
}

static void stepper_restore_all(void)
{
	if (fd_a >= 0)
		ioctl(fd_a, STEPPER_IOC_RESTORE);
	if (fd_b >= 0)
		ioctl(fd_b, STEPPER_IOC_RESTORE);
}

//Threshold를 적용한 움직임 정의
static void control_stepper_by_light(int fd, int light, const char *name)
{
	if (light > THRESHOLD_TOP) {
		printf("[smartshade] %s: light=%d > TOP(%d) → CW %d°\n",
		       name, light, THRESHOLD_TOP, ROTATE_DEGREE);
		stepper_move(fd, ROTATE_DEGREE, 0);
	} else if (light < THRESHOLD_BOTTOM) {
		printf("[smartshade] %s: light=%d < BOTTOM(%d) → CCW %d°\n",
		       name, light, THRESHOLD_BOTTOM, ROTATE_DEGREE);
		stepper_move(fd, ROTATE_DEGREE, 1);
	}
}

//MQTT Topic parse
static int parse_payload(const char *payload, int *light_a, int *light_b, int *intruder)
{
	if (sscanf(payload,
		   "{\"light_a\": %d, \"light_b\": %d, \"intruder\": %d}",
		   light_a, light_b, intruder) == 3)
		return 0;

	return -1;
}

static void set_system_state(int state)
{
	if(fd_sys < 0)
		return;

	if(ioctl(fd_sys, CMD_SET_SYSTEM_STATE, &state) < 0)
		perror("[smartshade] CMD_SET_SYSTEM_STATE failed\n");
}

static void handle_sensor_data(const char *payload)
{
	int light_a = 0, light_b = 0, intruder = 0;

	if (parse_payload(payload, &light_a, &light_b, &intruder) < 0) {
		fprintf(stderr, "[smartshade] invalid payload: %s\n", payload);
		return;
	}

	printf("[smartshade] light_a=%d light_b=%d intruder=%d\n",
	    light_a, light_b, intruder);

	if(intruder == 1) {
		printf("[smartshade] intruder detected -> STATE_ALERT\n");
		set_system_state(STATE_ALERT);
		return;
	}

	if (intruder == 0 && prev_sys_state == STATE_ALERT){
		printf("[smartshade] intruder undetected -> STATE 복구\n");
		if(ioctl(fd_sys, CMD_RELEASE_ALERT, &state) < 0)
			perror("[smartshade] CMD_RELEASSE_ALERT failed\n");
		return;
	}

	if(sys_state != STATE_NORMAL)
		return;

	control_stepper_by_light(fd_a, light_a, "stepper_a");
	control_stepper_by_light(fd_b, light_b, "stepper_b");
}

// Ultrasonic.c 사용
static void wait_and_process_sys_event(void)
{
	int state;

	if (fd_sys < 0)
		return;

	// 커널 드라이버가 깨워줄 때까지 이 read() 함수 내에서 유저 앱이 완전히 잠든다.
	if (read(fd_sys, &state, sizeof(state)) < 0) {
		perror("[smartshade] read sys_state failed");
		return;
	}

	sys_state = state;

	// 상태 전이에 따른 모터 안전 제어 (DISABLED 상태까지 안전하게 포괄)
	// 현재 상태가 '동작 불가'라면 모터를 강제로 정지하고 아무것도 하지 않음
    if (sys_state == STATE_DISABLED) {
        printf("[smartshade] DISABLED state. Stop every Motor\n");
        // 필요한 경우 여기에 모터 전원을 차단하는 함수를 추가.
        prev_sys_state = sys_state;
        return; // 아래의 모터 제어 로직을 타지 않고 함수를 즉시 빠져나갑니다.
    }

    // 평시(NORMAL)에서 비상/수동/동작불가 상태로 바뀌면 -> 창문 닫기(원점 정렬)
    if (prev_sys_state == STATE_NORMAL && sys_state != STATE_NORMAL)
        stepper_zero_all();

		
	// 비상/수동/동작불가 상태에서 다시 평시(NORMAL)로 돌아오면 -> 창문 다시 열기(기존 각도 복원)
    if (prev_sys_state != STATE_NORMAL && sys_state == STATE_NORMAL)
        stepper_restore_all();
	
	// 장애물(DISABLED)이 사라졌는데, 복귀한 상태가 여전히 평시가 아니라면(MANUAL 또는 ALERT)
    // 닫히던 중이었던 것으로 간주하고 끝까지 마저 닫는다!
    if (prev_sys_state == STATE_DISABLED && 
       (sys_state == STATE_MANUAL || sys_state == STATE_ALERT)) {
        // printf("[smartshade] 장애물 해제! 기존에 하던 닫기 동작(Zeroing)을 마저 완료합니다.\n");
        stepper_zero_all();
    }
	
	// if (prev_sys_state == STATE_NORMAL
	//     && (sys_state == STATE_MANUAL || sys_state == STATE_ALERT))
	// 	stepper_zero_all();	//MANUAL or ALERT 상태면 창문 닫기

	// if ((prev_sys_state == STATE_MANUAL || prev_sys_state == STATE_ALERT)
	//     && sys_state == STATE_NORMAL)
	// 	stepper_restore_all();	//NORMAL로 돌아오면 다시 열기

	prev_sys_state = sys_state;
}

static void on_connect(struct mosquitto *mosq, void *obj, int rc)
{
	int ret;

	(void)obj;

	if (rc != 0) {
		fprintf(stderr, "[smartshade] MQTT connect failed: rc=%d\n", rc);
		return;
	}

	printf("[smartshade] MQTT connected to %s:%d\n", MQTT_HOST, MQTT_PORT);

	ret = mosquitto_subscribe(mosq, NULL, MQTT_TOPIC, 0);
	if (ret != MOSQ_ERR_SUCCESS)
		fprintf(stderr, "[smartshade] subscribe failed: %s\n",
			mosquitto_strerror(ret));
	else
		printf("[smartshade] Subscribed: %s\n", MQTT_TOPIC);
}

static void on_message(struct mosquitto *mosq, void *obj,
		       const struct mosquitto_message *msg)
{
	(void)mosq;
	(void)obj;

	if (!msg->payload)
		return;

	handle_sensor_data((const char *)msg->payload);
}

static int mqtt_connect(struct mosquitto *mosq)
{
	int ret;

	while (1) {
		ret = mosquitto_connect(mosq, MQTT_HOST, MQTT_PORT, MQTT_KEEPALIVE);
		if (ret == MOSQ_ERR_SUCCESS)
			return 0;

		fprintf(stderr,
			"[smartshade] mosquitto_connect failed: %s - retry in %ds\n",
			mosquitto_strerror(ret), RECONNECT_DELAY_SEC);
		sleep(RECONNECT_DELAY_SEC);
	}
}

static int open_devices(void)
{
	fd_a = open(DEV_STEPPER_A, O_RDWR);
	if (fd_a < 0) {
		perror("[smartshade] open stepper_a");
		return -1;
	}

	fd_b = open(DEV_STEPPER_B, O_RDWR);
	if (fd_b < 0) {
		perror("[smartshade] open stepper_b");
		goto err_a;
	}

	fd_sys = open(DEV_SYS_STATE, O_RDWR);
	if (fd_sys < 0) {
		perror("[smartshade] open sys_state");
		goto err_b;
	}

	printf("[smartshade] Opened %s, %s, %s\n",
	       DEV_STEPPER_A, DEV_STEPPER_B, DEV_SYS_STATE);
	return 0;

err_b:
	close(fd_b);
	fd_b = -1;
err_a:
	close(fd_a);
	fd_a = -1;
	return -1;
}

int main(void)
{
	struct mosquitto *mosq = NULL;
	int ret;

	if (open_devices() < 0)
		return 1;

	mosquitto_lib_init();
	mosq = mosquitto_new(MQTT_CLIENT_ID, true, NULL);
	if (!mosq) {
		fprintf(stderr, "[smartshade] mosquitto_new failed\n");
		ret = 1;
		goto out_dev;
	}

	mosquitto_connect_callback_set(mosq, on_connect);
	mosquitto_message_callback_set(mosq, on_message);

	mqtt_connect(mosq);

	ret = mosquitto_loop_start(mosq);
	if (ret != MOSQ_ERR_SUCCESS) {
		fprintf(stderr, "[smartshade] loop_start failed: %s\n",
			mosquitto_strerror(ret));
		ret = 1;
		goto out_mqtt;
	}

	while (1)
		wait_and_process_sys_event();

	mosquitto_loop_stop(mosq, true);
	ret = 0;

out_mqtt:
	mosquitto_destroy(mosq);
	mosquitto_lib_cleanup();
out_dev:
	if (fd_sys >= 0)
		close(fd_sys);
	if (fd_b >= 0)
		close(fd_b);
	if (fd_a >= 0)
		close(fd_a);

	return ret;
}
