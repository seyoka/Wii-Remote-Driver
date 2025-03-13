/*
 * wii_remote_driver.c - A character/HID driver for a Wii Remote.
 *
 * This driver registers as a HID driver to capture raw reports from the Wii remote.
 * It performs basic input mapping (using button bit masks from your older working code)
 * and writes human-readable results to a circular buffer. The circular buffer is then
 * exposed via a character device (/dev/wii_remote) for user-space consumption.
 *
 * Additionally, an ioctl command triggers an output report (command 0x15) to request
 * a battery/status update, and the corresponding battery level (report ID 0x20) is also
 * written into the buffer. A /proc entry is created to report driver state.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/hid.h>
#include <linux/string.h>
#include <linux/ioctl.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#define DRIVER_NAME "wii_remote_driver"
#define DEVICE_NAME "wii_remote"
#define CIRC_BUFFER_SIZE 1024

/* IOCTL command to request a battery/status update */
#define WIIMOTE_IOCTL_REQUEST_STATUS _IO('W', 1)

/*  circular buffer for mapped output */
static char circ_buffer[CIRC_BUFFER_SIZE];
static int head = 0, tail = 0;
static DEFINE_MUTEX(circ_mutex);

/* pointer to the HID device instance */
static struct hid_device *wii_hid_dev = NULL;


static int wii_connected = 0;     /* 1 if connected, 0 if not */
static int wii_last_battery = -1; /* -1 means unknown */
static struct proc_dir_entry *wii_proc_entry;


static void circ_buffer_write(const char *data, size_t len)
{
    size_t i;
    mutex_lock(&circ_mutex);
    for (i = 0; i < len; i++) {
        int next = (head + 1) % CIRC_BUFFER_SIZE;
        if (next == tail) {
            printk(KERN_WARNING DRIVER_NAME ": circular buffer full, dropping data\n");
            break;
        }
        circ_buffer[head] = data[i];
        head = next;
    }
    mutex_unlock(&circ_mutex);
}

/*
 * perform_input_mapping - this parses a button report and write a human-readable string
 * into the circular buffer.
 *
 * :
 *   Byte 0: Report ID.
 *   Byte 1: D-pad and special buttons:
 *            Bit 0: D-pad Right
 *            Bit 1: D-pad Left
 *            Bit 2: D-pad Down
 *            Bit 3: D-pad Up
 *            Bit 4: Plus Button
 *            Bit 5: Minus Button
 *            Bit 6: Home Button
 *   Byte 2: Action buttons:
 *            Bit 0: A Button
 *            Bit 1: B Button
 *            Bit 2: Button 1
 *            Bit 3: Button 2
 */
static void perform_input_mapping(const u8 *data, int size)
{
    char mapping_output[256];
    int len = 0;

    if (size < 3) {
        printk(KERN_WARNING DRIVER_NAME ": Report too short for mapping\n");
        return;
    }

    u8 report_id = data[0];
    u8 btn_byte1 = data[1];
    u8 btn_byte2 = data[2];

    len += snprintf(mapping_output + len, sizeof(mapping_output) - len,
                    "Report: ID=%u, ", report_id);

    if (btn_byte1 & 0x01)
        len += snprintf(mapping_output + len, sizeof(mapping_output) - len, "Dpad_Right ");
    if (btn_byte1 & 0x02)
        len += snprintf(mapping_output + len, sizeof(mapping_output) - len, "Dpad_Left ");
    if (btn_byte1 & 0x04)
        len += snprintf(mapping_output + len, sizeof(mapping_output) - len, "Dpad_Down ");
    if (btn_byte1 & 0x08)
        len += snprintf(mapping_output + len, sizeof(mapping_output) - len, "Dpad_Up ");
    if (btn_byte1 & 0x10)
        len += snprintf(mapping_output + len, sizeof(mapping_output) - len, "Plus ");
    if (btn_byte1 & 0x20)
        len += snprintf(mapping_output + len, sizeof(mapping_output) - len, "Minus ");
    if (btn_byte1 & 0x40)
        len += snprintf(mapping_output + len, sizeof(mapping_output) - len, "Home ");

    if (btn_byte2 & 0x01)
        len += snprintf(mapping_output + len, sizeof(mapping_output) - len, "A ");
    if (btn_byte2 & 0x02)
        len += snprintf(mapping_output + len, sizeof(mapping_output) - len, "B ");
    if (btn_byte2 & 0x04)
        len += snprintf(mapping_output + len, sizeof(mapping_output) - len, "1 ");
    if (btn_byte2 & 0x08)
        len += snprintf(mapping_output + len, sizeof(mapping_output) - len, "2 ");

    if (len == 0)
        len = snprintf(mapping_output, sizeof(mapping_output), "No buttons pressed");

    if (len < sizeof(mapping_output) - 1) {
        mapping_output[len++] = '\n';
        mapping_output[len] = '\0';
    }

    circ_buffer_write(mapping_output, len);
}

/* Character device file operations */
static int device_open(struct inode *inode, struct file *file)
{
    return 0;
}

static int device_release(struct inode *inode, struct file *file)
{
    return 0;
}

static ssize_t device_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
    size_t bytes_copied = 0;

    mutex_lock(&circ_mutex);
    while (bytes_copied < count && tail != head) {
        if (copy_to_user(buf + bytes_copied, &circ_buffer[tail], 1)) {
            mutex_unlock(&circ_mutex);
            return -EFAULT;
        }
        tail = (tail + 1) % CIRC_BUFFER_SIZE;
        bytes_copied++;
    }
    mutex_unlock(&circ_mutex);
    return bytes_copied;
}

static long device_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    int ret = 0;
    switch (cmd) {
    case WIIMOTE_IOCTL_REQUEST_STATUS:
        if (wii_hid_dev) {
            u8 status_request[2] = { 0x15, 0x00 };
            printk(KERN_INFO "Sending battery status request (output report 0x15)\n");
            ret = hid_hw_raw_request(wii_hid_dev,
                                     status_request[0],
                                     status_request,
                                     sizeof(status_request),
                                     HID_OUTPUT_REPORT,
                                     HID_REQ_SET_REPORT);
            printk(KERN_INFO "Battery status request returned: %d\n", ret);
            if (ret < 0)
                printk(KERN_ERR DRIVER_NAME ": failed to send status request, error %d\n", ret);
        } else {
            printk(KERN_ERR DRIVER_NAME ": HID device not available for status request\n");
            ret = -ENODEV;
        }
        break;
    default:
        ret = -ENOTTY;
    }
    return ret;
}

static struct file_operations fops = {
    .owner          = THIS_MODULE,
    .open           = device_open,
    .release        = device_release,
    .read           = device_read,
    .unlocked_ioctl = device_ioctl,
};


static int wii_proc_show(struct seq_file *m, void *v)
{
    seq_printf(m, "Wii Remote Driver State:\n");
    seq_printf(m, "  Connected: %s\n", wii_connected ? "Yes" : "No");
    seq_printf(m, "  Last Battery: %d\n", wii_last_battery);
    return 0;
}

static int wii_proc_open(struct inode *inode, struct file *file)
{
    return single_open(file, wii_proc_show, NULL);
}


static const struct proc_ops wii_proc_ops = {
    .proc_open    = wii_proc_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};

/* Character device variables */
static int major;
static struct class *wii_class;
static struct cdev wii_cdev;

/*
 * wii_raw_event - HID raw event callback.
 *
 * When a new HID report is received from the Wii remote, this callback is invoked.
 * otherwise, we perform input mapping.
 */
static int wii_raw_event(struct hid_device *hdev, struct hid_report *report, u8 *data, int size)
{
    int i;
    printk(KERN_INFO "wii_raw_event: received report: ");
    for (i = 0; i < size; i++)
        printk(KERN_CONT "%02x ", data[i]);
    printk(KERN_CONT "\n");

    if (size > 0 && data[0] == 0x20) {
        printk(KERN_INFO "Battery status report detected.\n");
        if (size >= 2) {
            char battery_output[64];
            int len = snprintf(battery_output, sizeof(battery_output), "Battery: %d\n", data[1]);
            /* Cache the battery level */
            wii_last_battery = data[1];
            circ_buffer_write(battery_output, len);
        }
    } else {
        perform_input_mapping(data, size);
    }
    return 0;
}

/* HID probe: called when a matching device is connected */
static int wii_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
    int ret;

    ret = hid_parse(hdev);
    if (ret)
        return ret;

    ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
    if (ret)
        return ret;

    wii_hid_dev = hdev;
    wii_connected = 1;
    printk(KERN_INFO DRIVER_NAME ": Wii remote connected\n");
    return 0;
}

/* HID remove: called when the device is disconnected */
static void wii_remove(struct hid_device *hdev)
{
    wii_hid_dev = NULL;
    wii_connected = 0;
    printk(KERN_INFO DRIVER_NAME ": Wii remote disconnected\n");
}

/* HID device ID table for the Wii remote.
 * Updated for current kernels with four arguments.
 */
static const struct hid_device_id wii_remote_devices[] = {
    { HID_DEVICE(BUS_BLUETOOTH, 0x057e, 0x0306, 0) },
    { }
};
MODULE_DEVICE_TABLE(hid, wii_remote_devices);

/* HID driver structure */
static struct hid_driver wii_driver = {
    .name       = DRIVER_NAME,
    .id_table   = wii_remote_devices,
    .probe      = wii_probe,
    .remove     = wii_remove,
    .raw_event  = wii_raw_event,
};

static int __init wii_init(void)
{
    int ret;
    dev_t dev;


    wii_proc_entry = proc_create("wii_remote", 0, NULL, &wii_proc_ops);
    if (!wii_proc_entry) {
        printk(KERN_ERR DRIVER_NAME ": failed to create /proc/wii_remote\n");
        return -ENOMEM;
    }

    /* Allocate a character device region */
    ret = alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        printk(KERN_ERR DRIVER_NAME ": failed to allocate char device region\n");
        return ret;
    }
    major = MAJOR(dev);

    /* Initialize and add the character device */
    cdev_init(&wii_cdev, &fops);
    ret = cdev_add(&wii_cdev, dev, 1);
    if (ret < 0) {
        unregister_chrdev_region(dev, 1);
        printk(KERN_ERR DRIVER_NAME ": failed to add cdev\n");
        return ret;
    }

    /* Create a device class and file in /dev */
    wii_class = class_create(DEVICE_NAME);
    if (IS_ERR(wii_class)) {
        cdev_del(&wii_cdev);
        unregister_chrdev_region(dev, 1);
        printk(KERN_ERR DRIVER_NAME ": failed to create class\n");
        return PTR_ERR(wii_class);
    }
    device_create(wii_class, NULL, dev, NULL, DEVICE_NAME);

    /* Register the HID driver */
    ret = hid_register_driver(&wii_driver);
    if (ret) {
        device_destroy(wii_class, dev);
        class_destroy(wii_class);
        cdev_del(&wii_cdev);
        unregister_chrdev_region(dev, 1);
        printk(KERN_ERR DRIVER_NAME ": failed to register HID driver\n");
        return ret;
    }

    printk(KERN_INFO DRIVER_NAME ": driver loaded (major %d)\n", major);
    return 0;
}

static void __exit wii_exit(void)
{
    dev_t dev = MKDEV(major, 0);

    if (wii_proc_entry) {
        remove_proc_entry("wii_remote", NULL);
        wii_proc_entry = NULL;
    }

    hid_unregister_driver(&wii_driver);
    device_destroy(wii_class, dev);
    class_destroy(wii_class);
    cdev_del(&wii_cdev);
    unregister_chrdev_region(dev, 1);

    printk(KERN_INFO DRIVER_NAME ": driver unloaded\n");
}

module_init(wii_init);
module_exit(wii_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ryan, Ciaran and Peter ");
MODULE_DESCRIPTION("");
