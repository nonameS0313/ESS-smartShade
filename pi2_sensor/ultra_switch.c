#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/ktime.h>
#include <linux/timer.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include "io_common.h"

MODULE_LICENSE("GPL");

#define DEV_NAME "sys_state"

// GPIO 핀 번호 (라즈베리파이 실핀 번호에 맞게 수정 필요)
#define GPIO_TRIG 23
#define GPIO_ECHO 24
#define GPIO_SW   17

#define GPIO_LED_RED    18  // 침입 (STATE_ALERT) 조명
#define GPIO_LED_YELLOW 22  // 수동 상태 / 동작 불가 (STATE_MANUAL / STATE_DISABLED) 조명

// 글로벌 변수 보호를 위한 스핀락 및 대기 큐
static DEFINE_SPINLOCK(state_lock);
static DECLARE_WAIT_QUEUE_HEAD(state_wq);

static int current_state = STATE_NORMAL;
static int state_changed = 0; // 유저 앱 깨우기용 플래그

static int main_mode = STATE_NORMAL;

static ktime_t echo_start;
static struct timer_list sw_timer; // 스위치 디바이스 디바운싱 타이머
static struct tasklet_struct ultra_tasklet; // 초음파 바텀 하프

static dev_t dev_num;
static struct cdev *cd_cdev;

// -------------------------------------------------------------
// [LED] 타이머, 핸들러
// -------------------------------------------------------------
// LED 변경 함수
// 황색 점멸 제어용 타이머 및 플래그 추가
struct led_blink_data {
    struct timer_list timer; // kernel timer
    int gpio_pin;
};

static struct led_blink_data blink_dev;
static int blink_toggle = 0;

static void led_blink_timer_handler(struct timer_list *t) {
    // timer 주소로 전체 구조체 찾기
    struct led_blink_data *data = from_timer(data, t, timer);
    
    if(current_state == STATE_MANUAL || current_state == STATE_ALERT) {
        blink_toggle = !blink_toggle;
        gpio_set_value(data->gpio_pin, blink_toggle);
        // 500ms 뒤에 다시 이 타이머를 실행
        mod_timer(&data->timer, jiffies + msecs_to_jiffies(500));
    }
}

static void update_led_status(int state) {
    // 작동 중이던 점멸 타이머를 안전하게 정지
    del_timer(&blink_dev.timer);
    
    switch (state) {
        case STATE_NORMAL: // 전체 소등
        gpio_set_value(GPIO_LED_YELLOW, 0);
        gpio_set_value(GPIO_LED_RED,    0);
        break;
        case STATE_MANUAL: // 황색 점멸
        gpio_set_value(GPIO_LED_RED,    0);
        
        blink_dev.gpio_pin = GPIO_LED_YELLOW;
        
        blink_toggle = 1;
        gpio_set_value(GPIO_LED_YELLOW, blink_toggle); // 일단 켬
        mod_timer(&blink_dev.timer, jiffies + msecs_to_jiffies(500)); // 0.5초 뒤 점멸 시작
        break;
        case STATE_DISABLED:
        gpio_set_value(GPIO_LED_RED,    0);
        gpio_set_value(GPIO_LED_YELLOW, 1); // 켜짐 유지
        break;
        case STATE_ALERT: // 적색 점멸
        gpio_set_value(GPIO_LED_RED,    0);
        
        blink_dev.gpio_pin = GPIO_LED_RED;
        
        blink_toggle = 1;
        gpio_set_value(GPIO_LED_RED, blink_toggle); // 일단 켬
        mod_timer(&blink_dev.timer, jiffies + msecs_to_jiffies(500)); // 0.5초 뒤 점멸 시작
        break;
    }
}

// 초음파 센서 주기적 자동 트리거용 타이머 추가
static struct timer_list ultra_trig_timer;

static void ultra_trig_timer_handler(struct timer_list *t) {
    // 10us 동안 HIGH 신호를 주어 초음파 센서 구동
    gpio_set_value(GPIO_TRIG, 1);
    udelay(10);
    gpio_set_value(GPIO_TRIG, 0);

    // 100ms 뒤에 이 타이머 핸들러가 다시 실행되도록 예약 (무한 반복 루프)
    mod_timer(&ultra_trig_timer, jiffies + msecs_to_jiffies(100));
}

// 통합 상태 변경 함수
void change_system_state(int new_state) {
    unsigned long flags;
    spin_lock_irqsave(&state_lock, flags);

    // 변경하려는 새로운 상태가 비상이 아닌 기본 모드일 때만 main_mode 축을 업데이트 한다
    if (current_state != new_state) {
        if(new_state == STATE_NORMAL || new_state == STATE_MANUAL) {
            switch(main_mode){
                case STATE_ALERT:
                    printk("[ultra_switch] State changed : ALERT");
                    break;
                case STATE_DISABLED:
                    printk("[ultra_switch] State changed : DISABLED");
                    break;
                case STATE_MANUAL:
                    printk("[ultra_switch] State changed : MANUAL");
                    break;
                case STATE_NORMAL:
                    printk("[ultra_switch] State changed : NORMAL");
                    break;
                default:
                    break;
            }
            
            main_mode = new_state;
        }

        current_state = new_state;
        state_changed = 1;
        
        // 1. 커널 내에서 LED 즉시 변경 호출
        update_led_status(current_state);
        
        // 2. 대기 중인 유저 앱 스레드 깨우기
        wake_up_interruptible(&state_wq);
    }
    spin_unlock_irqrestore(&state_lock, flags);
}

// -------------------------------------------------------------
// [초음파 센서] 인터럽트 & 바텀하프(Tasklet)
// -------------------------------------------------------------
void ultra_bh_handler(unsigned long data) {
    // Tasklet (Bottom Half) 컨텍스트: 거리 계산 및 상태 판단
    s64 duration = ktime_to_us(data);
    // long distance = duration * 34 / 2000; // cm 계산 공식
    long distance;
    u64 temp = (u64)(duration * 34);
    do_div(temp, 2000);
    distance = (long)temp;

    // printk(KERN_INFO "[Ultra Debug] 측정된 거리: %ld cm (duration: %lld us)\n", distance, duration);
    
    if (distance < 10) { // 10cm 미만 장애물 감지 시
        change_system_state(STATE_DISABLED);
    } else if (current_state == STATE_DISABLED && distance >= 10) {
        // 장애물 해제 시 이전 상태로 안전 복귀
        change_system_state(main_mode);
    }
}

static irqreturn_t ultra_echo_irq_handler(int irq, void *dev_id) {
    ktime_t now = ktime_get();
    if (gpio_get_value(GPIO_ECHO) == 1) { // Echo 핀이 High가 되면 시작 시간 측정
        echo_start = now; // Rising Edge
    } else { // Low가 되면 끝난 시간을 바탕으로 시간 차를 구해
        ktime_t dummy = ktime_sub(now, echo_start); // Falling Edge
        // 바텀 하프로 데이터 넘기며 스케줄링
        ultra_tasklet.data = (unsigned long)dummy;
        tasklet_schedule(&ultra_tasklet);
    }
    return IRQ_HANDLED;
}

// -------------------------------------------------------------
// [물리 스위치] 인터럽트 & 바텀하프(Timer Debounce)
// -------------------------------------------------------------
static void sw_timer_handler(struct timer_list *t) {
    // 디바운싱 타이머 만료 시점 (Bottom Half)
    if (gpio_get_value(GPIO_SW) == 0) { // 여전히 눌려있다면 유효한 입력
        if(current_state == STATE_NORMAL) {
            change_system_state(STATE_MANUAL);
        } else if (current_state == STATE_MANUAL) {
            change_system_state(STATE_NORMAL);
        } else if (current_state == STATE_DISABLED) {
            main_mode = (main_mode == STATE_MANUAL) ? STATE_NORMAL : STATE_MANUAL;
        }
    }

    enable_irq(gpio_to_irq(GPIO_SW)); // 인터럽트 재개방
}

static irqreturn_t switch_irq_handler(int irq, void *dev_id) {
    disable_irq_nosync(gpio_to_irq(GPIO_SW)); // 채터링 방지를 위해 일시 잠금
    mod_timer(&sw_timer, jiffies + msecs_to_jiffies(20)); // 20ms 뒤 타이머 실행
    return IRQ_HANDLED;
}

// -------------------------------------------------------------
// 파일 오퍼레이션 (유저 앱 통신용 블로킹 Read)
// -------------------------------------------------------------
static ssize_t sys_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos) {
    // 상태 변화가 없으면 유저 스레드를 재움 (Blocking)
    unsigned long flags;
    int state;

    if (wait_event_interruptible(state_wq, state_changed != 0)) {
        return -ERESTARTSYS; 
    }

    // softirq 컨텍스트와 deadlock 방지를 위해 spin_lock_irqsave 사용
    // copy_to_user는 lock 안에서 sleep하면 안 되므로 spinlock 밖에서 호출
    spin_lock_irqsave(&state_lock, flags);
    if (state_changed) {
        state_changed = 0;
    }
    state = current_state;
    spin_unlock_irqrestore(&state_lock, flags);

    if (copy_to_user(buf, &state, sizeof(int))) {
        return -EFAULT;
    }

    return sizeof(int);
}

static long sys_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    int next_state;

    switch(cmd) {
        case CMD_ULTRA_TRIG:
            gpio_set_value(GPIO_TRIG, 1);
            udelay(10);
            gpio_set_value(GPIO_TRIG, 0);
            break;
        case CMD_SET_SYSTEM_STATE:
            // 외부 상태 주입
            if(copy_from_user(&next_state, (int __user *) arg, sizeof(int))) {
                return -EFAULT;
            }
            if(current_state != STATE_DISABLED) { // DISABLED 상태가 최우선, 수동 모드로도, 침입모드로도 전환 불가
                change_system_state(next_state);
            }
            break;
        case CMD_RELEASE_ALERT:
            // ALERT 상태 해제시 기존 모드로 복귀
            if(current_state == STATE_ALERT) {
                change_system_state(main_mode);
            }
            break;
        default:
            return -EINVAL;
    }

    return 0;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .read = sys_read,
    .unlocked_ioctl = sys_ioctl,
};

// -------------------------------------------------------------
// 모듈 등록 및 해제
// -------------------------------------------------------------
static int __init sys_io_init(void) {
    int ret;

    // 장치 번호 동적 할당
    ret = alloc_chrdev_region(&dev_num, 0, 1, DEV_NAME);
    if(ret < 0) {
        printk(KERN_ERR "장치 번호 할당 실패\n");
        return ret;
    }

    // 할당받은 주번호를 커널 로그로 확인하기 위해 출력
    printk(KERN_INFO "Major Number: %d, Minor Number: %d\n", MAJOR(dev_num), MINOR(dev_num));

    // cdev 구조체 초기화 및 파일 오퍼레이션 연결
    cd_cdev = cdev_alloc();
    if(!cd_cdev) {
        ret = -ENOMEM;
        goto err_unregister_region;
    }
    cdev_init(cd_cdev, &fops);

    // 생성된 캐릭터 디바이스를 시스템에 등록
    ret = cdev_add(cd_cdev, dev_num, 1);
    if(ret < 0) {
        printk(KERN_ERR "Failed to Register Character Device");
        goto err_unregister_region;
    }

    // GPIO 및 IRQ 설정 (Trig, Echo, Switch)
    if((ret = gpio_request(GPIO_TRIG, "TRIG")) < 0) goto err_cdev_del;
    gpio_direction_output(GPIO_TRIG, 0);

    if((ret = gpio_request(GPIO_ECHO, "ECHO")) < 0) goto err_free_trig;
    gpio_direction_input(GPIO_ECHO);

    if((ret = gpio_request(GPIO_SW, "SWITCH")) < 0) goto err_free_echo;
    gpio_direction_input(GPIO_SW);

    // LED GPIO 요청 및 출력 방향 설정 추가
    if((ret = gpio_request(GPIO_LED_RED, "LED_RED")) < 0) goto err_free_sw;
    gpio_direction_output(GPIO_LED_RED, 0);

    if((ret = gpio_request(GPIO_LED_YELLOW, "LED_YELLOW")) < 0) goto err_free_led_red;
    gpio_direction_output(GPIO_LED_YELLOW, 0);
    
    ret = request_irq(gpio_to_irq(GPIO_ECHO), ultra_echo_irq_handler, IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING, "ultra_irq", NULL);
    if (ret < 0) {
        printk(KERN_ERR "Echo IRQ 등록 실패: %d\n", ret);
        goto err_free_led_yellow;
    }
    
    ret = request_irq(gpio_to_irq(GPIO_SW), switch_irq_handler, IRQF_TRIGGER_FALLING, "switch_irq", NULL);
    
    if (ret < 0) {
        printk(KERN_ERR "Switch IRQ 등록 실패: %d\n", ret);
        goto err_free_irq_echo;
    }

    // Bottom Half 초기화
    tasklet_init(&ultra_tasklet, ultra_bh_handler, 0);
    timer_setup(&sw_timer, sw_timer_handler, 0);

    // 점멸 타이머 초기화
    timer_setup(&blink_dev.timer, led_blink_timer_handler, 0);
    
    // [추가] 자동 트리거 타이머 초기화 및 최초 실행 시작
    timer_setup(&ultra_trig_timer, ultra_trig_timer_handler, 0);
    mod_timer(&ultra_trig_timer, jiffies + msecs_to_jiffies(100));

    printk(KERN_INFO "System IO Driver Registered.\n");
    return 0;

    // 에러 발생 시 정상적인 클린업 패스 구축
err_free_irq_echo:
    del_timer_sync(&ultra_trig_timer);
    free_irq(gpio_to_irq(GPIO_ECHO), NULL);
err_free_led_yellow:
    gpio_free(GPIO_LED_YELLOW);
err_free_led_red:
    gpio_free(GPIO_LED_RED);
err_free_sw:
    gpio_free(GPIO_SW);
err_free_echo:
    gpio_free(GPIO_ECHO);
err_free_trig:
    gpio_free(GPIO_TRIG);
err_cdev_del:
    cdev_del(cd_cdev);
err_unregister_region:
    unregister_chrdev_region(dev_num, 1);
    return ret;
}

static void __exit sys_io_exit(void) {
    free_irq(gpio_to_irq(GPIO_ECHO), NULL);
    free_irq(gpio_to_irq(GPIO_SW), NULL);
    del_timer_sync(&sw_timer);
    del_timer_sync(&blink_dev.timer);
    del_timer_sync(&ultra_trig_timer);
    tasklet_kill(&ultra_tasklet);


    gpio_set_value(GPIO_LED_RED, 0);
    gpio_set_value(GPIO_LED_YELLOW, 0);
    gpio_free(GPIO_LED_RED); gpio_free(GPIO_LED_YELLOW);

    gpio_free(GPIO_TRIG); gpio_free(GPIO_ECHO); gpio_free(GPIO_SW);

    // 동적 할당 장치 번호 해제
    cdev_del(cd_cdev);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "System IO Driver Released.\n");
}

module_init(sys_io_init);
module_exit(sys_io_exit);