CC	:= clang
CFLAGS	:= -std=c23		\
	   -O2			\
	   -fstrict-aliasing	\
	   -pedantic		\
	   -Wall		\
	   -Werror		\
	   -Wextra		\
	   -Wshadow		\
	   -Wconversion		\
	   -Wstrict-aliasing
LDFLAGS	:=
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)

CFLAGS_COCOA := $(CFLAGS) -fobjc-arc -mmacosx-version-min=11.0
LDFLAGS_COCOA := -framework Cocoa

.PHONY: all clean terminal run

all: CHIP-8.app chip8

obj:
	mkdir -p obj

CHIP-8.app: chip8.m | obj
	$(CC) $(CFLAGS_COCOA) $(LDFLAGS_COCOA) -o obj/chip8-cocoa chip8.m
	mkdir -p CHIP-8.app/Contents/MacOS
	cp obj/chip8-cocoa CHIP-8.app/Contents/MacOS/chip8
	@echo '<?xml version="1.0" encoding="UTF-8"?>' > CHIP-8.app/Contents/Info.plist
	@echo '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' >> CHIP-8.app/Contents/Info.plist
	@echo '<plist version="1.0">' >> CHIP-8.app/Contents/Info.plist
	@echo '<dict>' >> CHIP-8.app/Contents/Info.plist
	@echo '    <key>CFBundleExecutable</key>' >> CHIP-8.app/Contents/Info.plist
	@echo '    <string>chip8</string>' >> CHIP-8.app/Contents/Info.plist
	@echo '    <key>CFBundleIdentifier</key>' >> CHIP-8.app/Contents/Info.plist
	@echo '    <string>com.chip8.emulator</string>' >> CHIP-8.app/Contents/Info.plist
	@echo '    <key>CFBundleName</key>' >> CHIP-8.app/Contents/Info.plist
	@echo '    <string>CHIP-8</string>' >> CHIP-8.app/Contents/Info.plist
	@echo '    <key>CFBundlePackageType</key>' >> CHIP-8.app/Contents/Info.plist
	@echo '    <string>APPL</string>' >> CHIP-8.app/Contents/Info.plist
	@echo '    <key>CFBundleVersion</key>' >> CHIP-8.app/Contents/Info.plist
	@echo '    <string>1.0</string>' >> CHIP-8.app/Contents/Info.plist
	@echo '    <key>NSHighResolutionCapable</key>' >> CHIP-8.app/Contents/Info.plist
	@echo '    <true/>' >> CHIP-8.app/Contents/Info.plist
	@echo '</dict>' >> CHIP-8.app/Contents/Info.plist
	@echo '</plist>' >> CHIP-8.app/Contents/Info.plist

terminal: chip8

chip8: chip8.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	@rm -rf CHIP-8.app chip8 obj *.o

run: CHIP-8.app
	open CHIP-8.app

else

TARGET	:= chip8
SRCS	:= chip8.c

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

clean:
	@rm -f $(TARGET) *.o

run: $(TARGET)
	./$(TARGET)

endif
