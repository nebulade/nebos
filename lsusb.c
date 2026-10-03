/* lsusb command: enumerate and print the USB device tree. */

#include "io.h"
#include "usb.h"

void program_lsusb_main(void)
{
	usb_enumerate_and_print();
}
