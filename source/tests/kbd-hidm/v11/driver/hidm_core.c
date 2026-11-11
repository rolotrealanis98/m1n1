/* SPDX-License-Identifier: MIT */
#include "hidm_core.h"
hm_u32 hm_copy_output(void *dst,hm_u32 cap,const void *src,hm_u32 n,hm_u32 *done) {
    *done=0;if(!dst || !src || cap<n) return HM_RANGE;hm_copy(dst,src,n);*done=n;return HM_OK;
}
/* Structural/sizing validation, not a replacement for HIDCLASS's parser.
 * Global PUSH/POP and report IDs are respected. No allocation or recursion. */
hm_u32 hm_parse_descriptor(const hm_u8 *d,hm_u32 len,hm_layout *l) {return hm_parse_descriptor_ex(d,len,l,HM_PAYLOAD);}
/* v1.1: feature reports (kind 2) may reach max_feature bytes when the service
 * advertises HM_CAP_BIG_FEATURE; input and output reports stay <= HM_PAYLOAD. */
hm_u32 hm_parse_descriptor_ex(const hm_u8 *d,hm_u32 len,hm_layout *l,hm_u32 max_feature) {
    struct globals {hm_u32 size,count,id;} g={0,0,0},stack[16];
    hm_u32 p=0,depth=0,sp=0,kind,rid;
    hm_u32 bits[3][256];hm_zero(bits,sizeof(bits));hm_zero(l,sizeof(*l));
    if(!d || !len || len>HM_MAX_DESC || max_feature<HM_PAYLOAD || max_feature>HM_MAX_FEATURE) return HM_RANGE;
    while(p<len) {
        hm_u32 b=d[p++],n=b&3,v=0,k,tag=b>>4,type=(b>>2)&3;
        if(b==0xfe) {if(len-p<2 || d[p]>len-p-2) return HM_RANGE;p+=2+d[p];continue;}
        if(n==3)n=4;if(n>len-p)return HM_RANGE;
        for(k=0;k<n;k++)v|=(hm_u32)d[p++]<<(8*k);
        if(type==1) {
            if(tag==7)g.size=v;else if(tag==9)g.count=v;
            else if(tag==8) {if(!v||v>255)return HM_RANGE;g.id=v;l->ids=1;}
            else if(tag==10) {if(sp==16)return HM_RANGE;stack[sp++]=g;}
            else if(tag==11) {if(!sp)return HM_RANGE;g=stack[--sp];}
        } else if(type==0) {
            if(tag==10) {if(++depth>32)return HM_RANGE;}
            else if(tag==12) {if(!depth)return HM_RANGE;depth--;}
            else if(tag==8||tag==9||tag==11) {
                hm_u32 total,limit;kind=tag==8?0:tag==9?1:2;limit=kind==2?max_feature:HM_PAYLOAD;
                if(!depth || !g.size || !g.count || g.size>32 || g.count>limit*8/g.size)return HM_RANGE;
                total=g.size*g.count;
                if(bits[kind][g.id]>limit*8-total)return HM_RANGE;
                bits[kind][g.id]+=total;
            }
        }
    }
    if(depth||sp)return HM_RANGE;
    for(kind=0;kind<3;kind++)for(rid=0;rid<256;rid++)if(bits[kind][rid]) {
        hm_u32 n=(bits[kind][rid]+7)/8+l->ids;
        if((l->ids&&!rid)||n>(kind==2?max_feature:HM_PAYLOAD))return HM_RANGE;
        l->length[kind][rid]=(hm_u16)n;
    }
    return HM_OK;
}
int hm_client_index(const hm_client *c,hm_u32 id) {
    hm_u32 i;for(i=0;i<c->count;i++)if(c->devices[i].info.id==id)return (int)i;return -1;
}
hm_u32 hm_client_command(hm_client *c,hm_request *q,hm_response *r) {
    hm_u32 off;q->seq=++c->seq;if(!q->seq)q->seq=++c->seq;q->abi=HM_VERSION;
    if(q->length>HM_PAYLOAD)return HM_RANGE;
    for(off=0;off<sizeof(*q);off+=4)c->io.write(c->io.ctx,1,HM_REQ_OFF+off,hm_le32((hm_u8 *)q+off));
    c->io.write(c->io.ctx,0,HM_R_DOORBELL,q->seq);
    for(off=0;off<sizeof(*r);off+=4)hm_put32((hm_u8 *)r+off,c->io.read(c->io.ctx,1,HM_RESP_OFF+off));
    if(r->seq!=q->seq || r->length>HM_PAYLOAD || r->reserved || r->status>HM_IO_ERROR)return HM_IO_ERROR;
    for(off=0;off<4;off++)if(r->pad[off])return HM_IO_ERROR;
    return r->status;
}
hm_u32 hm_client_open(hm_client *c,hm_io io) {
    hm_request q;hm_response r;hm_u32 i,j,n,off,rc;
    hm_zero(c,sizeof(*c));c->io=io;
    if(!io.read||!io.write)return HM_BAD_REQUEST;
    if(io.read(io.ctx,0,HM_R_MAGIC)!=HM_MAGIC || io.read(io.ctx,0,HM_R_VERSION)!=HM_VERSION)return HM_BAD_VERSION;
    hm_zero(&q,sizeof(q));q.op=HM_HELLO;
    rc=hm_client_command(c,&q,&r);if(rc)return rc;
    if(r.length || io.read(io.ctx,0,HM_R_READY)!=1 || io.read(io.ctx,0,HM_R_HEAD)!=0 || io.read(io.ctx,0,HM_R_TAIL)!=0)return HM_IO_ERROR;
    n=io.read(io.ctx,0,HM_R_COUNT);if(!n||n>HM_MAX_IFACES)return HM_RANGE;
    c->caps=io.read(io.ctx,0,HM_R_CAPS)&HM_CAP_BIG_FEATURE; /* 0 on a v1.0 service */
    for(i=0;i<n;i++) {
        hm_device *d=&c->devices[i];hm_zero(&q,sizeof(q));q.op=HM_ENUM;q.iface=i;
        rc=hm_client_command(c,&q,&r);if(rc)return rc;if(r.length!=sizeof(hm_info))return HM_RANGE;
        hm_copy(&d->info,r.payload,sizeof(hm_info));
        if(d->info.id>255 || d->info.flags!=HM_PRESENT || d->info.reserved || d->info.reserved2 ||
           !d->info.descriptor_length || d->info.descriptor_length>HM_MAX_DESC)return HM_RANGE;
        for(j=0;j<i;j++)if(c->devices[j].info.id==d->info.id)return HM_RANGE;
        for(off=0;off<d->info.descriptor_length;off+=q.length) {
            hm_zero(&q,sizeof(q));q.op=HM_DESCRIPTOR;q.iface=d->info.id;q.offset=off;
            q.length=d->info.descriptor_length-off;if(q.length>HM_PAYLOAD)q.length=HM_PAYLOAD;
            rc=hm_client_command(c,&q,&r);if(rc)return rc;if(r.length!=q.length)return HM_RANGE;
            hm_copy(d->descriptor+off,r.payload,r.length);
        }
        rc=hm_parse_descriptor_ex(d->descriptor,d->info.descriptor_length,&d->layout,
                                  c->caps?HM_MAX_FEATURE:HM_PAYLOAD);if(rc)return rc;
    }
    c->count=n;return HM_OK;
}
hm_u32 hm_client_pop(hm_client *c,hm_slot *slot) {
    hm_u32 h=c->io.read(c->io.ctx,0,HM_R_HEAD),off,rid;int i;
    if(h==c->tail)return HM_NOT_READY;
    if((hm_u32)(h-c->tail)>HM_SLOTS)return HM_RANGE;
    off=HM_RING_OFF+(c->tail%HM_SLOTS)*HM_SLOT_BYTES;
    for(rid=0;rid<sizeof(*slot);rid+=4)hm_put32((hm_u8 *)slot+rid,c->io.read(c->io.ctx,1,off+rid));
    /* Release the slot only after a full local copy; malformed slots are dropped. */
    c->tail++;c->io.write(c->io.ctx,0,HM_R_TAIL,c->tail);
    i=hm_client_index(c,slot->iface);
    if(i<0||!slot->length||slot->length>HM_PAYLOAD)return HM_RANGE;
    rid=c->devices[i].layout.ids?slot->payload[0]:0;
    if(slot->length!=c->devices[i].layout.length[0][rid])return HM_RANGE;
    return HM_OK;
}
hm_u32 hm_client_report(hm_client *c,hm_u32 id,hm_u32 op,hm_u32 rid,hm_u8 *buf,hm_u32 *len) {
    hm_request q;hm_response r;hm_u32 want,rc;int i=hm_client_index(c,id);
    if(i<0)return HM_NOT_FOUND;
    if(rid>255||!buf||!len||*len>HM_MAX_FEATURE || (op!=HM_OUTPUT&&op!=HM_GET_FEATURE&&op!=HM_SET_FEATURE))return HM_RANGE;
    want=c->devices[i].layout.length[op==HM_OUTPUT?1:2][rid];
    if(!want || (op==HM_GET_FEATURE?*len<want:*len!=want))return HM_RANGE;
    if(c->devices[i].layout.ids && buf[0]!=rid)return HM_RANGE;
    if(want>HM_PAYLOAD) {
        /* v1.1 large feature: GET only, in HM_PAYLOAD chunks; SET/OUTPUT refused. */
        hm_u32 off,chunk;
        if(op!=HM_GET_FEATURE || !c->caps)return HM_DENIED;
        for(off=0;off<want;off+=chunk) {
            chunk=want-off>HM_PAYLOAD?HM_PAYLOAD:want-off;
            hm_zero(&q,sizeof(q));q.op=op;q.iface=id;q.offset=off;q.length=chunk;q.report_id=rid;
            rc=hm_client_command(c,&q,&r);if(rc)return rc;
            if(r.length!=chunk || (!off&&c->devices[i].layout.ids&&r.payload[0]!=rid))return HM_IO_ERROR;
            hm_copy(buf+off,r.payload,chunk);
        }
        *len=want;return HM_OK;
    }
    hm_zero(&q,sizeof(q));q.op=op;q.iface=id;q.length=want;q.report_id=rid;
    if(op!=HM_GET_FEATURE)hm_copy(q.payload,buf,want);
    rc=hm_client_command(c,&q,&r);if(rc)return rc;
    if(op==HM_GET_FEATURE) {
        if(r.length!=want || (c->devices[i].layout.ids&&r.payload[0]!=rid))return HM_IO_ERROR;
        hm_copy(buf,r.payload,want);
    } else if(r.length) return HM_IO_ERROR;
    *len=want;return HM_OK;
}
