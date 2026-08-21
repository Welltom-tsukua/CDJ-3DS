TARGET := 3ds_one_deck
SOURCE := source/main.c source/audio.c
OBJECT := main.o audio.o
FONT_TTF := gfx/PressStart2P-Regular.ttf
FONT_BCFNT := romfs/PressStart2P-Regular.bcfnt
CIA_TARGET := $(TARGET)_himem.cia
CIA_ROMFS := $(TARGET).romfs
CIA_RSF := one_deck_himem.rsf
MAKEROM := tools/makerom/makerom.exe
THREEDSTOOL := tools/3dstool/3dstool.exe
FAAD_CFILES := $(notdir $(wildcard third_party/faad2/libfaad/*.c))
FAAD_OBJECTS := $(addprefix faad_,$(FAAD_CFILES:.c=.o)) faad_mp4read.o faad_unicode_support.o

ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO must point to C:/devkitPro)
endif
ifeq ($(strip $(DEVKITARM)),)
$(error DEVKITARM must point to the devkitARM directory)
endif

include $(DEVKITARM)/3ds_rules

ARCH    := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS  := -g -Wall -Wextra -O2 -mword-relocations -ffunction-sections $(ARCH) -I$(CTRULIB)/include -Ithird_party/minimp3 -D__3DS__
FAAD_CFLAGS := $(CFLAGS) -Ithird_party/faad2 -Ithird_party/faad2/include -Ithird_party/faad2/libfaad -Ithird_party/faad2/frontend -DPACKAGE_VERSION=\"2.11.2\" -DHAVE_LRINTF=1 -DHAVE_INTTYPES_H=1 -DHAVE_MEMCPY=1 -DHAVE_STRING_H=1 -DHAVE_STRINGS_H=1 -DHAVE_SYS_STAT_H=1 -DHAVE_SYS_TYPES_H=1
LDFLAGS := -specs=3dsx.specs -g $(ARCH)
LIBS    := -L$(CTRULIB)/lib -lcitro2d -lcitro3d -lctru -lm

.PHONY: all clean cia
all: $(TARGET).3dsx

cia: $(CIA_TARGET)

$(FONT_BCFNT): $(FONT_TTF)
	mkdir -p romfs
	mkbcfnt -s 18 -o $@ $<

main.o: source/main.c source/audio.h
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

audio.o: source/audio.c source/audio.h third_party/minimp3/minimp3.h third_party/faad2/neaacdec.h third_party/faad2/frontend/mp4read.h
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

faad_%.o: third_party/faad2/libfaad/%.c
	$(CC) $(FAAD_CFLAGS) -MMD -MP -c $< -o $@

faad_mp4read.o: third_party/faad2/frontend/mp4read.c third_party/faad2/frontend/mp4read.h
	$(CC) $(FAAD_CFLAGS) -MMD -MP -c $< -o $@

faad_unicode_support.o: third_party/faad2/frontend/unicode_support.c third_party/faad2/frontend/unicode_support.h
	$(CC) $(FAAD_CFLAGS) -MMD -MP -c $< -o $@

$(TARGET).elf: $(OBJECT) $(FAAD_OBJECTS)
	$(CC) $(LDFLAGS) $^ $(LIBS) -o $@

$(TARGET).smdh:
	smdhtool --create "3DS One Deck" "Rekordbox one-deck player prototype" "kirin" $(CTRULIB)/default_icon.png $@

$(TARGET).3dsx: $(TARGET).elf $(TARGET).smdh $(FONT_BCFNT)
	3dsxtool $(TARGET).elf $@ --smdh=$(TARGET).smdh --romfs=romfs

$(CIA_ROMFS): $(FONT_BCFNT) romfs/PioneerDJLogo.rgb565 romfs/rekordboxLogo.rgb565
	$(THREEDSTOOL) -cvtf romfs $@ --romfs-dir romfs

$(CIA_TARGET): $(TARGET).elf $(TARGET).smdh $(CIA_ROMFS) $(CIA_RSF)
	$(MAKEROM) -f cia -o $@ -rsf $(CIA_RSF) -target t -elf $(TARGET).elf -icon $(TARGET).smdh -romfs $(CIA_ROMFS) -desc app:4 -major 1 -minor 0 -micro 0

clean:
	rm -f $(OBJECT) $(FAAD_OBJECTS) $(OBJECT:.o=.d) $(FAAD_OBJECTS:.o=.d) $(TARGET).elf $(TARGET).smdh $(TARGET).3dsx $(CIA_TARGET) $(CIA_ROMFS) $(FONT_BCFNT)

-include $(OBJECT:.o=.d) $(FAAD_OBJECTS:.o=.d)
