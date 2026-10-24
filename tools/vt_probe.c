#include <stdio.h>
#include <stddef.h>
#include "vt.h"
int main(void) {
#define O(type,field) printf("\"" #type "_" #field "\":%zu,",offsetof(struct type,field))
    printf("{");
    O(vt,primary);O(vt,alternate);O(vt,cols);O(vt,rows);O(vt,alt);O(vt,history_count);O(vt,history_head);O(vt,history);
    O(vt_screen,order);O(vt_screen,cursor);O(vt_screen,lines);
    O(vt_cursor,x);O(vt_cursor,y);
    O(vt_line,cells);O(vt_cell,rune);O(vt_cell,combining);O(vt_cell,width);O(vt_cell,fg);O(vt_cell,bg);
    printf("\"cell_size\":%zu,\"line_size\":%zu,\"screen_size\":%zu}",sizeof(struct vt_cell),sizeof(struct vt_line),sizeof(struct vt_screen));
}
