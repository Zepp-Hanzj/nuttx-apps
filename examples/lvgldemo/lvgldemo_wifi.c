/****************************************************************************
 * apps/examples/lvgldemo/lvgldemo_wifi.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <lvgl/lvgl.h>

#include <netutils/netlib.h>
#include <wireless/wapi.h>

#include "lvgldemo_wifi.h"

#define WIFI_IFNAME       CONFIG_EXAMPLES_LVGLDEMO_WIFI_IFNAME
#define WIFI_SSID_LEN     (WAPI_ESSID_MAX_SIZE + 1)
#define WIFI_PASSWORD_LEN 65
#define WIFI_OPTIONS_LEN  512
#define WIFI_STATUS_LEN   96
#define WIFI_MAX_NETWORKS 12

enum wifi_command_e
{
  WIFI_CMD_NONE = 0,
  WIFI_CMD_SCAN,
  WIFI_CMD_CONNECT
};

struct wifi_demo_s
{
  pthread_mutex_t lock;
  sem_t command_sem;
  enum wifi_command_e command;
  char command_ssid[WIFI_SSID_LEN];
  char command_password[WIFI_PASSWORD_LEN];
  char scan_options[WIFI_OPTIONS_LEN];
  char displayed_options[WIFI_OPTIONS_LEN];
  char status[WIFI_STATUS_LEN];
  unsigned int update_id;
  unsigned int displayed_id;
  lv_obj_t *ssid;
  lv_obj_t *password;
  lv_obj_t *networks;
  lv_obj_t *status_label;
  lv_obj_t *keyboard;
};

static struct wifi_demo_s g_wifi;

static void wifi_set_status(FAR const char *format, ...)
{
  va_list ap;

  pthread_mutex_lock(&g_wifi.lock);
  va_start(ap, format);
  vsnprintf(g_wifi.status, sizeof(g_wifi.status), format, ap);
  va_end(ap);
  g_wifi.update_id++;
  pthread_mutex_unlock(&g_wifi.lock);
}

static void wifi_publish_scan(FAR const char *options)
{
  pthread_mutex_lock(&g_wifi.lock);
  strlcpy(g_wifi.scan_options, options, sizeof(g_wifi.scan_options));
  g_wifi.update_id++;
  pthread_mutex_unlock(&g_wifi.lock);
}

static bool wifi_options_contains(FAR const char *options,
                                  FAR const char *ssid)
{
  FAR const char *line = options;
  size_t ssid_len = strlen(ssid);

  while (*line != '\0')
    {
      FAR const char *end = strchr(line, '\n');
      size_t line_len = end == NULL ? strlen(line) : (size_t)(end - line);

      if (line_len == ssid_len && memcmp(line, ssid, ssid_len) == 0)
        {
          return true;
        }

      if (end == NULL)
        {
          break;
        }

      line = end + 1;
    }

  return false;
}

static int wifi_scan(int sock)
{
  struct wapi_list_s list;
  FAR struct wapi_scan_info_s *info;
  char options[WIFI_OPTIONS_LEN];
  size_t used = 0;
  int tries;
  int ret;
  int count = 0;

  wifi_set_status("Scanning for 2.4 GHz networks...");
  ret = wapi_scan_init(sock, WIFI_IFNAME, NULL);
  if (ret < 0)
    {
      wifi_set_status("Scan start failed: %d", ret);
      return ret;
    }

  for (tries = 0; tries < 50; tries++)
    {
      usleep(200000);
      ret = wapi_scan_stat(sock, WIFI_IFNAME);
      if (ret <= 0)
        {
          break;
        }
    }

  if (ret != 0)
    {
      wifi_set_status(ret < 0 ? "Scan failed: %d" : "Scan timed out", ret);
      return ret < 0 ? ret : -ETIMEDOUT;
    }

  memset(&list, 0, sizeof(list));
  ret = wapi_scan_coll(sock, WIFI_IFNAME, &list);
  if (ret < 0)
    {
      wifi_set_status("Reading scan results failed: %d", ret);
      return ret;
    }

  options[0] = '\0';
  for (info = list.head.scan; info != NULL; info = info->next)
    {
      size_t len;

      if (count >= WIFI_MAX_NETWORKS)
        {
          break;
        }

      if (!info->has_essid || info->essid[0] == '\0' ||
          wifi_options_contains(options, info->essid))
        {
          continue;
        }

      len = strnlen(info->essid, WIFI_SSID_LEN);
      if (used + len + 2 >= sizeof(options))
        {
          break;
        }

      if (used > 0)
        {
          options[used++] = '\n';
        }

      memcpy(&options[used], info->essid, len);
      used += len;
      options[used] = '\0';
      count++;
    }

  wapi_scan_coll_free(&list);
  wifi_publish_scan(count > 0 ? options : "No networks found");
  wifi_set_status("Found %d network%s", count, count == 1 ? "" : "s");
  return OK;
}

static int wifi_wait_associated(void)
{
  struct ether_addr ap;
  int sock;
  int tries;
  int ret = -ETIMEDOUT;

  sock = wapi_make_socket();
  if (sock < 0)
    {
      return sock;
    }

  for (tries = 0; tries < 50; tries++)
    {
      size_t i;

      memset(&ap, 0, sizeof(ap));
      ret = wapi_get_ap(sock, WIFI_IFNAME, &ap);
      if (ret >= 0)
        {
          for (i = 0; i < sizeof(ap.ether_addr_octet); i++)
            {
              if (ap.ether_addr_octet[i] != 0)
                {
                  close(sock);
                  return OK;
                }
            }
        }

      usleep(200000);
    }

  close(sock);
  return -ETIMEDOUT;
}

static int wifi_connect(FAR const char *ssid, FAR const char *password)
{
  struct wpa_wconfig_s config;
  struct in_addr address;
  char address_text[INET_ADDRSTRLEN];
  int ret;

  memset(&config, 0, sizeof(config));
  config.ifname   = WIFI_IFNAME;
  config.sta_mode = WAPI_MODE_MANAGED;
  config.ssid     = ssid;
  config.ssidlen  = strnlen(ssid, WIFI_SSID_LEN);

  if (password[0] == '\0')
    {
      config.auth_wpa    = IW_AUTH_WPA_VERSION_DISABLED;
      config.cipher_mode = IW_AUTH_CIPHER_NONE;
      config.alg         = WPA_ALG_NONE;
    }
  else
    {
      config.auth_wpa    = IW_AUTH_WPA_VERSION_WPA2;
      config.cipher_mode = IW_AUTH_CIPHER_CCMP;
      config.alg         = WPA_ALG_CCMP;
      config.passphrase  = password;
      config.phraselen   = strnlen(password, WIFI_PASSWORD_LEN);
    }

  wifi_set_status("Connecting to %s...", ssid);
  ret = wpa_driver_wext_associate(&config);
  if (ret < 0)
    {
      wifi_set_status("Association failed: %d", ret);
      return ret;
    }

  ret = wifi_wait_associated();
  if (ret < 0)
    {
      wifi_set_status("Association timed out; check password");
      return ret;
    }

  wifi_set_status("Associated; requesting an IPv4 address...");
  ret = netlib_obtain_ipv4addr(WIFI_IFNAME);
  if (ret < 0)
    {
      wifi_set_status("DHCP failed: %d", ret);
      return ret;
    }

  ret = wapi_make_socket();
  if (ret < 0)
    {
      wifi_set_status("Connected (IP query failed: %d)", ret);
      return OK;
    }

  if (wapi_get_ip(ret, WIFI_IFNAME, &address) < 0 ||
      inet_ntop(AF_INET, &address, address_text, sizeof(address_text)) == NULL)
    {
      close(ret);
      wifi_set_status("Connected to %s", ssid);
      return OK;
    }

  close(ret);
  wifi_set_status("Connected to %s - IP %s", ssid, address_text);
  return OK;
}

static FAR void *wifi_worker(FAR void *arg)
{
  char ssid[WIFI_SSID_LEN];
  char password[WIFI_PASSWORD_LEN];
  enum wifi_command_e command;
  int sock;
  int ret;

  sock = wapi_make_socket();
  if (sock < 0)
    {
      wifi_set_status("Cannot open WLAN control socket: %d", sock);
      return NULL;
    }

  wifi_set_status("Starting %s...", WIFI_IFNAME);
  ret = wapi_set_ifup(sock, WIFI_IFNAME);
  if (ret < 0)
    {
      wifi_set_status("Failed to start %s: %d", WIFI_IFNAME, ret);
    }
  else
    {
      wifi_set_status("Wi-Fi ready - tap Scan");
    }

  for (;;)
    {
      while (sem_wait(&g_wifi.command_sem) < 0 && errno == EINTR)
        {
        }

      pthread_mutex_lock(&g_wifi.lock);
      command = g_wifi.command;
      g_wifi.command = WIFI_CMD_NONE;
      strlcpy(ssid, g_wifi.command_ssid, sizeof(ssid));
      strlcpy(password, g_wifi.command_password, sizeof(password));
      pthread_mutex_unlock(&g_wifi.lock);

      if (command == WIFI_CMD_SCAN)
        {
          wifi_scan(sock);
        }
      else if (command == WIFI_CMD_CONNECT && ssid[0] != '\0')
        {
          wifi_connect(ssid, password);
        }
    }

  close(sock);
  return NULL;
}

static void wifi_submit(enum wifi_command_e command)
{
  pthread_mutex_lock(&g_wifi.lock);
  if (g_wifi.command == WIFI_CMD_NONE)
    {
      g_wifi.command = command;
      if (command == WIFI_CMD_CONNECT)
        {
          strlcpy(g_wifi.command_ssid,
                  lv_textarea_get_text(g_wifi.ssid),
                  sizeof(g_wifi.command_ssid));
          strlcpy(g_wifi.command_password,
                  lv_textarea_get_text(g_wifi.password),
                  sizeof(g_wifi.command_password));
        }

      sem_post(&g_wifi.command_sem);
    }

  pthread_mutex_unlock(&g_wifi.lock);
}

static void wifi_button_event(lv_event_t *event)
{
  wifi_submit((enum wifi_command_e)(uintptr_t)lv_event_get_user_data(event));
}

static void wifi_network_event(lv_event_t *event)
{
  FAR lv_obj_t *button = lv_event_get_current_target(event);
  FAR const char *selected;

  selected = lv_list_get_button_text(g_wifi.networks, button);
  if (selected != NULL)
    {
      lv_textarea_set_text(g_wifi.ssid, selected);
    }
}

static void wifi_update_networks(FAR const char *options)
{
  FAR const char *line = options;

  lv_obj_clean(g_wifi.networks);
  if (options[0] == '\0' || strcmp(options, "No networks found") == 0)
    {
      lv_list_add_text(g_wifi.networks, options[0] == '\0' ?
                       "Tap Scan to discover networks" : options);
      return;
    }

  while (*line != '\0')
    {
      FAR const char *end = strchr(line, '\n');
      char ssid[WIFI_SSID_LEN];
      size_t len = end == NULL ? strlen(line) : (size_t)(end - line);
      FAR lv_obj_t *button;

      if (len >= sizeof(ssid))
        {
          len = sizeof(ssid) - 1;
        }
      memcpy(ssid, line, len);
      ssid[len] = '\0';

      button = lv_list_add_button(g_wifi.networks, LV_SYMBOL_WIFI, ssid);
      lv_obj_add_event_cb(button, wifi_network_event, LV_EVENT_CLICKED, NULL);

      if (end == NULL)
        {
          break;
        }

      line = end + 1;
    }
}

static void wifi_textarea_event(lv_event_t *event)
{
  lv_obj_t *textarea = lv_event_get_target_obj(event);

  lv_keyboard_set_textarea(g_wifi.keyboard, textarea);
  lv_obj_remove_flag(g_wifi.keyboard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(g_wifi.keyboard);
}

static void wifi_keyboard_event(lv_event_t *event)
{
  lv_obj_add_flag(g_wifi.keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void wifi_ui_timer(lv_timer_t *timer)
{
  char status[WIFI_STATUS_LEN];
  char options[WIFI_OPTIONS_LEN];
  unsigned int update_id;
  bool changed = false;

  (void)timer;
  status[0] = '\0';
  options[0] = '\0';

  pthread_mutex_lock(&g_wifi.lock);
  update_id = g_wifi.update_id;
  if (update_id != g_wifi.displayed_id)
    {
      strlcpy(status, g_wifi.status, sizeof(status));
      strlcpy(options, g_wifi.scan_options, sizeof(options));
      g_wifi.displayed_id = update_id;
      changed = true;
    }
  pthread_mutex_unlock(&g_wifi.lock);

  if (!changed)
    {
      return;
    }

  lv_label_set_text(g_wifi.status_label, status);
  if (options[0] != '\0' &&
      strcmp(options, g_wifi.displayed_options) != 0)
    {
      wifi_update_networks(options);
      strlcpy(g_wifi.displayed_options, options,
              sizeof(g_wifi.displayed_options));
    }
}

static lv_obj_t *wifi_make_label(FAR lv_obj_t *parent, FAR const char *text,
                                 int x, int y)
{
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, text);
  lv_obj_set_pos(label, x, y);
  return label;
}

void lvgldemo_wifi_create(void)
{
  pthread_attr_t attr;
  pthread_t thread;
  lv_obj_t *screen = lv_screen_active();
  lv_obj_t *button;
  lv_obj_t *label;
  int ret;

  memset(&g_wifi, 0, sizeof(g_wifi));
  pthread_mutex_init(&g_wifi.lock, NULL);
  sem_init(&g_wifi.command_sem, 0, 0);
  strlcpy(g_wifi.status, "Initializing AP6181...", sizeof(g_wifi.status));
  g_wifi.update_id = 1;

  lv_obj_clean(screen);
  lv_obj_set_style_bg_color(screen, lv_color_hex(0xf3f6fa), 0);

  label = wifi_make_label(screen, "AP6181 Wi-Fi", 28, 18);

  g_wifi.status_label = wifi_make_label(screen, g_wifi.status, 28, 55);
  lv_obj_set_style_text_color(g_wifi.status_label, lv_color_hex(0x3264a8), 0);

  wifi_make_label(screen, "Networks", 28, 99);
  g_wifi.networks = lv_list_create(screen);
  lv_obj_set_pos(g_wifi.networks, 28, 122);
  lv_obj_set_size(g_wifi.networks, 520, 142);
  lv_list_add_text(g_wifi.networks, "Tap Scan to discover networks");

  button = lv_button_create(screen);
  lv_obj_set_pos(button, 570, 122);
  lv_obj_set_size(button, 190, 48);
  label = lv_label_create(button);
  lv_label_set_text(label, "Scan");
  lv_obj_center(label);
  lv_obj_add_event_cb(button, wifi_button_event, LV_EVENT_CLICKED,
                      (FAR void *)(uintptr_t)WIFI_CMD_SCAN);

  wifi_make_label(screen, "SSID", 28, 274);
  g_wifi.ssid = lv_textarea_create(screen);
  lv_textarea_set_one_line(g_wifi.ssid, true);
  lv_textarea_set_max_length(g_wifi.ssid, WAPI_ESSID_MAX_SIZE);
  lv_obj_set_pos(g_wifi.ssid, 28, 298);
  lv_obj_set_size(g_wifi.ssid, 350, 48);
  lv_obj_add_event_cb(g_wifi.ssid, wifi_textarea_event, LV_EVENT_FOCUSED, NULL);

  wifi_make_label(screen, "Password", 398, 274);
  g_wifi.password = lv_textarea_create(screen);
  lv_textarea_set_one_line(g_wifi.password, true);
  lv_textarea_set_password_mode(g_wifi.password, true);
  lv_textarea_set_max_length(g_wifi.password, WIFI_PASSWORD_LEN - 1);
  lv_obj_set_pos(g_wifi.password, 398, 298);
  lv_obj_set_size(g_wifi.password, 362, 48);
  lv_obj_add_event_cb(g_wifi.password, wifi_textarea_event,
                      LV_EVENT_FOCUSED, NULL);

  button = lv_button_create(screen);
  lv_obj_set_pos(button, 570, 365);
  lv_obj_set_size(button, 190, 54);
  label = lv_label_create(button);
  lv_label_set_text(label, "Connect");
  lv_obj_center(label);
  lv_obj_add_event_cb(button, wifi_button_event, LV_EVENT_CLICKED,
                      (FAR void *)(uintptr_t)WIFI_CMD_CONNECT);

  wifi_make_label(screen,
                  "WPA2-PSK and open networks are supported. Leave password blank for open Wi-Fi.",
                  28, 438);

  g_wifi.keyboard = lv_keyboard_create(screen);
  lv_obj_set_size(g_wifi.keyboard, LV_PCT(100), 210);
  lv_obj_align(g_wifi.keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(g_wifi.keyboard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(g_wifi.keyboard, wifi_keyboard_event,
                      LV_EVENT_READY, NULL);
  lv_obj_add_event_cb(g_wifi.keyboard, wifi_keyboard_event,
                      LV_EVENT_CANCEL, NULL);

  lv_timer_create(wifi_ui_timer, 200, NULL);

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize(&attr,
                           CONFIG_EXAMPLES_LVGLDEMO_WIFI_WORKER_STACKSIZE);
  ret = pthread_create(&thread, &attr, wifi_worker, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      wifi_set_status("Failed to start Wi-Fi worker: %d", ret);
    }
}
