# The port does not assemble this file: it loads both tables from the
# converted sys/ folder, taken from the player's main.dol (see
# platform/src/dvd/dol_data.cpp); the .bcsv files are not in this repository.

.section .rodata

.global StoryEventBCSV   # 0x8053DC20 - 0x8053DDFF
StoryEventBCSV:
	.incbin "src/Game/System/StoryEvent.bcsv"

.global GalaxyIDBCSV     # 0x8053DE00 - 0x8053EB1F
GalaxyIDBCSV:
	.incbin "src/Game/System/GalaxyID.bcsv"
