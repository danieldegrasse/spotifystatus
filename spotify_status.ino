#include <Adafruit_Protomatter.h>
#include <Fonts/FreeMonoBoldOblique24pt7b.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <NetworkClientSecure.h>
#include <arduino-timer.h>

#include "app_secrets.h"

/* Matrix display configuration */
#define HEIGHT  32 // Matrix height (pixels)
#define WIDTH   64 // Matrix width (pixels)

uint8_t rgbPins[]  = {42, 41, 40, 38, 39, 37};
uint8_t addrPins[] = {45, 36, 48, 35, 21};
uint8_t clockPin   = 2;
uint8_t latchPin   = 47;
uint8_t oePin      = 14;

#define NUM_ADDR_PINS 4

Adafruit_Protomatter matrix(
  WIDTH, 4, 1, rgbPins, NUM_ADDR_PINS, addrPins,
  clockPin, latchPin, oePin, true);

/* Wireless configuration */
const char ssid[] = WIFI_SSID;
const char psk[] = WIFI_PASSPHRASE;

/* HTTPS constants */
const char now_playing_url[] = "https://api.spotify.com/v1/me/player/currently-playing";
const char token_refresh_url[] = "https://accounts.spotify.com/api/token";
char auth_header[223];
char refresh_body[214];

/* DigiCert root CA used by spotify. Valid until 2038 */
const char *rootCACertificate = R"string_literal(
-----BEGIN CERTIFICATE-----
MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh
MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3
d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH
MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT
MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j
b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG
9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI
2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx
1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ
q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz
tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ
vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP
BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV
5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY
1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4
NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG
Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91
8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe
pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl
MrY=
-----END CERTIFICATE-----
)string_literal";

/* Sets clock- required for HTTPS certificate verification */
static bool setClock(void *arg) {
	configTime(0, 0, "pool.ntp.org");

	Serial.print(F("Waiting for NTP time sync: "));
	time_t nowSecs = time(nullptr);
	while (nowSecs < 8 * 3600 * 2) {
		delay(500);
		Serial.print(F("."));
		yield();
		nowSecs = time(nullptr);
	}

	Serial.println();
	struct tm timeinfo;
	gmtime_r(&nowSecs, &timeinfo);
	Serial.print(F("Current time: "));
	Serial.println(asctime(&timeinfo));
	return true;
}

/* Refreshes authorization code used for spotify requests */
static bool refreshAuth(void *arg) {
	int ret;

	/* Invalidate old auth header */
	memset(auth_header, 0, sizeof(auth_header));
	/* Create secure network connection */
	NetworkClientSecure *client = new NetworkClientSecure;
	if (!client) {
		Serial.println("Error, could not create network client");
		return false;
	}
	client->setCACert(rootCACertificate);

	/*
	 * Now, send HTTP POST request to refresh the auth token- this
	 * follows the auth flow described here:
	 * https://developer.spotify.com/documentation/web-api/tutorials/refreshing-tokens
	 */
	HTTPClient https;
	https.begin(*client, token_refresh_url);
	/* Add content-type and authorization header */
	https.addHeader("Content-Type", "application/x-www-form-urlencoded");
	https.addHeader("Authorization", SPOTIFY_AUTH_SECRET);
	ret = snprintf(refresh_body, sizeof(refresh_body),
		       "grant_type=refresh_token&client_id=%s&refresh_token=%s",
		       SPOTIFY_AUTH_CLIENT_ID, SPOTIFY_AUTH_REFRESH_TOKEN);
	if (ret >= sizeof(refresh_body)) {
		Serial.println("Error, refresh token or client ID was an unexpected length");
		return false;
	}
	/* Send post request with data */
	ret = https.POST(String(refresh_body));
	if (ret < 0) {
		Serial.printf("Error getting url: %d\r\n", ret);
		/* Do reschedule this event, this could be a network error */
		return true;
	} else if (ret != 200) {
		Serial.printf("Refresh API returned error code: %d\r\n", ret);
		Serial.println(https.getString());
		/* Do reschedule this event, this could be an API error */
		return true;
	}

	/* Deserialize JSON response to get auth token */
	JsonDocument response;
	DeserializationError error = deserializeJson(response, https.getString().c_str());
	if (error) {
		Serial.print(F("deserializeJson() failed: "));
		Serial.println(error.f_str());
		return false;
	}

	const char *auth_token = response["access_token"];
	ret = snprintf(auth_header, sizeof(auth_header), "Bearer %s", auth_token);
	if (ret >= sizeof(auth_header)) {
		Serial.println("Error, auth token from endpoint was unexpected length");
		return false;
	}
	Serial.printf("Updated auth header to %s\r\n", auth_header);
	return true;
}

/* Requests song data from Spotify API */
static bool requestSong(void *arg) {
	int ret;

	if (strlen(auth_header) == 0) {
		Serial.println("Error, auth header not available");
		return false;
	}

	/* Create secure network connection */
	NetworkClientSecure *client = new NetworkClientSecure;
	if (!client) {
		Serial.println("Error, could not create network client");
		return false;
	}
	client->setCACert(rootCACertificate);

	/* Send HTTP GET request to read currently playing data */
	HTTPClient https;
	https.begin(*client, now_playing_url);
	https.addHeader("Authorization", auth_header);
	ret = https.GET();
	if (ret < 0) {
		Serial.printf("Error getting url: %d\r\n", ret);
		/* Do reschedule this event, this could be a network error */
		return true;
	} else if (ret != 200) {
		Serial.printf("Now playing API returned error code: %d\r\n", ret);
		Serial.println(https.getString());
		/* Do reschedule this event, this could be an API error */
		return true;
	}
	String payload = https.getString();
	JsonDocument response;
	DeserializationError error = deserializeJson(response, payload.c_str());
	if (error) {
		Serial.print(F("deserializeJson() failed: "));
		Serial.println(error.f_str());
		return false;
	}

	const char *song_name = response["item"]["name"];

	Serial.printf("Song name: %s\r\n", song_name);
	matrix.fillScreen(0);
	matrix.setCursor(0, 0);
	matrix.println(song_name);
	matrix.show(); /* Copy data to matrix buffers */
	return true;
}

/* Timer which manages all periodic tasks */
auto timer = timer_create_default();

/* Setup function. Runs once at program start */
void setup(void) {
  	Serial.begin(115200);
	/* Start wifi in station mode */
	WiFi.mode(WIFI_STA);
	WiFi.begin(ssid, psk);
	Serial.printf("Connecting to wireless network %s\r\n", ssid);
	while (WiFi.status() != WL_CONNECTED) {
		delay(500);
		Serial.print(".");
	}
	Serial.println("");
	Serial.println("WiFi connected");
	Serial.println("IP address: ");
	Serial.println(WiFi.localIP());

	ProtomatterStatus status = matrix.begin();
	Serial.printf("Protomatter begin() status: %d\r\n", status);
	matrix.fillScreen(0);
	matrix.show(); /* Show initial matrix data */

	/* Run all periodic tasks at boot */
	if (!setClock(NULL)) {
		return;
	}
	if (!refreshAuth(NULL)) {
		return;
	}
	if (!requestSong(NULL)) {
		return;
	}

	/* Schedule periodic tasks */
	/* Resync NTP time once daily */
	timer.every(3600 * 24 * 1000, setClock);
	/* Reauthorize with spotify every 50 minutes */
	timer.every(50 * 60 * 1000, refreshAuth);
	/* Request song data every 10 seconds */
	timer.every(10000, requestSong);
}

/* Main loop- runs continuously */
void loop() {
	/* Tick the timer */
	timer.tick();
}

