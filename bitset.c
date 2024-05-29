#ifdef DEBUG_FLAG
	#include <stdio.h>
#endif
#include "bitset.h"

void bitset_set(uint8_t* set, int idx) {
	set[idx>>3] |= 1<<(idx&7);
#ifdef DEBUG_FLAG
	printf("set bit %d\n", idx);
#endif
}

void bitset_clear(uint8_t* set, int idx) {
	set[idx>>3] &= ~(1<<(idx&7));
#ifdef DEBUG_FLAG
	printf("cleared bit %d\n", idx);
#endif
}

int bitset_is_set(uint8_t* set, int idx) {
	return (set[idx>>3] & (1<<(idx&7))) != 0;
}

int bitset_is_clear(uint8_t* set, int idx) {
	return (set[idx>>3] & (1<<(idx&7))) == 0;
}

int bitset_ctz(uint8_t* set, int size) {
	uint32_t word;
	int off = 0;
	size = (size + 7) >> 3; // num. bytes for size bits
	for (; size > 4; size -= 4) {
		word = ((uint32_t) set[0])     |
			   ((uint32_t) set[1])<<8  |
			   ((uint32_t) set[2])<<16 |
			   ((uint32_t) set[3])<<24;
		if (word) {
			return off + __builtin_ctz(word);
		}
		set += 4;
		off += 32;
	}
	word = 0;
	for (int i = 0; i < size; i++) {
		word |= ((uint32_t) set[i])<<(i<<3);
	}
	return off + (word ? __builtin_ctz(word) : (size << 3));
}

int bitset_next_set(uint8_t* set, int size, int idx) {
	uint32_t word = 0;
	int nBits = size - idx;
	int nBytes = (nBits + 7) >> 3;
	int off = (idx >> 3) << 3;
	set += idx >> 3;
	for (int i = 0; i < 4 && i < nBytes; i++) {
		word |= ((uint32_t) set[i]) << (i<<3);
	}
	word &= 0xffffffff<<(idx&7);
	if (word) {
		return off + __builtin_ctz(word);
	}
	if (nBytes > 4) {
		off += 32;
		return off + bitset_ctz(set+4, nBits - (32 - (idx&7)));
	}
	return -1;
}
