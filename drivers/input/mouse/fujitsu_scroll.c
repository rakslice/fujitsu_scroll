// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fujitsu Scroll Devices PS/2 mouse driver
 *
 *   2021 Sam Mertens <smertens.public@gmail.com>
 *     Used the original synaptics.c source as a framework to support
 *     the Fujitsu scroll devices in the Fujitsu Lifebook T901/P772
 *     laptops
 *
 *
 * Trademarks are the property of their respective owners.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/dmi.h>
#include <linux/serio.h>
#include <linux/libps2.h>
#include <linux/rmi.h>
#include <linux/slab.h>
#include "psmouse.h"
#include "fujitsu_scroll.h"

/* #ifdef CONFIG_MOUSE_PS2_FUJITSU_SCROLL */

static short fujitsu_capacitance = FJS_CAPACITANCE_THRESHOLD;
static short fujitsu_threshold = FJS_POSITION_CHANGE_THRESHOLD;
//static short fujitsu_bitshift = FJS_MOVEMENT_BITSHIFT;
static short fujitsu_movement_divisor = FJS_MOVEMENT_DIVISOR;

module_param(fujitsu_capacitance, short, 0644);
MODULE_PARM_DESC(fujitsu_capaciance, "Capacitance threshold");
module_param(fujitsu_threshold, short, 0644);
MODULE_PARM_DESC(fujitsu_threshold, "Change threshold");
//module_param(fujitsu_bitshift, short, 0644);
//MODULE_PARM_DESC(fujitsu_bitshift, "Movement bitshift (reducer)");
module_param(fujitsu_movement_divisor, short, 0644);
MODULE_PARM_DESC(fujitsu_movement_divisor, "Movement divisor (reducer)");

int fujitsu_scroll_detect(struct psmouse *psmouse, bool set_properties)
{
	struct ps2dev *ps2dev = &psmouse->ps2dev;
	u8 param[4] = { 0 };

/*	printk(KERN_INFO "HERE\n"); */

#if defined(CONFIG_DMI) && defined(CONFIG_X86)
	struct dmi_system_id present_dmi_table[] = {
		{
		 .matches = {
			     DMI_MATCH(DMI_SYS_VENDOR, "FUJITSU"),
			     },
		  },
		{ }
	};

	if (!dmi_check_system(present_dmi_table))
		return -ENODEV;
#endif

	/* printk(KERN_INFO "HERE\n"); */

	ps2_command(ps2dev, param, PSMOUSE_CMD_SETRES);
	ps2_command(ps2dev, param, PSMOUSE_CMD_SETRES);
	ps2_command(ps2dev, param, PSMOUSE_CMD_SETRES);
	ps2_command(ps2dev, param, PSMOUSE_CMD_SETRES);
	ps2_command(ps2dev, param, PSMOUSE_CMD_GETINFO);

	if (param[1] != FUJITSU_SCROLL_ID)
		return -ENODEV;

	if (set_properties) {
		psmouse->vendor = "Fujitsu";
		switch (param[0]) {
		case FUJITSU_SCROLL_WHEEL_ID:
			psmouse->name = "Scroll Wheel";
			__set_bit(FJS_WHEEL_AXIS, psmouse->dev->relbit);
			break;
		case FUJITSU_SCROLL_SENSOR_ID:
			psmouse->name = "Scroll Sensor";
			__set_bit(FJS_SENSOR_AXIS, psmouse->dev->relbit);
			break;
		default:
			psmouse->name = "Unknown";
		}
	}

	return 0;
}

void fujitsu_scroll_init_sequence(struct psmouse *psmouse)
{
	struct ps2dev *ps2dev = &psmouse->ps2dev;
	u8 param[4] = { 0 };
	int error;

	error = ps2_sliced_command(ps2dev, FJS_INIT_MODE);
	param[0] = 0x14;
	ps2_command(ps2dev, param, PSMOUSE_CMD_SETRATE);
}

int fujitsu_scroll_query_hardware(struct psmouse *psmouse)
{
	struct ps2dev *ps2dev = &psmouse->ps2dev;
	struct fujitsu_scroll_data *priv = psmouse->private;
	u8 param[4];

	ps2_sliced_command(ps2dev, 0);
	ps2_command(ps2dev, param, PSMOUSE_CMD_GETINFO);

	if (param[0] == FUJITSU_SCROLL_WHEEL_ID) {
		priv->type = FUJITSU_SCROLL_WHEEL;
		priv->axis = FJS_WHEEL_AXIS;
	} else {
		priv->type = FUJITSU_SCROLL_SENSOR;
		priv->axis = FJS_SENSOR_AXIS;
	}

	return 0;
}


/*****************************************************************************
 *	Functions to interpret the packets
 ****************************************************************************/

int shortest_circle_movement(int new_pos, int prev_pos)
{
	int movement = new_pos - prev_pos;
	if (movement > FJS_MAX_POS_CHG) {
		movement -= FJS_RANGE;
	} else if (movement < -FJS_MAX_POS_CHG) {
		movement += FJS_RANGE;
	}
	return movement;
}

/*
#define FUJITSU_SCROLL_DEBUG 1
*/

/*
 *  called for each full received packet from the device
 */
static void fujitsu_scroll_process_packet(struct psmouse *psmouse)
{
	struct input_dev *dev = psmouse->dev;
	struct fujitsu_scroll_data *priv = psmouse->private;

	unsigned int capacitance;
	unsigned int position;
	unsigned int guard_area_touched;
	
	int movement;
	int movement_since_start;
	int device_movement;

	position = ((psmouse->packet[1] & 0x0f) << 8) + psmouse->packet[2];
	capacitance = psmouse->packet[0] & 0x3f;
	guard_area_touched = psmouse->packet[4] & 0x10;

#ifdef FUJITSU_SCROLL_DEBUG
	if (guard_area_touched && !priv->guard_area_touched_prev) {
		printk(KERN_INFO "guard touch start pos %d\n", position); 
	}
	else if (!guard_area_touched && priv->guard_area_touched_prev) {
		printk(KERN_INFO "guard touch end pos %d\n", position); 
	}
#endif
	if (capacitance >= fujitsu_capacitance) {
	  if (!priv->finger_down) {
	    priv->finger_down = 1;
	    priv->last_event_position = position;
	    priv->event_start_position = position;
	    priv->ignore_event = priv->guard_area_touched_prev? 1 : 0;
	    priv->capacitance_avg = capacitance;
#ifdef FUJITSU_SCROLL_DEBUG
	    printk(KERN_INFO "touch event start pos %d ignore %d gatp %d\n", position, priv->ignore_event,
			    priv->guard_area_touched_prev); 
#endif
	  } else {

	    if (priv->type == FUJITSU_SCROLL_WHEEL) { // scroll wheel
		movement = shortest_circle_movement(position, priv->last_event_position);
	   	movement_since_start = shortest_circle_movement(position, priv->event_start_position);
	    } else {  // scroll sensor
	    	movement = position - priv->last_event_position;
	   	movement_since_start = position - priv->event_start_position;
	    }

	    if (!priv->changed_enough) {
	    	if (movement_since_start > fujitsu_threshold ||
		    movement_since_start < -fujitsu_threshold) {
			priv->changed_enough = 1;
#ifdef FUJITSU_SCROLL_DEBUG
	    		printk(KERN_INFO "past movement threshold pos %d\n", position); 
#endif
		}
	    }

	    if (guard_area_touched) {
		    if (!priv->ignore_event) {
		    	priv->ignore_event = 1;
#ifdef FUJITSU_SCROLL_DEBUG
	    		printk(KERN_INFO "guard touched, ignoring touch event, pos %d\n", position); 
#endif
		    }
	    }

	    if (capacitance > FJS_CAPACITANCE_PALM_IGNORE_THRESHOLD) {
		    if (!priv->ignore_event) {
		    	priv->ignore_event = 1;
#ifdef FUJITSU_SCROLL_DEBUG
	    		printk(KERN_INFO "cap over palm ignore thres, ignoring touch event, pos %d cap %d\n",
					position, capacitance); 
#endif
		    }
	    }

	    if (priv->changed_enough) {
	      device_movement = - movement / fujitsu_movement_divisor;
	      /*
	      if (movement < 0) {
		      shifted_move = -((-movement) >> fujitsu_bitshift);
	      } else { 
		      shifted_move = movement >> fujitsu_bitshift;
	      }
	      */
	     
	      if ((device_movement != 0) && !priv->ignore_event) {
	      	input_report_rel(dev, priv->axis, device_movement);
	      	priv->last_event_position = position;
	      }

#ifdef FUJITSU_SCROLL_DEBUG
	      priv->capacitance_avg = (priv->capacitance_avg * 7 + capacitance) / 8;
#endif
	    }
	  }
	} else if (1 == priv->finger_down) {
	  if (guard_area_touched) {
		  priv->ignore_event = 1;
	  }
#ifdef FUJITSU_SCROLL_DEBUG
	  printk(KERN_INFO "touch event end reason %s",
			  priv->ignore_event? "guard engaged" : "touch stopped"
			  ); 
	  printk(KERN_INFO "pos %d cap avg %d",
			  position,
			  priv->capacitance_avg
			  ); 
#endif
	  priv->finger_down = 0;
          if (priv->changed_enough) {
	   	movement_since_start = shortest_circle_movement(position, priv->event_start_position);
#ifdef FUJITSU_SCROLL_DEBUG
	  	printk(KERN_INFO "moved %d\n", movement_since_start); 
#endif
	  }
	  priv->changed_enough = 0;
#ifdef FUJITSU_SCROLL_DEBUG
	  	printk(KERN_INFO "");
#endif
	}

	priv->guard_area_touched_prev = guard_area_touched? 1 : 0;
	
	input_sync(dev);
}

static psmouse_ret_t fujitsu_scroll_process_byte(struct psmouse *psmouse)
{
	if (psmouse->pktcnt >= FJS_PACKET_SIZE) {	/* Full packet received */
		fujitsu_scroll_process_packet(psmouse);
		return PSMOUSE_FULL_PACKET;
	}

	return PSMOUSE_GOOD_DATA;
}

/*****************************************************************************
 *	Driver initialization/cleanup functions
 ****************************************************************************/

static void fujitsu_scroll_disconnect(struct psmouse *psmouse)
{
	struct fujitsu_scroll_data *priv = psmouse->private;

	psmouse_reset(psmouse);
	kfree(priv);
	psmouse->private = NULL;
}

static int fujitsu_scroll_reconnect(struct psmouse *psmouse)
{
	psmouse_reset(psmouse);
	fujitsu_scroll_init_sequence(psmouse);

	return 0;
}

int fujitsu_scroll_init(struct psmouse *psmouse)
{
	struct fujitsu_scroll_data *priv;

	psmouse_reset(psmouse);

	psmouse->private = priv = kzalloc(sizeof(struct fujitsu_scroll_data),
					  GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	psmouse->protocol_handler = fujitsu_scroll_process_byte;
	psmouse->pktsize = FJS_PACKET_SIZE;

	psmouse->disconnect = fujitsu_scroll_disconnect;
	psmouse->reconnect = fujitsu_scroll_reconnect;
	psmouse->resync_time = 0;

	fujitsu_scroll_query_hardware(psmouse);
	input_set_capability(psmouse->dev, EV_REL, priv->axis);
	fujitsu_scroll_init_sequence(psmouse);

	return 0;
}
/*
#endif */ /* CONFIG_MOUSE_PS2_FUJITSU_SCROLL */
