/**
 * Set the value of the specified position of the bitmap to 1.
 * 
 * params:
 *      bitmap: The bitmap
 *      bitmap_size: The size of the bitmap(How many chars in the bitmap array)
 * 
 * return:
 *      Return positive int if it is successfully set to the value the specified postion;
 *      Return negetive int if it is failed to set the value.
 */
int set_bitmap(const unsigned short position, char *bitmap, int bitmap_size);


/**
 * Get the vaule of the specified position of the bitmap.
 * 
 * params:
 *      position:   The position we want to get data from the bitmap.
 *      bitmap:     The bitmap.
 *      bitmap_size:    The size of the bitmap.
 * 
 * return:
 *      Return 1 if the value of the specified position is 1.
 *      Return 0 if the value of the specified position is 0.
 *      Return -1 if there is error.
 */
int get_bitmap(const unsigned short position, char *bitmap, int bitmap_size);
