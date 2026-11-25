/* Production storage code, real files, deterministic NVS and capacity faults. */
#include "apps/home/home_profile.h"
#include "esp_spiffs.h"
#include "esp_partition.h"
#include "nvs.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char stored[13], pending[13];
static bool has_order, fail_commit;
static char avatar_stored[2], avatar_pending[2];
static bool avatar_saved, avatar_dirty, cards_dirty;
static size_t free_bytes = 2 * 1024 * 1024;
esp_err_t nvs_open(const char *ns,int mode,nvs_handle_t *h) { (void)ns;*h=mode;strcpy(pending,stored);strcpy(avatar_pending,avatar_stored);avatar_dirty=cards_dirty=false;return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_get_str(nvs_handle_t h,const char *key,char *out,size_t *size) {
    (void)h;if(!strcmp(key,"portrait_v2")){if(!avatar_saved)return ESP_ERR_NOT_FOUND;assert(*size>=2);strcpy(out,avatar_stored);*size=2;return ESP_OK;}
    if(strcmp(key,"cards_v1")||!has_order)return ESP_ERR_NOT_FOUND;
    if(*size<strlen(stored)+1)return ESP_ERR_INVALID_SIZE;
    strcpy(out,stored);*size=strlen(stored)+1;return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t h,const char *key,const char *value) {
    (void)h;if(!strcmp(key,"portrait_v2")){assert(strlen(value)==1);strcpy(avatar_pending,value);avatar_dirty=true;return ESP_OK;}
    assert(!strcmp(key,"cards_v1"));assert(strlen(value)<sizeof(pending));strcpy(pending,value);cards_dirty=true;return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h;if(fail_commit)return ESP_FAIL;if(cards_dirty){strcpy(stored,pending);has_order=true;}if(avatar_dirty){strcpy(avatar_stored,avatar_pending);avatar_saved=true;}return ESP_OK; }
esp_err_t esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t *c) { assert(c->max_files==4);return ESP_OK; }
void vTaskDelay(unsigned ticks) { (void)ticks; }
const esp_partition_t *esp_partition_find_first(int type,int subtype,const char *label) { (void)type;(void)subtype;(void)label;assert(!"mounted storage must not be scanned");return NULL; }
esp_err_t esp_partition_read(const esp_partition_t *p,size_t offset,void *out,size_t size) { (void)p;(void)offset;(void)out;(void)size;assert(!"mounted storage must not be scanned");return ESP_FAIL; }
esp_err_t esp_spiffs_info(const char *label,size_t *total,size_t *used) { assert(!strcmp(label,"storage"));*total=2*1024*1024;*used=*total-free_bytes;return ESP_OK; }

static unsigned char jpeg[HOME_SHARE_MAX_BYTES];static size_t jpeg_size;
static void upload(unsigned slot) {
    for(size_t i=0;i<jpeg_size;i+=97){size_t n=jpeg_size-i;if(n>97)n=97;
        assert(home_profile_write_card_chunk(slot,jpeg+i,n,i==0,i+n==jpeg_size)==ESP_OK);}
}
static void exact_file(const char *path) {
    FILE *f=fopen(path,"rb");assert(f);unsigned char data[HOME_SHARE_MAX_BYTES];size_t n=fread(data,1,sizeof(data),f);assert(feof(f));fclose(f);assert(n==jpeg_size&&!memcmp(data,jpeg,n));
}
static void portraits(void) {
    uint8_t legacy[HOME_AVATAR_LEGACY_BYTES], tones[HOME_AVATAR_BYTES], color[HOME_AVATAR_COLOR_BYTES], readback[HOME_AVATAR_MAX_BYTES];
    memset(legacy,0xa5,sizeof(legacy));
    FILE *f=fopen(SPIFFS_BASE "/portrait.bin","wb");assert(f);
    assert(fwrite(legacy,1,sizeof(legacy),f)==sizeof(legacy));fclose(f);
    assert(home_profile_has_avatar());assert(home_profile_read_avatar(readback,sizeof(readback))==ESP_OK);
    for(size_t i=0;i<HOME_PORTRAIT_W*HOME_PORTRAIT_H;i++)
        assert(home_portrait_index(readback,i)==((legacy[i/8]&(128>>(i%8)))?3:0));
    assert(home_profile_read_avatar(readback,sizeof(readback)-1)==ESP_ERR_INVALID_ARG);
    for(size_t i=0;i<sizeof(tones);i++)tones[i]=(uint8_t)i;
    home_portrait_init_header(tones,3);
    assert(home_profile_write_avatar_chunk(tones,97,true,false,sizeof(tones))==ESP_OK);
    home_profile_abort_avatar_upload();assert(home_profile_read_avatar(readback,sizeof(readback))==ESP_OK);
    assert(readback[HOME_PORTRAIT_HEADER_BYTES]==0xcc);
    fail_commit=true;
    assert(home_profile_write_avatar_chunk(tones,sizeof(tones),true,true,sizeof(tones))==ESP_FAIL);
    assert(home_profile_read_avatar(readback,sizeof(readback))==ESP_OK&&readback[HOME_PORTRAIT_HEADER_BYTES]==0xcc);fail_commit=false;
    for(size_t i=0;i<sizeof(tones);i+=97){size_t n=sizeof(tones)-i;if(n>97)n=97;
        assert(home_profile_write_avatar_chunk(tones+i,n,i==0,i+n==sizeof(tones),sizeof(tones))==ESP_OK);}
    assert(home_profile_read_avatar(readback,sizeof(readback))==ESP_OK&&!memcmp(tones,readback,sizeof(tones)));
    home_profile_init();assert(home_profile_has_avatar());
    assert(home_profile_read_avatar(readback,sizeof(readback))==ESP_OK&&!memcmp(tones,readback,sizeof(tones)));
    assert(home_profile_write_avatar_chunk(tones,12,true,true,sizeof(tones))==ESP_ERR_INVALID_SIZE);
    assert(home_profile_write_avatar_chunk(tones,sizeof(tones),true,false,sizeof(tones))==ESP_OK);
    assert(home_profile_write_avatar_chunk(tones,1,false,true,sizeof(tones))==ESP_ERR_INVALID_SIZE);
    assert(home_profile_read_avatar(readback,sizeof(readback))==ESP_OK&&!memcmp(tones,readback,sizeof(tones)));
    assert(home_profile_write_avatar_chunk(legacy,sizeof(legacy),true,true,sizeof(legacy))==ESP_OK);
    home_profile_init();assert(home_profile_read_avatar(readback,sizeof(readback))==ESP_OK&&readback[HOME_PORTRAIT_HEADER_BYTES]==0xcc);
    uint8_t v2[HOME_AVATAR_V2_BYTES];memset(v2,0x1b,sizeof(v2));
    assert(home_profile_write_avatar_chunk(v2,sizeof(v2),true,true,sizeof(v2))==ESP_OK);
    assert(home_profile_read_avatar(readback,sizeof(readback))==ESP_OK&&readback[4]==0);
    assert(!memcmp(readback+HOME_PORTRAIT_HEADER_BYTES,v2,sizeof(v2)));
    for(size_t i=0;i<4;i++)assert(home_portrait_index(readback,i)==i);
    tones[4]=4;assert(home_profile_write_avatar_chunk(tones,sizeof(tones),true,true,sizeof(tones))==ESP_ERR_INVALID_ARG);
    assert(home_profile_read_avatar(readback,sizeof(readback))==ESP_OK&&readback[4]==0);
    home_portrait_init_header(tones,2);tones[6]=1;
    assert(home_profile_write_avatar_chunk(tones,sizeof(tones),true,true,sizeof(tones))==ESP_ERR_INVALID_ARG);
    home_portrait_init_header(tones,2);memset(tones+HOME_PORTRAIT_HEADER_BYTES,0,HOME_AVATAR_V2_BYTES);
    memset(tones+HOME_PORTRAIT_HEADER_BYTES+HOME_AVATAR_V2_BYTES,0xff,HOME_PORTRAIT_MASK_BYTES);
    assert(home_portrait_index(tones,0)==1);tones[HOME_PORTRAIT_HEADER_BYTES+HOME_AVATAR_V2_BYTES]=0x7f;
    assert(home_portrait_index(tones,0)==0&&home_portrait_index(tones,1)==1);

    for(size_t i=0;i<sizeof(color);i++)color[i]=(uint8_t)(i*7);
    home_portrait_init_color_header(color,UI_THEME_COUNT,100);
    memset(color+HOME_AVATAR_INDEX_OFFSET,0,HOME_AVATAR_COLOR_BYTES-HOME_AVATAR_INDEX_OFFSET);
    color[HOME_AVATAR_INDEX_OFFSET]=0x21;
    assert(home_portrait_upload_size(HOME_AVATAR_COLOR_BYTES)&&home_portrait_upload_size(HOME_AVATAR_MAX_BYTES));
    assert(home_profile_write_avatar_chunk(color,sizeof(color),true,true,sizeof(color))==ESP_OK);
    assert(home_profile_read_avatar(readback,sizeof(readback))==ESP_OK&&!memcmp(color,readback,sizeof(color)));
    assert(home_portrait_is_color(readback)&&home_portrait_valid_header(readback));
    assert(home_portrait_color_index(readback,0)==2&&home_portrait_color_index(readback,1)==1);
    home_portrait_init_color_header(color,UI_THEME_COUNT+1,50);
    assert(home_profile_write_avatar_chunk(color,sizeof(color),true,true,sizeof(color))==ESP_ERR_INVALID_ARG);
    home_portrait_init_color_header(color,3,101);
    assert(home_profile_write_avatar_chunk(color,sizeof(color),true,true,sizeof(color))==ESP_ERR_INVALID_ARG);
    home_portrait_init_color_header(color,3,50);color[7]=1;
    assert(home_profile_write_avatar_chunk(color,sizeof(color),true,true,sizeof(color))==ESP_ERR_INVALID_ARG);
    // Reject format/size mismatches: PT35 header in PT36 payload and vice versa.
    home_portrait_init_header(color,3);
    assert(home_profile_write_avatar_chunk(color,sizeof(color),true,true,sizeof(color))==ESP_ERR_INVALID_ARG);
    home_portrait_init_color_header(tones,3,50);
    assert(home_profile_write_avatar_chunk(tones,sizeof(tones),true,true,sizeof(tones))==ESP_ERR_INVALID_ARG);
    assert(home_profile_read_avatar(readback,sizeof(readback))==ESP_OK&&readback[4]==UI_THEME_COUNT&&readback[5]==100);

    uint8_t rgb565[HOME_AVATAR_RGB565_BYTES], rgb565_readback[HOME_AVATAR_RGB565_BYTES], rgb565_pixels[HOME_PORTRAIT_RGB565_BYTES];
    for(size_t i=0;i<sizeof(rgb565);i++)rgb565[i]=(uint8_t)((i*13+5)&0xff);
    home_portrait_init_rgb565_header(rgb565,100);
    memset(rgb565+HOME_AVATAR_RGB565_MASK_OFFSET,0xff,HOME_PORTRAIT_MASK_BYTES);
    rgb565[HOME_AVATAR_RGB565_MASK_OFFSET]=0x7f; /* pixel 0 masked out, pixels 1..7 visible */
    assert(home_portrait_upload_size(HOME_AVATAR_RGB565_BYTES));
    assert(home_profile_write_avatar_chunk(rgb565,sizeof(rgb565),true,true,sizeof(rgb565))==ESP_OK);
    assert(home_profile_avatar_is_rgb565());
    assert(home_profile_read_avatar(readback,sizeof(readback))==ESP_ERR_INVALID_SIZE);
    assert(home_profile_read_avatar(rgb565_readback,sizeof(rgb565_readback))==ESP_OK&&!memcmp(rgb565,rgb565_readback,sizeof(rgb565)));
    assert(home_profile_read_avatar_rgb565(rgb565_pixels,sizeof(rgb565_pixels),0x16171a)==ESP_OK);
    /* 0x16171a -> RGB565 (2<<11)|(5<<5)|3 = 0x10a3 (lo=0xa3, hi=0x10) */
    assert(rgb565_pixels[0]==0xa3&&rgb565_pixels[1]==0x10);
    assert(rgb565_pixels[2]==rgb565[HOME_PORTRAIT_HEADER_BYTES+2]&&rgb565_pixels[3]==rgb565[HOME_PORTRAIT_HEADER_BYTES+3]);
    assert(home_profile_read_avatar_rgb565(rgb565_pixels,sizeof(rgb565_pixels)-1,0)==ESP_ERR_INVALID_ARG);
    home_portrait_init_rgb565_header(rgb565,100);rgb565[4]=1;
    assert(home_profile_write_avatar_chunk(rgb565,sizeof(rgb565),true,true,sizeof(rgb565))==ESP_ERR_INVALID_ARG);
    home_portrait_init_rgb565_header(rgb565,101);
    assert(home_profile_write_avatar_chunk(rgb565,sizeof(rgb565),true,true,sizeof(rgb565))==ESP_ERR_INVALID_ARG);
    home_portrait_init_rgb565_header(rgb565,50);rgb565[6]=1;
    assert(home_profile_write_avatar_chunk(rgb565,sizeof(rgb565),true,true,sizeof(rgb565))==ESP_ERR_INVALID_ARG);
    home_portrait_init_color_header(rgb565,0,50);
    assert(home_profile_write_avatar_chunk(rgb565,sizeof(rgb565),true,true,sizeof(rgb565))==ESP_ERR_INVALID_ARG);
    home_portrait_init_rgb565_header(color,50);
    assert(home_profile_write_avatar_chunk(color,sizeof(color),true,true,sizeof(color))==ESP_ERR_INVALID_ARG);
    home_profile_init();
    assert(home_profile_avatar_is_rgb565()&&home_profile_read_avatar(rgb565_readback,sizeof(rgb565_readback))==ESP_OK&&rgb565_readback[3]=='7'&&rgb565_readback[5]==100);
    puts("Portrait storage: legacy formats, PT36 palette/indices, PT37 RGB565/mask, masks, malformed headers, abort, failed commit and reboot: PASS");
}
int main(int argc,char **argv) {
    assert(argc==2);assert(mkdir(SPIFFS_BASE,0700)==0);
    portraits();
    FILE *f=fopen(argv[1],"rb");assert(f);jpeg_size=fread(jpeg,1,sizeof(jpeg),f);assert(feof(f));fclose(f);assert(jpeg_size>24);
    char legacy[HOME_SHARE_PATH_MAX];snprintf(legacy,sizeof(legacy),SPIFFS_BASE "/share.jpg");
    f=fopen(legacy,"wb");assert(f);assert(fwrite(jpeg,1,jpeg_size,f)==jpeg_size);fclose(f);
    home_profile_init();home_share_gallery_t g;assert(home_profile_gallery_get(&g)==ESP_OK&&g.count==1&&g.slots[0]==0);
    char original[HOME_SHARE_PATH_MAX];assert(home_profile_share_path_at(0,original,sizeof(original)));assert(!strcmp(original,legacy));
    // Upload cancellation and bad JPEG must leave the old card untouched.
    assert(home_profile_write_card_chunk(0,jpeg,20,true,false)==ESP_OK);home_profile_abort_card_upload();exact_file(original);
    assert(home_profile_write_card_chunk(0,(const uint8_t *)"broken",6,true,true)==ESP_ERR_INVALID_ARG);exact_file(original);
    free_bytes=HOME_SHARE_STORAGE_RESERVE+HOME_SHARE_MAX_BYTES-1;
    assert(home_profile_write_card_chunk(0,jpeg,jpeg_size,true,true)==ESP_ERR_NO_MEM);exact_file(original);free_bytes=2*1024*1024;
    assert(home_profile_write_card_chunk(6,jpeg,jpeg_size,true,true)==ESP_ERR_INVALID_ARG);
    assert(home_profile_write_card_chunk(0,jpeg,10,true,false)==ESP_OK);
    assert(home_profile_write_card_chunk(0,jpeg,HOME_SHARE_MAX_BYTES,false,true)==ESP_ERR_INVALID_SIZE);exact_file(original);
    fail_commit=true;assert(home_profile_write_card_chunk(0,jpeg,jpeg_size,true,true)==ESP_FAIL);exact_file(original);fail_commit=false;
    for(unsigned i=1;i<6;i++)upload(i);
    assert(home_profile_gallery_get(&g)==ESP_OK&&g.count==6);
    for(unsigned i=0;i<6;i++){assert(g.slots[i]==i&&g.bytes[i]==jpeg_size);char path[HOME_SHARE_PATH_MAX];assert(home_profile_share_path_at(i,path,sizeof(path)));exact_file(path);}
    uint8_t reverse[]={5,4,3,2,1,0};assert(home_profile_gallery_order(reverse,6)==ESP_OK);
    home_profile_init();assert(home_profile_gallery_get(&g)==ESP_OK&&g.count==6&&g.slots[0]==5&&g.slots[5]==0);
    uint8_t duplicate[]={5,5};assert(home_profile_gallery_order(duplicate,2)==ESP_ERR_INVALID_ARG);
    uint8_t unknown[]={6};assert(home_profile_gallery_order(unknown,1)==ESP_ERR_INVALID_ARG);
    fail_commit=true;assert(home_profile_gallery_order(NULL,0)==ESP_FAIL);assert(home_profile_gallery_get(&g)==ESP_OK&&g.count==6);fail_commit=false;
    // Inactive-bank replacement commits only when complete; a reboot ignores it.
    assert(home_profile_write_card_chunk(5,jpeg,10,true,false)==ESP_OK);
    home_profile_init();assert(home_profile_share_path_at(0,original,sizeof(original)));exact_file(original);home_profile_abort_card_upload();
    upload(5);assert(home_profile_share_path_at(0,original,sizeof(original)));exact_file(original);
    // Invalid geometry and missing EOI never replace the saved image.
    unsigned char tail=jpeg[jpeg_size-1];jpeg[jpeg_size-1]=0;
    assert(home_profile_write_card_chunk(5,jpeg,jpeg_size,true,true)==ESP_ERR_INVALID_ARG);jpeg[jpeg_size-1]=tail;exact_file(original);
    uint8_t retained[]={0,5};assert(home_profile_gallery_order(retained,2)==ESP_OK);
    assert(home_profile_gallery_get(&g)==ESP_OK&&g.count==2&&g.slots[0]==0&&g.slots[1]==5);
    uint8_t deleted[]={1};assert(home_profile_gallery_order(deleted,1)==ESP_ERR_NOT_FOUND);
    assert(home_profile_gallery_order(NULL,0)==ESP_OK);
    home_profile_init();assert(home_profile_gallery_get(&g)==ESP_OK&&g.count==0&&!home_profile_has_share_image());
    assert(access(legacy,F_OK)!=0);upload(3);assert(home_profile_gallery_get(&g)==ESP_OK&&g.count==1&&g.slots[0]==3);
    // Corrupt persisted order fails closed, instead of deleting/replacing cards.
    strcpy(stored,"3a3b");home_profile_init();assert(home_profile_gallery_get(&g)==ESP_ERR_INVALID_STATE);
    puts("Card storage: legacy migration, 6 streamed images, capacity reserve, malformed JPEG, size/slot bounds, aborted uploads, failed NVS commit, reboot, reorder/delete and corrupt metadata: PASS");
}
