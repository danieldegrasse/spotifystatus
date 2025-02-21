# List of source files
SRCS=spotify_status.ino
# Board name
FQBN=esp32:esp32:adafruit_matrixportal_esp32s3
FQBN_OPTS=USBMode=hwcdc

.PHONY: clean upload

build/spotify_status.elf : $(SRCS)
	arduino-cli compile -v --jobs 0 --fqbn $(FQBN):$(FQBN_OPTS) --output-dir build .

clean:
	if [ -d build ]; then rm -r build; fi
