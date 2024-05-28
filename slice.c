#include <stdlib.h>

#include "slice.h"

slice_t slice_new(int capacity, int size) {
	slice_t s;
	s.ptr = malloc(capacity * size);
	s.len = 0;
	s.cap = capacity;
	s.size = size;
	return s;
}

slice_t slice_copy(slice_t s) {
	slice_t new_s = slice_new(s.cap, s.size);
	memcpy(new_s.ptr, s.ptr, s.len*s.size);
	new_s.len = s.len;
	return new_s;
}

void slice_free(slice_t s) {
	free(s.ptr);
}

slice_t slice_append(slice_t s, void* value) {
	if (s.cap == 0) {
		s = slice_new(8, s.size);
	} else if (s.len >= s.cap) {
		s.cap += (s.cap > 1024) ? s.cap / 4 : s.cap;
		s.ptr = realloc(s.ptr, s.cap * s.size);
	}
#ifdef DEBUG_FLAG
	printf("copying %d bytes\n", s.size);
#endif
	memcpy(s.ptr + (s.len++ * s.size), value, s.size);
	return s;
}
