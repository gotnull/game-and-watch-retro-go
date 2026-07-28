
#if !defined (COVERFLOW)
  #define COVERFLOW 0
#endif /* COVERFLOW */
#if !defined (BIG_BANK)
#define BIG_BANK 1
#endif
#if (BIG_BANK == 1) && (EXTFLASH_SIZE <= 128*1024*1024)
#define EMU_DATA 
#else
#define EMU_DATA __attribute__((section(".extflash_emu_data")))
#endif
extern const rom_system_t amiga_system;
extern const uint8_t _binary__Users_fulvio_development_game_and_watch_retro_go_roms_amiga_Kickstart_v1_3_rom_start[];
extern const uint8_t _binary__Users_fulvio_development_game_and_watch_retro_go_roms_amiga_Kickstart_v1_3_img_start[];
uint8_t SAVE_AMIGA_0[0]  __attribute__((section (".saveflash"))) __attribute__((aligned(4096)));

const retro_emulator_file_t amiga_roms[] EMU_DATA = {
	{
#if CHEAT_CODES == 1
		.id = 2,
#endif
		.name = "Kickstart v1.3",
		.ext = "rom",
		.address = _binary__Users_fulvio_development_game_and_watch_retro_go_roms_amiga_Kickstart_v1_3_rom_start,
		.size = 262144,
		#if COVERFLOW != 0
		.img_address = _binary__Users_fulvio_development_game_and_watch_retro_go_roms_amiga_Kickstart_v1_3_img_start,
		.img_size = 4642,
		#endif
		.save_address = SAVE_AMIGA_0,
		.save_size = sizeof(SAVE_AMIGA_0),
		.system = &amiga_system,
		.region = REGION_NTSC,
		.mapper = 0,
		.game_config = 255,
#if CHEAT_CODES == 1
		.cheat_codes = NULL,
		.cheat_descs = 0,
		.cheat_count = 0,
#endif
	},

};
const uint32_t amiga_roms_count = 1;

const rom_system_t amiga_system EMU_DATA = {
	.system_name = "Amiga 500",
	.roms = amiga_roms,
	.extension = "amiga",
	#if COVERFLOW != 0
	.cover_width = 128,
	.cover_height = 96,
	#endif 
	.roms_count = 1,
};
