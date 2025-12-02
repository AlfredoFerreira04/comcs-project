// alert_udp_server.c
// Compile: gcc alert_udp_server.c -o alert_udp_server -lcjson
// Run: ./alert_udp_server
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
#include <math.h>        // For fabs() function used in differential calculation
#include <cjson/cJSON.h> // For JSON parsing (Smartdata model)
#include <MQTTClient.h>
#include <pthread.h>
#include <unistd.h>

// Network Configuration (Req 2a)
#define PORT 5005
#define BUFFER_SIZE 8192
#define MAX_DEVICES 1024
#define ALERT_LOGFILE "alerts.log" // File for logging critical events (Req 2d)

// Equipment valid ranges (for basic data validation)
#define TEMP_MIN 0.0
#define TEMP_MAX 50.0
#define HUM_MIN 20.0
#define HUM_MAX 80.0

// Alert thresholds (differential) (Req 2e)
#define TEMP_DIFF_THRESHOLD 2.0 // degrees
#define HUM_DIFF_THRESHOLD 5.0  // percent

#define MQTT_ADDRESS "ssl://4979254f05ea480283d67c6f0d9f7525.s1.eu.hivemq.cloud:8883"
#define MQTT_CLIENT_ID "udp_alert_server"
#define MQTT_ALERT_TOPIC "/comcs/g04/alerts"

// Replace with your HiveMQ credentials
#define MQTT_USERNAME "web_client"
#define MQTT_PASSWORD "Password1"

MQTTClient client;
// Structure to track the state of each sending device (Req 2c)
typedef struct
{
    char id[128];
    double temperature; // Last reported temperature
    double humidity;    // Last reported humidity
    char dateObserved[64];
    struct sockaddr_in addr; // Client's network address
    int has_seq;             // Flag: 1 if we have processed a sequence number before
    long last_seq;           // Last sequence number processed (for Guaranteed Delivery check)
    time_t last_seen;
} device_t;

// Global storage for tracking connected devices (Req 2c)
static device_t devices[MAX_DEVICES];
static int device_count = 0;
static FILE *alert_log = NULL;

void *mqtt_thread_func(void *arg)
{
    while (1)
    {
        // Keep MQTT alive
        MQTTClient_yield();
        usleep(100 * 1000); // 100 ms sleep to avoid busy loop
    }
    return NULL;
}

static void log_alert(const char *message)
{
    // print timestamped message to stdout and file
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char timebuf[64];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm_now);

    printf("[%s] %s\n", timebuf, message);

    if (alert_log)
    {
        fprintf(alert_log, "[%s] %s\n", timebuf, message);
        fflush(alert_log);
    }
}

// Function to log alerts to stdout and a file (Req 2d)
static void log_alert_dual(const char *device,
                           const char *alert_type,
                           const char *message)
{
    /* --------- 1) PRINT & SAVE LOG ENTRY --------- */
    char formatted[256];
    snprintf(formatted, sizeof(formatted),
             "%s: device=%s: %s",
             alert_type, device, message);

    log_alert(formatted);

    /* --------- 2) BUILD JSON ALERT FOR MQTT --------- */
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return;

    cJSON_AddStringToObject(root, "device", device);
    cJSON_AddStringToObject(root, "alertType", alert_type);
    cJSON_AddStringToObject(root, "message", message);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str)
        return;

    /* --------- 3) PUBLISH JSON TO MQTT --------- */

    MQTTClient_message msg = MQTTClient_message_initializer;
    MQTTClient_deliveryToken token;

    msg.payload = json_str;
    msg.payloadlen = (int)strlen(json_str);
    msg.qos = 1;
    msg.retained = 0;

    int rc = MQTTClient_publishMessage(client, MQTT_ALERT_TOPIC, &msg, &token);
    if (rc == MQTTCLIENT_SUCCESS)
    {
        MQTTClient_waitForCompletion(client, token, 2000);
    }
    else
    {
        log_alert("WARNING: Failed to publish MQTT alert.");
    }

    free(json_str);
}

// Looks up a device based on its unique ID
static device_t *find_device_by_id(const char *id)
{
    for (int i = 0; i < device_count; ++i)
    {
        if (strcmp(devices[i].id, id) == 0)
            return &devices[i];
    }
    return NULL;
}

// Looks up a device based on its network address (less reliable, but available)
static device_t *find_device_by_addr(struct sockaddr_in *addr)
{
    for (int i = 0; i < device_count; ++i)
    {
        if (devices[i].addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
            devices[i].addr.sin_port == addr->sin_port)
            return &devices[i];
    }
    return NULL;
}

// Adds a new device or retrieves an existing one (Req 2c)
static device_t *add_or_get_device(const char *id, struct sockaddr_in *addr)
{
    device_t *d = find_device_by_id(id);
    if (d)
    {
        // If device exists, update its network address and last seen time
        d->addr = *addr;
        d->last_seen = time(NULL);
        return d;
    }

    // Add new device if space is available
    if (device_count >= MAX_DEVICES)
        return NULL;
    d = &devices[device_count++];

    // Initialize new device struct
    strncpy(d->id, id, sizeof(d->id) - 1);
    d->id[sizeof(d->id) - 1] = '\0';
    d->temperature = 0;
    d->humidity = 0;
    d->dateObserved[0] = '\0';
    d->addr = *addr;
    d->has_seq = 0;
    d->last_seq = -1;
    d->last_seen = time(NULL);
    return d;
}

// Sends an ACK message back to the client for QoS=1 (Guaranteed Delivery) (Req 2b)
static void send_ack(int sockfd, struct sockaddr_in *client_addr, socklen_t addrlen, const char *id, long seq)
{
    if (!id)
        return;

    // Use cJSON to build the ACK response
    cJSON *ack = cJSON_CreateObject();
    if (!ack)
    {
        perror("cJSON_CreateObject failed in send_ack");
        return;
    }

    cJSON_AddStringToObject(ack, "type", "ACK");
    cJSON_AddStringToObject(ack, "id", id);
    cJSON_AddNumberToObject(ack, "seq", (double)seq); // Sequence number must match the received one

    char *out = cJSON_PrintUnformatted(ack); // Print compact JSON string
    if (out)
    {
        // Send the ACK via the UDP socket
        ssize_t sent = sendto(sockfd, out, strlen(out), 0, (struct sockaddr *)client_addr, addrlen);
        if (sent < 0)
        {
            perror("sendto (ACK) failed");
        }
        free(out); // Free cJSON string
    }
    else
    {
        perror("cJSON_PrintUnformatted failed in send_ack");
    }
    cJSON_Delete(ack); // Free cJSON object
}

int main()
{
    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    char buffer[BUFFER_SIZE];
    char client_ip_str[INET_ADDRSTRLEN];
    char log_message[BUFFER_SIZE + 256];
    pthread_t mqtt_thread;
    int mqtt_thread_created = 0;

    // Open the alert log file for appending
    alert_log = fopen(ALERT_LOGFILE, "a");
    if (!alert_log)
    {
        perror("Failed to open alert log file");
    }

    // Create UDP socket (AF_INET for IPv4, SOCK_DGRAM for UDP)
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
    {
        perror("Failed to create socket");
        exit(EXIT_FAILURE);
    }

    // Configure server address structure
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY; // Listen on all interfaces
    server_addr.sin_port = htons(PORT);       // Convert port to network byte order

    // Bind server socket to the address and port (Req 2a)
    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("Bind failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    printf("Alert UDP server running on port %d...\n", PORT);

    MQTTClient_create(&client, MQTT_ADDRESS, MQTT_CLIENT_ID, MQTTCLIENT_PERSISTENCE_NONE, NULL);

    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    MQTTClient_SSLOptions ssl_opts = MQTTClient_SSLOptions_initializer;
    ssl_opts.enableServerCertAuth = 1;
    ssl_opts.trustStore = "./cert.pem";

    conn_opts.keepAliveInterval = 20;
    conn_opts.cleansession = 1;
    conn_opts.username = MQTT_USERNAME;
    conn_opts.password = MQTT_PASSWORD;
    conn_opts.ssl = &ssl_opts;

    int rc;
    if ((rc = MQTTClient_connect(client, &conn_opts)) != MQTTCLIENT_SUCCESS)
    {
        printf("Failed to connect, return code %d\n", rc);
        exit(-1);
    }
    else
    {
        printf("Connected to MQTT broker at %s\n", MQTT_ADDRESS);
        if (pthread_create(&mqtt_thread, NULL, mqtt_thread_func, NULL) != 0)
        {
            perror("Failed to create MQTT thread");
            exit(EXIT_FAILURE);
        }
        mqtt_thread_created = 1;
    }

    // Main server loop (Req 2c)
    while (1)
    {
        socklen_t len = sizeof(client_addr);

        // Receive data from any client. Blocks until a packet is received.
        ssize_t n = recvfrom(sockfd, buffer, BUFFER_SIZE - 1, 0,
                             (struct sockaddr *)&client_addr, &len);

        if (n < 0)
        {
            if (errno == EINTR)
                continue; // Handle interrupted system calls
            perror("Error receiving data");
            continue;
        }

        buffer[n] = '\0'; // Null-terminate the received data

        // Convert client's IP address to a readable string
        if (inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip_str, INET_ADDRSTRLEN) == NULL)
        {
            strcpy(client_ip_str, "UNKNOWN_IP");
        }

        // Parse incoming JSON payload (Req 2g: Smartdata model)
        cJSON *root = cJSON_Parse(buffer);
        if (!root)
        {
            snprintf(log_message, sizeof(log_message), "Received invalid JSON from %s:%d -> %s",
                     client_ip_str, ntohs(client_addr.sin_port), buffer);
            log_alert(log_message);
            cJSON_Delete(root);
            continue;
        }

        // Extract key fields (Req 2g)
        cJSON *jid = cJSON_GetObjectItemCaseSensitive(root, "id");
        cJSON *jtemp = cJSON_GetObjectItemCaseSensitive(root, "temperature");
        cJSON *jhum = cJSON_GetObjectItemCaseSensitive(root, "relativeHumidity");
        cJSON *jdate = cJSON_GetObjectItemCaseSensitive(root, "dateObserved");
        cJSON *jseq = cJSON_GetObjectItemCaseSensitive(root, "seq");
        cJSON *jqos = cJSON_GetObjectItemCaseSensitive(root, "qos");

        // Basic validation for mandatory fields (Req 2d)
        if (!cJSON_IsString(jid) || !cJSON_IsNumber(jtemp) || !cJSON_IsNumber(jhum))
        {
            snprintf(log_message, sizeof(log_message), "Missing mandatory fields in JSON from %s:%d -> %s",
                     client_ip_str, ntohs(client_addr.sin_port), buffer);
            log_alert(log_message);
            cJSON_Delete(root);
            continue;
        }

        // Safely extract values
        const char *id = jid->valuestring;
        double temp = jtemp->valuedouble;
        double hum = jhum->valuedouble;
        const char *dateObserved = cJSON_IsString(jdate) ? jdate->valuestring : "";
        long seq = -1;
        int qos = 0;

        if (cJSON_IsNumber(jseq))
        {
            double seq_val = cJSON_GetNumberValue(jseq);
            if (seq_val >= 0)
            {
                seq = (long)seq_val;
            }
        }
        if (cJSON_IsNumber(jqos))
            qos = jqos->valueint;

        // Add or retrieve device state (Req 2c)
        device_t *dev = add_or_get_device(id, &client_addr);
        if (!dev)
        {
            snprintf(log_message, sizeof(log_message), "Device list full, cannot record device %s", id);
            log_alert(log_message);
            cJSON_Delete(root);
            continue;
        }

        // --- QoS CHECK & ACK LOGIC (Req 2b) ---
        if (qos == 1)
        {
            if (seq == -1)
            {
                // Ignore QoS 1 packets without a sequence number
                snprintf(log_message, sizeof(log_message), "QoS 1 packet missing 'seq' field from device %s", id);
                log_alert(log_message);
                cJSON_Delete(root);
                continue;
            }
            if (dev->has_seq && seq == dev->last_seq)
            {
                // DUPLICATE PACKET: Resend ACK and ignore data to prevent duplicate processing
                snprintf(log_message, sizeof(log_message), "Duplicate seq %ld from device %s - resending ACK",
                         seq, id);
                log_alert(log_message);
                send_ack(sockfd, &client_addr, len, id, seq);
                cJSON_Delete(root);
                continue; // Skip data processing for duplicates
            }
        }
        // --- END QoS CHECK ---

        // Store reading (only if not a duplicate)
        dev->temperature = temp;
        dev->humidity = hum;
        strncpy(dev->dateObserved, dateObserved, sizeof(dev->dateObserved) - 1);
        dev->dateObserved[sizeof(dev->dateObserved) - 1] = '\0';
        dev->last_seen = time(NULL);

        if (qos == 1)
        {
            dev->has_seq = 1;
            dev->last_seq = seq;
            // Send ACK for successful receipt and processing
            send_ack(sockfd, &client_addr, len, id, seq);
        }

        // Print received reading (Req 2d)
        printf("Received from %s:%d -> id=%s temp=%.2f hum=%.2f qos=%d seq=%ld\n",
               client_ip_str, ntohs(client_addr.sin_port),
               id, temp, hum, qos, seq);

        // --- ALERTING: Range Validation ---
        if (temp < TEMP_MIN || temp > TEMP_MAX)
        {
            snprintf(log_message, sizeof(log_message), "Temperature %.2f outside of range [%.1f,%.1f]", temp, TEMP_MIN, TEMP_MAX);
            log_alert_dual(id, "TEMPERATURE_OUT_OF_RANGE", log_message);
        }
        if (hum < HUM_MIN || hum > HUM_MAX)
        {
            snprintf(log_message, sizeof(log_message), "Humidity %.2f outside of range [%.1f,%.1f]", hum, HUM_MIN, HUM_MAX);
            log_alert_dual(id, "HUMIDITY_OUT_OF_RANGE", log_message);
        }

        // --- ALERTING: Differential Calculation (Req 2e) ---
        for (int i = 0; i < device_count; ++i)
        {
            device_t *other = &devices[i];
            if (strcmp(other->id, dev->id) == 0)
                continue; // Skip comparing device to itself

            // Calculate absolute difference using fabs() from <math.h>
            double temp_diff = fabs(dev->temperature - other->temperature);
            double hum_diff = fabs(dev->humidity - other->humidity);

            // Check if either differential exceeds its threshold
            if (temp_diff >= TEMP_DIFF_THRESHOLD || hum_diff >= HUM_DIFF_THRESHOLD)
            {
                snprintf(log_message, sizeof(log_message), "Compared with %s, temperature differs by %+0.2f°C and humidity by %+0.2f%% (thresholds: %+0.2f°C / %+0.2f%%, respectively).",
                         other->id, temp_diff, hum_diff, TEMP_DIFF_THRESHOLD, HUM_DIFF_THRESHOLD);
                log_alert_dual(id, "DIFFERENTIAL_ALERT", log_message); // Log the alert (Req 2f - now satisfied by logging)
            }
        }

        cJSON_Delete(root); // Clean up JSON object
    }

    // Stop MQTT thread
    if (mqtt_thread_created)
    {
        pthread_cancel(mqtt_thread);
        pthread_join(mqtt_thread, NULL);
    }

    if (MQTTClient_isConnected(client))
        MQTTClient_disconnect(client, 1000);
    MQTTClient_destroy(&client);

    // Close log file and UDP socket
    if (alert_log)
        fclose(alert_log);
    close(sockfd);
    return 0;
}
