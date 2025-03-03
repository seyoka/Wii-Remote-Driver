#include <linux/module.h>
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/slab.h>

/* Nintendo Wii Remote vendor/product IDs.
 * Officially: Vendor = 0x057E, Product = 0x0306.
 */
#define WIIMOTE_VENDOR_ID  0x057E  
#define WIIMOTE_PRODUCT_ID 0x0306

/* Our custom driver data structure */
struct wii_data {
    struct input_dev *input;
};

/*
 * Custom raw_event callback.
 * This function is called whenever a new HID report is received.
 */
static int wii_parse_report(struct hid_device *hdev, struct hid_report *report,
                            u8 *data, int size)
{
    struct wii_data *wii = hid_get_drvdata(hdev);
    struct input_dev *in_dev = wii->input;

    /* Expect at least 3 bytes:
     * data[0]: Report ID
     * data[1]: First byte of button data
     * data[2]: Second byte of button data
     */
    if (size < 3)
        return 0;
    
    u8 report_id = data[0];
    u8 btn_byte1 = data[1];
    u8 btn_byte2 = data[2];

    /*
     * Mapping for the first byte (btn_byte1):
     * Bit 0: D-pad Right
     * Bit 1: D-pad Left
     * Bit 2: D-pad Down
     * Bit 3: D-pad Up
     * Bit 4: Plus Button
     * Bit 5: Minus Button
     * Bit 6: Home Button
     */
    bool dpad_right = btn_byte1 & 0x01;
    bool dpad_left  = btn_byte1 & 0x02;
    bool dpad_down  = btn_byte1 & 0x04;
    bool dpad_up    = btn_byte1 & 0x08;
    bool btn_plus   = btn_byte1 & 0x10;
    bool btn_minus  = btn_byte1 & 0x20;
    bool btn_home   = btn_byte1 & 0x40;

    /*
     * Mapping for the second byte (btn_byte2):
     * Bit 0: A Button
     * Bit 1: B Button
     * Bit 2: Button 1
     * Bit 3: Button 2
     */
    bool btn_A = btn_byte2 & 0x01;
    bool btn_B = btn_byte2 & 0x02;
    bool btn_1 = btn_byte2 & 0x04;
    bool btn_2 = btn_byte2 & 0x08;

    printk(KERN_INFO "Wii Remote Report: ID=%u, dpad(R:%d L:%d U:%d D:%d), plus=%d, minus=%d, home=%d, A=%d, B=%d, 1=%d, 2=%d\n",
           report_id, dpad_right, dpad_left, dpad_up, dpad_down,
           btn_plus, btn_minus, btn_home, btn_A, btn_B, btn_1, btn_2);

    /* Report these events via our allocated input device */
    input_report_key(in_dev, KEY_RIGHT, dpad_right);
    input_report_key(in_dev, KEY_LEFT,  dpad_left);
    input_report_key(in_dev, KEY_UP,    dpad_up);
    input_report_key(in_dev, KEY_DOWN,  dpad_down);
    input_report_key(in_dev, KEY_KPPLUS, btn_plus);
    input_report_key(in_dev, KEY_KPMINUS, btn_minus);
    input_report_key(in_dev, KEY_HOME,  btn_home);
    input_report_key(in_dev, KEY_A,     btn_A);
    input_report_key(in_dev, KEY_B,     btn_B);
    input_report_key(in_dev, KEY_1,     btn_1);
    input_report_key(in_dev, KEY_2,     btn_2);
    input_sync(in_dev);

    return 0;
}

/*
 * Probe function: called when a device matching our ID table is connected.
 * Initializes the device, allocates an input device, and starts the HID hardware.
 */
static int wii_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
    int ret;
    struct wii_data *wii;

    ret = hid_parse(hdev);
    if (ret) {
        hid_err(hdev, "Failed to parse HID reports\n");
        return ret;
    }

    ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
    if (ret) {
        hid_err(hdev, "Failed to start HID device\n");
        return ret;
    }

    wii = devm_kzalloc(&hdev->dev, sizeof(*wii), GFP_KERNEL);
    if (!wii)
        return -ENOMEM;

    /* Allocate a new input device for the Wii Remote */
    wii->input = devm_input_allocate_device(&hdev->dev);
    if (!wii->input)
        return -ENOMEM;

    /* Set a name and id for the input device */
    wii->input->name = "Wii Remote";
    wii->input->id.bustype = hdev->bus;
    wii->input->id.vendor  = hdev->vendor;
    wii->input->id.product = hdev->product;
    wii->input->id.version = hdev->version;

    /* Declare that this device can generate key events */
    __set_bit(EV_KEY, wii->input->evbit);
    __set_bit(KEY_RIGHT, wii->input->keybit);
    __set_bit(KEY_LEFT,  wii->input->keybit);
    __set_bit(KEY_UP,    wii->input->keybit);
    __set_bit(KEY_DOWN,  wii->input->keybit);
    __set_bit(KEY_KPPLUS, wii->input->keybit);
    __set_bit(KEY_KPMINUS, wii->input->keybit);
    __set_bit(KEY_HOME,  wii->input->keybit);
    __set_bit(KEY_A,     wii->input->keybit);
    __set_bit(KEY_B,     wii->input->keybit);
    __set_bit(KEY_1,     wii->input->keybit);
    __set_bit(KEY_2,     wii->input->keybit);

    ret = input_register_device(wii->input);
    if (ret) {
        hid_err(hdev, "Failed to register input device\n");
        return ret;
    }

    /* Save our driver data pointer */
    hid_set_drvdata(hdev, wii);

    printk(KERN_INFO "Wii Remote driver probed successfully\n");
    return 0;
}

/*
 * Remove function: called when the device is disconnected or the module is unloaded.
 * Cleans up the hardware state.
 */
static void wii_remove(struct hid_device *hdev)
{
    hid_hw_stop(hdev);
    printk(KERN_INFO "Wii Remote driver removed\n");
}

/*
 * Device ID table: lists the devices supported by this driver.
 * Here we use BUS_BLUETOOTH and supply vendor, product, and version=0.
 */
static const struct hid_device_id wii_ids[] = {
    { HID_DEVICE(BUS_BLUETOOTH, WIIMOTE_VENDOR_ID, WIIMOTE_PRODUCT_ID, 0) },
    { }
};
MODULE_DEVICE_TABLE(hid, wii_ids);

/*
 * HID driver structure: ties together the probe, remove, ID table, and raw event callback.
 * We set .raw_event to our custom callback to intercept raw HID reports.
 */
static struct hid_driver wii_driver = {
    .name      = "wii_remote",
    .id_table  = wii_ids,
    .probe     = wii_probe,
    .remove    = wii_remove,
    .raw_event = wii_parse_report,
};

module_hid_driver(wii_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Skeleton Wii Remote driver for reading basic inputs");

