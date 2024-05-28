#ifndef SLICE_H
#define SLICE_H

typedef struct {
	void* ptr;
	int len, cap;
	int size;
} slice_t;

slice_t slice_new(int capacity, int size);
slice_t slice_copy(slice_t s);
void slice_free(slice_t s);

slice_t slice_append(slice_t s, void* value);

#endif
