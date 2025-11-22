// alert_udp_server.c
// Compile: gcc alert_udp_server.c -o alert_udp_server -lcjson
// Run: ./alert_udp_server
//
// Requires cJSON library (install libcjson-dev / cjson).
// On Debian/Ubuntu: sudo apt install libcjson-dev
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <math.h>
#include <cjson/cJSON.h>

#define PORT 5005
#define BUFFER_SIZE 8192
#define MAX_DEVICES 1024
#define ALERT_LOGFILE "alerts.log"

// Equipment valid ranges
#define TEMP_MIN 0.0
#define TEMP_MAX 50.0
#define HUM_MIN 20.0
#define HUM_MAX 80.0

// Alert thresholds (differential)
#define TEMP_DIFF_THRESHOLD 2.0    // degrees
#define HUM_DIFF_THRESHOLD 5.0     // percent

typedef struct {
    char id[128];
    double temperature;
    double humidity;
    char dateObserved[64];
    struct sockaddr_in addr;
    int has_seq;
    long last_seq; // last sequence number processed (for guaranteed delivery)
    time_t last_seen;
} device_t;

static device_t devices[MAX_DEVICES];
static int device_count = 0;
static FILE *alert_log = NULL;

// Now accepts a pre-formatted string for simplicity.
static void log_alert(const char *message) {
    // print timestamped message to stdout and file
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char timebuf[64];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm_now);

    printf("[%s] %s\n", timebuf, message);

    if (alert_log) {
        fprintf(alert_log, "[%s] %s\n", timebuf, message);
        fflush(alert_log);
    }
}

static device_t* find_device_by_id(const char* id) {
    for (int i = 0; i < device_count; ++i) {
        if (strcmp(devices[i].id, id) == 0) return &devices[i];
    }
    return NULL;
}

static device_t* find_device_by_addr(struct sockaddr_in *addr) {
    for (int i = 0; i < device_count; ++i) {
        if (devices[i].addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
            devices[i].addr.sin_port == addr->sin_port) return &devices[i];
    }
    return NULL;
}

static device_t* add_or_get_device(const char* id, struct sockaddr_in *addr) {
    device_t *d = find_device_by_id(id);
    if (d) {
        // update address if changed
        d->addr = *addr;
        d->last_seen = time(NULL);
        return d;
    }
    if (device_count >= MAX_DEVICES) return NULL;
    d = &devices[device_count++];
    strncpy(d->id, id, sizeof(d->id)-1);
    d->id[sizeof(d->id)-1] = '\0';
    d->temperature = 0;
    d->humidity = 0;
    d->dateObserved[0] = '\0';
    d->addr = *addr;
    d->has_seq = 0;
    d->last_seq = -1;
    d->last_seen = time(NULL);
    return d;
}

static void send_ack(int sockfd, struct sockaddr_in *client_addr, socklen_t addrlen, const char* id, long seq) {
    if (!id) return;
    cJSON *ack = cJSON_CreateObject();
    if (!ack) {
        perror("cJSON_CreateObject failed in send_ack");
        return;
    }
    cJSON_AddStringToObject(ack, "type", "ACK");
    cJSON_AddStringToObject(ack, "id", id);
    cJSON_AddNumberToObject(ack, "seq", (double)seq); // Ensure correct number type for cJSON
    char *out = cJSON_PrintUnformatted(ack);
    if (out) {
        ssize_t sent = sendto(sockfd, out, strlen(out), 0, (struct sockaddr*)client_addr, addrlen);
        if (sent < 0) {
            perror("sendto (ACK) failed");
        }
        free(out);
    } else {
        perror("cJSON_PrintUnformatted failed in send_ack");
    }
    cJSON_Delete(ack);
}

int main() {
    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    char buffer[BUFFER_SIZE];
    char client_ip_str[INET_ADDRSTRLEN];
    
    // --- FIX: Increased size to avoid 'truncation' warnings from compiler ---
    // Safely accommodate the largest possible field (the 8192-byte buffer) plus overhead.
    char log_message[BUFFER_SIZE + 256]; 

    alert_log = fopen(ALERT_LOGFILE, "a");
    if (!alert_log) {
        perror("Failed to open alert log file");
    }

    // Create UDP socket
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Failed to create socket");
        exit(EXIT_FAILURE);
    }

    // Bind server to port
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(sockfd, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    printf("Alert UDP server running on port %d...\n", PORT);

    while (1) {
        socklen_t len = sizeof(client_addr);
        ssize_t n = recvfrom(sockfd, buffer, BUFFER_SIZE - 1, 0,
                             (struct sockaddr *) &client_addr, &len);

        if (n < 0) {
            if (errno == EINTR) continue;
            perror("Error receiving data");
            continue;
        }

        buffer[n] = '\0';  // Null-terminate

        if (inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip_str, INET_ADDRSTRLEN) == NULL) {
            strcpy(client_ip_str, "UNKNOWN_IP");
        }

        // Parse JSON
        cJSON *root = cJSON_Parse(buffer);
        if (!root) {
            snprintf(log_message, sizeof(log_message), "Received invalid JSON from %s:%d -> %s", 
                     client_ip_str, ntohs(client_addr.sin_port), buffer);
            log_alert(log_message);
            cJSON_Delete(root); // Added cleanup
            continue;
        }

        cJSON *jid = cJSON_GetObjectItemCaseSensitive(root, "id");
        cJSON *jtype = cJSON_GetObjectItemCaseSensitive(root, "type");
        cJSON *jtemp = cJSON_GetObjectItemCaseSensitive(root, "temperature");
        cJSON *jhum = cJSON_GetObjectItemCaseSensitive(root, "relativeHumidity");
        cJSON *jdate = cJSON_GetObjectItemCaseSensitive(root, "dateObserved");
        cJSON *jseq = cJSON_GetObjectItemCaseSensitive(root, "seq");
        cJSON *jqos = cJSON_GetObjectItemCaseSensitive(root, "qos");

        if (!cJSON_IsString(jid) || !cJSON_IsNumber(jtemp) || !cJSON_IsNumber(jhum)) {
            snprintf(log_message, sizeof(log_message), "Missing mandatory fields in JSON from %s:%d -> %s", 
                     client_ip_str, ntohs(client_addr.sin_port), buffer);
            log_alert(log_message);
            cJSON_Delete(root);
            continue;
        }

        const char *id = jid->valuestring;
        double temp = jtemp->valuedouble;
        double hum = jhum->valuedouble;
        const char *dateObserved = cJSON_IsString(jdate) ? jdate->valuestring : "";
        long seq = -1;
        int qos = 0;
        
        if (cJSON_IsNumber(jseq)) {
            double seq_val = cJSON_GetNumberValue(jseq);
            if (seq_val >= 0) {
                seq = (long)seq_val;
            } else {
                snprintf(log_message, sizeof(log_message), "Invalid sequence number value: %.0f from device %s", seq_val, id);
                log_alert(log_message);
            }
        }

        if (cJSON_IsNumber(jqos)) qos = jqos->valueint;

        // add or update device record
        device_t *dev = add_or_get_device(id, &client_addr);
        if (!dev) {
            snprintf(log_message, sizeof(log_message), "Device list full, cannot record device %s", id);
            log_alert(log_message);
            cJSON_Delete(root);
            continue;
        }

        // If qos==1 (guaranteed), check duplicate seq
        if (qos == 1) {
            if (seq == -1) {
                snprintf(log_message, sizeof(log_message), "QoS 1 packet missing 'seq' field from device %s (%s:%d)",
                            id, client_ip_str, ntohs(client_addr.sin_port));
                log_alert(log_message);
                cJSON_Delete(root);
                continue;
            }
            if (dev->has_seq && seq == dev->last_seq) {
                // duplicate: resend ACK (to help clients that missed it)
                snprintf(log_message, sizeof(log_message), "Duplicate seq %ld from device %s (%s:%d) - resending ACK",
                            seq, id, client_ip_str, ntohs(client_addr.sin_port));
                log_alert(log_message);
                send_ack(sockfd, &client_addr, len, id, seq);
                cJSON_Delete(root);
                continue; // ignore duplicate reading
            }
        }

        // Store reading
        dev->temperature = temp;
        dev->humidity = hum;
        strncpy(dev->dateObserved, dateObserved, sizeof(dev->dateObserved)-1);
        dev->dateObserved[sizeof(dev->dateObserved)-1] = '\0';
        dev->last_seen = time(NULL);

        if (qos == 1) {
            dev->has_seq = 1;
            dev->last_seq = seq;
            // send ACK immediately
            send_ack(sockfd, &client_addr, len, id, seq);
        }

        // Print received reading (This uses printf directly, not log_alert)
        printf("Received from %s:%d -> id=%s temp=%.2f hum=%.2f qos=%d seq=%ld\n",
               client_ip_str, ntohs(client_addr.sin_port),
               id, temp, hum, qos, seq);

        // Validate ranges
        int out_of_range = 0;
        if (temp < TEMP_MIN || temp > TEMP_MAX) {
            snprintf(log_message, sizeof(log_message), "OUT OF RANGE: device=%s temperature %.2f outside [%.1f,%.1f]", id, temp, TEMP_MIN, TEMP_MAX);
            log_alert(log_message);
            out_of_range = 1;
        }
        if (hum < HUM_MIN || hum > HUM_MAX) {
            snprintf(log_message, sizeof(log_message), "OUT OF RANGE: device=%s humidity %.2f outside [%.1f,%.1f]", id, hum, HUM_MIN, HUM_MAX);
            log_alert(log_message);
            out_of_range = 1;
        }

        // Compare with other devices to compute differentials
        for (int i = 0; i < device_count; ++i) {
            device_t *other = &devices[i];
            if (strcmp(other->id, dev->id) == 0) continue; // skip self
            // only compare with devices that have been seen recently (optional)
            double temp_diff = fabs(dev->temperature - other->temperature);
            double hum_diff  = fabs(dev->humidity - other->humidity);
            if (temp_diff >= TEMP_DIFF_THRESHOLD || hum_diff >= HUM_DIFF_THRESHOLD) {
                snprintf(log_message, sizeof(log_message), "DIFFERENTIAL ALERT: %s vs %s -> temp_diff=%.2f (th=%.2f) hum_diff=%.2f (th=%.2f)",
                          dev->id, other->id, temp_diff, TEMP_DIFF_THRESHOLD, hum_diff, HUM_DIFF_THRESHOLD);
                log_alert(log_message);
            }
        }

        cJSON_Delete(root);
    }

    if (alert_log) fclose(alert_log);
    close(sockfd);
    return 0;
}
