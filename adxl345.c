
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/spi/spidev.h>

#define SPI_DEVICE "/dev/spidev0.0"
#define SPI_SPEED  5000000
#define CSV_FILE   "logs/adxl345.csv"

static volatile sig_atomic_t running = 1;

static void stop_program(int sig)
{
    (void)sig;
    running = 0;
}

/* Read ADXL345 registers using SPI */
static int read_registers(int fd, uint8_t reg,
                          uint8_t *data, int len)
{
    uint8_t tx[8] = {0};
    uint8_t rx[8] = {0};

    if (len < 1 || len > 6)
        return -1;

    tx[0] = reg | 0x80;

    if (len > 1)
        tx[0] |= 0x40;

    struct spi_ioc_transfer transfer = {0};

    transfer.tx_buf = (uintptr_t)tx;
    transfer.rx_buf = (uintptr_t)rx;
    transfer.len = len + 1;
    transfer.speed_hz = SPI_SPEED;
    transfer.bits_per_word = 8;

    if (ioctl(fd, SPI_IOC_MESSAGE(1), &transfer) < 0)
        return -1;

    memcpy(data, rx + 1, len);
    return 0;
}

/* Write an ADXL345 register */
static int write_register(int fd, uint8_t reg, uint8_t value)
{
    uint8_t buffer[2] = {reg, value};

    if (write(fd, buffer, sizeof(buffer)) != sizeof(buffer))
        return -1;

    return 0;
}

/* Generate timestamp with milliseconds */
static void get_timestamp(char *buffer, size_t size)
{
    struct timespec ts;
    struct tm local_time;

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &local_time);

    strftime(buffer, size, "%y-%m-%d %H:%M:%S", &local_time);

    size_t used = strlen(buffer);

    snprintf(buffer + used, size - used, ".%03ld",
             ts.tv_nsec / 1000000);
}

int main(void)
{
    signal(SIGINT, stop_program);
    signal(SIGTERM, stop_program);

    /* Create logs directory if necessary */
    if (mkdir("logs", 0755) < 0) {
        struct stat st;

        if (stat("logs", &st) < 0 || !S_ISDIR(st.st_mode)) {
            perror("Cannot create logs directory");
            return 1;
        }
    }

    /* Open CSV in append/read mode */
    FILE *csv = fopen(CSV_FILE, "a+");

    if (csv == NULL) {
        perror("Cannot open CSV file");
        return 1;
    }

    /* Create heading only if file is empty */
    if (fseek(csv, 0, SEEK_END) != 0) {
        perror("CSV seek failed");
        fclose(csv);
        return 1;
    }

    if (ftell(csv) == 0) {
        if (fprintf(csv,
                    "    Timestamp     ,X Axis, Y Axis, Z Axis \n") < 0 ||
            fflush(csv) == EOF) {
            perror("CSV heading write failed");
            fclose(csv);
            return 1;
        }
    }

    /* Open SPI device */
    int fd = open(SPI_DEVICE, O_RDWR);

    if (fd < 0) {
        perror("Cannot open SPI device");
        fclose(csv);
        return 1;
    }

    uint8_t mode = SPI_MODE_3;
    uint8_t bits = 8;
    uint32_t speed = SPI_SPEED;

    if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
        perror("SPI configuration failed");
        close(fd);
        fclose(csv);
        return 1;
    }

    /* Verify sensor identity */
    uint8_t device_id = 0;

    if (read_registers(fd, 0x00, &device_id, 1) < 0) {
        perror("Cannot read ADXL345 ID");
        close(fd);
        fclose(csv);
        return 1;
    }

    printf("ADXL345 Device ID: 0x%02X\n", device_id);

    if (device_id != 0xE5) {
        fprintf(stderr,
                "Incorrect device ID. Check SPI wiring and mode.\n");
        close(fd);
        fclose(csv);
        return 1;
    }

    /* Configure sensor */
    if (write_register(fd, 0x2D, 0x00) < 0 ||
        write_register(fd, 0x31, 0x08) < 0 ||
        write_register(fd, 0x2C, 0x0A) < 0 ||
        write_register(fd, 0x2D, 0x08) < 0) {
        perror("ADXL345 configuration failed");
        close(fd);
        fclose(csv);
        return 1;
    }

    printf("\nADXL345 initialized successfully!\n");
    printf("CSV file: %s\n\n", CSV_FILE);

    /* Terminal column headings */
    printf("%-23s | %12s | %12s | %12s\n",
           "Timestamp  | ", "X Axis (g) |  ",
           "Y Axis (g)|  ", "Z Axis (g)");

    printf("--------------------------------------------------------------------\n");
    printf("Recording data. Press Ctrl+C to stop.\n\n");

    while (running) {
        uint8_t data[6];

        if (read_registers(fd, 0x32, data, 6) < 0) {
            perror("Acceleration read failed");
            break;
        }

        int16_t x = (int16_t)(
            (uint16_t)data[0] | ((uint16_t)data[1] << 8));

        int16_t y = (int16_t)(
            (uint16_t)data[2] | ((uint16_t)data[3] << 8));

        int16_t z = (int16_t)(
            (uint16_t)data[4] | ((uint16_t)data[5] << 8));

        /* Full-resolution sensitivity: approximately 3.9 mg/LSB */
 //       double x_g = x * 0.0039;
   //     double y_g = y * 0.0039;
     //   double z_g = z * 0.0039;
	double x_g = x * 0.0039 * 9.80665;
	double y_g = y * 0.0039 * 9.80665;
	double z_g = z * 0.0039 * 9.80665;




        char timestamp[40];
        get_timestamp(timestamp, sizeof(timestamp));

        /* Print readings in aligned terminal columns */
        printf("%-23s |x = %12.3f |  y = %12.3f | z = %12.3f\n",
               timestamp, x_g, y_g, z_g);

        /* Save readings to CSV */
        if (fprintf(csv, "%s, %.3f, %.3f, %.3f\n",
                    timestamp, x_g, y_g, z_g) < 0) {
            perror("CSV write failed");
            break;
        }

        /* Flush every reading */
        if (fflush(csv) == EOF) {
            perror("CSV flush failed");
            break;
        }

        usleep(200);  /* Original delay */
    }

    fclose(csv);
    close(fd);

    printf("\nADXL345 recording stopped.\n");
    printf("Data saved in %s\n", CSV_FILE);

    return 0;
}
