//
// Sources:
// - https://github.com/bablokb/pico-bme280
// - https://github.com/cniles/picow-iot
//
#include "hardware/spi.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "pico/sync.h"
#include <math.h>
#include <stdio.h>

#include "lwip/altcp_tcp.h"
#include "lwip/altcp_tls.h"
#include "lwip/apps/mqtt.h"
#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#include "lwip/apps/mqtt_priv.h"

#include "bme280.h"
#include "bme280_user.h"

#define ID_LENGTH ((2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES) + 1)

char id[ID_LENGTH];
char sensor_topic[256];

auto_init_mutex(connecting_mutex);
static bool connecting = false;

static float altitude = 65.0f;

typedef struct MQTT_CLIENT_T_ {
  ip_addr_t remote_addr;
  mqtt_client_t *mqtt_client;
  u32_t received;
  u32_t counter;
  u32_t reconnect;
} MQTT_CLIENT_T;

err_t mqtt_test_connect(MQTT_CLIENT_T *state);

bool get_connecting_state() {
  bool connecting_state;

  mutex_enter_blocking(&connecting_mutex);
  connecting_state = connecting;
  mutex_exit(&connecting_mutex);

  return connecting_state;
}

void set_connecting_state(bool value) {
  mutex_enter_blocking(&connecting_mutex);
  connecting = value;
  mutex_exit(&connecting_mutex);
}

// Perform initialisation
static MQTT_CLIENT_T *mqtt_client_init(void) {
  MQTT_CLIENT_T *state = (MQTT_CLIENT_T *)calloc(1, sizeof(MQTT_CLIENT_T));
  if (!state) {
    printf("failed to allocate state\n");
    return NULL;
  }
  state->received = 0;
  return state;
}

void dns_found(const char *name, const ip_addr_t *ipaddr, void *callback_arg) {
  MQTT_CLIENT_T *state = (MQTT_CLIENT_T *)callback_arg;
  printf("DNS query finished with resolved addr of %s.\n",
         ip4addr_ntoa(ipaddr));
  state->remote_addr = *ipaddr;
}

void run_dns_lookup(MQTT_CLIENT_T *state) {
  printf("Running DNS query for %s.\n", MQTT_SERVER_HOST);

  cyw43_arch_lwip_begin();
  err_t err = dns_gethostbyname(MQTT_SERVER_HOST, &(state->remote_addr), dns_found, state);
  cyw43_arch_lwip_end();

  if (err == ERR_ARG) {
    printf("failed to start DNS query\n");
    return;
  }

  if (err == ERR_OK) {
    printf("no lookup needed");
    return;
  }

  while (state->remote_addr.addr == 0) {
    cyw43_arch_poll();
    sleep_ms(1);
  }
}

u32_t data_in = 0;

u8_t buffer[1025];
u8_t data_len = 0;

static void mqtt_pub_start_cb(void *arg, const char *topic, u32_t tot_len) {
  printf("mqtt_pub_start_cb: topic %s\n", topic);

  if (tot_len > 1024) {
    printf("Message length exceeds buffer size, discarding");
  } else {
    data_in = tot_len;
    data_len = 0;
  }
}

static void mqtt_pub_data_cb(void *arg, const u8_t *data, u16_t len, u8_t flags) {
  if (data_in > 0) {
    data_in -= len;
    memcpy(&buffer[data_len], data, len);
    data_len += len;

    if (data_in == 0) {
      buffer[data_len] = 0;
      printf("Message received: %s\n", &buffer);
    }
  }
}

static void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status) {
  if (status != 0) {
    printf("Error during connection: err %d.\n", status);
  } else {
    // Update the connecting state.
    set_connecting_state(false);
    printf("MQTT connected.\n");
  }
}

void mqtt_pub_request_cb(void *arg, err_t err) {
  MQTT_CLIENT_T *state = (MQTT_CLIENT_T *)arg;
  printf("mqtt_pub_request_cb: err %d\n", err);
  state->received++;
}

void mqtt_sub_request_cb(void *arg, err_t err) {
  printf("mqtt_sub_request_cb: err %d\n", err);
}

err_t publish_sensor_data(MQTT_CLIENT_T* state, struct bme280_data *sensor_data) {
  char buffer[256];

  float alt_fac = powf(1.0 - altitude / 44330.0, 5.255);

  float temp = 0.01f * sensor_data->temperature;
  float press = 0.01f * sensor_data->pressure / alt_fac;
  float humidity = 1.0f / 1024.0f * sensor_data->humidity;

  sprintf(buffer, "{\"cycle\":%d,\"temperature\":%0.1f,\"pressure\":%0.0f,\"humidity\":%0.0f}", state->counter, temp, press, humidity);


  cyw43_arch_lwip_begin();
  err_t err = mqtt_publish(state->mqtt_client, sensor_topic, buffer, strlen(buffer), 0, 0, mqtt_pub_request_cb, state);
  cyw43_arch_lwip_end();
  if (err != ERR_OK) {
    printf("Publish err: %d\n", err);
  }

  return err;
}

err_t mqtt_test_connect(MQTT_CLIENT_T *state) {
  struct mqtt_connect_client_info_t ci;
  err_t err;

  memset(&ci, 0, sizeof(ci));

  ci.client_id = id;
  ci.client_user = NULL;
  ci.client_pass = NULL;
  ci.keep_alive = 60;
  ci.will_topic = NULL;
  ci.will_msg = NULL;
  ci.will_retain = 0;
  ci.will_qos = 0;

  set_connecting_state(true);

  const struct mqtt_connect_client_info_t *client_info = &ci;
  err = mqtt_client_connect(state->mqtt_client, &(state->remote_addr),
                            MQTT_SERVER_PORT, mqtt_connection_cb, state,
                            client_info);

  if (err != ERR_OK) {
    printf("mqtt_connect return %d\n", err);
  }

  return err;
}

// ---------------------------------------------------------------------------
// hardware-specific intialization
// SPI_* constants from CMakeLists.txt or user.h

void init_hw() {
  stdio_init_all();

  spi_init(SPI_PORT, 1000000); // SPI with 1Mhz
  gpio_set_function(SPI_RX, GPIO_FUNC_SPI);
  gpio_set_function(SPI_SCK, GPIO_FUNC_SPI);
  gpio_set_function(SPI_TX, GPIO_FUNC_SPI);

  gpio_init(SPI_CS);
  gpio_set_dir(SPI_CS, GPIO_OUT);
  gpio_put(SPI_CS, 1); // Chip select is active-low
}

// ---------------------------------------------------------------------------
// initialize sensor

int8_t init_sensor(struct bme280_dev *dev, uint32_t *delay) {
  int8_t rslt = BME280_OK;
  uint8_t settings;

  // give sensor time to startup
  sleep_ms(5); // datasheet: 2ms

  // basic initialization
  dev->intf_ptr = SPI_PORT; // SPI_PORT is an address
  dev->intf = BME280_SPI_INTF;
  dev->read = user_spi_read;
  dev->write = user_spi_write;
  dev->delay_us = user_delay_us;
  rslt = bme280_init(dev);

#ifdef DEBUG
  printf("[DEBUG] chip-id: 0x%x\n", dev->chip_id);
#endif
  if (rslt != BME280_OK) {
    return rslt;
  }

  // configure for forced-mode, indoor navigation
  dev->settings.osr_h = BME280_OVERSAMPLING_1X;
  dev->settings.osr_p = BME280_OVERSAMPLING_16X;
  dev->settings.osr_t = BME280_OVERSAMPLING_2X;
  dev->settings.filter = BME280_FILTER_COEFF_16;

  settings = BME280_OSR_PRESS_SEL | BME280_OSR_TEMP_SEL | BME280_OSR_HUM_SEL |
             BME280_FILTER_SEL;
  if (rslt != BME280_OK) {
    return rslt;
  }
  rslt = bme280_set_sensor_settings(settings, dev);
  *delay = bme280_cal_meas_delay(&dev->settings);
#ifdef DEBUG
  printf("[DEBUG] delay: %u µs\n", *delay);
#endif

  return rslt;
}

// ---------------------------------------------------------------------------
// read sensor values

int8_t read_sensor(struct bme280_dev *dev, uint32_t *delay, struct bme280_data *data) {
  int8_t rslt;
  rslt = bme280_set_sensor_mode(BME280_FORCED_MODE, dev);
  if (rslt != BME280_OK) {
    return rslt;
  }

  dev->delay_us(*delay, dev->intf_ptr);
  return bme280_get_sensor_data(BME280_ALL, data, dev);
}

// ---------------------------------------------------------------------------
// print sensor data to console

void print_data(struct bme280_data *data) {
  float alt_fac = powf(1.0 - altitude / 44330.0, 5.255);

  float temp = 0.01f * data->temperature;
  float press = 0.01f * data->pressure / alt_fac;
  float hum = 1.0f / 1024.0f * data->humidity;

  printf("%0.1f deg C, %0.0f hPa, %0.0f%%\n", temp, press, hum);
}

// ---------------------------------------------------------------------------
// main loop: read data and print data to console

int main() {
  struct bme280_dev dev;
  struct bme280_data sensor_data;
  int8_t rslt;
  uint32_t delay; // calculated delay between measurements

  if (cyw43_arch_init()) {
    printf("failed to initialise cyw43");
    return -1;
  }

  cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
  cyw43_arch_enable_sta_mode();

  printf("Connecting to WiFi...\n");
  if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000)) {
    printf("failed to  connect.\n");
    return 1;
  } else {
    printf("Connected.\n");
  }

  MQTT_CLIENT_T *state = mqtt_client_init();

  run_dns_lookup(state);


  init_hw();
  rslt = init_sensor(&dev, &delay);

  if (rslt != BME280_OK) {
    printf("could not initialize sensor. RC: %d\n", rslt);
    return -1;
  }

  state->mqtt_client = mqtt_client_new();
  state->counter = 0;
  if (state->mqtt_client == NULL) {
    printf("Failed to create new mqtt client\n");
    return -2;
  }

  // Build the topic str
  char id[ID_LENGTH];
  pico_get_unique_board_id_string(id, ID_LENGTH);
  for (int i = 0; i < ID_LENGTH; i++) {
    if ('A' <= id[i] && id[i] <= 'Z') {
      id[i] = id[i] + 32;
    }
  }
  sprintf(sensor_topic, "fizzle/tele/%s/sensor", id);

  if (mqtt_test_connect(state) == ERR_OK) {
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);

    absolute_time_t timeout = nil_time;
    mqtt_set_inpub_callback(state->mqtt_client, mqtt_pub_start_cb, mqtt_pub_data_cb, 0);

    while (true) {
      cyw43_arch_poll();
      absolute_time_t now = get_absolute_time();

      if (is_nil_time(timeout) || absolute_time_diff_us(now, timeout) <= 0) {
        if (mqtt_client_is_connected(state->mqtt_client)) {

          // sample the sensor
          if (read_sensor(&dev, &delay, &sensor_data) != BME280_OK) {
            printf("error reading sensor");
            continue;
          }

          cyw43_arch_lwip_begin();
          cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);

          if (publish_sensor_data(state, &sensor_data) != ERR_OK) {
            printf("error publishing sensor data");
          }
          
          timeout = make_timeout_time_ms(5000);
          state->counter++;

          cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
          cyw43_arch_lwip_end();
        } else {
          // We are not connected to the MQTT server. Determine if we should try
          // to connect, or are alreadying trying to connect.
          if (get_connecting_state()) {
            printf("already attempting to connect\n");
          } else {
            printf("not connected, reconnecting\n");
            mqtt_test_connect(state);
          }
        }
      }
    }
  }

  cyw43_arch_deinit();
  return ERR_OK;
}
