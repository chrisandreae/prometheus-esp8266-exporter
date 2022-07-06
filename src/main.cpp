#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <Adafruit_INA219.h>

#include "config.h"
#include "version.h"

Adafruit_INA219 ina219;

#define EXPLODE4(arr) (arr[0], arr[1], arr[2], arr[3])

enum LogLevel {
    DEBUG,
    INFO,
    ERROR,
};

void setup_sensor();
void setup_wifi();
void setup_http_server();
void handle_http_root();
void handle_http_not_found();
void handle_http_home_client();
void handle_http_metrics();
void read_sensors(boolean force=false);
bool read_sensor(float (*function)(), float *value);
void log_request();
void log(char const *message, LogLevel level=LogLevel::INFO);

ESP8266WebServer http_server(HTTP_SERVER_PORT);

float busvoltage = 0;
float current_mA = 0;

uint32_t previous_read_time = 0;

void setup(void) {
    char message[128];
    Serial.begin(76800);
    setup_sensor();
    setup_wifi();
    setup_http_server();
    snprintf(message, 128, "Prometheus namespace: %s", PROM_NAMESPACE);
    log(message);
    log("Setup done");
}

void setup_sensor() {
    log("Setting up sensor");

    if (! ina219.begin()) {
        Serial.println("Failed to find INA219 chip");
        while (1) { delay(10); }
    }

    // Test read
    read_sensors(true);
    log("sensor ready", LogLevel::DEBUG);
}

void setup_wifi() {
    char message[128];
    log("Setting up Wi-Fi");
    snprintf(message, 128, "Wi-Fi SSID: %s", WIFI_SSID);
    log(message, LogLevel::DEBUG);
    snprintf(message, 128, "MAC address: %s", WiFi.macAddress().c_str());
    log(message, LogLevel::DEBUG);
    snprintf(message, 128, "Initial hostname: %s", WiFi.hostname().c_str());
    log(message, LogLevel::DEBUG);

    WiFi.mode(WIFI_STA);

    #if WIFI_IPV4_STATIC == true
        log("Using static IPv4 adressing");
        IPAddress static_address(WIFI_IPV4_ADDRESS);
        IPAddress static_subnet(WIFI_IPV4_SUBNET_MASK);
        IPAddress static_gateway(WIFI_IPV4_GATEWAY);
        IPAddress static_dns1(WIFI_IPV4_DNS_1);
        IPAddress static_dns2(WIFI_IPV4_DNS_2);
        if (!WiFi.config(static_address, static_gateway, static_subnet, static_dns1, static_dns2)) {
            log("Failed to configure static addressing", LogLevel::ERROR);
        }
    #endif

    #ifdef WIFI_HOSTNAME
        log("Requesting hostname: " WIFI_HOSTNAME);
        if (WiFi.hostname(WIFI_HOSTNAME)) {
            log("Hostname changed");
        } else {
            log("Failed to change hostname (too long?)", LogLevel::ERROR);
        }
    #endif

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        log("Wi-Fi connection not ready, waiting", LogLevel::DEBUG);
        delay(500);
    }

    log("Wi-Fi connected.");
    snprintf(message, 128, "SSID: %s", WiFi.SSID().c_str());
    log(message);
    snprintf(message, 128, "BSSID: %s", WiFi.BSSIDstr().c_str());
    log(message);
    snprintf(message, 128, "Hostname: %s", WiFi.hostname().c_str());
    log(message);
    snprintf(message, 128, "MAC address: %s", WiFi.macAddress().c_str());
    log(message);
    snprintf(message, 128, "IPv4 address: %s", WiFi.localIP().toString().c_str());
    log(message);
    snprintf(message, 128, "IPv4 subnet mask: %s", WiFi.subnetMask().toString().c_str());
    log(message);
    snprintf(message, 128, "IPv4 gateway: %s", WiFi.gatewayIP().toString().c_str());
    log(message);
    snprintf(message, 128, "Primary DNS server: %s", WiFi.dnsIP(0).toString().c_str());
    log(message);
    snprintf(message, 128, "Secondary DNS server: %s", WiFi.dnsIP(1).toString().c_str());
    log(message);
}
void setup_http_server() {
    char message[128];
    log("Setting up HTTP server");
    http_server.on("/", HTTPMethod::HTTP_GET, handle_http_root);
    http_server.on(HTTP_METRICS_ENDPOINT, HTTPMethod::HTTP_GET, handle_http_metrics);
    http_server.onNotFound(handle_http_not_found);
    http_server.begin();
    log("HTTP server started", LogLevel::DEBUG);
    snprintf(message, 128, "Metrics endpoint: %s", HTTP_METRICS_ENDPOINT);
    log(message);
}

void loop(void) {
    http_server.handleClient();
}

void handle_http_root() {
    log_request();
    static size_t const BUFSIZE = 256;
    static char const *response_template =
        "Prometheus ESP8266 DHT Exporter by HON95.\n"
        "\n"
        "Project: https://github.com/HON95/prometheus-esp8266-dht-exporter\n"
        "\n"
        "Usage: %s\n";
    char response[BUFSIZE];
    snprintf(response, BUFSIZE, response_template, HTTP_METRICS_ENDPOINT);
    http_server.send(200, "text/plain; charset=utf-8", response);
}

void handle_http_metrics() {
    log_request();
    static size_t const BUFSIZE = 1024;
    static char const *response_template =
        "# HELP " PROM_NAMESPACE "_info Metadata about the device.\n"
        "# TYPE " PROM_NAMESPACE "_info gauge\n"
        "# UNIT " PROM_NAMESPACE "_info \n"
        PROM_NAMESPACE "_info{version=\"%s\",board=\"%s\",sensor=\"%s\"} 1\n"
        "# HELP " PROM_NAMESPACE "_battery_voltage Battery Voltage.\n"
        "# TYPE " PROM_NAMESPACE "_battery_voltage gauge\n"
        "# UNIT " PROM_NAMESPACE "_battery_voltage V\n"
        PROM_NAMESPACE "_battery_voltage %f\n"
        "# HELP " PROM_NAMESPACE "_battery_current Battery Current.\n"
        "# TYPE " PROM_NAMESPACE "_battery_current gauge\n"
        "# UNIT " PROM_NAMESPACE "_battery_current mA\n"
        PROM_NAMESPACE "_battery_current %f\n";

    read_sensors();
    if (isnan(busvoltage) || isnan(current_mA)) {
        http_server.send(500, "text/plain; charset=utf-8", "Sensor error.");
        return;
    }

    char response[BUFSIZE];
    snprintf(response, BUFSIZE, response_template, VERSION, BOARD_NAME, SENSOR_NAME, busvoltage, current_mA);
    http_server.send(200, "text/plain; charset=utf-8", response);
}

void handle_http_not_found() {
    log_request();
    http_server.send(404, "text/plain; charset=utf-8", "Not found.");
}


void read_voltage_sensor() {
    log("Reading voltage sensor ...", LogLevel::DEBUG);
    bool result = read_sensor([] {
          return ina219.getBusVoltage_V();
      }, &busvoltage);
    if (!result) {
        log("Failed to read voltage sensor.", LogLevel::ERROR);
    }
}

void read_current_sensor() {
    log("Reading current sensor ...", LogLevel::DEBUG);
    bool result = read_sensor([] {
        return ina219.getCurrent_mA();
    }, &current_mA);
    if (!result) {
        log("Failed to read current sensor.", LogLevel::ERROR);
    }
}

void read_sensors(boolean force) {
    uint32_t current_time = millis();
    previous_read_time = current_time;

    read_voltage_sensor();
    read_current_sensor();
}

bool read_sensor(float (*function)(), float *value) {
    bool success = false;
    for (int i = 0; i < READ_TRY_COUNT; i++) {
        *value = function();
        if (!isnan(*value)) {
            success = true;
            break;
        }
        log("Failed to read sensor.", LogLevel::DEBUG);
    }
    return success;
}


void get_http_method_name(char *name, size_t name_length, HTTPMethod method) {
    switch (method) {
    case HTTP_GET:
        snprintf(name, name_length, "GET");
        break;
    case HTTP_HEAD:
        snprintf(name, name_length, "HEAD");
        break;
    case HTTP_POST:
        snprintf(name, name_length, "POST");
        break;
    case HTTP_PUT:
        snprintf(name, name_length, "PUT");
        break;
    case HTTP_PATCH:
        snprintf(name, name_length, "PATCH");
        break;
    case HTTP_DELETE:
        snprintf(name, name_length, "DELETE");
        break;
    case HTTP_OPTIONS:
        snprintf(name, name_length, "OPTIONS");
        break;
    default:
        snprintf(name, name_length, "UNKNOWN");
        break;
    }
}

void log_request() {
    char message[128];
    char method_name[16];
    get_http_method_name(method_name, 16, http_server.method());
    snprintf(message, 128, "Request: client=%s:%u method=%s path=%s",
            http_server.client().remoteIP().toString().c_str(), http_server.client().remotePort(), method_name, http_server.uri().c_str());
    log(message, LogLevel::INFO);
}

void log(char const *message, LogLevel level) {
    if (DEBUG_MODE == 0 && level == LogLevel::DEBUG) {
        return;
    }
    // Will overflow after a while
    float seconds = millis() / 1000.0;
    char str_level[10];
    switch (level) {
        case DEBUG:
            strcpy(str_level, "DEBUG");
            break;
        case INFO:
            strcpy(str_level, "INFO");
            break;
        case ERROR:
            strcpy(str_level, "ERROR");
            break;
        default:
            break;
    }
    char record[150];
    snprintf(record, 150, "[%10.3f] [%-5s] %s", seconds, str_level, message);
    Serial.println(record);
}
