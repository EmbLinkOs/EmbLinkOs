/* Host-side offsets for the read-only native UI inspector. No guest code. */
#include <stdio.h>
#include <stddef.h>
#include "instance.h"
#include "scene.h"
#define FIELD(type,field,name) printf("\"%s\":%zu,",name,offsetof(struct type,field))
int main(void) {
    printf("{\"instance_size\":%zu,\"node_size\":%zu,",sizeof(struct instance),sizeof(struct scene_node));
    FIELD(instance,parent,"i_parent");FIELD(instance,explicit_key,"i_key");FIELD(instance,kind,"i_kind");FIELD(instance,scene_node,"i_scene");FIELD(instance,shadow.has_text,"i_has_text");FIELD(instance,shadow.text,"i_text");
    FIELD(scene_node,self,"n_self");FIELD(scene_node,parent,"n_parent");FIELD(scene_node,tx,"n_tx");FIELD(scene_node,ty,"n_ty");
    FIELD(scene_node,sx,"n_sx");FIELD(scene_node,sy,"n_sy");FIELD(scene_node,width,"n_width");FIELD(scene_node,height,"n_height");
    FIELD(scene_node,layer,"n_layer");FIELD(scene_node,opacity,"n_opacity");FIELD(scene_node,clip_children,"n_clip");FIELD(scene_node,hit_passthrough,"n_pass");
    printf("\"page_size\":%d}",SCENE_PAGE_SIZE);
    return 0;
}
