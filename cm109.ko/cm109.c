/*
 * Driver for the VoIP USB phones with CM109 chipsets.
 *
 * Copyright (C) 2007 - 2008 Alfred E. Heggestad <aeh@db.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 2.
 */

/*
 *   Tested devices:
 * - Komunikate KIP1000
 * - Genius G-talk
 * - Allied-Telesis Corega USBPH01
 * - Topcom Webt@lker 211 (keypad works but not the screen)
 * - ...
 * If device is handled by usbhid driver instead of this driver,
 * see remark near definition of constant VENDOR_ID, on how to resolve the
 * problem
 *
 * This driver is based on the yealink.c driver
 *
 * Thanks to:
 *   - Authors of yealink.c
 *   - Thomas Reitmayr
 *   - Oliver Neukum for good review comments and code
 *   - Shaun Jackman <sjackman@gmail.com> for Genius G-talk keymap
 *   - Dmitry Torokhov for valuable input and review
 *
 * Todo:
 *   - Read/write EEPROM
 *
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/rwsem.h>
#include <linux/usb/input.h>

#define DRIVER_VERSION "20080805"
#define DRIVER_AUTHOR  "Alfred E. Heggestad"
#define DRIVER_DESC    "CM109 phone driver"

static char *phone = "kip1000";
module_param(phone, charp, S_IRUSR);
MODULE_PARM_DESC(phone, "Phone name {kip1000, gtalk, usbph01, atcom}");

enum {
   /* HID Registers */
   HID_IR0 = 0x00, /* Record/Playback-mute button, Volume up/down  */
   HID_IR1 = 0x01, /* GPI, generic registers or EEPROM_DATA0       */
   HID_IR2 = 0x02, /* Generic registers or EEPROM_DATA1            */
   HID_IR3 = 0x03, /* Generic registers or EEPROM_CTRL             */
   HID_OR0 = 0x00, /* Mapping control, buzzer, SPDIF (offset 0x04) */
   HID_OR1 = 0x01, /* GPO - General Purpose Output                 */
   HID_OR2 = 0x02, /* Set GPIO to input/output mode                */
   HID_OR3 = 0x03, /* SPDIF status channel or EEPROM_CTRL          */

   /* HID_IR0 */
   /* bits 7-6
      When bit7 HID_OR0 == 0:
      HID_IR0-3 are programmed by MCU (and GPI)
      0: HID_IR1 is used as GPI
      1: HID_IR0-3 are used as generic HID registers
      2: Values written to HID_IR0-3 are also mapped
      to MCU_CTRL, EEPROM_DATA0-1,
      EEPROM_CTRL
      3: Reserved
    */
   HID_IR_GPI   = 0 << 6,
   HID_IR_GENERIC_HID_REG = 1 << 6,
   HID_IR_MAP_MCU_EEPROM  = 2 << 6,
   RECORD_MUTE   = 1 << 3,
   PLAYBACK_MUTE = 1 << 2,
   VOLUME_DOWN   = 1 << 1,
   VOLUME_UP     = 1 << 0,

   /* HID_OR0 */
   /* bits 7-6
      0: HID_OR1-2 are used for GPO; HID_OR0, 3 are used for buzzer
         and SPDIF
      1: HID_OR0-3 are used as generic HID registers
      2: Values written to HID_OR0-3 are also mapped to MCU_CTRL,
         EEPROM_DATA0-1, EEPROM_CTRL (see Note)
      3: Reserved
    */
   HID_OR_GPO_BUZ_SPDIF   = 0 << 6,
   HID_OR_GENERIC_HID_REG = 1 << 6,
   HID_OR_MAP_MCU_EEPROM  = 2 << 6,

   BUZZER_ON = 1 << 5,

   /* up to 256 normal keys, up to 16 special keys */
   KEYMAP_SIZE = 256 + 16,
};

/* CM109 protocol packet */
struct cm109_ctl_packet {
   u8 byte[4];
} __attribute__ ((packed));

enum {
   USB_PKT_LEN = sizeof(struct cm109_ctl_packet),
};

/* CM109 device structure */
struct cm109_dev {
   struct input_dev *idev;  /* input device */
   struct usb_device *udev; /* usb device */
   struct usb_interface *intf;

   /* irq input channel */
   struct cm109_ctl_packet *irq_data;
   dma_addr_t irq_dma;
   struct urb *irq_urb;

   /* control output channel */
   struct cm109_ctl_packet *ctl_data;
   dma_addr_t ctl_dma;
   struct usb_ctrlrequest *ctl_req;
   struct urb *ctl_urb;
   /*
    * The 3 bitfields below are protected by ctl_submit_lock.
    * They have to be separate since they are accessed from IRQ
    * context.
    */
   unsigned irq_urb_pending:1;   /* irq_urb is in flight */
   unsigned submit_ctl_urb:1;    /* ctl_urb must be submitted */
   unsigned ctl_urb_pending:1;   /* ctl_urb is in flight */
   spinlock_t ctl_submit_lock;

   unsigned char buzzer_state;   /* on/off */

   /* flags */
   unsigned open:1;
   unsigned resetting:1;
   unsigned shutdown:1;

   /* This mutex protects writes to the above flags */
   struct mutex pm_mutex;

   unsigned short keymap[KEYMAP_SIZE];

   char phys[64];   /* physical device path */
   u32 keystatus;   /* bit fields : last reported status of keys : 1 pressed, 0 released */
   size_t keyindex; /* 16=new scan  0,4,8,12=scan columns  */
   u8 gpi;          /* Cached value of GPI (high nibble) */
};

/******************************************************************************
 * CM109 key interface
 *****************************************************************************/

static unsigned short special_keymap(int code)
{
   if (code > 0xff) {
      switch (code - 0xff) {
      case RECORD_MUTE: return KEY_MUTE;
      case PLAYBACK_MUTE:  return KEY_MUTE;
      case VOLUME_DOWN: return KEY_VOLUMEDOWN;
      case VOLUME_UP:      return KEY_VOLUMEUP;
      }
   }
   return KEY_RESERVED;
}

/* Map device buttons to internal key events.
 *
 * The "up" and "down" keys, are symbolised by arrows on the button.
 * The "pickup" and "hangup" keys are symbolised by a green and red phone
 * on the button.

 Komunikate KIP1000 Keyboard Matrix

     -> -- 1 -- 2 -- 3  --> GPI pin 4 (0x10)
      |    |    |    |
     <- -- 4 -- 5 -- 6  --> GPI pin 5 (0x20)
      |    |    |    |
     END - 7 -- 8 -- 9  --> GPI pin 6 (0x40)
      |    |    |    |
     OK -- * -- 0 -- #  --> GPI pin 7 (0x80)
      |    |    |    |

     /|\  /|\  /|\  /|\
      |    |    |    |
GPO
pin:  3    2    1    0
     0x8  0x4  0x2  0x1

 */
static unsigned short keymap_kip1000(int scancode)
{
   switch (scancode) {           /* phone key:   */
   case 0x82: return KEY_NUMERIC_0;    /*   0          */
   case 0x14: return KEY_NUMERIC_1;    /*   1          */
   case 0x12: return KEY_NUMERIC_2;    /*   2          */
   case 0x11: return KEY_NUMERIC_3;    /*   3          */
   case 0x24: return KEY_NUMERIC_4;    /*   4          */
   case 0x22: return KEY_NUMERIC_5;    /*   5          */
   case 0x21: return KEY_NUMERIC_6;    /*   6          */
   case 0x44: return KEY_NUMERIC_7;    /*   7          */
   case 0x42: return KEY_NUMERIC_8;    /*   8          */
   case 0x41: return KEY_NUMERIC_9;    /*   9          */
   case 0x81: return KEY_NUMERIC_POUND;      /*   #          */
   case 0x84: return KEY_NUMERIC_STAR;    /*   *          */
   case 0x88: return KEY_ENTER;        /*   pickup     */
   case 0x48: return KEY_ESC;       /*   hangup     */
   case 0x28: return KEY_LEFT;         /*   IN         */
   case 0x18: return KEY_RIGHT;        /*   OUT        */
   default:   return special_keymap(scancode);
   }
}

/*
  Contributed by Shaun Jackman <sjackman@gmail.com>

  Genius G-Talk keyboard matrix
     0 1 2 3
  4: 0 4 8 Talk
  5: 1 5 9 End
  6: 2 6 # Up
  7: 3 7 * Down
*/
static unsigned short keymap_gtalk(int scancode)
{
   switch (scancode) {
   case 0x11: return KEY_NUMERIC_0;
   case 0x21: return KEY_NUMERIC_1;
   case 0x41: return KEY_NUMERIC_2;
   case 0x81: return KEY_NUMERIC_3;
   case 0x12: return KEY_NUMERIC_4;
   case 0x22: return KEY_NUMERIC_5;
   case 0x42: return KEY_NUMERIC_6;
   case 0x82: return KEY_NUMERIC_7;
   case 0x14: return KEY_NUMERIC_8;
   case 0x24: return KEY_NUMERIC_9;
   case 0x44: return KEY_NUMERIC_POUND;   /* # */
   case 0x84: return KEY_NUMERIC_STAR; /* * */
   case 0x18: return KEY_ENTER;     /* Talk (green handset) */
   case 0x28: return KEY_ESC;    /* End (red handset) */
   case 0x48: return KEY_UP;     /* Menu up (rocker switch) */
   case 0x88: return KEY_DOWN;      /* Menu down (rocker switch) */
   default:   return special_keymap(scancode);
   }
}

/*
 * Keymap for Allied-Telesis Corega USBPH01
 * http://www.alliedtelesis-corega.com/2/1344/1437/1360/chprd.html
 *
 * Contributed by july@nat.bg
 */
static unsigned short keymap_usbph01(int scancode)
{
   switch (scancode) {
   case 0x11: return KEY_NUMERIC_0;    /*   0          */
   case 0x21: return KEY_NUMERIC_1;    /*   1          */
   case 0x41: return KEY_NUMERIC_2;    /*   2          */
   case 0x81: return KEY_NUMERIC_3;    /*   3          */
   case 0x12: return KEY_NUMERIC_4;    /*   4          */
   case 0x22: return KEY_NUMERIC_5;    /*   5          */
   case 0x42: return KEY_NUMERIC_6;    /*   6          */
   case 0x82: return KEY_NUMERIC_7;    /*   7          */
   case 0x14: return KEY_NUMERIC_8;    /*   8          */
   case 0x24: return KEY_NUMERIC_9;    /*   9          */
   case 0x44: return KEY_NUMERIC_POUND;      /*   #          */
   case 0x84: return KEY_NUMERIC_STAR;    /*   *          */
   case 0x18: return KEY_ENTER;        /*   pickup     */
   case 0x28: return KEY_ESC;       /*   hangup     */
   case 0x48: return KEY_LEFT;         /*   IN         */
   case 0x88: return KEY_RIGHT;        /*   OUT        */
   default:   return special_keymap(scancode);
   }
}

/*
 * Keymap for ATCom AU-100
 * http://www.atcom.cn/products.html
 * http://www.packetizer.com/products/au100/
 * http://www.voip-info.org/wiki/view/AU-100
 *
 * Contributed by daniel@gimpelevich.san-francisco.ca.us
 */
static unsigned short keymap_atcom(int scancode)
{
   switch (scancode) {           /* phone key:   */
   case 0x82: return KEY_NUMERIC_0;    /*   0          */
   case 0x11: return KEY_NUMERIC_1;    /*   1          */
   case 0x12: return KEY_NUMERIC_2;    /*   2          */
   case 0x14: return KEY_NUMERIC_3;    /*   3          */
   case 0x21: return KEY_NUMERIC_4;    /*   4          */
   case 0x22: return KEY_NUMERIC_5;    /*   5          */
   case 0x24: return KEY_NUMERIC_6;    /*   6          */
   case 0x41: return KEY_NUMERIC_7;    /*   7          */
   case 0x42: return KEY_NUMERIC_8;    /*   8          */
   case 0x44: return KEY_NUMERIC_9;    /*   9          */
   case 0x84: return KEY_NUMERIC_POUND;      /*   #          */
   case 0x81: return KEY_NUMERIC_STAR;    /*   *          */
   case 0x18: return KEY_ENTER;        /*   pickup     */
   case 0x28: return KEY_ESC;       /*   hangup     */
   case 0x48: return KEY_LEFT;         /* left arrow   */
   case 0x88: return KEY_RIGHT;        /* right arrow  */
   default:   return special_keymap(scancode);
   }
}

static unsigned short keymap_webtalker(int scancode)
{
   switch (scancode) {           /* phone key:   */
   case 0x15: return KEY_NUMERIC_0;    /*   0          */
   case 0x05: return KEY_NUMERIC_1;    /*   1          */
   case 0x06: return KEY_NUMERIC_2;    /*   2          */
   case 0x07: return KEY_NUMERIC_3;    /*   3          */
   case 0x0A: return KEY_NUMERIC_4;    /*   4          */
   case 0x0B: return KEY_NUMERIC_5;    /*   5          */
   case 0x0C: return KEY_NUMERIC_6;    /*   6          */
   case 0x0F: return KEY_NUMERIC_7;    /*   7          */
   case 0x10: return KEY_NUMERIC_8;    /*   8          */
   case 0x11: return KEY_NUMERIC_9;    /*   9          */
   case 0x16: return KEY_NUMERIC_POUND;      /*   #          */
   case 0x14: return KEY_NUMERIC_STAR;    /*   *          */
   case 0x08: return KEY_ENTER;        /*   pickup     */
   case 0x0D: return KEY_ESC;       /*   hangup     */
   case 0x00: return KEY_UP;     /* Menu up */
   case 0x03: return KEY_DOWN;      /* Menu down */
   case 0x02: return KEY_VOLUMEDOWN;      /* left arrow   */
   case 0x01: return KEY_VOLUMEUP;        /* right arrow  */
   case 0x12: return KEY_MUTE;
   /* case 0x18: return ???; */ /* Handsfree */
   case 0x17: return KEY_BACKSPACE; /* Delete */
   case 0x13: return KEY_PHONE; /* Skype */
   default:   return special_keymap(scancode);
   }
}

static unsigned short (*keymap)(int) = keymap_kip1000;

/*
 * Completes a request by converting the data into events for the
 * input subsystem.
 */
static inline void report_key(struct cm109_dev *dev, int key, int value)
{
   struct input_dev *idev = dev->idev;
   /* printk(KERN_INFO KBUILD_MODNAME "report_key(key = %d, value = %d)\n", (int)(key), (int)(value)); */
   input_report_key(idev, key, value);
   input_sync(idev);
}

/******************************************************************************
 * CM109 usb communication interface
 *****************************************************************************/

/*
 * IRQ handler
 */
static void cm109_irq_urb_callback(struct urb *urb)
{
   struct cm109_dev *dev = urb->context;
   const int status = urb->status;
   int error;
   u8 keybit = (1 << (dev->keyindex >> 2));
   int gpio_mode = 0;

   dev_dbg(&dev->intf->dev, "### URB IRQ: [0x%02x 0x%02x 0x%02x 0x%02x] keyindex=%lu\n",
        (unsigned int)(dev->irq_data->byte[0]),
        (unsigned int)(dev->irq_data->byte[1]),
        (unsigned int)(dev->irq_data->byte[2]),
        (unsigned int)(dev->irq_data->byte[3]),
        (unsigned long)(dev->keyindex));
   /* printk(KERN_INFO KBUILD_MODNAME "### URB IRQ: [0x%02x 0x%02x 0x%02x 0x%02x] keyindex=%lu\n",
        (unsigned int)(dev->irq_data->byte[0]),
        (unsigned int)(dev->irq_data->byte[1]),
        (unsigned int)(dev->irq_data->byte[2]),
        (unsigned int)(dev->irq_data->byte[3]),
        (unsigned long)(dev->keyindex)); */

   if (status) {
      if ((-ESHUTDOWN == status)
          || (-ECONNRESET == status)
          || (-ENOENT == status)) {
         return;
      }
      dev_err_ratelimited(&dev->intf->dev, "%s: urb status %d\n",
                __func__, status);
      goto out;
   }

   /* Special keys */
   {
      u8 special = dev->irq_data->byte[HID_IR0] & 0x0f;
      u32 keystatusmask = (1 << 16);
      if ((special & VOLUME_UP)) {
         if (!(dev->keystatus & keystatusmask)) {
            // Key pressed
            report_key(dev, dev->keymap[255 + VOLUME_UP], 1);
            dev->keystatus |= keystatusmask;
         }
      }
      else {
         if ((dev->keystatus & keystatusmask)) {
            // Key released
            report_key(dev, dev->keymap[255 + VOLUME_UP], 0);
            dev->keystatus &= (~(keystatusmask));
         }
      }
      keystatusmask = (1 << 17);
      if ((special & VOLUME_DOWN)) {
         if (!(dev->keystatus & keystatusmask)) {
            // Key pressed
            report_key(dev, dev->keymap[255 + VOLUME_DOWN], 1);
            dev->keystatus |= keystatusmask;
         }
      }
      else {
         if ((dev->keystatus & keystatusmask)) {
            // Key released
            report_key(dev, dev->keymap[255 + VOLUME_DOWN], 0);
            dev->keystatus &= (~(keystatusmask));
         }
      }
      if ((special & PLAYBACK_MUTE)) {
         unsigned short keycode = dev->keymap[255 + PLAYBACK_MUTE];
         report_key(dev, keycode, 1);
         report_key(dev, keycode, 0);
      }
      if ((special & RECORD_MUTE)) {
         unsigned short keycode = dev->keymap[255 + RECORD_MUTE];
         report_key(dev, keycode, 1);
         report_key(dev, keycode, 0);
      }
   }

   if (HID_IR_GPI == (dev->irq_data->byte[HID_IR0] & 0xC0)) {
      gpio_mode = 1;
      /* Scan key column */
      if (dev->keyindex >= 16) {
         /* Any changes ? */
         if ((dev->gpi & 0xf0) == (dev->irq_data->byte[HID_IR1] & 0xf0)) {
            goto out;
         }
         dev->gpi = dev->irq_data->byte[HID_IR1] & 0xf0;
         dev->keyindex = 0;
         keybit = 1;
      }
      else {
         u8 code = dev->irq_data->byte[HID_IR1];
         u8 mask;
         size_t i;
         for (i = 0, mask = 0x10; (i < 4); i += 1, mask <<= 1) {
            u32 keystatusmask = (1 << (dev->keyindex + i));
            if ((code & mask)) {
               if (!(dev->keystatus & keystatusmask)) {
                  // Key pressed
                  report_key(dev, dev->keymap[mask | keybit], 1);
                  dev->keystatus |= keystatusmask;
               }
            }
            else {
               if ((dev->keystatus & keystatusmask)) {
                  // Key released
                  report_key(dev, dev->keymap[mask | keybit], 0);
                  dev->keystatus &= (~(keystatusmask));
               }
            }
         }
         dev->keyindex += 4;
         keybit <<= 1;
      }
   }
   else {
      u8 hid = dev->irq_data->byte[HID_IR1];
      if (0 == (hid & 0x80)) {
         // Key pressed
         report_key(dev, dev->keymap[hid], 1);
      }
      else {
         // Key released
         report_key(dev, dev->keymap[hid & 0x7F], 0);
      }
   }

 out:

   spin_lock(&(dev->ctl_submit_lock));

   dev->irq_urb_pending = 0;

   if (likely(!dev->shutdown)) {
      if (gpio_mode) {
         // Set GPIO
         if (keybit >= 0x10) {
            keybit = 0x0f;
         }
         dev->ctl_data->byte[HID_OR1] = keybit;
         dev->ctl_data->byte[HID_OR2] = keybit;
         dev->submit_ctl_urb = 1;
      }
      if (!dev->ctl_urb_pending) {
         if (dev->submit_ctl_urb) {
            dev->submit_ctl_urb = 0;
            dev->ctl_urb_pending = 1;
            error = usb_submit_urb(dev->ctl_urb, GFP_ATOMIC);
            if (error) {
               dev_err(&dev->intf->dev,
                  "%s: usb_submit_urb (ctl_urb) failed %d\n",
                  __func__, error);
            }
         }
         else {
            dev->irq_urb_pending = 1;
            error = usb_submit_urb(dev->irq_urb, GFP_ATOMIC);
            if (error) {
               dev_err(&dev->intf->dev,
                  "%s: usb_submit_urb (irq_urb) failed %d\n",
                  __func__, error);
            }
         }
      }
   }

   spin_unlock(&(dev->ctl_submit_lock));
}

static void cm109_ctl_urb_callback(struct urb *urb)
{
   struct cm109_dev *dev = urb->context;
   const int status = urb->status;
   int error;

   dev_dbg(&dev->intf->dev, "### URB CTL: [0x%02x 0x%02x 0x%02x 0x%02x]\n",
        dev->ctl_data->byte[0],
        dev->ctl_data->byte[1],
        dev->ctl_data->byte[2],
        dev->ctl_data->byte[3]);

   if (status) {
      if ((-ESHUTDOWN == status)
          || (-ECONNRESET == status)
          || (-ENOENT == status)) {
         return;
      }
      dev_err_ratelimited(&dev->intf->dev, "%s: urb status %d\n",
                __func__, status);
   }

   spin_lock(&(dev->ctl_submit_lock));

   dev->ctl_urb_pending = 0;

   if (likely(!dev->shutdown)) {
      if ((dev->submit_ctl_urb) || (status)) {
         dev->submit_ctl_urb = 0;
         dev->ctl_urb_pending = 1;
         error = usb_submit_urb(dev->ctl_urb, GFP_ATOMIC);
         if (error) {
            dev_err(&dev->intf->dev,
               "%s: usb_submit_urb (ctl_urb) failed %d\n",
               __func__, error);
         }
      }
      else if (likely(!dev->irq_urb_pending)) {
         /* ask for key data */
         dev->irq_urb_pending = 1;
         error = usb_submit_urb(dev->irq_urb, GFP_ATOMIC);
         if (error) {
            dev_err(&dev->intf->dev,
               "%s: usb_submit_urb (irq_urb) failed %d\n",
               __func__, error);
         }
      }
   }

   spin_unlock(&(dev->ctl_submit_lock));
}

static void cm109_toggle_buzzer_async(struct cm109_dev *dev)
{
   unsigned long flags;

   spin_lock_irqsave(&(dev->ctl_submit_lock), flags);

   if (dev->buzzer_state) {
      dev->ctl_data->byte[HID_OR0] |= BUZZER_ON;
   }
   else {
      dev->ctl_data->byte[HID_OR0] &= ~BUZZER_ON;
   }
   dev->submit_ctl_urb = 1;

   if (!dev->ctl_urb_pending) {
      int error;

      dev->submit_ctl_urb = 0;
      dev->ctl_urb_pending = 1;
      error = usb_submit_urb(dev->ctl_urb, GFP_ATOMIC);
      if (error) {
         dev_err(&dev->intf->dev,
            "%s: usb_submit_urb (ctl_urb) failed %d\n",
            __func__, error);
      }
   }

   spin_unlock_irqrestore(&(dev->ctl_submit_lock), flags);
}

static void cm109_toggle_buzzer_sync(struct cm109_dev *dev, int on)
{
   int error;

   if (on) {
      dev->ctl_data->byte[HID_OR0] |= BUZZER_ON;
   }
   else {
      dev->ctl_data->byte[HID_OR0] &= ~BUZZER_ON;
   }

   error = usb_control_msg(dev->udev,
            usb_sndctrlpipe(dev->udev, 0),
            dev->ctl_req->bRequest,
            dev->ctl_req->bRequestType,
            le16_to_cpu(dev->ctl_req->wValue),
            le16_to_cpu(dev->ctl_req->wIndex),
            dev->ctl_data,
            USB_PKT_LEN, USB_CTRL_SET_TIMEOUT);
   if (error < 0 && error != -EINTR) {
      dev_err(&dev->intf->dev, "%s: usb_control_msg() failed %d\n",
         __func__, error);
   }
}

static void cm109_stop_traffic(struct cm109_dev *dev)
{
   dev->shutdown = 1;
   /*
    * Make sure other CPUs see this
    */
   smp_wmb();

   usb_kill_urb(dev->ctl_urb);
   usb_kill_urb(dev->irq_urb);

   cm109_toggle_buzzer_sync(dev, 0);

   dev->shutdown = 0;
   smp_wmb();
}

static void cm109_restore_state(struct cm109_dev *dev)
{
   if (dev->open) {
      /*
       * Restore buzzer state.
       * This will also kick regular URB submission
       */
      cm109_toggle_buzzer_async(dev);
   }
}

/******************************************************************************
 * input event interface
 *****************************************************************************/

static int cm109_input_open(struct input_dev *idev)
{
   struct cm109_dev *dev = input_get_drvdata(idev);
   int error;

   error = usb_autopm_get_interface(dev->intf);
   if (error < 0) {
      dev_err(&idev->dev, "%s - cannot autoresume, result %d\n",
         __func__, error);
      return error;
   }

   mutex_lock(&dev->pm_mutex);

   dev->buzzer_state = 0;
   dev->keystatus = 0;  /* no keys pressed */
   dev->keyindex = 16;
   dev->gpi = 0;

   /* issue INIT */
   dev->ctl_data->byte[HID_OR0] = HID_OR_GPO_BUZ_SPDIF;
   dev->ctl_data->byte[HID_OR1] = 0x0f;
   dev->ctl_data->byte[HID_OR2] = 0x0f;
   dev->ctl_data->byte[HID_OR3] = 0x00;
   dev->irq_urb_pending = 0;
   dev->submit_ctl_urb = 0;
   dev->ctl_urb_pending = 1;
   error = usb_submit_urb(dev->ctl_urb, GFP_KERNEL);
   if (error) {
      dev_err(&dev->intf->dev, "%s: usb_submit_urb (ctl_urb) failed %d\n",
         __func__, error);
   }
   else {
      dev->open = 1;
   }

   mutex_unlock(&dev->pm_mutex);

   if (error) {
      usb_autopm_put_interface(dev->intf);
   }

   return error;
}

static void cm109_input_close(struct input_dev *idev)
{
   struct cm109_dev *dev = input_get_drvdata(idev);

   mutex_lock(&dev->pm_mutex);

   /*
    * Once we are here event delivery is stopped so we
    * don't need to worry about someone starting buzzer
    * again
    */
   cm109_stop_traffic(dev);
   dev->open = 0;

   mutex_unlock(&dev->pm_mutex);

   usb_autopm_put_interface(dev->intf);
}

static int cm109_input_ev(struct input_dev *idev, unsigned int type,
           unsigned int code, int value)
{
   struct cm109_dev *dev = input_get_drvdata(idev);

   dev_dbg(&dev->intf->dev,
      "input_ev: type=%u code=%u value=%d\n", type, code, value);

   if (type != EV_SND) {
      return -EINVAL;
   }

   switch (code) {
   case SND_TONE:
   case SND_BELL:
      dev->buzzer_state = !!value;
      if (!dev->resetting)
         cm109_toggle_buzzer_async(dev);
      return 0;

   default:
      return -EINVAL;
   }
}


/******************************************************************************
 * Linux interface and usb initialisation
 *****************************************************************************/

struct driver_info {
   char *name;
};

static const struct driver_info info_cm109 = {
   .name = "CM109 USB driver",
};

enum {
   VENDOR_ID        = 0x0d8c, /* C-Media Electronics */
   /*
    CM109 defines range 0x0008 - 0x000f
    If the device is handled by driver usbhid there are two solutions :
    - manually unbind the device from driver usbhid and bind it to driver cm109
        cd /sys/bus/usb/drivers/usbhid; echo "2-2:1.3" > unbind; cd ../cm109; echo "2-2:1.3" > bind
    - put the following line in file /etc/modprobe.d/usbhid
        options usbhid quirks=0x0d8c:0x0008:0x0004
      Where 0x0d8c:0x0008 are vendor id and product id, and 0x0004 means HID_QUIRK_IGNORE
   */
   PRODUCT_ID_CM109_8 = 0x0008, /* Topcom Webt@lker 211 */
   PRODUCT_ID_CM109_E = 0x000e, /* Audio Adapter (Planet UP-100, Genius G-Talk) */
};

/* table of devices that work with this driver */
static const struct usb_device_id cm109_usb_table[] = {
   {
      .match_flags = USB_DEVICE_ID_MATCH_DEVICE |
            USB_DEVICE_ID_MATCH_INT_INFO,
      .idVendor = VENDOR_ID,
      .idProduct = PRODUCT_ID_CM109_8,
      .bInterfaceClass = USB_CLASS_HID,
      .bInterfaceSubClass = 0,
      .bInterfaceProtocol = 0,
      .driver_info = (kernel_ulong_t) &info_cm109
   },
   {
      .match_flags = USB_DEVICE_ID_MATCH_DEVICE |
            USB_DEVICE_ID_MATCH_INT_INFO,
      .idVendor = VENDOR_ID,
      .idProduct = PRODUCT_ID_CM109_E,
      .bInterfaceClass = USB_CLASS_HID,
      .bInterfaceSubClass = 0,
      .bInterfaceProtocol = 0,
      .driver_info = (kernel_ulong_t) &info_cm109
   },
   /* you can add more devices here with product ID 0x0008 - 0x000f */
   { }
};

static void cm109_usb_cleanup(struct cm109_dev *dev)
{
   kfree(dev->ctl_req);
   if (dev->ctl_data) {
      usb_free_coherent(dev->udev, USB_PKT_LEN,
              dev->ctl_data, dev->ctl_dma);
   }
   if (dev->irq_data) {
      usb_free_coherent(dev->udev, USB_PKT_LEN,
              dev->irq_data, dev->irq_dma);
   }

   usb_free_urb(dev->irq_urb);   /* parameter validation in core/urb */
   usb_free_urb(dev->ctl_urb);   /* parameter validation in core/urb */
   kfree(dev);
}

static void cm109_usb_disconnect(struct usb_interface *interface)
{
   struct cm109_dev *dev = usb_get_intfdata(interface);

   usb_set_intfdata(interface, NULL);
   input_unregister_device(dev->idev);
   cm109_usb_cleanup(dev);
}

static int cm109_usb_probe(struct usb_interface *intf,
            const struct usb_device_id *id)
{
   struct usb_device *udev = interface_to_usbdev(intf);
   struct driver_info *nfo = (struct driver_info *)id->driver_info;
   struct usb_host_interface *interface;
   struct usb_endpoint_descriptor *endpoint;
   struct cm109_dev *dev;
   struct input_dev *input_dev = NULL;
   int ret, pipe, i;
   int error = -ENOMEM;

   interface = intf->cur_altsetting;
   endpoint = &interface->endpoint[0].desc;

   if (!usb_endpoint_is_int_in(endpoint)) {
      return -ENODEV;
   }

   dev = kzalloc(sizeof(*dev), GFP_KERNEL);
   if (!dev) {
      return -ENOMEM;
   }

   spin_lock_init(&(dev->ctl_submit_lock));
   mutex_init(&(dev->pm_mutex));

   dev->udev = udev;
   dev->intf = intf;

   dev->idev = input_dev = input_allocate_device();
   if (!input_dev) {
      goto err_out;
   }

   /* allocate usb buffers */
   dev->irq_data = usb_alloc_coherent(udev, USB_PKT_LEN,
                  GFP_KERNEL, &dev->irq_dma);
   if (!dev->irq_data) {
      goto err_out;
   }

   dev->ctl_data = usb_alloc_coherent(udev, USB_PKT_LEN,
                  GFP_KERNEL, &dev->ctl_dma);
   if (!dev->ctl_data) {
      goto err_out;
   }

   dev->ctl_req = kmalloc(sizeof(*(dev->ctl_req)), GFP_KERNEL);
   if (!dev->ctl_req) {
      goto err_out;
   }

   /* allocate urb structures */
   dev->irq_urb = usb_alloc_urb(0, GFP_KERNEL);
   if (!dev->irq_urb) {
      goto err_out;
   }

   dev->ctl_urb = usb_alloc_urb(0, GFP_KERNEL);
   if (!dev->ctl_urb) {
      goto err_out;
   }

   /* get a handle to the interrupt data pipe */
   pipe = usb_rcvintpipe(udev, endpoint->bEndpointAddress);
   ret = usb_maxpacket(udev, pipe, usb_pipeout(pipe));
   if (ret != USB_PKT_LEN) {
      dev_err(&intf->dev, "invalid payload size %d, expected %d\n",
         ret, USB_PKT_LEN);
   }

   /* initialise irq urb */
   usb_fill_int_urb(dev->irq_urb, udev, pipe, dev->irq_data,
          USB_PKT_LEN,
          cm109_irq_urb_callback, dev, endpoint->bInterval);
   dev->irq_urb->transfer_dma = dev->irq_dma;
   dev->irq_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
   dev->irq_urb->dev = udev;

   /* initialise ctl urb */
   dev->ctl_req->bRequestType = USB_TYPE_CLASS | USB_RECIP_INTERFACE |
               USB_DIR_OUT;
   dev->ctl_req->bRequest = USB_REQ_SET_CONFIGURATION;
   dev->ctl_req->wValue = cpu_to_le16(0x200);
   dev->ctl_req->wIndex = cpu_to_le16(interface->desc.bInterfaceNumber);
   dev->ctl_req->wLength = cpu_to_le16(USB_PKT_LEN);

   usb_fill_control_urb(dev->ctl_urb, udev, usb_sndctrlpipe(udev, 0),
              (void *)dev->ctl_req, dev->ctl_data, USB_PKT_LEN,
              cm109_ctl_urb_callback, dev);
   dev->ctl_urb->transfer_dma = dev->ctl_dma;
   dev->ctl_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
   dev->ctl_urb->dev = udev;

   /* find out the physical bus location */
   usb_make_path(udev, dev->phys, sizeof(dev->phys));
   strlcat(dev->phys, "/input0", sizeof(dev->phys));

   /* register settings for the input device */
   input_dev->name = nfo->name;
   input_dev->phys = dev->phys;
   usb_to_input_id(udev, &(input_dev->id));
   input_dev->dev.parent = &(intf->dev);

   input_set_drvdata(input_dev, dev);
   input_dev->open = cm109_input_open;
   input_dev->close = cm109_input_close;
   input_dev->event = cm109_input_ev;

   input_dev->keycode = dev->keymap;
   input_dev->keycodesize = sizeof(unsigned char);
   input_dev->keycodemax = ARRAY_SIZE(dev->keymap);

   input_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_SND);
   input_dev->sndbit[0] = BIT_MASK(SND_BELL) | BIT_MASK(SND_TONE);

   /* register available key events */
   for (i = 0; i < KEYMAP_SIZE; i++) {
      unsigned short k = keymap(i);
      dev->keymap[i] = k;
      __set_bit(k, input_dev->keybit);
   }
   __clear_bit(KEY_RESERVED, input_dev->keybit);

   error = input_register_device(dev->idev);
   if (error) {
      goto err_out;
   }

   usb_set_intfdata(intf, dev);

   return 0;

err_out:
   input_free_device(input_dev);
   cm109_usb_cleanup(dev);
   return error;
}

static int cm109_usb_suspend(struct usb_interface *intf, pm_message_t message)
{
   struct cm109_dev *dev = usb_get_intfdata(intf);

   dev_info(&intf->dev, "cm109: usb_suspend (event=%d)\n", message.event);

   mutex_lock(&dev->pm_mutex);
   cm109_stop_traffic(dev);
   mutex_unlock(&dev->pm_mutex);

   return 0;
}

static int cm109_usb_resume(struct usb_interface *intf)
{
   struct cm109_dev *dev = usb_get_intfdata(intf);

   dev_info(&intf->dev, "cm109: usb_resume\n");

   mutex_lock(&dev->pm_mutex);
   cm109_restore_state(dev);
   mutex_unlock(&dev->pm_mutex);

   return 0;
}

static int cm109_usb_pre_reset(struct usb_interface *intf)
{
   struct cm109_dev *dev = usb_get_intfdata(intf);

   mutex_lock(&dev->pm_mutex);

   /*
    * Make sure input events don't try to toggle buzzer
    * while we are resetting
    */
   dev->resetting = 1;
   smp_wmb();

   cm109_stop_traffic(dev);

   return 0;
}

static int cm109_usb_post_reset(struct usb_interface *intf)
{
   struct cm109_dev *dev = usb_get_intfdata(intf);

   dev->resetting = 0;
   smp_wmb();

   cm109_restore_state(dev);

   mutex_unlock(&dev->pm_mutex);

   return 0;
}

static struct usb_driver cm109_driver = {
   .name    = "cm109",
   .probe      = cm109_usb_probe,
   .disconnect = cm109_usb_disconnect,
   .suspend = cm109_usb_suspend,
   .resume     = cm109_usb_resume,
   .reset_resume  = cm109_usb_resume,
   .pre_reset  = cm109_usb_pre_reset,
   .post_reset = cm109_usb_post_reset,
   .id_table   = cm109_usb_table,
   .supports_autosuspend = 1,
};

static int __init cm109_handle_params(void)
{
   /* Load the phone keymap */
   if (!strcasecmp(phone, "kip1000")) {
      keymap = keymap_kip1000;
      printk(KERN_INFO KBUILD_MODNAME ": "
         "Keymap for Komunikate KIP1000 phone loaded\n");
   }
   else if (!strcasecmp(phone, "gtalk")) {
      keymap = keymap_gtalk;
      printk(KERN_INFO KBUILD_MODNAME ": "
         "Keymap for Genius G-talk phone loaded\n");
   }
   else if (!strcasecmp(phone, "usbph01")) {
      keymap = keymap_usbph01;
      printk(KERN_INFO KBUILD_MODNAME ": "
         "Keymap for Allied-Telesis Corega USBPH01 phone loaded\n");
   }
   else if (!strcasecmp(phone, "atcom")) {
      keymap = keymap_atcom;
      printk(KERN_INFO KBUILD_MODNAME ": "
         "Keymap for ATCom AU-100 phone loaded\n");
   }
   else if (!strcasecmp(phone, "webtalker")) {
      keymap = keymap_webtalker;
      printk(KERN_INFO KBUILD_MODNAME ": "
         "Keymap for Webt@lker 211 phone loaded\n");
   }
   else {
      printk(KERN_ERR KBUILD_MODNAME ": "
         "Unsupported phone: %s\n", phone);
      return -EINVAL;
   }

   return 0;
}

static int __init cm109_init(void)
{
   int err;

   err = cm109_handle_params();
   if (err) {
      return err;
   }

   err = usb_register(&(cm109_driver));
   if (err) {
      return err;
   }

   printk(KERN_INFO KBUILD_MODNAME ": "
      DRIVER_DESC ": " DRIVER_VERSION " (C) " DRIVER_AUTHOR "\n");

   return 0;
}

static void __exit cm109_exit(void)
{
   usb_deregister(&cm109_driver);
}

module_init(cm109_init);
module_exit(cm109_exit);

MODULE_DEVICE_TABLE(usb, cm109_usb_table);

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
