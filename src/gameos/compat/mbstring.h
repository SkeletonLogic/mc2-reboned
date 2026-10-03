#pragma once
// PORT: The browser build uses the game's narrow single-byte text path.
inline unsigned char* _mbsinc(const unsigned char* current)
{
	return const_cast<unsigned char*>(current ? current + 1 : current);
}
inline unsigned char* _mbsdec(const unsigned char* start, const unsigned char* current)
{
	return const_cast<unsigned char*>(start && current && current > start ? current - 1 : nullptr);
}
inline int isleadbyte(unsigned char) { return 0; }
