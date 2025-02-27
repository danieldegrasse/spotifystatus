#include <Adafruit_Protomatter.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <NetworkClientSecure.h>
#include <arduino-timer.h>
#include <TJpg_Decoder.h>

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
  WIDTH, 6, 1, rgbPins, NUM_ADDR_PINS, addrPins,
  clockPin, latchPin, oePin, true);

/* Wireless configuration */
const char ssid[] = WIFI_SSID;
const char psk[] = WIFI_PASSPHRASE;

/* HTTPS constants */
const char now_playing_url[] = "https://api.spotify.com/v1/me/player/currently-playing";
const char token_refresh_url[] = "https://accounts.spotify.com/api/token";
char auth_header[223];
char refresh_body[214];

/* Song state data */
char song_name[128];
uint8_t image_data[128*128];
/* Elapsed song time in ms */
uint32_t t_elapsed;
/* Total song duration in ms */
uint32_t t_duration;

enum audio_state {
	AUDIO_STOPPED,
	AUDIO_PAUSED,
	AUDIO_PLAYING
} audio_state = AUDIO_STOPPED;


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

const char *timezone = TIMEZONE;

/* Sets clock- required for HTTPS certificate verification */
static bool setClock(void *arg) {
	configTzTime(timezone, "pool.ntp.org");

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
	NetworkClientSecure client;
	client.setCACert(rootCACertificate);

	/*
	 * Now, send HTTP POST request to refresh the auth token- this
	 * follows the auth flow described here:
	 * https://developer.spotify.com/documentation/web-api/tutorials/refreshing-tokens
	 */
	HTTPClient https;
	https.begin(client, token_refresh_url);
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
		if (https.getSize() > 0) {
			Serial.println(https.getString());
		}
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

/* Callback to draw decoded JPEG on screen */
static bool matrixOutput(int16_t x, int16_t y, uint16_t w,
			 uint16_t h, uint16_t* bitmap) {
	if ((y + h) >= matrix.height()) {
		/* Clip the height of the bitmap */
		h = matrix.height() - y;
	}

	matrix.drawRGBBitmap(x, y, bitmap, w, h);

	/* Continue decoding */
	return 1;
}

/* Downloads and displays album art */
static bool displayAlbumArt(const char *url) {
	int ret;

	Serial.printf("Downloading image from %s\r\n", url);

	/* Create secure network connection */
	NetworkClientSecure client;
	client.setCACert(rootCACertificate);

	/* Send HTTP GET request to read currently playing data */
	HTTPClient https;
	https.begin(client, url);
	ret = https.GET();
	if (ret < 0) {
		Serial.printf("Error getting url: %d\r\n", ret);
		/* Do reschedule this event, this could be a network error */
		return true;
	} else if (ret != 200) {
		Serial.printf("Album art url returned error code: %d\r\n", ret);
		/* Do reschedule this event, this could be an API error */
		return true;
	}
	int len = https.getSize();
	int img_size = len;
	uint8_t *rptr = image_data;

	if (img_size > sizeof(image_data)) {
		Serial.printf("Error, album art JPEG size %d is too large\r\n", img_size);
		/* We can continue running the song checking code */
		return true;
	}

	NetworkClient *stream = https.getStreamPtr();
	while (https.connected() && (len > 0 || len == -1)) {
		/* Read bytes into the image array */
		size_t size = stream->available();

		if (size) {
			/* Read up to 1024 byte chunk */
			if (size > 1024) {
				size = 1024;
			}
			int c = stream->readBytes(rptr, size);

			if (len > 0) {
				len -= c;
			}
			rptr += c;
		}
		delay(1);
	}
	Serial.printf("Downloaded album art, size %d\r\n", img_size);

	uint16_t w, h;
	JRESULT rc = TJpgDec.getJpgSize(&w, &h, image_data, img_size);
	if (rc != JDR_OK) {
		Serial.printf("Failed to decode JPG: %d\r\n", rc);
		return true;
	}
	Serial.printf("Displaying art with W=%d, H=%d\r\n", w, h);
	TJpgDec.drawJpg(0, 0, image_data, img_size);

	return true;
}

/* Show idle background display for when music isn't playing */
static bool idleDisplay(void *arg) {
	if (audio_state != AUDIO_STOPPED) {
		return true;
	}

	time_t rawtime;
	struct tm * timeinfo;
	char buffer[80];

	time (&rawtime);
	timeinfo = localtime(&rawtime);
	strftime(buffer,80,"%k:%M",timeinfo);

	/* Display the time when idle */
	matrix.setCursor(10,15);
	matrix.setTextColor(0xfb1f);
	matrix.printf("%s", buffer);
	matrix.show();
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
	NetworkClientSecure client;
	client.setCACert(rootCACertificate);

	/* Send HTTP GET request to read currently playing data */
	HTTPClient https;
	https.begin(client, now_playing_url);
	https.addHeader("Authorization", auth_header);
	ret = https.GET();
	if (ret < 0) {
		Serial.printf("Error getting url: %d\r\n", ret);
		/* Do reschedule this event, this could be a network error */
		return true;
	} else if (ret == 204) {
		Serial.println("No music is playing...");
		audio_state = AUDIO_STOPPED;
		/* Do reschedule this event, we need to check if music starts */
		return true;
	} else if (ret != 200) {
		Serial.printf("Now playing API returned error code: %d\r\n", ret);
		if (https.getSize() > 0) {
			Serial.println(https.getString());
		}
		/* Do reschedule this event, this could be an API error */
		return true;
	}

	String payload = https.getString();
	https.end();
	JsonDocument response;
	DeserializationError error = deserializeJson(response, payload.c_str());
	if (error) {
		Serial.print(F("deserializeJson() failed: "));
		Serial.println(error.f_str());
		return false;
	}
	audio_state = response["is_playing"].as<bool>() ? AUDIO_PLAYING: AUDIO_PAUSED;
	matrix.setTextColor(0x0);

	if (strncmp(song_name, response["item"]["name"], sizeof(song_name)) == 0) {
		Serial.println("Song is unchanged");
		/* Check again in a bit */
		return true;
	}

	strlcpy(song_name, response["item"]["name"], sizeof(song_name));

	/*
	 * Now, attempt to download the album art. We use the last element
	 * in the array, since that seems to have the smallest dimensions
	 */
	int img_idx = response["item"]["album"]["images"].size() - 1;
	JsonObject image = response["item"]["album"]["images"][img_idx];
	int img_size = image["height"].as<int>() * image["width"].as<int>();

	if (img_size > sizeof(image_data)) {
		Serial.printf("Skipping image download, size of %d is too large\r\n", img_size);
		return true;
	}

	if (!displayAlbumArt(image["url"])) {
		return false;
	}

	/* Set song duration data */
	t_elapsed = response["progress_ms"];
	t_duration = response["item"]["duration_ms"];

	Serial.printf("Song name: %s\r\n", song_name);
	/* Print over the album art */
	matrix.setCursor(1, 1);
	matrix.println(song_name);

	matrix.show(); /* Copy data to matrix buffers */
	return true;
}

static bool printStats(void *arg) {
#if defined(ARDUINO_ADAFRUIT_MATRIXPORTAL_ESP32S3)
	/* Print free memory */
	Serial.printf("Free memory: %db\r\n", esp_get_free_heap_size());
#endif
	return true;
}

static bool updateSongElapsed(void *arg) {
	uint32_t elapsed_cnt;
	int x_off;

	if (audio_state != AUDIO_PLAYING) {
		return true;
	}

	/* Calculate elapsed and remaining pixel counts */
	if (t_elapsed > t_duration) {
		/*
		 * Song should be over soon, or no song is playing.
		 * no need to increment more.
		 */
		return true;
	}
	t_elapsed += 1000;
	elapsed_cnt = (WIDTH * t_elapsed) / t_duration;
	Serial.printf("Song is %d/%d blocks completed\r\n", elapsed_cnt, WIDTH);
	/* Update song elapsed duration display */
	for (x_off = 0; x_off < elapsed_cnt; x_off++) {
		matrix.writePixel(x_off, HEIGHT - 1, 0xFFFF);
	}
	for (; x_off < WIDTH; x_off++) {
		matrix.writePixel(x_off, HEIGHT - 1, 0x0);
	}
	matrix.show();

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

	TJpgDec.setJpgScale(1);
	TJpgDec.setCallback(matrixOutput);

	ProtomatterStatus status = matrix.begin();
	Serial.printf("Protomatter begin() status: %d\r\n", status);
	matrix.fillScreen(0);
	matrix.setTextColor(0x0);
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
	if(!idleDisplay(NULL)) {
		return;
	}

	/* Schedule periodic tasks */
	/* Resync NTP time once daily */
	timer.every(3600 * 24 * 1000, setClock);
	/* Reauthorize with spotify every 50 minutes */
	timer.every(50 * 60 * 1000, refreshAuth);
	/* Request song data every 10 seconds */
	timer.every(10000, requestSong);
	/* Update song progress every second */
	timer.every(1000, updateSongElapsed);
	/* Dump system stats to serial every 20 seconds */
	timer.every(20000, printStats);
	/* Run idle display every 10 seconds */
	timer.every(10000, idleDisplay);
}

/* Main loop- runs continuously */
void loop() {
	/* Tick the timer */
	timer.tick();
}

