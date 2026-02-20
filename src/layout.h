#include <stdint.h>

__attribute__((export_name("init_screen"))) int32_t init_screen(int32_t w,
                                                                int32_t h);
__attribute__((export_name("resize_screen"))) int32_t resize_screen(int32_t w,
                                                                      int32_t h);
__attribute__((export_name("move_handle"))) int32_t move_handle(int32_t handle_index,
                                                                  int32_t x,
                                                                  int32_t y);
__attribute__((export_name("move_corner"))) int32_t
move_corner(int32_t area_index, int32_t corner_index, int32_t x, int32_t y);
__attribute__((export_name("set_area_content"))) int32_t
set_area_content(int32_t area_index, int32_t content_id);
__attribute__((export_name("set_handle_content"))) int32_t
set_handle_content(int32_t handle_index, int32_t content_id);
__attribute__((export_name("get_data_ptr"))) int32_t get_data_ptr(void);
