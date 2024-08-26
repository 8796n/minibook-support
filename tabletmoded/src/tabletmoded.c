#include <fcntl.h>
#include <grp.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <libevdev/libevdev.h>

#include "debug.h"
#include "device.h"
#include "server.h"
#include "vdevice.h"

#define VERSION "tabletmoded 1.1.0"

#define TABLETMODED_SOCK "/var/run/tabletmoded.sock"

// Accelerometer
// For detect the tablet mode
#define ACCEL_SCREEN_PATH "/sys/bus/iio/devices/iio:device0/"
#define ACCEL_BASE_PATH "/sys/bus/iio/devices/iio:device1/"

// Mouse
#define MINIBOOK_MOUSE_DEVICE "/dev/input/by-id/usb-0603_0003-event-mouse"
#define MINIBOOKX_MOUSE_DEVICE                                                 \
    "/dev/input/by-path/"                                                      \
    "pci-0000:00:15.3-platform-i2c_designware.3-event-mouse"
// Keyboard
#define KEYBOARD_DEVICE "/dev/input/by-path/platform-i8042-serio-0-event-kbd" 

#define LID_STATE_PATH "/proc/acpi/button/lid/LID0/state"

int mousefd = -1, keyboardfd = -1;
struct libevdev *mouse = NULL;
struct libevdev *keyboard = NULL;

server_t *server_addr = NULL;

int output = -1;
int is_enabled_tabletmode = 0;
int is_enabled_detection = 1;

// Recovery the device
void recovery_device() {
    if (output != -1) {
        ioctl(output, UI_DEV_DESTROY);
        close(output);
    }
     if (mousefd != -1) {
        libevdev_grab(mouse, LIBEVDEV_UNGRAB);
        close(mousefd);
    }
    if (keyboardfd != -1) {
         libevdev_grab(keyboard, LIBEVDEV_UNGRAB);
        close(keyboardfd);
    }
   
}

// Signal handler
void sigint_handler() {
    if (server_addr != NULL) {
        stop_server(server_addr);
    }
    recovery_device();
    exit(EXIT_SUCCESS);
}

int new_device() {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd == -1) {
        perror("Cannot open the output device");
        recovery_device();
        exit(EXIT_FAILURE);
    }

    // Enable the synchronization events
    ioctl(fd, UI_SET_EVBIT, EV_SYN);

    // Enable the switch events
    ioctl(fd, UI_SET_EVBIT, EV_SW);

    // Enable the tabletmode switch
    ioctl(fd, UI_SET_SWBIT, SW_TABLET_MODE);

    // Setup the device
    struct uinput_setup uisetup = {0};
    memset(&uisetup, 0, sizeof(uisetup));
    strcpy(uisetup.name, "MiniBookSupport Virtual Switch");
    uisetup.id.bustype = BUS_USB;
    uisetup.id.vendor = 0x1234;
    uisetup.id.product = 0x5678;
    uisetup.id.version = 1;

    if (ioctl(fd, UI_DEV_SETUP, &uisetup) < 0) {
        perror("Failed to setup the virtual device");
        recovery_device();
        exit(EXIT_FAILURE);
    }

    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        perror("Failed to create the virtual device");
        recovery_device();
        exit(EXIT_FAILURE);
    }
    return fd;
}

// Print the help message
void print_help() {
    printf("Usage: ./tabletmoded [-d] [-h] [--version]\n");
    printf("Options:\n");
    printf("  -d: Enable debug mode\n");
    printf("  -h: Print this help message\n");
    printf("  --version: Print the version\n");
}

// Parse the command line arguments
void parse_args(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) {
            enable_debug();
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("%s\n", VERSION);
            exit(EXIT_SUCCESS);
        } else if (strcmp(argv[i], "-h") == 0) {
            print_help();
            exit(EXIT_SUCCESS);
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_help();
            exit(EXIT_FAILURE);
        }
    }
}

// Send command to the other devices
int send_command(const char *path, uint8_t cmd, uint8_t data) {
    int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd == -1) {
        perror("Cannot create the socket");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, path);

    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("Cannot connect to the server");
        close(sockfd);
        return -1;
    }

    uint8_t buffer[2] = {cmd, data};
    if (send(sockfd, buffer, sizeof(buffer), 0) == -1) {
        perror("Cannot send the command");
        return -1;
    }

    uint8_t res;
    if (recv(sockfd, &res, sizeof(res), 0) == -1) {
        perror("Cannot receive the response");
        return -1;
    }

    close(sockfd);
    return (int)res;
}

void set_tabletmode(int value) {
    is_enabled_tabletmode = value;
    if (value == 1){
        if(libevdev_grab(mouse, LIBEVDEV_GRAB) != 0){
            perror("Cannot grab Mouse");
        }
        if(libevdev_grab(keyboard, LIBEVDEV_GRAB) != 0){
            perror("Cannot grab Keyboard");
        }
    }else{
        if(libevdev_grab(mouse, LIBEVDEV_UNGRAB) != 0){
            perror("Cannot ungrab Mouse");
        }
        if(libevdev_grab(keyboard, LIBEVDEV_UNGRAB) != 0){
            perror("Cannot ungrab Keyboard");
        }
    }
    emit(output, EV_SW, SW_TABLET_MODE, value);
    emit(output, EV_SYN, SYN_REPORT, 0);
}

// Server callback
uint8_t server_callback(uint8_t type, uint8_t data) {
    debug_printf("Server callback: %d %d\n", type, data);
    switch (type) {
    case 0:
        debug_printf("Set tabletmode detection: %d\n", data);
        is_enabled_detection = data;
        return 0;
    case 1:
        return (uint8_t)is_enabled_detection;
    default:
        break;
    }
    return 0;
}

// Main
int main(int argc, char *argv[]) {
    // Parse the command line arguments
    parse_args(argc, argv);

	int rc;

    output = new_device();
    if (output == -1) {
        perror("Cannot create the output device");
        recovery_device();
        return (EXIT_FAILURE);
    }

    // Register the signal handler
    signal(SIGINT, sigint_handler);

    // Create the server
    server_t server;
    server_addr = &server;
    // Setup the server
    setup_server(&server, TABLETMODED_SOCK, server_callback);

    // Tweaks the permission
    // chown wheel group
    struct group *grp = getgrnam("wheel");
    if (grp == NULL) {
        grp = getgrnam("sudo"); // for Ubuntu
    }
    if (grp == NULL) {
        perror("getgrnam");
        perror("Skip the permission tweaks");
    } else {
        if (chown(TABLETMODED_SOCK, -1, grp->gr_gid) == -1) {
            perror("chown");
            return -1;
        }
    }

    // Start the server
    if (start_server(&server) == 1) {
        perror("Cannot start the server");
        recovery_device();
        return (EXIT_FAILURE);
    }

    // Check the device model
    char device_model[256] = {0};
    get_device_model(device_model, sizeof(device_model));
    debug_printf("Device model: %s\n", device_model);
    if (strstr(device_model, "MiniBook") == NULL) {
        fprintf(stderr, "This device is not supported\n");
        recovery_device();
        return (EXIT_FAILURE);
    }
    // for MIniBook X (10-inch)
    if (strncmp(device_model, "MiniBook X", 10) == 0) {
        // Check the base accelerometer is available
        struct stat st;
        if (stat(ACCEL_BASE_PATH, &st) == -1) {
            // Enable the accelerometer
            // echo mxc4005 0x15 > /sys/bus/i2c/devices/i2c-0/new_device
            FILE *fp = fopen("/sys/bus/i2c/devices/i2c-0/new_device", "w");
            if (fp == NULL) {
                perror("Cannot open the new_device");
                recovery_device();
                return (EXIT_FAILURE);
            }
            fprintf(fp, "mxc4005 0x15\n");
            fclose(fp);
            sleep(1); // Wait for the device
        }
        // Check the base accelerometer is available again
        if (stat(ACCEL_BASE_PATH, &st) == -1) {
            fprintf(stderr, "Cannot enable the base accelerometer\n");
            recovery_device();
            return (EXIT_FAILURE);
        }
        mousefd = open(MINIBOOKX_MOUSE_DEVICE, O_RDONLY);
    } else {
        mousefd = open(MINIBOOK_MOUSE_DEVICE, O_RDONLY);
    }
	if (mousefd < 0) {
		perror("Failed to open Mouse device");
		return (EXIT_FAILURE);
	}
	rc = libevdev_new_from_fd(mousefd, &mouse);
	if (rc < 0) {
		perror("Failed to init libevdev");
		return (EXIT_FAILURE);
	}
    // Keybord
    keyboardfd = open(KEYBOARD_DEVICE, O_RDONLY);
    if (keyboardfd < 0) {
        perror("Failed to open Keyboard device");
        return (EXIT_FAILURE);
    }
	rc = libevdev_new_from_fd(keyboardfd, &keyboard);
	if (rc < 0) {
		perror("Failed to init libevdev");
		return (EXIT_FAILURE);
	}

    // Accelerometer scale
    float accel_screen_scale = 0.0f;
    float accel_base_scale = 0.0f;
    FILE *fp = fopen(ACCEL_SCREEN_PATH "in_accel_scale", "r");
    if (fp == NULL) {
        perror("Cannot open the accel scale: " ACCEL_SCREEN_PATH
               "in_accel_scale");
        recovery_device();
        return (EXIT_FAILURE);
    }
    fscanf(fp, "%f", &accel_screen_scale);
    fclose(fp);
    fp = fopen(ACCEL_BASE_PATH "in_accel_scale", "r");
    if (fp == NULL) {
        perror("Cannot open the accel scale: " ACCEL_BASE_PATH
               "in_accel_scale");
        recovery_device();
        return (EXIT_FAILURE);
    }
    fscanf(fp, "%f", &accel_base_scale);
    fclose(fp);

    // Accelerometer x, y, z
    double accel_screen_x = 0.0;
    double accel_screen_y = 0.0;
    double accel_screen_z = 0.0;
    double accel_base_x = 0.0;
    double accel_base_y = 0.0;
    double accel_base_z = 0.0;

    // Main loop
    while (1) {
        // 無効化されている場合はなにもしない
        if (is_enabled_detection == 0) {
            sleep(1);
            continue;
        }

        // 画面が閉じている場合はなにもしない
        FILE *fp_lid_status = fopen(LID_STATE_PATH, "r");
        if (fp_lid_status == NULL) {
            perror("Cannot open Lid status");
            recovery_device();
            return (EXIT_FAILURE);
        }
        char lid_status[256];
        if (fgets(lid_status, sizeof(lid_status), fp_lid_status) == NULL){
            perror("Cannot read Lid status");
            fclose(fp_lid_status);
            recovery_device();
            return (EXIT_FAILURE);
        }
        fclose(fp_lid_status);
        if (strstr(lid_status, "closed") != NULL){
            set_tabletmode(0);
            sleep(1);
            continue;
        }

        FILE *fp_screen_x = fopen(ACCEL_SCREEN_PATH "in_accel_x_raw", "r");
        if (fp_screen_x == NULL) {
            perror("Cannot open the accel x: " ACCEL_SCREEN_PATH
                   "in_accel_x_raw");
            recovery_device();
            return (EXIT_FAILURE);
        }
        FILE *fp_screen_y = fopen(ACCEL_SCREEN_PATH "in_accel_y_raw", "r");
        if (fp_screen_y == NULL) {
            perror("Cannot open the accel y: " ACCEL_SCREEN_PATH
                   "in_accel_y_raw");
            recovery_device();
            return (EXIT_FAILURE);
        }
        FILE *fp_screen_z = fopen(ACCEL_SCREEN_PATH "in_accel_z_raw", "r");
        if (fp_screen_z == NULL) {
            perror("Cannot open the accel z: " ACCEL_SCREEN_PATH
                   "in_accel_z_raw");
            recovery_device();
            return (EXIT_FAILURE);
        }
        FILE *fp_base_x = fopen(ACCEL_BASE_PATH "in_accel_x_raw", "r");
        if (fp_base_x == NULL) {
            perror("Cannot open the accel x: " ACCEL_BASE_PATH
                   "in_accel_x_raw");
            recovery_device();
            return (EXIT_FAILURE);
        }
        FILE *fp_base_y = fopen(ACCEL_BASE_PATH "in_accel_y_raw", "r");
        if (fp_base_y == NULL) {
            perror("Cannot open the accel z: " ACCEL_BASE_PATH
                   "in_accel_y_raw");
            recovery_device();
            return (EXIT_FAILURE);
        }
        FILE *fp_base_z = fopen(ACCEL_BASE_PATH "in_accel_z_raw", "r");
        if (fp_base_z == NULL) {
            perror("Cannot open the accel z: " ACCEL_BASE_PATH
                   "in_accel_z_raw");
            recovery_device();
            return (EXIT_FAILURE);
        }
        // Read the accelerometer
        fscanf(fp_screen_x, "%lf", &accel_screen_x);
        fscanf(fp_screen_y, "%lf", &accel_screen_y);
        fscanf(fp_screen_z, "%lf", &accel_screen_z);
        fscanf(fp_base_x, "%lf", &accel_base_x);
        fscanf(fp_base_y, "%lf", &accel_base_y);
        fscanf(fp_base_z, "%lf", &accel_base_z);

        // Close the files
        fclose(fp_screen_x);
        fclose(fp_screen_y);
        fclose(fp_screen_z);
        fclose(fp_base_x);
        fclose(fp_base_y);
        fclose(fp_base_z);

        // Scale the accelerometer
        accel_screen_x *= accel_screen_scale;
        accel_screen_y *= accel_screen_scale;
        accel_screen_z *= accel_screen_scale;
        accel_base_x *= accel_base_scale;
        accel_base_y *= accel_base_scale;
        accel_base_z *= accel_base_scale;

        debug_printf("Screen: x:%f y:%f z:%f\n", accel_screen_x, accel_screen_y,
                     accel_screen_z);
        debug_printf("Base  : x:%f y:%f z:%f\n", accel_base_x, accel_base_y,
                     accel_base_z);

        // Get the angle from x, z
        double angle_screen =
            -atan2(accel_screen_x, accel_screen_z) * 180.0 / M_PI;
        double angle_base = -atan2(accel_base_x, accel_base_z) * 180.0 / M_PI;

        double angle = angle_base - angle_screen;

        // このあたりはフィーリングで補正している
        if (angle < 0 && angle_base < 0 && angle_screen > 0) {
            angle += 360.0;
        }

        if (accel_screen_x > 3.0 || accel_screen_x < -3.0 ||
            accel_screen_z > 3.0 || accel_screen_z < -3.0) {

            if (360 - angle < 60 && angle > 0 && !is_enabled_tabletmode) {
                set_tabletmode(1);
            } else if (angle < 10 && angle > -60 && !is_enabled_tabletmode) {
                set_tabletmode(1);
            } else if (angle > 10 && angle < 180 && is_enabled_tabletmode) {
                set_tabletmode(0);
            }
        }

        debug_printf("angle_screen: %f\n", angle_screen);
        debug_printf("angle_base: %f\n", angle_base);
        debug_printf("diff: %f\n", angle);
        debug_printf("tabletmode: %d\n", is_enabled_tabletmode);

        sleep(1);
    }

    // Never reach here...
    ioctl(output, UI_DEV_DESTROY);
    close(output);
    libevdev_grab(mouse, LIBEVDEV_UNGRAB);
    close(mousefd);
    libevdev_grab(keyboard, LIBEVDEV_UNGRAB);
    close(keyboardfd);

    return (EXIT_SUCCESS);
}
