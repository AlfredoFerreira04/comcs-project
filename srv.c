// alert_udp_server.c
// Compile: gcc alert_udp_server.c -o alert_udp_server -lpaho-mqtt3c -lcjson -lm
// Run: ./alert_udp_server
//
// Requires cJSON and Paho MQTT C client.
// On Debian/Ubuntu:
//   sudo apt install libcjson-dev libpaho-mqtt3c-dev

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
#include <math.h> // For fabs()
#include <signal.h>
#include <cjson/cJSON.h> // cJSON library
#include <MQTTClient.h>

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

// MQTT settings
#define MQTT_ADDRESS "ssl://4979254f05ea480283d67c6f0d9f7525.s1.eu.hivemq.cloud:8883"
#define MQTT_CLIENT_ID "udp_alert_server"
#define MQTT_ALERT_TOPIC "comcs/g04/alerts"

// MQTT credentials
#define MQTT_USERNAME "web_client"
#define MQTT_PASSWORD "Password1"

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

static MQTTClient mqtt_client;
static volatile sig_atomic_t running = 1;

static void handle_signal(int sig)
{
    (void)sig;
    running = 0;
}

// Function to log alerts to stdout, file, and MQTT (Req 2d)
static void log_alert(const char *message)
{
    // Get current timestamp
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char timebuf[64];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm_now);

    // Build full log message that includes timestamp
    char full_msg[1024];
    snprintf(full_msg, sizeof(full_msg), "[%s] %s", timebuf, message);

    // Print to standard output
    printf("%s\n", full_msg);

    // Print to file and ensure it is written immediately
    if (alert_log)
    {
        fprintf(alert_log, "%s\n", full_msg);
        fflush(alert_log);
    }

    // Publish to MQTT (if client is initialized)
    MQTTClient_message pubmsg = MQTTClient_message_initializer;
    MQTTClient_deliveryToken token;

    pubmsg.payload = (void *)full_msg;
    pubmsg.payloadlen = (int)strlen(full_msg);
    pubmsg.qos = 1;
    pubmsg.retained = 0;

    if (&mqtt_client)
    {
        int rc = MQTTClient_publishMessage(mqtt_client, MQTT_ALERT_TOPIC, &pubmsg, &token);
        if (rc != MQTTCLIENT_SUCCESS)
        {
            fprintf(stderr, "MQTT publish failed: %d (%s)\n", rc, MQTTClient_strerror(rc));
        }
        else
        {
            // Wait for completion but do not block too long
            MQTTClient_waitForCompletion(mqtt_client, token, 2000);
        }
    }
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

// Looks up a device based on its network address
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
        d->addr = *addr;
        d->last_seen = time(NULL);
        return d;
    }

    if (device_count >= MAX_DEVICES)
        return NULL;
    d = &devices[device_count++];

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

// Sends an ACK message back to the client for QoS=1 (Req 2b)
static void send_ack(int sockfd, struct sockaddr_in *client_addr, socklen_t addrlen, const char *id, long seq)
{
    if (!id)
        return;

    cJSON *ack = cJSON_CreateObject();
    if (!ack)
    {
        perror("cJSON_CreateObject failed in send_ack");
        return;
    }

    cJSON_AddStringToObject(ack, "type", "ACK");
    cJSON_AddStringToObject(ack, "id", id);
    cJSON_AddNumberToObject(ack, "seq", (double)seq);

    char *out = cJSON_PrintUnformatted(ack);
    if (out)
    {
        ssize_t sent = sendto(sockfd, out, strlen(out), 0, (struct sockaddr *)client_addr, addrlen);
        if (sent < 0)
        {
            perror("sendto (ACK) failed");
        }
        free(out);
    }
    else
    {
        perror("cJSON_PrintUnformatted failed in send_ack");
    }
    cJSON_Delete(ack);
}

int main()
{
    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    char buffer[BUFFER_SIZE];
    char client_ip_str[INET_ADDRSTRLEN];
    char log_message[BUFFER_SIZE + 256];

    // Setup signal handler for graceful shutdown
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // Initialize MQTT client
    MQTTClient client;

    MQTTClient_create(&client, MQTT_URL, MQTT_CLIENTID, MQTTCLIENT_PERSISTENCE_NONE, NULL);

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

    printf("Connected to MQTT broker at %s\n", MQTT_ADDRESS);

    // Open the alert log file for appending
    alert_log = fopen(ALERT_LOGFILE, "a");
    if (!alert_log)
    {
        perror("Failed to open alert log file");
    }

    // Create UDP socket
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
    {
        perror("Failed to create socket");
        MQTTClient_disconnect(mqtt_client, 1000);
        MQTTClient_destroy(&mqtt_client);
        return EXIT_FAILURE;
    }

    // Configure server address structure
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY; // Listen on all interfaces
    server_addr.sin_port = htons(PORT);

    // Bind server socket to the address and port
    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("Bind failed");
        close(sockfd);
        MQTTClient_disconnect(mqtt_client, 1000);
        MQTTClient_destroy(&mqtt_client);
        return EXIT_FAILURE;
    }

    printf("Alert UDP server running on port %d...\n", PORT);

    // Main server loop
    while (running)
    {
        socklen_t len = sizeof(client_addr);
        ssize_t n = recvfrom(sockfd, buffer, BUFFER_SIZE - 1, 0,
                             (struct sockaddr *)&client_addr, &len);

        if (n < 0)
        {
            if (errno == EINTR)
                continue; // interrupted by signal
            perror("Error receiving data");
            continue;
        }

        buffer[n] = '\0'; // Null-terminate the received data

        // Convert client's IP address to a readable string
        if (inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip_str, INET_ADDRSTRLEN) == NULL)
        {
            strcpy(client_ip_str, "UNKNOWN_IP");
        }

        // Parse incoming JSON payload
        cJSON *root = cJSON_Parse(buffer);
        if (!root)
        {
            snprintf(log_message, sizeof(log_message), "Received invalid JSON from %s:%d -> %s",
                     client_ip_str, ntohs(client_addr.sin_port), buffer);
            log_alert(log_message);
            cJSON_Delete(root);
            continue;
        }

        // Extract key fields
        cJSON *jid = cJSON_GetObjectItemCaseSensitive(root, "id");
        cJSON *jtemp = cJSON_GetObjectItemCaseSensitive(root, "temperature");
        cJSON *jhum = cJSON_GetObjectItemCaseSensitive(root, "relativeHumidity");
        cJSON *jdate = cJSON_GetObjectItemCaseSensitive(root, "dateObserved");
        cJSON *jseq = cJSON_GetObjectItemCaseSensitive(root, "seq");
        cJSON *jqos = cJSON_GetObjectItemCaseSensitive(root, "qos");

        // Basic validation
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
                seq = (long)seq_val;
        }
        if (cJSON_IsNumber(jqos))
            qos = jqos->valueint;

        // Add or retrieve device state
        device_t *dev = add_or_get_device(id, &client_addr);
        if (!dev)
        {
            snprintf(log_message, sizeof(log_message), "Device list full, cannot record device %s", id);
            log_alert(log_message);
            cJSON_Delete(root);
            continue;
        }

        // QoS logic
        if (qos == 1)
        {
            if (seq == -1)
            {
                snprintf(log_message, sizeof(log_message), "QoS 1 packet missing 'seq' field from device %s", id);
                log_alert(log_message);
                cJSON_Delete(root);
                continue;
            }
            if (dev->has_seq && seq == dev->last_seq)
            {
                snprintf(log_message, sizeof(log_message), "Duplicate seq %ld from device %s - resending ACK",
                         seq, id);
                log_alert(log_message);
                send_ack(sockfd, &client_addr, len, id, seq);
                cJSON_Delete(root);
                continue;
            }
        }

        // Store reading
        dev->temperature = temp;
        dev->humidity = hum;
        strncpy(dev->dateObserved, dateObserved, sizeof(dev->dateObserved) - 1);
        dev->dateObserved[sizeof(dev->dateObserved) - 1] = '\0';
        dev->last_seen = time(NULL);

        if (qos == 1)
        {
            dev->has_seq = 1;
            dev->last_seq = seq;
            send_ack(sockfd, &client_addr, len, id, seq);
        }

        // Print received reading
        printf("Received from %s:%d -> id=%s temp=%.2f hum=%.2f qos=%d seq=%ld\n",
               client_ip_str, ntohs(client_addr.sin_port),
               id, temp, hum, qos, seq);

        // Range alerts
        if (temp < TEMP_MIN || temp > TEMP_MAX)
        {
            snprintf(log_message, sizeof(log_message), "OUT OF RANGE: device=%s temperature %.2f outside [%.1f,%.1f]",
                     id, temp, TEMP_MIN, TEMP_MAX);
            log_alert(log_message);
        }
        if (hum < HUM_MIN || hum > HUM_MAX)
        {
            snprintf(log_message, sizeof(log_message), "OUT OF RANGE: device=%s humidity %.2f outside [%.1f,%.1f]",
                     id, hum, HUM_MIN, HUM_MAX);
            log_alert(log_message);
        }

        // Differential alerts
        for (int i = 0; i < device_count; ++i)
        {
            device_t *other = &devices[i];
            if (strcmp(other->id, dev->id) == 0)
                continue;

            double temp_diff = fabs(dev->temperature - other->temperature);
            double hum_diff = fabs(dev->humidity - other->humidity);

            if (temp_diff >= TEMP_DIFF_THRESHOLD || hum_diff >= HUM_DIFF_THRESHOLD)
            {
                snprintf(log_message, sizeof(log_message), "DIFFERENTIAL ALERT: %s vs %s -> temp_diff=%.2f (th=%.2f) hum_diff=%.2f (th=%.2f)",
                         dev->id, other->id, temp_diff, TEMP_DIFF_THRESHOLD, hum_diff, HUM_DIFF_THRESHOLD);
                log_alert(log_message);
            }
        }

        cJSON_Delete(root);
    } // end while running

    // Cleanup
    printf("Shutting down...\n");
    if (alert_log)
        fclose(alert_log);
    close(sockfd);

    MQTTClient_disconnect(mqtt_client, 1000);
    MQTTClient_destroy(&mqtt_client);

    return 0;
}
