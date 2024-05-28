#ifndef BITSET_H
#define BITSET_H

#include <stdint.h>

void bitset_set(uint8_t* set, int idx);
void bitset_clear(uint8_t* set, int idx);

int bitset_is_set(uint8_t* set, int idx);
int bitset_is_clear(uint8_t* set, int idx);

int bitset_next_set(uint8_t* set, int size, int idx);
int bitset_next_clear(uint8_t* set, int size, int idx);

int bitset_ctz(uint8_t* set, int size);
int bitset_next_set(uint8_t* set, int size, int idx);

//BITSET_H
#endif
