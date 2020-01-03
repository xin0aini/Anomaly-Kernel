// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018-2019 Sultan Alsawaf <sultan@kerneltoast.com>.
 */

#define pr_fmt(fmt) "devfreq_boost: " fmt

#include <linux/devfreq_boost.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kthread.h>
#include <uapi/linux/sched/types.h>
#endif

static unsigned short flex_boost_duration __read_mostly = CONFIG_FLEX_DEVFREQ_BOOST_DURATION_MS;
static unsigned short input_boost_duration __read_mostly = CONFIG_DEVFREQ_INPUT_BOOST_DURATION_MS;
static unsigned int devfreq_thread_prio __read_mostly = CONFIG_DEVFREQ_THREAD_PRIORITY;
static unsigned int devfreq_boost_freq_low  __read_mostly = CONFIG_DEVFREQ_MSM_CPUBW_BOOST_FREQ_LOW;
static unsigned int devfreq_boost_freq __read_mostly = CONFIG_DEVFREQ_MSM_CPUBW_BOOST_FREQ;
static unsigned int devfreq_boost_ddr_freq_low  __read_mostly = CONFIG_DEVFREQ_MSM_DDRBW_BOOST_FREQ_LOW;
static unsigned int devfreq_boost_ddr_freq __read_mostly = CONFIG_DEVFREQ_MSM_DDRBW_BOOST_FREQ;
static unsigned int devfreq_boost_gpu_freq_low  __read_mostly = CONFIG_DEVFREQ_MSM_GPUBW_BOOST_FREQ_LOW;
static unsigned int devfreq_boost_gpu_freq __read_mostly = CONFIG_DEVFREQ_MSM_GPUBW_BOOST_FREQ;

module_param(flex_boost_duration, short, 0644);
module_param(input_boost_duration, short, 0644);
module_param(devfreq_boost_freq, uint, 0644);
module_param(devfreq_boost_freq_low, uint, 0644);

enum {
	SCREEN_OFF,
	INPUT_BOOST,
	MAX_BOOST
};

struct boost_dev {
	struct devfreq *df;
	struct delayed_work input_unboost;
	struct delayed_work max_unboost;
	struct work_struct boost;
	unsigned long *boost_freq;
	unsigned long *boost_freq_low;
	wait_queue_head_t boost_waitq;
	atomic_long_t max_boost_expires;
	unsigned long boost_freq;
	unsigned long state;
};

struct df_boost_drv {
	struct boost_dev devices[DEVFREQ_MAX];
	struct notifier_block fb_notif;
};

static void devfreq_input_unboost(struct work_struct *work);
static void devfreq_max_unboost(struct work_struct *work);

#define BOOST_DEV_INIT(b, dev) .devices[dev] = {				\
	.input_unboost =							\
		__DELAYED_WORK_INITIALIZER((b).devices[dev].input_unboost,	\
					   devfreq_input_unboost, 0),		\
	.max_unboost =								\
		__DELAYED_WORK_INITIALIZER((b).devices[dev].max_unboost,	\
					   devfreq_max_unboost, 0),		\
	.boost_waitq =								\
		__WAIT_QUEUE_HEAD_INITIALIZER((b).devices[dev].boost_waitq),	\
	.boost_freq = freq							\
}

static struct df_boost_drv df_boost_drv_g __read_mostly = {
	BOOST_DEV_INIT(df_boost_drv_g, DEVFREQ_MSM_CPUBW),
	BOOST_DEV_INIT(df_boost_drv_g, DEVFREQ_MSM_DDRBW),
	BOOST_DEV_INIT(df_boost_drv_g, DEVFREQ_MSM_GPUBW)
};

static void __devfreq_boost_kick(struct boost_dev *b)
{
	if (!READ_ONCE(b->df) || test_bit(SCREEN_OFF, &b->state))
		return;

	set_bit(INPUT_BOOST, &b->state);
	if (!mod_delayed_work(system_unbound_wq, &b->input_unboost,
		msecs_to_jiffies(CONFIG_DEVFREQ_INPUT_BOOST_DURATION_MS)))
		wake_up(&b->boost_waitq);
}

void devfreq_boost_kick(enum df_device device)
{
	struct df_boost_drv *d = &df_boost_drv_g;

	__devfreq_boost_kick(d->devices + device);
}

static void __devfreq_boost_kick_max(struct boost_dev *b,
				     unsigned int duration_ms)
{
	unsigned long boost_jiffies = msecs_to_jiffies(duration_ms);
	unsigned long curr_expires, new_expires;

	if (!READ_ONCE(b->df) || test_bit(SCREEN_OFF, &b->state))
		return;

	do {
		curr_expires = atomic_long_read(&b->max_boost_expires);
		new_expires = jiffies + boost_jiffies;

		/* Skip this boost if there's a longer boost in effect */
		if (time_after(curr_expires, new_expires))
			return;
	} while (atomic_long_cmpxchg(&b->max_boost_expires, curr_expires,
				     new_expires) != curr_expires);

	set_bit(MAX_BOOST, &b->state);
	if (!mod_delayed_work(system_unbound_wq, &b->max_unboost,
			      boost_jiffies))
		wake_up(&b->boost_waitq);
}

void devfreq_boost_kick_max(enum df_device device, unsigned int duration_ms)
{
	struct df_boost_drv *d = &df_boost_drv_g;

	__devfreq_boost_kick_max(d->devices + device, duration_ms);
}

void devfreq_register_boost_device(enum df_device device, struct devfreq *df)
{
	struct df_boost_drv *d = &df_boost_drv_g;
	struct boost_dev *b;

	df->is_boost_device = true;
	b = d->devices + device;
	WRITE_ONCE(b->df, df);
}

static void devfreq_input_unboost(struct work_struct *work)
{
	struct boost_dev *b = container_of(to_delayed_work(work),
					   typeof(*b), input_unboost);

	clear_bit(INPUT_BOOST, &b->state);
	wake_up(&b->boost_waitq);
}

static void devfreq_max_unboost(struct work_struct *work)
{
	struct boost_dev *b = container_of(to_delayed_work(work),
					   typeof(*b), max_unboost);

	clear_bit(MAX_BOOST, &b->state);
	wake_up(&b->boost_waitq);
}

static void devfreq_update_boosts(struct boost_dev *b, unsigned long state)
{
	struct devfreq *df = b->df;
	if (!READ_ONCE(b->df))
		return;
	if (!test_bit(SCREEN_ON, &state)) {
		mutex_lock(&df->lock);
		df->min_freq = df->profile->freq_table[0];
		df->max_boost = test_bit(WAKE_BOOST, &state) ? 
					true :
					false;
		update_devfreq(df);
		mutex_unlock(&df->lock);
	} else {
		mutex_lock(&df->lock);
		df->min_freq = test_bit(FLEX_BOOST, &state) ?
			b->boost_freq_low :
			df->profile->freq_table[0];
		df->min_freq = test_bit(INPUT_BOOST, &state) ?
			b->boost_freq :
			df->profile->freq_table[0];
		df->max_boost = test_bit(MAX_BOOST, &state);
		update_devfreq(df);
		mutex_unlock(&df->lock);
	}
}

	mutex_lock(&df->lock);
	if (test_bit(SCREEN_OFF, &state)) {
		df->min_freq = df->profile->freq_table[0];
		df->max_boost = false;
	} else {
		df->min_freq = test_bit(INPUT_BOOST, &state) ?
			       min(b->boost_freq, df->max_freq) :
			       df->profile->freq_table[0];
		df->max_boost = test_bit(MAX_BOOST, &state);
	}
	update_devfreq(df);
	mutex_unlock(&df->lock);
}

static int devfreq_boost_thread(void *data)
{
	static const struct sched_param sched_max_rt_prio = {
		.sched_priority = MAX_RT_PRIO - 1
	};
	struct boost_dev *b = data;
	unsigned long old_state = 0;

	sched_setscheduler_nocheck(current, SCHED_FIFO, &sched_max_rt_prio);

	while (1) {
		bool should_stop = false;
		unsigned long curr_state;

		wait_event(b->boost_waitq,
			(curr_state = READ_ONCE(b->state)) != old_state ||
			(should_stop = kthread_should_stop()));

		if (should_stop)
			break;

		old_state = curr_state;
		devfreq_update_boosts(b, curr_state);
	}

	return 0;
}

static int fb_notifier_cb(struct notifier_block *nb, unsigned long action,
			  void *data)
{
	struct df_boost_drv *d = container_of(nb, typeof(*d), fb_notif);
	int i, *blank = ((struct fb_event *)data)->data;

	/* Parse framebuffer blank events as soon as they occur */
	if (action != FB_EARLY_EVENT_BLANK)
		return NOTIFY_OK;

	/* Boost when the screen turns on and unboost when it turns off */
	for (i = 0; i < DEVFREQ_MAX; i++) {
		struct boost_dev *b = d->devices + i;

		if (*blank == FB_BLANK_UNBLANK) {
			clear_bit(SCREEN_OFF, &b->state);
			__devfreq_boost_kick_max(b,
				CONFIG_DEVFREQ_WAKE_BOOST_DURATION_MS);
		} else {
			set_bit(SCREEN_OFF, &b->state);
			wake_up(&b->boost_waitq);
		}
	}

	return NOTIFY_OK;
}

static void devfreq_boost_input_event(struct input_handle *handle,
				      unsigned int type, unsigned int code,
				      int value)
{
	struct df_boost_drv *d = handle->handler->private;
	int i;

	for (i = 0; i < DEVFREQ_MAX; i++)
		__devfreq_boost_kick(d->devices + i);
}

static int devfreq_boost_input_connect(struct input_handler *handler,
				       struct input_dev *dev,
				       const struct input_device_id *id)
{
	struct input_handle *handle;
	int ret;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "devfreq_boost_handle";

	ret = input_register_handle(handle);
	if (ret)
		goto free_handle;

	ret = input_open_device(handle);
	if (ret)
		goto unregister_handle;

	return 0;

unregister_handle:
	input_unregister_handle(handle);
free_handle:
	kfree(handle);
	return ret;
}

static void devfreq_boost_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id devfreq_boost_ids[] = {
	/* Multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y) }
	},
	/* Touchpad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) }
	},
	/* Keypad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) }
	},
	{ }
};

static struct input_handler devfreq_boost_input_handler = {
	.event		= devfreq_boost_input_event,
	.connect	= devfreq_boost_input_connect,
	.disconnect	= devfreq_boost_input_disconnect,
	.name		= "devfreq_boost_handler",
	.id_table	= devfreq_boost_ids
};

static int __init devfreq_boost_init(void)
{
	struct df_boost_drv *d = &df_boost_drv_g;
	struct task_struct *thread[DEVFREQ_MAX];
	int i, ret;

	for (i = 0; i < DEVFREQ_MAX; i++) {
		struct boost_dev *b = d->devices + i;

		thread[i] = kthread_run(devfreq_boost_thread, b,
					"devfreq_boostd/%d", i);
		b->wq_i = alloc_workqueue("devfreq_boost_wq_i", WQ_POWER_EFFICIENT, 0);
		b->wq_f = alloc_workqueue("devfreq_boost_wq_f", WQ_POWER_EFFICIENT, 0);
		b->wq_m = alloc_workqueue("devfreq_boost_wq_m", WQ_POWER_EFFICIENT, 0);
		
		if (i==DEVFREQ_MSM_CPUBW) {
			b->boost_freq = &devfreq_boost_freq;
			b->boost_freq_low = &devfreq_boost_freq_low;
		}
		if (i==DEVFREQ_MSM_DDRBW) {
			b->boost_freq = &devfreq_boost_ddr_freq;
			b->boost_freq_low = &devfreq_boost_ddr_freq_low;
		}
		if (i==DEVFREQ_MSM_GPUBW) {
			b->boost_freq = &devfreq_boost_gpu_freq;
			b->boost_freq_low = &devfreq_boost_gpu_freq_low;
		}
		
		thread[i] = kthread_run_low_power(devfreq_boost_thread, b,
						      "devfreq_boostd/%d", i);
		if (IS_ERR(thread[i])) {
			ret = PTR_ERR(thread[i]);
			pr_err("Failed to create kthread, err: %d\n", ret);
			goto stop_kthreads;
		}
	}

	devfreq_boost_input_handler.private = d;
	ret = input_register_handler(&devfreq_boost_input_handler);
	if (ret) {
		pr_err("Failed to register input handler, err: %d\n", ret);
		goto stop_kthreads;
	}

	d->fb_notif.notifier_call = fb_notifier_cb;
	d->fb_notif.priority = INT_MAX;
	ret = fb_register_client(&d->fb_notif);
	if (ret) {
		pr_err("Failed to register fb notifier, err: %d\n", ret);
		goto unregister_handler;
	}

	return 0;

unregister_handler:
	input_unregister_handler(&devfreq_boost_input_handler);
stop_kthreads:
	while (i--)
		kthread_stop(thread[i]);
	return ret;
}
late_initcall(devfreq_boost_init);
