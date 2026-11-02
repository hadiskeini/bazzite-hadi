// SPDX-License-Identifier: GPL-2.0-only
/*
 * The Mac's battery in the VM, as BAT0 and ADP0 (for UPower and Omarchy's bar).
 *
 * From try-omarchy (github.com/omacom/try-omarchy), (c) Try Omarchy
 * contributors: guest/native-module/try-omarchy-battery, renamed for OmacVM.
 * GPL-2.0-only, as the original file (the rest of try-omarchy is MIT).
 *
 * A root-only agent writes one whole snapshot per write() to the `state`
 * attribute:
 *
 *   present=1 status=discharging capacity=57 ac=0 time_to_empty=8100 time_to_full=-1
 *   present=0 ac=1
 *
 * One write is one consistent snapshot: consumers can never observe a new
 * percentage beside a stale charging flag. -1 means no estimate. A malformed
 * line is rejected whole and the previous state is retained.
 *
 * Since 1.1.0 a line may also carry current_now (µA, signed: below 0 while
 * the battery gives power) and power_now (µW). Left out: not known. With
 * them UPower has a real rate, so watts and time left; without, it guesses
 * one from charge steps, and the guess is noise. The agent sends them only to
 * 1.1.0 or newer (an older module rejects the whole line).
 *
 * Lock ordering: omb_register_lock -> omb_state_lock. get_property() takes
 * only omb_state_lock; power_supply registration calls take only
 * omb_register_lock, never while omb_state_lock is held, because
 * power_supply_unregister() waits for readers holding omb_state_lock.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/string.h>

struct omb_state {
	bool present;
	int status;
	int capacity;
	bool ac_online;
	int time_to_empty;
	int time_to_full;
	int charge_limit;
	int charge_now;
	int charge_full;
	int charge_full_design;
	int voltage_now;
	int cycle_count;
	bool has_current;
	int current_now;
	int power_now;
};

static struct platform_device *omb_pdev;
static struct power_supply *omb_ac;
static struct power_supply *omb_bat;
static DEFINE_MUTEX(omb_register_lock);	/* serializes writers + registration */
static DEFINE_MUTEX(omb_state_lock);	/* guards omb_state */
static struct omb_state omb_state = {
	.present = false,
	.status = POWER_SUPPLY_STATUS_UNKNOWN,
	.capacity = 0,
	.ac_online = true,
	.time_to_empty = -1,
	.time_to_full = -1,
	.charge_limit = -1,
	.charge_now = -1,
	.charge_full = -1,
	.charge_full_design = -1,
	.voltage_now = -1,
	.cycle_count = -1,
	.has_current = false,
	.current_now = 0,
	.power_now = -1,
};

static const struct {
	const char *token;
	int status;
} omb_status_tokens[] = {
	{ "charging", POWER_SUPPLY_STATUS_CHARGING },
	{ "discharging", POWER_SUPPLY_STATUS_DISCHARGING },
	{ "full", POWER_SUPPLY_STATUS_FULL },
	{ "not-charging", POWER_SUPPLY_STATUS_NOT_CHARGING },
	{ "unknown", POWER_SUPPLY_STATUS_UNKNOWN },
};

static const char *omb_status_token(int status)
{
	size_t index;

	for (index = 0; index < ARRAY_SIZE(omb_status_tokens); index++)
		if (omb_status_tokens[index].status == status)
			return omb_status_tokens[index].token;
	return "unknown";
}

static enum power_supply_property omb_bat_properties[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG,
	POWER_SUPPLY_PROP_TIME_TO_FULL_AVG,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
	POWER_SUPPLY_PROP_TIME_TO_FULL_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_POWER_NOW,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_MODEL_NAME,
};

static enum power_supply_property omb_ac_properties[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static int omb_bat_get_property(struct power_supply *psy,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	int error = 0;

	mutex_lock(&omb_state_lock);
	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = omb_state.status;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = omb_state.present ? 1 : 0;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = omb_state.capacity;
		break;
	case POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG:
	case POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW:
		if (omb_state.time_to_empty < 0)
			error = -ENODATA;
		else
			val->intval = omb_state.time_to_empty;
		break;
	case POWER_SUPPLY_PROP_TIME_TO_FULL_AVG:
	case POWER_SUPPLY_PROP_TIME_TO_FULL_NOW:
		if (omb_state.time_to_full < 0)
			error = -ENODATA;
		else
			val->intval = omb_state.time_to_full;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		/* The kernel's sign: below 0 while discharging. Only then, so
		 * UPower (current < 0 means discharging) never contradicts the
		 * Mac's status, e.g. on the charger with the battery helping. */
		if (!omb_state.has_current) {
			error = -ENODATA;
			break;
		}
		val->intval = abs(omb_state.current_now);
		if (omb_state.status == POWER_SUPPLY_STATUS_DISCHARGING)
			val->intval = -val->intval;
		break;
	case POWER_SUPPLY_PROP_POWER_NOW:
		val->intval = omb_state.power_now;
		if (val->intval < 0)
			error = -ENODATA;
		break;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD:
		/* No limit in macOS: 100, the driver default ("charges to
		 * full"), rather than no value, which tools read as unknown. */
		val->intval = omb_state.charge_limit < 0 ? 100 : omb_state.charge_limit;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		val->intval = omb_state.charge_now;
		if (val->intval < 0)
			error = -ENODATA;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		val->intval = omb_state.charge_full;
		if (val->intval < 0)
			error = -ENODATA;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = omb_state.charge_full_design;
		if (val->intval < 0)
			error = -ENODATA;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = omb_state.voltage_now;
		if (val->intval < 0)
			error = -ENODATA;
		break;
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		val->intval = omb_state.cycle_count;
		if (val->intval < 0)
			error = -ENODATA;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "Apple";
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "Mac Battery";
		break;
	default:
		error = -EINVAL;
		break;
	}
	mutex_unlock(&omb_state_lock);
	return error;
}

static int omb_ac_get_property(struct power_supply *psy,
			       enum power_supply_property psp,
			       union power_supply_propval *val)
{
	if (psp != POWER_SUPPLY_PROP_ONLINE)
		return -EINVAL;
	mutex_lock(&omb_state_lock);
	val->intval = omb_state.ac_online ? 1 : 0;
	mutex_unlock(&omb_state_lock);
	return 0;
}

static const struct power_supply_desc omb_bat_desc = {
	.name = "BAT0",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = omb_bat_properties,
	.num_properties = ARRAY_SIZE(omb_bat_properties),
	.get_property = omb_bat_get_property,
};

static const struct power_supply_desc omb_ac_desc = {
	.name = "ADP0",
	.type = POWER_SUPPLY_TYPE_MAINS,
	.properties = omb_ac_properties,
	.num_properties = ARRAY_SIZE(omb_ac_properties),
	.get_property = omb_ac_get_property,
};

static int omb_parse(const char *buf, size_t count, struct omb_state *next)
{
	bool saw_present = false, saw_ac = false;
	bool saw_status = false, saw_capacity = false;
	char *copy, *cursor, *token;
	int error = -EINVAL;

	next->present = false;
	next->status = POWER_SUPPLY_STATUS_UNKNOWN;
	next->capacity = 0;
	next->ac_online = false;
	next->time_to_empty = -1;
	next->time_to_full = -1;
	next->charge_limit = -1;
	next->charge_now = -1;
	next->charge_full = -1;
	next->charge_full_design = -1;
	next->voltage_now = -1;
	next->cycle_count = -1;
	next->has_current = false;
	next->current_now = 0;
	next->power_now = -1;

	copy = kstrndup(buf, count, GFP_KERNEL);
	if (!copy)
		return -ENOMEM;
	cursor = copy;
	while ((token = strsep(&cursor, " \n")) != NULL) {
		char *value;

		if (!*token)
			continue;
		value = strchr(token, '=');
		if (!value)
			goto out;
		*value++ = '\0';
		if (!strcmp(token, "present")) {
			if (kstrtobool(value, &next->present))
				goto out;
			saw_present = true;
		} else if (!strcmp(token, "ac")) {
			if (kstrtobool(value, &next->ac_online))
				goto out;
			saw_ac = true;
		} else if (!strcmp(token, "status")) {
			size_t index;

			for (index = 0; index < ARRAY_SIZE(omb_status_tokens); index++)
				if (!strcmp(value, omb_status_tokens[index].token))
					break;
			if (index == ARRAY_SIZE(omb_status_tokens))
				goto out;
			next->status = omb_status_tokens[index].status;
			saw_status = true;
		} else if (!strcmp(token, "capacity")) {
			if (kstrtoint(value, 10, &next->capacity) ||
			    next->capacity < 0 || next->capacity > 100)
				goto out;
			saw_capacity = true;
		} else if (!strcmp(token, "charge_limit")) {
			if (kstrtoint(value, 10, &next->charge_limit) ||
			    (next->charge_limit != -1 &&
			     (next->charge_limit < 1 || next->charge_limit > 99)))
				goto out;
		} else if (!strcmp(token, "charge_now") ||
			   !strcmp(token, "charge_full") ||
			   !strcmp(token, "charge_full_design") ||
			   !strcmp(token, "voltage_now") ||
			   !strcmp(token, "cycle_count")) {
			int *target;
			bool allow_zero = false;

			if (!strcmp(token, "charge_now")) {
				target = &next->charge_now;
				allow_zero = true;
			} else if (!strcmp(token, "charge_full")) {
				target = &next->charge_full;
			} else if (!strcmp(token, "charge_full_design")) {
				target = &next->charge_full_design;
			} else if (!strcmp(token, "voltage_now")) {
				target = &next->voltage_now;
			} else {
				target = &next->cycle_count;
				allow_zero = true;
			}
			if (kstrtoint(value, 10, target) || *target < -1 ||
			    (!allow_zero && *target == 0))
				goto out;
		} else if (!strcmp(token, "current_now")) {
			/* INT_MIN has no positive twin for abs(). */
			if (kstrtoint(value, 10, &next->current_now) ||
			    next->current_now == INT_MIN)
				goto out;
			next->has_current = true;
		} else if (!strcmp(token, "power_now")) {
			if (kstrtoint(value, 10, &next->power_now) ||
			    next->power_now < -1)
				goto out;
		} else if (!strcmp(token, "time_to_empty")) {
			if (kstrtoint(value, 10, &next->time_to_empty) ||
			    next->time_to_empty < -1)
				goto out;
		} else if (!strcmp(token, "time_to_full")) {
			if (kstrtoint(value, 10, &next->time_to_full) ||
			    next->time_to_full < -1)
				goto out;
		} else {
			goto out;
		}
	}
	if (!saw_present || !saw_ac)
		goto out;
	if (next->present && (!saw_status || !saw_capacity))
		goto out;
	error = 0;
out:
	kfree(copy);
	return error;
}

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct omb_state snapshot;
	int length;

	mutex_lock(&omb_state_lock);
	snapshot = omb_state;
	mutex_unlock(&omb_state_lock);
	if (!snapshot.present)
		return sysfs_emit(buf, "present=0 ac=%d\n",
				  snapshot.ac_online ? 1 : 0);
	length = sysfs_emit(buf,
			    "present=1 status=%s capacity=%d ac=%d time_to_empty=%d time_to_full=%d charge_limit=%d charge_now=%d charge_full=%d charge_full_design=%d voltage_now=%d cycle_count=%d",
			    omb_status_token(snapshot.status), snapshot.capacity,
			    snapshot.ac_online ? 1 : 0, snapshot.time_to_empty,
			    snapshot.time_to_full, snapshot.charge_limit,
			    snapshot.charge_now, snapshot.charge_full,
			    snapshot.charge_full_design, snapshot.voltage_now,
			    snapshot.cycle_count);
	/* What the agent wrote: left out when not known. */
	if (snapshot.has_current)
		length += sysfs_emit_at(buf, length, " current_now=%d",
					snapshot.current_now);
	if (snapshot.power_now >= 0)
		length += sysfs_emit_at(buf, length, " power_now=%d",
					snapshot.power_now);
	length += sysfs_emit_at(buf, length, "\n");
	return length;
}

static ssize_t state_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct omb_state next;
	bool ac_changed, bat_changed;
	int error;

	error = omb_parse(buf, count, &next);
	if (error)
		return error;

	mutex_lock(&omb_register_lock);
	mutex_lock(&omb_state_lock);
	ac_changed = next.ac_online != omb_state.ac_online;
	bat_changed = next.present != omb_state.present ||
		      next.status != omb_state.status ||
		      next.capacity != omb_state.capacity ||
		      next.time_to_empty != omb_state.time_to_empty ||
		      next.time_to_full != omb_state.time_to_full ||
		      next.charge_limit != omb_state.charge_limit ||
		      next.charge_now != omb_state.charge_now ||
		      next.charge_full != omb_state.charge_full ||
		      next.charge_full_design != omb_state.charge_full_design ||
		      next.voltage_now != omb_state.voltage_now ||
		      next.cycle_count != omb_state.cycle_count ||
		      next.has_current != omb_state.has_current ||
		      next.current_now != omb_state.current_now ||
		      next.power_now != omb_state.power_now;
	omb_state = next;
	mutex_unlock(&omb_state_lock);

	/* Registration outside omb_state_lock: unregister waits for readers. */
	if (next.present && !omb_bat) {
		struct power_supply_config config = {};
		struct power_supply *battery;

		battery = power_supply_register(&omb_pdev->dev, &omb_bat_desc,
						&config);
		if (IS_ERR(battery)) {
			error = PTR_ERR(battery);
			mutex_lock(&omb_state_lock);
			omb_state.present = false;
			mutex_unlock(&omb_state_lock);
			mutex_unlock(&omb_register_lock);
			return error;
		}
		omb_bat = battery;
		bat_changed = false; /* registration already notified */
	} else if (!next.present && omb_bat) {
		power_supply_unregister(omb_bat);
		omb_bat = NULL;
		bat_changed = false;
	}
	if (bat_changed && omb_bat)
		power_supply_changed(omb_bat);
	if (ac_changed && omb_ac)
		power_supply_changed(omb_ac);
	mutex_unlock(&omb_register_lock);
	return count;
}

static DEVICE_ATTR_ADMIN_RW(state);

static int __init omb_init(void)
{
	struct power_supply_config config = {};
	int error;

	omb_pdev = platform_device_register_simple("omacvm-battery", -1,
						   NULL, 0);
	if (IS_ERR(omb_pdev))
		return PTR_ERR(omb_pdev);

	error = device_create_file(&omb_pdev->dev, &dev_attr_state);
	if (error)
		goto unregister_pdev;

	omb_ac = power_supply_register(&omb_pdev->dev, &omb_ac_desc, &config);
	if (IS_ERR(omb_ac)) {
		error = PTR_ERR(omb_ac);
		omb_ac = NULL;
		goto remove_file;
	}
	/* BAT0 appears on the first present=1 snapshot; a desktop Mac never
	 * creates it, so the guest bar has nothing to render. */
	return 0;

remove_file:
	device_remove_file(&omb_pdev->dev, &dev_attr_state);
unregister_pdev:
	platform_device_unregister(omb_pdev);
	return error;
}

static void __exit omb_exit(void)
{
	/* Removing the attribute drains in-flight state_store writers, so
	 * nothing can touch the supplies or the platform device below. */
	device_remove_file(&omb_pdev->dev, &dev_attr_state);
	mutex_lock(&omb_register_lock);
	if (omb_bat) {
		power_supply_unregister(omb_bat);
		omb_bat = NULL;
	}
	mutex_unlock(&omb_register_lock);
	power_supply_unregister(omb_ac);
	platform_device_unregister(omb_pdev);
}

module_init(omb_init);
module_exit(omb_exit);

MODULE_AUTHOR("Try Omarchy contributors, OmacVM");
MODULE_DESCRIPTION("Mirror the host Mac's battery as guest BAT0/ADP0");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.1.0");
