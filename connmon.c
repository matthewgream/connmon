// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#include <arpa/inet.h>
#include <ctype.h>
#include <ifaddrs.h>
#include <json-c/json.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "include/util_linux.h"

#define MQTT_CONNECT_TIMEOUT 60
#define MQTT_PUBLISH_QOS 0
#define MQTT_PUBLISH_RETAIN false
#include "include/mqtt_linux.h"

#define CONFIG_MAX_ENTRIES 48
#include "include/config_linux.h"

#include "include/http_linux.h"

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#define CONFIG_FILE_DEFAULT "connmon.default"

#define CLOUDFLARE_CHECK_PERIOD_DEFAULT 0
#define UPNP_CHECK_PERIOD_DEFAULT 0
#define CONNECTIVITY_CHECK_PERIOD_DEFAULT 60
#define HEARTBEAT_PERIOD_DEFAULT 60

#define MQTT_SERVER_DEFAULT ""
#define MQTT_CLIENT_DEFAULT "connmon"
#define MQTT_TOPIC_PREFIX_DEFAULT "system/connection"

#define MAX_UPNP_MAPPINGS 10

#define IP_ADDRESS_URL "https://api.ipify.org"

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#define PRINTF_ERROR printf_stderr
#define PRINTF_INFO printf_stdout

void printf_stdout(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stdout, format, args);
    va_end(args);
}

void printf_stderr(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

const struct option config_options[] = {
    { "config", required_argument, 0, 0 },
    { "cloudflare-check-period", required_argument, 0, 0 },
    { "cloudflare-verbose", required_argument, 0, 0 },
    { "cloudflare-dns-name", required_argument, 0, 0 },
    { "cloudflare-zone-id", required_argument, 0, 0 },
    { "cloudflare-token", required_argument, 0, 0 },
    { "upnp-check-period", required_argument, 0, 0 },
    { "upnp-verbose", required_argument, 0, 0 },
    { "upnp-service", required_argument, 0, 0 },
    { "upnp-port-external", required_argument, 0, 0 },
    { "upnp-port-internal", required_argument, 0, 0 },
    { "upnp-protocol", required_argument, 0, 0 },
    { "connectivity-check-period", required_argument, 0, 0 },
    { "connectivity-verbose", required_argument, 0, 0 },
    { "connectivity-url", required_argument, 0, 0 },
    { "heartbeat-period", required_argument, 0, 0 },
    { "heartbeat-verbose", required_argument, 0, 0 },
    { "mqtt-server", required_argument, 0, 0 },
    { "mqtt-client", required_argument, 0, 0 },
    { "mqtt-topic-prefix", required_argument, 0, 0 },
    { 0, 0, 0, 0 },
};

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

typedef struct {
    const char *dns_name;
    const char *zone_id;
    const char *token;
    bool enabled;
    bool verbose;
} cloudflare_config_t;

typedef struct {
    const char *service;
    int port_external;
    int port_internal;
    const char *protocol;
} upnp_mapping_t;

typedef struct {
    upnp_mapping_t mappings[MAX_UPNP_MAPPINGS];
    int count;
    bool enabled;
    bool verbose;
} upnp_config_t;

typedef struct {
    const char *url;
    bool enabled;
    bool verbose;
} connectivity_config_t;

typedef struct {
    bool enabled;
    bool verbose;
} heartbeat_config_t;

typedef struct {
    int cloudflare_check_period;
    int upnp_check_period;
    int connectivity_check_period;
    int heartbeat_period;
} timing_config_t;

cloudflare_config_t cloudflare_config;
upnp_config_t upnp_config;
connectivity_config_t connectivity_config;
heartbeat_config_t heartbeat_config;
timing_config_t timing_config;
MqttConfig mqtt_config;
const char *mqtt_topic_prefix;
bool mqtt_enabled;
char hostname[256];

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static bool get_public_ip(char *string, size_t length) { return http_get(IP_ADDRESS_URL, string, length); }

static bool get_local_ip(char *string, size_t length) {
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == -1)
        return false;
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL)
            continue;
        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
            char ip[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip)) != NULL && (strncmp(ip, "192.", 4) == 0 || strncmp(ip, "10.", 3) == 0)) {
                freeifaddrs(ifaddr);
                strncpy(string, ip, length);
                return true;
            }
        }
    }
    freeifaddrs(ifaddr);
    return false;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static char *cloudflare_api_request(const char *endpoint, const char *method, const char *data) {
    char url[512];
    snprintf(url, sizeof(url), "https://api.cloudflare.com/client/v4/zones/%s%s", cloudflare_config.zone_id, endpoint);
    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", cloudflare_config.token);
    const char *headers[] = { auth_header, "Content-Type: application/json" };
    return http_request(url, method, headers, sizeof(headers) / sizeof(const char *), data);
}

static bool cloudflare_update(char *status_message, size_t status_size) {

    if (!cloudflare_config.enabled) {
        snprintf(status_message, status_size, "update disabled");
        return true;
    }

    char *zone_response = cloudflare_api_request("", "GET", NULL);
    if (!zone_response) {
        snprintf(status_message, status_size, "failed to fetch zone info");
        return false;
    }
    json_object *zone_json = json_tokener_parse(zone_response);
    if (!zone_json) {
        snprintf(status_message, status_size, "failed to parse zone response");
        free(zone_response);
        return false;
    }

    json_object *zone_result_obj, *zone_name_obj;
    const char *zone_name = (json_object_object_get_ex(zone_json, "result", &zone_result_obj) && json_object_object_get_ex(zone_result_obj, "name", &zone_name_obj))
                                ? json_object_get_string(zone_name_obj)
                                : NULL;
    if (!zone_name) {
        snprintf(status_message, status_size, "could not get zone name");
        json_object_put(zone_json);
        free(zone_response);
        return false;
    }

    char full_dns_name[256];
    snprintf(full_dns_name, sizeof(full_dns_name), "%s.%s", cloudflare_config.dns_name, zone_name);

    json_object_put(zone_json);
    free(zone_response);

    char *records_response = cloudflare_api_request("/dns_records", "GET", NULL);
    if (!records_response) {
        snprintf(status_message, status_size, "failed to fetch DNS records");
        return false;
    }
    json_object *records_json = json_tokener_parse(records_response);
    if (!records_json) {
        snprintf(status_message, status_size, "failed to parse DNS records");
        free(records_response);
        return false;
    }

    const char *record_id = NULL, *record_ip = NULL;
    json_object *records_result;
    if (json_object_object_get_ex(records_json, "result", &records_result)) {
        for (size_t i = 0; i < json_object_array_length(records_result); i++) {
            json_object *record = json_object_array_get_idx(records_result, i);
            json_object *name_obj;
            if (json_object_object_get_ex(record, "name", &name_obj)) {
                const char *name = json_object_get_string(name_obj);
                if (strcmp(name, full_dns_name) == 0) {
                    json_object *id_obj, *content_obj;
                    if (json_object_object_get_ex(record, "id", &id_obj))
                        record_id = json_object_get_string(id_obj);
                    if (json_object_object_get_ex(record, "content", &content_obj))
                        record_ip = json_object_get_string(content_obj);
                    break;
                }
            }
        }
    }
    if (!record_id) {
        snprintf(status_message, status_size, "no matching DNS record found for %s", full_dns_name);
        json_object_put(records_json);
        free(records_response);
        return false;
    }

    char public_ip[64];
    if (!get_public_ip(public_ip, sizeof(public_ip))) {
        snprintf(status_message, status_size, "failed to get current IP address");
        json_object_put(records_json);
        free(records_response);
        return false;
    }

    if (strcmp(public_ip, record_ip) == 0) {
        snprintf(status_message, status_size, "IP valid at %s", public_ip);
        json_object_put(records_json);
        free(records_response);
        return true;
    }

    json_object *update_json = json_object_new_object();
    json_object_object_add(update_json, "name", json_object_new_string(cloudflare_config.dns_name));
    json_object_object_add(update_json, "type", json_object_new_string("A"));
    json_object_object_add(update_json, "content", json_object_new_string(public_ip));
    json_object_object_add(update_json, "proxied", json_object_new_boolean(false));
    json_object_object_add(update_json, "ttl", json_object_new_int(600));

    char endpoint[256];
    snprintf(endpoint, sizeof(endpoint), "/dns_records/%s", record_id);

    bool success            = false;
    const char *update_data = json_object_to_json_string(update_json);
    char *update_response   = cloudflare_api_request(endpoint, "PUT", update_data);
    json_object_put(update_json);
    json_object_put(records_json);
    free(records_response);
    if (update_response) {
        json_object *update_result = json_tokener_parse(update_response);
        if (update_result) {
            json_object *success_obj;
            if (json_object_object_get_ex(update_result, "success", &success_obj))
                success = json_object_get_boolean(success_obj);
            json_object_put(update_result);
        }
        free(update_response);
    }
    snprintf(status_message, status_size, "IP update %s from %s to %s", success ? "succeeded" : "failed", record_ip, public_ip);

    return success;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>

static bool upnp_update_single(const upnp_mapping_t *mapping, struct UPNPUrls *urls, struct IGDdatas *data, const char *local_ip, const char *externalIPAddress,
                               char *status_message, size_t status_size) {

    char intClient[64] = "", intPort[16] = "", desc[256] = "", enabled[16] = "", duration[16] = "", extPort[16];
    snprintf(extPort, sizeof(extPort), "%d", mapping->port_external);

    char protocol_upper[16];
    strcpy(protocol_upper, mapping->protocol);
    for (char *p = protocol_upper; *p; p++)
        *p = (char)toupper(*p);

    int result           = UPNP_GetSpecificPortMappingEntry(urls->controlURL, data->first.servicetype, extPort, protocol_upper, NULL, intClient, intPort, desc, enabled, duration);
    bool mapping_exists  = (result == UPNPCOMMAND_SUCCESS);
    bool mapping_correct = false;
    if (mapping_exists)
        if (strcmp(intClient, local_ip) == 0 && atoi(intPort) == mapping->port_internal && strstr(desc, mapping->service) != NULL)
            mapping_correct = true;
    if (mapping_correct) {
        snprintf(status_message, status_size, "'%s' exists %s:%d->%s:%d %s", mapping->service, local_ip, mapping->port_internal, externalIPAddress, mapping->port_external,
                 mapping->protocol);
        return true;
    }
    if (mapping_exists)
        if ((result = UPNP_DeletePortMapping(urls->controlURL, data->first.servicetype, extPort, protocol_upper, NULL)) != UPNPCOMMAND_SUCCESS)
            printf("warning: failed to remove old UPnP mapping: %s\n", strupnperror(result));

    char intPortStr[16];
    snprintf(intPortStr, sizeof(intPortStr), "%d", mapping->port_internal);

    result = UPNP_AddPortMapping(urls->controlURL, data->first.servicetype, extPort, intPortStr, local_ip, mapping->service, mapping->protocol, NULL, "0");
    if (result != UPNPCOMMAND_SUCCESS)
        result = UPNP_AddPortMapping(urls->controlURL, data->first.servicetype, extPort, intPortStr, local_ip, mapping->service, protocol_upper, NULL, "0");

    bool success = (result == UPNPCOMMAND_SUCCESS);
    snprintf(status_message, status_size, "'%s' creation %s %s:%d->%s:%d %s%s%s", mapping->service, success ? "succeeded" : "failed", local_ip, mapping->port_internal,
             externalIPAddress, mapping->port_external, mapping->protocol, success ? "" : ": ", success ? "" : strupnperror(result));

    return success;
}

static bool upnp_update(char *status_message, size_t status_size) {

    if (!upnp_config.enabled) {
        snprintf(status_message, status_size, "mapping disabled");
        return true;
    }

    if (upnp_config.count == 0) {
        snprintf(status_message, status_size, "no mappings configured");
        return true;
    }

    char local_ip[64];
    if (!get_local_ip(local_ip, sizeof(local_ip))) {
        snprintf(status_message, status_size, "failed to get local IP address");
        return false;
    }

    struct UPNPUrls urls;
    struct IGDdatas data;
    char lanaddr[64]        = "";
    int error               = 0;
    struct UPNPDev *devlist = upnpDiscover(2000, NULL, NULL, 0, 0, 2, &error);
    if (!devlist) {
        snprintf(status_message, status_size, "no UPnP devices found");
        return false;
    }
#if MINIUPNPC_API_VERSION < 18
    int result = UPNP_GetValidIGD(devlist, &urls, &data, lanaddr, sizeof(lanaddr));
#else
    int result = UPNP_GetValidIGD(devlist, &urls, &data, lanaddr, sizeof(lanaddr), NULL, 0);
#endif
    if (result != 1 && result != 2) {
        snprintf(status_message, status_size, "no valid UPnP IGD found");
        freeUPNPDevlist(devlist);
        return false;
    }

    char externalIPAddress[64] = "";
    UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, externalIPAddress);

    int success_count        = 0;
    char overall_status[512] = "";
    int overall_offset       = 0;

    for (int i = 0; i < upnp_config.count; i++) {
        char individual_status[256] = "";
        if (upnp_update_single(&upnp_config.mappings[i], &urls, &data, local_ip, externalIPAddress, individual_status, sizeof(individual_status)))
            success_count++;
        overall_offset += snprintf(&overall_status[overall_offset], sizeof(overall_status) - (size_t)overall_offset, "[%d]: %s, ", i + 1, individual_status);
    }
    if (overall_offset > 1)
        overall_status[overall_offset - 2] = '\0';

    FreeUPNPUrls(&urls);
    freeUPNPDevlist(devlist);

    snprintf(status_message, status_size, "%d/%d succeeded - %s", success_count, upnp_config.count, overall_status);

    return (success_count == upnp_config.count);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static bool check_connectivity(char *status_message, size_t status_size) {
    char details[256];
    bool success = http_head(connectivity_config.url, details, sizeof(details));
    snprintf(status_message, status_size, "connection to '%s' %s (%s)", connectivity_config.url, success ? "succeeded" : "failed", details);
    return success;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static bool do_heartbeat(char *status_message, size_t status_size) {
    static unsigned long counter = 0;
    snprintf(status_message, status_size, "daemon active (%lu)", ++counter);
    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

static void publish_status(const char *event_type, bool success, const char *message, bool verbose) {

    if (verbose)
        printf("%s: %s\n", event_type, message);

    if (mqtt_enabled) {

        char topic[256 + 64];
        snprintf(topic, sizeof(topic), "%s/%s", mqtt_topic_prefix, hostname);

        json_object *json = json_object_new_object();
        json_object_object_add(json, "timestamp", json_object_new_int64(time(NULL)));
        json_object_object_add(json, "hostname", json_object_new_string(hostname));
        json_object_object_add(json, "event", json_object_new_string(event_type));
        json_object_object_add(json, "success", json_object_new_boolean(success));
        json_object_object_add(json, "message", json_object_new_string(message));

        if (strcmp(event_type, "heartbeat") == 0) {
            json_object_object_add(json, "cloudflare_enabled", json_object_new_boolean(cloudflare_config.enabled));
            json_object_object_add(json, "upnp_enabled", json_object_new_boolean(upnp_config.enabled));
            json_object_object_add(json, "upnp_mappings", json_object_new_int(upnp_config.count));
        }

        const char *json_str = json_object_to_json_string(json);
        mqtt_send(topic, json_str, (int)strlen(json_str));

        json_object_put(json);
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

volatile bool running = true;

void signal_handler(int sig __attribute__((unused))) {
    if (running)
        running = false;
}

void process_loop(void) {

    time_t last_cloudflare = 0, last_upnp = 0, last_connectivity = 0, last_heartbeat = 0;

    char status_message[512];

    while (running) {

        if (running && cloudflare_config.enabled && intervalable(timing_config.cloudflare_check_period, &last_cloudflare, true))
            publish_status("cloudflare", cloudflare_update(status_message, sizeof(status_message)), status_message, cloudflare_config.verbose);

        if (running && upnp_config.enabled && intervalable(timing_config.upnp_check_period, &last_upnp, true))
            publish_status("upnp", upnp_update(status_message, sizeof(status_message)), status_message, upnp_config.verbose);

        if (running && connectivity_config.enabled && intervalable(timing_config.connectivity_check_period, &last_connectivity, true))
            publish_status("connectivity", check_connectivity(status_message, sizeof(status_message)), status_message, connectivity_config.verbose);

        if (running && intervalable(timing_config.heartbeat_period, &last_heartbeat, false))
            publish_status("heartbeat", do_heartbeat(status_message, sizeof(status_message)), status_message, heartbeat_config.verbose);

        sleep(1);
    }
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

bool config_load_upnp_mappings(void) {

    upnp_config.count = 0;

    const char *service  = config_get_string("upnp-service", "");
    int port_external    = config_get_integer("upnp-port-external", 0);
    int port_internal    = config_get_integer("upnp-port-internal", 0);
    const char *protocol = config_get_string("upnp-protocol", "tcp");
    if (strlen(service) > 0 && port_external > 0 && port_internal > 0) {
        upnp_config.mappings[0].service       = service;
        upnp_config.mappings[0].port_external = port_external;
        upnp_config.mappings[0].port_internal = port_internal;
        upnp_config.mappings[0].protocol      = protocol;
        upnp_config.count                     = 1;
    }
    for (int i = 1; i <= MAX_UPNP_MAPPINGS; i++) {
        char key[64];
        snprintf(key, sizeof(key), "upnp-service[%d]", i);
        const char *idx_service = config_get_string(key, "");
        snprintf(key, sizeof(key), "upnp-port-external[%d]", i);
        int idx_port_external = config_get_integer(key, 0);
        snprintf(key, sizeof(key), "upnp-port-internal[%d]", i);
        int idx_port_internal = config_get_integer(key, 0);
        snprintf(key, sizeof(key), "upnp-protocol[%d]", i);
        const char *idx_protocol = config_get_string(key, "tcp");
        if (strlen(idx_service) > 0 && idx_port_external > 0 && idx_port_internal > 0) {
            if (upnp_config.count < MAX_UPNP_MAPPINGS) {
                upnp_config.mappings[upnp_config.count].service       = idx_service;
                upnp_config.mappings[upnp_config.count].port_external = idx_port_external;
                upnp_config.mappings[upnp_config.count].port_internal = idx_port_internal;
                upnp_config.mappings[upnp_config.count].protocol      = idx_protocol;
                upnp_config.count++;
            }
        } else if (strlen(idx_service) > 0 || idx_port_external > 0 || idx_port_internal > 0) {
            break;
        }
    }
    return upnp_config.count > 0;
}

bool config_load_settings(const int argc, const char *argv[]) {

    if (!config_load(CONFIG_FILE_DEFAULT, argc, argv, config_options))
        return false;

    timing_config.cloudflare_check_period   = config_get_integer("cloudflare-check-period", CLOUDFLARE_CHECK_PERIOD_DEFAULT);
    timing_config.upnp_check_period         = config_get_integer("upnp-check-period", UPNP_CHECK_PERIOD_DEFAULT);
    timing_config.connectivity_check_period = config_get_integer("connectivity-check-period", CONNECTIVITY_CHECK_PERIOD_DEFAULT);
    timing_config.heartbeat_period          = config_get_integer("heartbeat-period", HEARTBEAT_PERIOD_DEFAULT);

    cloudflare_config.dns_name = config_get_string("cloudflare-dns-name", "");
    cloudflare_config.zone_id  = config_get_string("cloudflare-zone-id", "");
    cloudflare_config.token    = config_get_string("cloudflare-token", "");
    cloudflare_config.enabled =
        timing_config.cloudflare_check_period > 0 && (strlen(cloudflare_config.dns_name) > 0 && strlen(cloudflare_config.zone_id) > 0 && strlen(cloudflare_config.token) > 0);
    cloudflare_config.verbose = config_get_bool("cloudflare-verbose", true);
    if (cloudflare_config.enabled)
        printf("cloudflare: '%s' (%d, %s)\n", cloudflare_config.dns_name, timing_config.cloudflare_check_period, cloudflare_config.verbose ? "verbose" : "quiet");

    config_load_upnp_mappings();
    upnp_config.enabled = timing_config.upnp_check_period > 0 && (upnp_config.count > 0);
    upnp_config.verbose = config_get_bool("upnp-verbose", true);
    if (upnp_config.enabled)
        for (int i = 0; i < upnp_config.count; i++)
            printf("upnp: [%d] '%s' %d->%d %s (%d, %s)\n", i + 1, upnp_config.mappings[i].service, upnp_config.mappings[i].port_internal, upnp_config.mappings[i].port_external,
                   upnp_config.mappings[i].protocol, timing_config.upnp_check_period, upnp_config.verbose ? "verbose" : "quiet");

    connectivity_config.url     = config_get_string("connectivity-url", "");
    connectivity_config.enabled = timing_config.connectivity_check_period > 0 && (strlen(connectivity_config.url) > 0);
    connectivity_config.verbose = config_get_bool("connectivity-verbose", true);
    if (connectivity_config.enabled)
        printf("connectivity: '%s' (%d, %s)\n", connectivity_config.url, timing_config.connectivity_check_period, connectivity_config.verbose ? "verbose" : "quiet");

    heartbeat_config.enabled = (timing_config.heartbeat_period > 0);
    heartbeat_config.verbose = config_get_bool("heartbeat-verbose", false);
    if (heartbeat_config.enabled)
        printf("heartbeat: (%d, %s)\n", timing_config.heartbeat_period, heartbeat_config.verbose ? "verbose" : "quiet");

    mqtt_config.server = config_get_string("mqtt-server", MQTT_SERVER_DEFAULT);
    mqtt_config.client = config_get_string("mqtt-client", MQTT_CLIENT_DEFAULT);
    mqtt_config.debug  = false;
    mqtt_topic_prefix  = config_get_string("mqtt-topic-prefix", MQTT_TOPIC_PREFIX_DEFAULT);
    mqtt_enabled       = mqtt_config.server != NULL && (strlen(mqtt_config.server) > 0);

    if (gethostname(hostname, sizeof(hostname)) != 0)
        strcpy(hostname, "unknown");

    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

int main(int argc, const char **argv) {

    setbuf(stdout, NULL);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (!config_load_settings(argc, argv)) {
        fprintf(stderr, "failed to load configuration\n");
        return EXIT_FAILURE;
    }
    if (!cloudflare_config.enabled && !upnp_config.enabled) {
        fprintf(stderr, "both Cloudflare and UPnP are disabled, nothing to do\n");
        return EXIT_FAILURE;
    }

    if (mqtt_enabled && !mqtt_begin(&mqtt_config)) {
        fprintf(stderr, "failed to initialize MQTT\n");
        return EXIT_FAILURE;
    }

    http_begin();
    publish_status("startup", true, "daemon started", true);
    process_loop();
    publish_status("shutdown", true, "daemon stopped", true);
    http_end();

    if (mqtt_enabled)
        mqtt_end();

    return EXIT_SUCCESS;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
