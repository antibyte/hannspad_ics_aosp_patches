/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "lights"
#include <cutils/log.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <hardware/lights.h>

static pthread_once_t g_init = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

struct led_restore {
	char restore_trigger[25];
	char restore_brightness[5];
	};		
static struct led_restore hanns_led;

char const *const LCD_FILE = "/sys/class/backlight/pwm-backlight/brightness";
char const *const LEDBL_FILE = "/sys/class/leds/wifi\:\:blink/brightness";
char const *const LEDON_FILE = "/sys/class/leds/wifi\:\:led/brightness";
char const *const TRIGGER_FILE = "/sys/class/leds/wifi\:\:led/trigger";

#define IMAX 0 // 12.75mA power consumption

// Slope values, based on total blink of 1000ms
#define SLOPE_UP_1		450
#define SLOPE_UP_2		(500-SLOPE_UP_1)
#define SLOPE_DOWN_1	SLOPE_UP_2
#define SLOPE_DOWN_2	SLOPE_UP_1
// brightness at mid-slope, on 0 - 127 scale
#define MID_BRIGHTNESS  31

void init_g_lock(void)
{
	pthread_mutex_init(&g_lock, NULL);
}

static int write_int(char const *path, int value)
{
	int fd;
	static int already_warned;

	already_warned = 0;

	LOGV("write_int: path %s, value %d", path, value);
	fd = open(path, O_RDWR);

	if (fd >= 0) {
		char buffer[20];
		int bytes = sprintf(buffer, "%d\n", value);
		int amt = write(fd, buffer, bytes);
		close(fd);
		return amt == -1 ? -errno : 0;
	} else {
		if (already_warned == 0) {
			LOGE("write_int failed to open %s\n", path);
			already_warned = 1;
		}
		return -errno;
	}
}

static int rgb_to_brightness(struct light_state_t const *state)
{
	int color = state->color & 0x00ffffff;

	return ((77*((color>>16) & 0x00ff))
		+ (150*((color>>8) & 0x00ff)) + (29*(color & 0x00ff))) >> 8;
}

static int set_light_backlight(struct light_device_t *dev,
			struct light_state_t const *state)
{
	int err = 0;
	int brightness = rgb_to_brightness(state);

	pthread_mutex_lock(&g_lock);
	err = write_int(LCD_FILE, brightness);

	pthread_mutex_unlock(&g_lock);
	return err;
}

static int close_lights(struct light_device_t *dev)
{
	LOGV("close_light is called");
	if (dev)
		free(dev);

	return 0;
}

/* LEDs */

static void save_state(void)
{

	int err = 0;
	int len = 0;
	int fd;
	char buffer[250];
	int p1 = 0;
	int p2 = 0;

	pthread_mutex_lock(&g_lock);

	memset (&hanns_led,0 ,sizeof(struct led_restore));

	fd = open(TRIGGER_FILE, O_RDWR);
	if (fd >= 0) {
		len = read(fd, buffer, sizeof(buffer));
		if (len==-1) {
			LOGE("failed to read trigger");
		} else {
			p1=strcspn(buffer, "[") + 1;
			p2=strcspn(buffer + p1, "]");			
			strncpy(hanns_led.restore_trigger,buffer + p1,p2);		
			strcat(hanns_led.restore_trigger ,"\n");
			LOGI("LED trigger saved: %s", hanns_led.restore_trigger);
		}
		close(fd);
	} else {
		LOGE("failed to open %s!", TRIGGER_FILE);
		err =  -errno;
	}

	fd = open(LEDON_FILE, O_RDWR);
	if (fd >= 0) {
		len = read(fd, hanns_led.restore_brightness, sizeof(hanns_led.restore_brightness));
		if (len==-1) {
			LOGE("failed to read LED brightness");
		} else {
			LOGI("LED brightness saved: %s", hanns_led.restore_brightness);
		}
		close(fd);
	} else {
		LOGE("failed to open %s!", LEDON_FILE);
		err =  -errno;
	}
	pthread_mutex_unlock(&g_lock);

}

static void restore_state(void)
{
	int err = 0;
	int len = 0;
	int fd;
	pthread_mutex_lock(&g_lock);
	fd = open(LEDON_FILE, O_RDWR);
	if (fd >= 0) {
		LOGI("restoring LED brightness : %s", hanns_led.restore_brightness);
		len = write(fd, hanns_led.restore_brightness, strlen(hanns_led.restore_brightness));
		if (len==-1)
			LOGE("failed to restore brightness");

		close(fd);
	} else {
		LOGE("failed to open %s!", LEDON_FILE);
		err =  -errno;
	}

	fd = open(TRIGGER_FILE, O_RDWR);
	if (fd >= 0) {
		LOGI("restoring LED trigger : %s", hanns_led.restore_trigger);
		len = write(fd, hanns_led.restore_trigger, strlen(hanns_led.restore_trigger));
		if (len==-1)
			LOGE("failed to restore trigger");

		close(fd);
	} else {
		LOGE("failed to open %s!", TRIGGER_FILE);
		err =  -errno;
	}
	pthread_mutex_unlock(&g_lock);


}

static int write_leds(int onoff)
{
	int err = 0;
	int fd;

	pthread_mutex_lock(&g_lock);

	fd = open(LEDBL_FILE, O_RDWR);
	if (fd >= 0) {
		err = write_int(LEDBL_FILE, onoff?3:16);
		if (err)
			LOGE("failed to write flashmode");

		close(fd);
	} else {
		LOGE("failed to open %s!", LEDBL_FILE);
		err =  -errno;
	}

	if (onoff) {
		fd = open(LEDON_FILE, O_RDWR);
		if (fd >= 0) {
			err = write_int(LEDON_FILE, 32);
			if (err)
				LOGE("failed to write brightness");
			close(fd);
		} else {
			LOGE("failed to open %s!", LEDON_FILE);
			err =  -errno;
		}
	}

	pthread_mutex_unlock(&g_lock);

	return err;
}

static int set_light_leds(struct light_state_t const *state, int type)
{

	int onoff;
	switch (state->flashMode) {
	case LIGHT_FLASH_NONE:
		onoff=0;
		restore_state();
		break;
	case LIGHT_FLASH_TIMED:
	case LIGHT_FLASH_HARDWARE:
		save_state();
		onoff=1;
		break;
	default:
		return -EINVAL;
	}

	return write_leds(onoff);
}

static int set_light_leds_notifications(struct light_device_t *dev,
			struct light_state_t const *state)
{
	return set_light_leds(state, 0);
}

/*
static int set_light_leds_attention(struct light_device_t *dev,
			struct light_state_t const *state)
{
	return set_light_leds(state, 1);
}*/

static int open_lights(const struct hw_module_t *module, char const *name,
						struct hw_device_t **device)
{
	int (*set_light)(struct light_device_t *dev,
		struct light_state_t const *state);

	if (0 == strcmp(LIGHT_ID_BACKLIGHT, name))
		set_light = set_light_backlight;
	else if (0 == strcmp(LIGHT_ID_NOTIFICATIONS, name))
		set_light = set_light_leds_notifications;
/*	else if (0 == strcmp(LIGHT_ID_ATTENTION, name))
		set_light = set_light_leds_attention;*/
	else
		return -EINVAL;

	pthread_once(&g_init, init_g_lock);

	struct light_device_t *dev = malloc(sizeof(struct light_device_t));
	memset(dev, 0, sizeof(*dev));

	dev->common.tag = HARDWARE_DEVICE_TAG;
	dev->common.version = 0;
	dev->common.module = (struct hw_module_t *)module;
	dev->common.close = (int (*)(struct hw_device_t *))close_lights;
	dev->set_light = set_light;

	*device = (struct hw_device_t *)dev;

	return 0;
}

static struct hw_module_methods_t lights_module_methods = {
	.open =  open_lights,
};

const struct hw_module_t HAL_MODULE_INFO_SYM = {
	.tag = HARDWARE_MODULE_TAG,
	.version_major = 1,
	.version_minor = 0,
	.id = LIGHTS_HARDWARE_MODULE_ID,
	.name = "lights Module",
	.author = "Google, Inc.",
	.methods = &lights_module_methods,
};
