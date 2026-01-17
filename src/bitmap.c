#include "bitmap.h"
#include <stdio.h>

int set_bitmap(const unsigned short position, char *bitmap, int bitmap_size)
{
    if (NULL == bitmap || position >= bitmap_size * sizeof(char) * 8)
    {
        fprintf(stderr, "set_bitmap: The bitmap is NULL or the position is out of bound. position:{%d}, bitmap_size:{%d}\n", position, bitmap_size);
        return -1;
    }

    unsigned short array_index = position / sizeof(char);
    unsigned short offset = position % sizeof(char);

    char mask = 0x80 >> offset;
    bitmap[array_index] |= mask;

    return position;
}

int get_bitmap(const unsigned short position, char *bitmap, int bitmap_size)
{
    if (NULL == bitmap || position >= bitmap_size * (sizeof(char) * 8))
    {
        fprintf(stderr, "get_bitmap: The bitmap is NULL or the position is out of bound.\n");
        return -1;
    }

    unsigned short array_index = position / sizeof(char);
    unsigned short offset = position % sizeof(char);

    char mask = 0x80 >> offset;
    if ((bitmap[array_index] & mask) == 0)
    {
        return 0;
    }
    else
    {
        return 1;
    }
}