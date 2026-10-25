#include "console.h"
#include "boot/boot_protocol.h"
#include "crypto/sha256.h"
#include "update_boot.h"

static struct update_state selected;
static unsigned selected_slot,selected_flags;
static int have_selected;
static void bytes_copy(void *d,const void *s,UINTN n) {uint8_t *to=d;const uint8_t *from=s;while(n--)*to++=*from++;}
static int bytes_equal(const void *a,const void *b,UINTN n) {const uint8_t *x=a,*y=b;while(n--)if(*x++!=*y++)return 0;return 1;}
static void wide(const char *s,CHAR16 *out) {while(*s)*out++=(uint8_t)*s++;*out=0;}
static EFI_FILE_PROTOCOL *root_open(EFI_HANDLE image,EFI_LOADED_IMAGE_PROTOCOL **loaded) {
    EFI_GUID image_guid=EFI_LOADED_IMAGE_PROTOCOL_GUID,fs_guid=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=0;EFI_FILE_PROTOCOL *root=0;
    if(EFI_ERROR(ST->BootServices->HandleProtocol(image,&image_guid,(void **)loaded)) || !*loaded)return 0;
    if(EFI_ERROR(ST->BootServices->HandleProtocol((*loaded)->DeviceHandle,&fs_guid,(void **)&fs)) || !fs)return 0;
    return EFI_ERROR(fs->OpenVolume(fs,&root))?0:root;
}
static int read_exact(EFI_FILE_PROTOCOL *root,const char *path,void *out,UINTN bytes) {
    CHAR16 name[80];wide(path,name);EFI_FILE_PROTOCOL *f=0;
    if(EFI_ERROR(root->Open(root,&f,name,1,0)))return 0;
    UINTN n=bytes;
    int ok=!EFI_ERROR(f->Read(f,&n,out)) && n==bytes;
    f->Close(f);return ok;
}
/* Journal pages live in reserved ESP sectors, outside FAT metadata. Each
 * record occupies a separate 4-KiB page; updating one cannot rewrite the other
 * record's directory/FAT sector. Buffers satisfy firmware DMA alignment. */
static EFI_BLOCK_IO_PROTOCOL *control;
static uint8_t *control_page;
static int control_open(EFI_LOADED_IMAGE_PROTOCOL *loaded) {
    EFI_GUID guid=EFI_BLOCK_IO_PROTOCOL_GUID;EFI_PHYSICAL_ADDRESS page=0;
    if(EFI_ERROR(ST->BootServices->HandleProtocol(loaded->DeviceHandle,&guid,(void **)&control)) ||
       !control || !control->Media || !control->Media->MediaPresent || !control->Media->LogicalPartition ||
       (control->Media->BlockSize!=512 && control->Media->BlockSize!=4096) || control->Media->IoAlign>4096 ||
       EFI_ERROR(ST->BootServices->AllocatePages(AllocateAnyPages,EfiLoaderData,1,&page)))return 0;
    control_page=(uint8_t *)page;
    if(EFI_ERROR(control->ReadBlocks(control,control->Media->MediaId,0,4096,control_page)))con_die("cannot read boot control device");
    if(!bytes_equal(control_page+3,"EMBUPD1 ",8))return 0;
    if(control_page[11]!=0 || control_page[12]!=2 || (control_page[14]|((unsigned)control_page[15]<<8))<128)
        con_die("invalid managed ESP geometry");
    return 1;
}
static int control_read(unsigned offset,uint8_t out[512]) {
    if(EFI_ERROR(control->ReadBlocks(control,control->Media->MediaId,offset/control->Media->BlockSize,4096,control_page)))return 0;
    bytes_copy(out,control_page,512);return 1;
}
static int write_record(int index,const struct update_state *state) {
    uint8_t expected[512];if(update_state_encode(state,expected) || control->Media->ReadOnly)return 0;
    for(unsigned i=0;i<4096;i++)control_page[i]=0;
    bytes_copy(control_page,expected,512);
    uint64_t offset=index?UPDATE_CONTROL1_OFFSET:UPDATE_CONTROL0_OFFSET;
    if(EFI_ERROR(control->WriteBlocks(control,control->Media->MediaId,offset/control->Media->BlockSize,4096,control_page)) ||
       EFI_ERROR(control->FlushBlocks(control)))return 0;
    uint8_t got[512];return control_read(offset,got) && bytes_equal(expected,got,512);
}
static void *file_device_path(EFI_LOADED_IMAGE_PROTOCOL *loaded,const char *filename) {
    EFI_GUID guid=EFI_DEVICE_PATH_PROTOCOL_GUID;uint8_t *parent=0;
    if(EFI_ERROR(ST->BootServices->HandleProtocol(loaded->DeviceHandle,&guid,(void **)&parent)) || !parent)return 0;
    UINTN prefix=0;
    while(prefix<1024) {
        unsigned len=parent[prefix+2]|((unsigned)parent[prefix+3]<<8);
        if(len<4 || len>1024-prefix)return 0;
        if(parent[prefix]==0x7f && parent[prefix+1]==0xff)break;
        prefix+=len;
    }
    if(prefix>=1024)return 0;
    UINTN chars=0;while(filename[chars])chars++;
    UINTN node=4+(chars+1)*2,total=prefix+node+4;uint8_t *path=0;
    if(EFI_ERROR(ST->BootServices->AllocatePool(EfiLoaderData,total,(void **)&path)))return 0;
    bytes_copy(path,parent,prefix);path[prefix]=4;path[prefix+1]=4;
    path[prefix+2]=(uint8_t)node;path[prefix+3]=(uint8_t)(node>>8);
    for(UINTN i=0;i<=chars;i++){path[prefix+4+i*2]=(uint8_t)filename[i];path[prefix+5+i*2]=0;}
    uint8_t *end=path+prefix+node;end[0]=0x7f;end[1]=0xff;end[2]=4;end[3]=0;return path;
}
static int chain(EFI_HANDLE parent,EFI_LOADED_IMAGE_PROTOCOL *loaded,EFI_FILE_PROTOCOL *root,unsigned mode,unsigned slot) {
    const char *name=slot?"\\EFI\\EMBLINK\\SLOT1.EFI":"\\EFI\\EMBLINK\\SLOT0.EFI";
    UINTN size=selected.slot[slot].efi_bytes;uint8_t *image=0;uint8_t hash[32];
    if(!size || size>UPDATE_EFI_CAPACITY || EFI_ERROR(ST->BootServices->AllocatePool(EfiLoaderData,size,(void **)&image)))return 0;
    int ok=read_exact(root,name,image,size);
    if(ok){sha256(image,size,hash);ok=bytes_equal(hash,selected.slot[slot].efi_sha256,32);}
    void *path=ok?file_device_path(loaded,name):0;EFI_HANDLE child=0;
    if(!path || EFI_ERROR(ST->BootServices->LoadImage(0,parent,path,image,size,&child)))ok=0;
    ST->BootServices->FreePool(image);if(path)ST->BootServices->FreePool(path);
    if(!ok)return 0;
    EFI_GUID guid=EFI_LOADED_IMAGE_PROTOCOL_GUID;EFI_LOADED_IMAGE_PROTOCOL *info=0;
    struct update_boot_options *options=0;
    if(EFI_ERROR(ST->BootServices->HandleProtocol(child,&guid,(void **)&info)) || !info ||
       EFI_ERROR(ST->BootServices->AllocatePool(EfiLoaderData,sizeof *options,(void **)&options)))return 0;
    options->magic=UPDATE_BOOT_OPTIONS_MAGIC;options->size=sizeof *options;options->mode=mode;
    options->slot=slot;options->flags=selected_flags;
    if(update_state_encode(&selected,options->state))return 0;
    info->LoadOptionsSize=sizeof *options;info->LoadOptions=options;
    con_print("update: starting OS slot ");con_print(slot?"B\n":"A\n");
    EFI_STATUS status=ST->BootServices->StartImage(child,0,0);
    con_print("update: selected EFI returned ");con_printhex(status);con_print("\n");return 0;
}
int update_boot_child(EFI_HANDLE image,unsigned *mode) {
    EFI_GUID guid=EFI_LOADED_IMAGE_PROTOCOL_GUID;EFI_LOADED_IMAGE_PROTOCOL *loaded=0;
    if(EFI_ERROR(ST->BootServices->HandleProtocol(image,&guid,(void **)&loaded)) || !loaded ||
       loaded->LoadOptionsSize!=sizeof(struct update_boot_options) || !loaded->LoadOptions)return 0;
    struct update_boot_options *o=loaded->LoadOptions;
    if(o->magic!=UPDATE_BOOT_OPTIONS_MAGIC || o->size!=sizeof *o || o->slot>1 || o->mode>BOOT_MODE_LIVE || (o->flags&~14u) ||
       update_state_decode(o->state,sizeof o->state,&selected))con_die("invalid update boot options");
    if(o->slot!=selected.active && (o->slot!=selected.pending || selected.attempts))con_die("unselected OS slot");
    selected_slot=o->slot;selected_flags=o->flags;have_selected=1;*mode=o->mode;return 1;
}
void update_boot_dispatch(EFI_HANDLE image,unsigned mode) {
    EFI_LOADED_IMAGE_PROTOCOL *loaded=0;EFI_FILE_PROTOCOL *root=root_open(image,&loaded);if(!root)return;
    if(!control_open(loaded)){root->Close(root);return;}
    uint8_t records[2][512]={{0}};
    (void)control_read(UPDATE_CONTROL0_OFFSET,records[0]);
    (void)control_read(UPDATE_CONTROL1_OFFSET,records[1]);
    int record=update_state_choose(records[0],records[1],&selected);
    if(record<0) {
        uint8_t factory[512];
        if(!control_read(UPDATE_FACTORY_OFFSET,factory) || update_state_decode(factory,512,&selected)) {
            root->Close(root);con_die("update state damaged; use independent recovery media");return;
        }
        mode=BOOT_MODE_RECOVERY;selected_flags=12;
        con_print("update: journal damaged; factory recovery selected\n");
    }
    unsigned slot=selected.active;int changed=0;
    if(mode==BOOT_MODE_NORMAL || mode==BOOT_MODE_LIVE) {
        struct update_state next=selected;
        if(update_state_boot(&next,&slot,&changed))con_die("update journal sequence exhausted");
        if(changed) {
            if(record<0 || !write_record(1-record,&next)) {
                slot=selected.active;con_print("update: cannot persist trial; using confirmed OS\n");
            } else {selected=next;record=1-record;}
        }
    }
    selected_slot=slot;have_selected=1;
    if(selected.flags&UPDATE_REVERTED)selected_flags|=4;
    if(slot==selected.pending)selected_flags|=2;
    if(chain(image,loaded,root,mode,slot))__builtin_unreachable();
    if(slot!=selected.active) {
        struct update_state next=selected;
        if(!update_state_reject(&next,UPDATE_IMAGE_FAILED) && record>=0 && write_record(1-record,&next))selected=next;
        selected_slot=selected.active;selected_flags=4;
        con_print("update: trial image unavailable; reverting\n");
        chain(image,loaded,root,mode,selected.active);
    }
    root->Close(root);selected_slot=selected.active;selected_flags=12;
    con_print("update: boot images unavailable; embedded recovery only\n");
}
void update_boot_fill(struct boot_protocol *bp) {
    if(!have_selected)return;
    bytes_copy(bp->update_root_guid,selected.slot[selected_slot].root_guid,16);
    bytes_copy(bp->update_data_guid,selected.data_guid,16);
    bp->update_revision=selected.revision;bp->update_release=selected.slot[selected_slot].release;
    bp->update_flags=1|selected_flags;bp->update_slot=(uint8_t)selected_slot;
    if(selected_flags&8)bp->boot_mode=BOOT_MODE_RECOVERY;
}
