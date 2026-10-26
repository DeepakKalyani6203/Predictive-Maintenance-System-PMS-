
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glob.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

volatile sig_atomic_t running = 1;

void stop_logger(int sig)
{
    (void)sig;
    running = 0;
}

int main(void)
{
    signal(SIGINT, stop_logger);
    signal(SIGTERM, stop_logger);

    system("mkdir -p logs");

    FILE *csv = fopen("logs/ds18b20.csv", "a");

    if (csv == NULL) {
        perror("Error opening data file");
        return 1;
    }

    fseek(csv, 0, SEEK_END);

    if (ftell(csv) == 0) {
        fprintf(csv, "Temp_C,Time\n");
        fflush(csv);
    }

    printf("DS18B20 Temperature Logger Started\n");
    printf("Data file: logs/ds18b20.csv\n");
    printf("Press Ctrl+C to stop.\n\n");

    while (running) {
        glob_t paths = {0};

        int result = glob(
            "/sys/bus/w1/devices/28-*/w1_slave",
            0, NULL, &paths
        );

        if (result != 0 || paths.gl_pathc == 0) {
            printf("DS18B20 sensor not found.\n");
            globfree(&paths);
            sleep(2);
            continue;
        }

        FILE *sensor = fopen(paths.gl_pathv[0], "r");

        if (sensor == NULL) {
            perror("Error reading sensor");
            globfree(&paths);
            sleep(1);
            continue;
        }

        char line1[256], line2[256];

        int valid =
            fgets(line1, sizeof(line1), sensor) != NULL &&
            fgets(line2, sizeof(line2), sensor) != NULL;

        fclose(sensor);

        char *p = valid ? strstr(line2, "t=") : NULL;

        if (valid && strstr(line1, "YES") && p != NULL) {
            double temperature = strtol(p + 2, NULL, 10) / 1000.0;

            time_t now = time(NULL);
            struct tm local_time;
            char timestamp[32];

            if (localtime_r(&now, &local_time) != NULL &&
                strftime(timestamp, sizeof(timestamp),
                         "%y-%m-%d %H:%M:%S", &local_time) > 0) {

                printf("%.3f C,  %s\n", temperature, timestamp);

                fprintf(csv, "%.3f, %s\n",
                        temperature, timestamp);

                fflush(csv);
            }
        } else {
            printf("Invalid sensor reading. Retrying...\n");
        }

        globfree(&paths);
//        sleep(0.00001);
    }

    fclose(csv);

    printf("\nLogger stopped. Data saved.\n");

    return 0;
}
