/* SPDX-License-Identifier: MIT */
#include "hidm_script.h"
/* Original synthetic HID fixtures, authored from HID item encodings.
 * Keyboard: report 1 input (9 bytes), LEDs (2 bytes); report 2 feature (2).
 * These bytes are NOT Apple's descriptor and are not copied from Linux. */
static const hm_u8 keyboard[]={
 0x05,1,0x09,6,0xa1,1,0x85,1,0x05,7,0x19,0xe0,0x29,0xe7,
 0x15,0,0x25,1,0x75,1,0x95,8,0x81,2,0x75,8,0x95,1,0x81,1,
 0x05,8,0x19,1,0x29,5,0x75,1,0x95,5,0x91,2,0x75,3,0x95,1,0x91,1,
 0x05,7,0x19,0,0x29,0x65,0x15,0,0x25,0x65,0x75,8,0x95,6,0x81,0,
 0x06,0,0xff,0x09,1,0x85,2,0x15,0,0x26,0xff,0,0x75,8,0x95,1,0xb1,2,0xc0
};
static const hm_u8 pointer[]={
 0x05,1,0x09,2,0xa1,1,0x85,1,0x09,1,0xa1,0,
 0x05,9,0x19,1,0x29,3,0x15,0,0x25,1,0x95,3,0x75,1,0x81,2,
 0x95,1,0x75,5,0x81,1,0x05,1,0x09,0x30,0x09,0x31,
 0x15,0x81,0x25,0x7f,0x75,8,0x95,2,0x81,6,0xc0,0xc0
};
static const hm_u8 keys[][9]={
 {1,0,0,4,0,0,0,0,0},{1,0,0,0,0,0,0,0,0}, /* a */
 {1,0,0,0x16,0,0,0,0,0},{1,0,0,0,0,0,0,0,0}, /* s */
 {1,2,0,0,0,0,0,0,0},{1,2,0,4,0,0,0,0,0}, /* shift, A */
 {1,2,0,0,0,0,0,0,0},{1,0,0,0,0,0,0,0,0},
 {1,0,0,4,0x16,7,0,0,0},{1,0,0,0,0,0,0,0,0}, /* a+s+d */
 {1,0,0,0x28,0,0,0,0,0},{1,0,0,0,0,0,0,0,0}
};
static hm_u32 report(void *ctx,hm_u32 id,hm_u32 op,hm_u32 rid,hm_u8 *p,hm_u32 *n) {
    hm_script *b=ctx;
    if(id!=2||*n!=2)return HM_DENIED;
    if(op==HM_OUTPUT&&rid==1&&p[0]==1) {b->leds=p[1]&31;return HM_OK;}
    if(rid==2&&op==HM_SET_FEATURE&&p[0]==2) {b->feature=p[1];return HM_OK;}
    if(rid==2&&op==HM_GET_FEATURE) {p[0]=2;p[1]=b->feature;return HM_OK;}
    return HM_DENIED;
}
hm_u32 hm_script_init(hm_service *s,hm_script *b,int mouse) {
    hm_backend_interface v[2];hm_zero(v,sizeof(v));hm_zero(b,sizeof(*b));b->pointer=mouse;
    v[0].info.id=2;v[0].info.flags=HM_PRESENT;v[0].info.vendor=0x05ac;
    v[0].info.product=0xf001;v[0].info.version=1;v[0].info.descriptor_length=sizeof(keyboard);v[0].descriptor=keyboard;
    v[1]=v[0];v[1].info.id=1;v[1].info.product=0xf002;v[1].info.descriptor_length=sizeof(pointer);v[1].descriptor=pointer;
    return hm_service_init(s,v,mouse?2:1,report,b);
}
hm_u32 hm_script_tick(hm_service *s,hm_script *b,hm_u64 time) {
    hm_u32 rc;
    if(b->step>=sizeof(keys)/sizeof(keys[0]))return HM_NOT_FOUND;
    rc=hm_service_push(s,2,keys[b->step],sizeof(keys[0]),time);
    if(rc==HM_OK) {
        b->step++;
        if(b->pointer) {const hm_u8 p[4]={1,0,4,0};(void)hm_service_push(s,1,p,sizeof(p),time);}
    }
    return rc;
}
