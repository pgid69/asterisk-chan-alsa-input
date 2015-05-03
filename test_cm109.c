#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

int main (int argc, char **argv)
{
   int ret = -1;
   int fd = -1;

   do { /* Empty loop */
      int version;
      struct input_id device_info;
      char name[256] = "Unknown";

      if (argc < 2) {
         ret = -1;
         fprintf(stderr, "Missing device file\n");
         break;
      }

      fd = open(argv[1], O_RDWR | O_SYNC);
      if (fd < 0) {
         ret = -1;
         perror("evdev open");
         break;
      }

      ret = ioctl(fd, EVIOCGNAME(sizeof (name)), name);
      if (ret < 0) {
         perror("evdev ioctl");
         break;
      }

      printf("The device on %s says its name is %s\n",
             argv[1], name);

      /* ioctl() accesses the underlying driver */
      ret = ioctl(fd, EVIOCGVERSION, &(version));
      if (ret) {
         perror("evdev ioctl");
         break;
      }

      /* the EVIOCGVERSION ioctl() returns an int */
      /* so we unpack it and display it */
      printf("evdev driver version is %d.%d.%d\n",
             (int)(version >> 16), (int)((version >> 8) & 0xff),
             (int)(version & 0xff));

      /* suck out some device information */
      ret = ioctl(fd, EVIOCGID, &(device_info));
      if (ret) {
         perror("evdev ioctl");
         break;
      }

      /* the EVIOCGID ioctl() returns input_devinfo
       * structure - see <linux/input.h>
       * So we work through the various elements,
       * displaying each of them
       */
      printf("vendor %04hx product %04hx version %04hx",
             (unsigned short)(device_info.vendor),
             (unsigned short)(device_info.product),
             (unsigned short)(device_info.version));
      switch (device_info.bustype) {
         case BUS_PCI : {
               printf (" is on a PCI bus\n");
               break;
            }
         case BUS_USB : {
               printf (" is on a Universal Serial Bus\n");
               break;
            }
         default: {
               printf (" is on a bus that is not a PCI nor an USB bus\n");
               break;
            }
      }

      {
         size_t yalv;
         __u8 evtype_b[(EV_MAX + 7) / 8];
         memset (evtype_b, 0, sizeof(evtype_b));
         ret = ioctl (fd, EVIOCGBIT(0, EV_MAX), evtype_b);
         if (ret < 0) {
            perror ("evdev ioctl");
            break;
         }

         printf ("Supported event types:\n");

         for (yalv = 0; yalv < EV_MAX; yalv += 1) {
            if ((evtype_b[yalv >> 3] & (1 << (yalv & 0x07)))) {
               /* the bit is set in the event types list */
               printf("  Event type 0x%02x ", (unsigned int)(yalv));
               switch (yalv) {
                  case EV_SYN :
                     printf (" (Synch Events)\n");
                     break;
                  case EV_KEY :
                     printf (" (Keys or Buttons)\n");
                     break;
                  case EV_REL :
                     printf (" (Relative Axes)\n");
                     break;
                  case EV_ABS :
                     printf (" (Absolute Axes)\n");
                     break;
                  case EV_MSC :
                     printf (" (Miscellaneous)\n");
                     break;
                  case EV_LED :
                     printf (" (LEDs)\n");
                     break;
                  case EV_SND :
                     printf (" (Sounds)\n");
                     break;
                  case EV_REP :
                     printf (" (Repeat)\n");
                     break;
                  case EV_FF :
                  case EV_FF_STATUS:
                     printf (" (Force Feedback)\n");
                     break;
                  case EV_PWR:
                     printf (" (Power Management)\n");
                     break;
                  default:
                     printf (" (Unknown: 0x%04hx)\n", (unsigned short)(yalv));
                     break;
               }
            }
         }
      }

      {
         struct input_event ev; /* the event */
         size_t yalv;

         printf("Make the device ring\n");

         for (yalv = 0; (yalv < 2); yalv += 1) {
            ev.type = EV_SND;
            ev.code = SND_BELL;
            ev.value = 1;
            if (sizeof(ev) != write(fd, &(ev), sizeof(ev))) {
               perror("write");
               break;
            }
            usleep(2000000);

            ev.type = EV_SND;
            ev.code = SND_BELL;
            ev.value = 0;
            if (sizeof(ev) != write(fd, &(ev), sizeof(ev))) {
               perror("write");
               break;
            }

            usleep(2000000);
         }
      }

      printf("Waiting for events\n");

      for (;;) {
         int yalv;
         /* how many bytes were read */
         ssize_t rb;
         /* the events (up to 64 at once) */
         struct input_event ev[64];

         rb = read(fd, ev, sizeof(ev));

         if (rb < ((ssize_t)(sizeof(struct input_event)))) {
            perror("evtest: short read");
            ret = rb;
            break;
         }

         for (yalv = 0;
              (yalv < (rb / sizeof (struct input_event)));
              yalv += 1) {
            if (EV_KEY == ev[yalv].type) {
               printf("type %hu (EV_KEY) code %hu value %d\n",
                      (unsigned short)(ev[yalv].type),
                      (unsigned short)(ev[yalv].code),
                      (int)(ev[yalv].value));
            }
            else {
               printf("type %hu (unknown) code %hu value %d\n",
                      (unsigned short)(ev[yalv].type),
                      (unsigned short)(ev[yalv].code),
                      (int)(ev[yalv].value));
            }
         }
      }
   } while (0);

   if (fd >= 0) {
      close (fd);
      fd = -1;
   }

   return (ret);
}
