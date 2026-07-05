
// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

#include <curl/curl.h>

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

typedef struct {
    char *data;
    size_t size;
    size_t length;
} curl_buffer_t;

static size_t curl_write_callback_static(void *data, size_t size, size_t nmemb, void *userp) {
    curl_buffer_t *buffer = (curl_buffer_t *)userp;
    size_t length = size * nmemb;
    if (length > ((buffer->size - 1) - buffer->length))
        length = ((buffer->size - 1) - buffer->length);
    memcpy(&(buffer->data[buffer->length]), data, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return length;
}

static size_t curl_write_callback_dynamic(void *data, size_t size, size_t nmemb, void *userp) {
    curl_buffer_t *buffer = (curl_buffer_t *)userp;
    size_t length = size * nmemb;
    char *ptr = realloc(buffer->data, buffer->size + length + 1);
    if (!ptr)
        return 0;
    buffer->size += length + 1;
    buffer->data = ptr;
    memcpy(&(buffer->data[buffer->length]), data, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return length;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

bool http_get(const char *url, char *response, const size_t response_size) {

    CURL *curl = curl_easy_init();
    if (!curl)
        return false;

    curl_buffer_t buffer = { .data = response, .size = response_size, .length = 0 };

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_callback_static);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&buffer);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);

    curl_easy_cleanup(curl);

    return (res == CURLE_OK);
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

char *http_request(const char *url, const char *method, const char **headers, size_t nheaders, const char *data) {

    CURL *curl = curl_easy_init();
    if (!curl)
        return NULL;

    struct curl_slist *curl_headers = NULL;
    for (size_t i = 0; i < nheaders; i++)
        curl_headers = curl_slist_append(curl_headers, headers[i]);

    curl_buffer_t response = { .data = malloc(1), .size = 0, .length = 0 };

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, curl_headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_callback_dynamic);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&response);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);

    if (data)
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);

    CURLcode res = curl_easy_perform(curl);

    if (curl_headers != NULL)
        curl_slist_free_all(curl_headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        free(response.data);
        return NULL;
    }

    return response.data;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

bool http_head(const char *url, char *status, size_t status_size) {

    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(status, status_size, "failed to initialize");
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);

    long response_code = 0;
    curl_off_t total_time = 0;
    long header_size = 0;

    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME_T, &total_time);
        curl_easy_getinfo(curl, CURLINFO_HEADER_SIZE, &header_size);
    }

    curl_easy_cleanup(curl);

    bool success = (res == CURLE_OK && response_code > 0);
    if (res != CURLE_OK)
        snprintf(status, status_size, "%s", curl_easy_strerror(res));
    else if (response_code == 0)
        snprintf(status, status_size, "no response");
    else
        snprintf(status, status_size, "HTTP %ld, %.3fms, %ld bytes", response_code, (double)total_time / 1000.0, header_size);

    return success;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------

bool http_begin(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    return true;
}

bool http_end(void) {
    curl_global_cleanup();
    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------
