/*
 * Wi-Fi for the one thing the game needs it for so far: the time of day (SNTP),
 * so a score can say when it was made. No game state depends on it.
 *
 * The radio is off unless it is working. At boot, with a saved network, it joins,
 * asks an NTP server, and shuts down again; the clock then runs on its own until
 * the next boot (there is no battery-backed RTC, so a power cycle forgets it).
 *
 * Setting up a network is done from a phone: provision_start() makes the board
 * its own hotspot (random WPA2 passphrase, shown to the player as a QR code), and
 * a small web page lists the networks the board can see. The player picks one,
 * types its password on the phone and the board saves and joins it. The page also
 * sends the phone's UTC offset, which is what local time is taken from.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    WIFI_LINK_IDLE,          /* radio off (maybe with a saved network, maybe not) */
    WIFI_LINK_CONNECTING,    /* joining the saved network / waiting for the time */
    WIFI_LINK_PROVISIONING,  /* the setup hotspot and page are up */
    WIFI_LINK_FAILED,        /* the last attempt to join or get the time failed */
} wifi_link_state_t;

/** Once, after nvs_flash_init(). Joins the saved network, if there is one. */
void wifi_link_init(void);

wifi_link_state_t wifi_link_state(void);

/** A network is saved; its name (empty when none). */
bool wifi_link_has_network(void);
const char *wifi_link_saved_ssid(void);

/** Start the setup hotspot and page. False if the radio could not start. */
bool wifi_link_provision_start(void);
/** Stop it again (back to IDLE, or onwards to CONNECTING if a network was just saved). */
void wifi_link_provision_stop(void);
/** What a phone needs to join the hotspot: its name and passphrase. */
void wifi_link_provision_info(char *ssid, size_t ssid_len, char *pass, size_t pass_len);
/** A network was chosen on the phone since provisioning started (it is now joining). */
bool wifi_link_provision_done(void);

/** Join the saved network again (after FAILED). */
void wifi_link_retry(void);
/** Forget the saved network. */
void wifi_link_forget(void);

/** The clock has been set from the network since boot. */
bool wifi_link_time_valid(void);
/** UTC seconds since 1970, or 0 when the clock has not been set. */
uint32_t wifi_link_unix_time(void);
/** The phone's offset from UTC, in minutes east, as sent at setup (0 if none). */
int wifi_link_tz_minutes(void);
