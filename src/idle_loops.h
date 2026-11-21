/* Wait loops mGBA's own table lacks (idle_loops = known): each only reads memory until an interrupt sets a flag, so
 * halting there gives the same pictures. Keyed by game code and ROM CRC32; found with mGBA's detection, checked on the desktop. */
static const struct {
	char code[5];
	uint32_t crc;
	uint32_t address;
} idleKnown[] = {
	{ "AFZE", 0xbd5e9798, 0x08000c26 }, /* F-Zero - Maximum Velocity (USA, Europe) */
	{ "AMWE", 0x58575f65, 0x08012eda }, /* Muppet Pinball Mayhem (USA) */
	{ "AY5E", 0xdfd07a36, 0x08075d8e }, /* Yu-Gi-Oh! - The Eternal Duelist Soul (USA) */
	{ "AZ2E", 0x302e75f3, 0x0809227c }, /* Zoids - Legacy (USA) */
	{ "AZLE", 0x8e91cd13, 0x080683a8 }, /* Legend of Zelda, The - A Link to the Past & Four Swords (USA) */
	{ "BDTE", 0x8686436e, 0x08000652 }, /* River City Ransom EX (USA) */
	{ "BENE", 0xaf8af2e8, 0x0802f3a8 }, /* Eragon (USA) */
	{ "BFZE", 0x781aab58, 0x08000c2a }, /* F-Zero - GP Legend (USA) */
	{ "BM5E", 0x25c394d1, 0x08033ee8 }, /* Mario vs. Donkey Kong (USA, Australia) */
};
