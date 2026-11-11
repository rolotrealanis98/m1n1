/* SPDX-License-Identifier: MIT */
#include "hidm_service.h"
static int lookup(hm_service *s,hm_u32 id) {
    hm_u32 i;for(i=0;i<s->count;i++) if(s->interfaces[i].info.id==id) return (int)i;
    return -1;
}
/* Short-item walk: does the descriptor declare a Report ID (global tag 8)? */
static int has_ids(const hm_u8 *d,hm_u32 n) {
    hm_u32 p=0,k;
    while(p<n) {
        hm_u8 b=d[p];
        if(b==0xfe) {if(n-p<3) return 0;p+=3u+d[p+1];continue;}
        k=b&3u;if(k==3)k=4;
        if((b&0xfcu)==0x84u) return 1;
        p+=1u+k;
    }
    return 0;
}
static void write_slot(hm_service *s,const hm_slot *slot) {
    hm_copy(s->data+HM_RING_OFF+(s->head%HM_SLOTS)*HM_SLOT_BYTES,slot,sizeof(*slot));s->head++;
}
/* Publish coalesced reports, oldest first, while the ring has room. */
static void flush(hm_service *s) {
    hm_u32 n=0,k;
    while(n<s->pending && (hm_u32)(s->head-s->tail)<HM_SLOTS) write_slot(s,&s->pend[n++]);
    if(!n) return;
    for(k=n;k<s->pending;k++) hm_copy(&s->pend[k-n],&s->pend[k],sizeof(hm_slot));
    s->pending-=n;
}
static void drop_pending(hm_service *s,hm_u32 id) {
    hm_u32 k,j=0;
    for(k=0;k<s->pending;k++) if(s->pend[k].iface!=id) hm_copy(&s->pend[j++],&s->pend[k],sizeof(hm_slot));
    s->pending=j;
}
hm_u32 hm_service_init(hm_service *s,const hm_backend_interface *v,hm_u32 n,hm_backend_report cb,void *ctx) {
    hm_u32 i,j;hm_zero(s,sizeof(*s));
    if(!v || !n || n>HM_MAX_IFACES) return HM_RANGE;
    for(i=0;i<n;i++) {
        if(v[i].info.id>255 || v[i].info.flags!=HM_PRESENT || v[i].info.reserved || v[i].info.reserved2 ||
           !v[i].descriptor || !v[i].info.descriptor_length || v[i].info.descriptor_length>HM_MAX_DESC) return HM_RANGE;
        for(j=0;j<i;j++) if(v[i].info.id==v[j].info.id) return HM_BAD_REQUEST;
    }
    hm_copy(s->interfaces,v,n*sizeof(*v));s->count=n;s->report=cb;s->backend=ctx;
    for(i=0;i<n;i++) if(has_ids(v[i].descriptor,v[i].info.descriptor_length)) s->ids|=1u<<i;
    return HM_OK;
}
static void command(hm_service *s,hm_u32 seq) {
    hm_request q;hm_response r;int i;hm_u32 n;
    /* Copy all attacker-controlled request bytes before validation. */
    hm_copy(&q,s->data+HM_REQ_OFF,sizeof(q));hm_zero(&r,sizeof(r));r.seq=seq;r.status=HM_BAD_REQUEST;
    if(!seq || q.seq!=seq || q.reserved || q.report_id>255 || q.length>HM_PAYLOAD) goto done;
    if(q.abi!=HM_VERSION) {r.status=HM_BAD_VERSION;goto done;}
    if(q.op==HM_HELLO) {
        if(q.iface || q.offset || q.length || q.report_id) goto done;
        s->ready=1;s->active=0;s->enabled=0;s->head=s->tail=0;s->overflow=0;s->last_seq=0;s->pending=0;
        r.status=HM_OK;goto done;
    }
    if(!s->ready) {r.status=HM_NOT_READY;goto done;}
    if(seq==s->last_seq) goto done; /* do not replay modifying commands */
    if(q.op==HM_BYE) {
        if(q.iface||q.offset||q.length||q.report_id) goto done;
        s->ready=s->active=s->enabled=0;s->pending=0;r.status=HM_OK;goto done;
    }
    if(q.op==HM_ENUM) {
        if(q.offset||q.length||q.report_id) goto done;
        if(q.iface>=s->count) {r.status=HM_NOT_FOUND;goto done;}
        hm_copy(r.payload,&s->interfaces[q.iface].info,sizeof(hm_info));r.length=sizeof(hm_info);r.status=HM_OK;goto done;
    }
    i=lookup(s,q.iface);if(i<0) {r.status=HM_NOT_FOUND;goto done;}
    if(q.op==HM_DESCRIPTOR) {
        n=s->interfaces[i].info.descriptor_length;
        if(q.report_id || !q.length || q.offset>n || q.length>n-q.offset) {r.status=HM_RANGE;goto done;}
        hm_copy(r.payload,s->interfaces[i].descriptor+q.offset,q.length);r.length=q.length;r.status=HM_OK;goto done;
    }
    if(q.offset) goto done;
    if(q.op==HM_ACTIVATE || q.op==HM_DEACTIVATE) {
        if(q.length||q.report_id) goto done;
        if(q.op==HM_ACTIVATE) s->active|=1u<<i;else {s->active&=~(1u<<i);drop_pending(s,q.iface);}
        r.status=HM_OK;goto done;
    }
    if(q.op!=HM_OUTPUT && q.op!=HM_GET_FEATURE && q.op!=HM_SET_FEATURE) goto done;
    if(!q.length) {r.status=HM_RANGE;goto done;}
    r.status=HM_DENIED;
    if(s->report) {
        n=q.length;
        r.status=s->report(s->backend,q.iface,q.op,q.report_id,q.payload,&n);
        if(r.status>HM_IO_ERROR || n>q.length) {r.status=HM_IO_ERROR;goto done;}
        if(r.status==HM_OK && q.op==HM_GET_FEATURE) {r.length=n;hm_copy(r.payload,q.payload,n);}
    }
done:
    s->last_seq=seq;hm_copy(s->data+HM_RESP_OFF,&r,sizeof(r));
}
int hm_service_irq(const hm_service *s) {return s->ready && s->enabled && s->head!=s->tail;}
hm_u32 hm_service_read(hm_service *s,hm_u32 page,hm_u32 off) {
    if(off%4 || off>HM_PAGE-4) return 0;
    if(page==1) return hm_le32(s->data+off);
    if(page!=0) return 0;
    switch(off) {
    case HM_R_MAGIC:return HM_MAGIC;case HM_R_VERSION:return HM_VERSION;
    case HM_R_READY:return s->ready;case HM_R_COUNT:return s->count;
    case HM_R_HEAD:return s->head;case HM_R_TAIL:return s->tail;
    case HM_R_OVERFLOW:return s->overflow;case HM_R_ENABLE:return s->enabled;
    case HM_R_IRQ:return s->ready && s->head!=s->tail;default:return 0;
    }
}
void hm_service_write(hm_service *s,hm_u32 page,hm_u32 off,hm_u32 v) {
    if(off%4 || off>HM_PAGE-4) return;
    if(page==1) {if(off<sizeof(hm_request)) hm_put32(s->data+off,v);return;}
    if(page!=0) return;
    switch(off) {
    case HM_R_ENABLE:s->enabled=v&1;break;
    case HM_R_TAIL: /* unsigned subtraction handles UINT32 rollover */
        if((hm_u32)(v-s->tail)<=(hm_u32)(s->head-s->tail)) {s->tail=v;flush(s);}
        break;
    case HM_R_DOORBELL:command(s,v);break;
    default:break;
    }
}
hm_u32 hm_service_push(hm_service *s,hm_u32 id,const hm_u8 *data,hm_u32 len,hm_u64 stamp) {
    int i=lookup(s,id);hm_slot slot;hm_u32 k;
    if(i<0) return HM_NOT_FOUND;
    if(!data || !len || len>HM_PAYLOAD) return HM_RANGE;
    if(!s->ready || !(s->active&(1u<<i))) return HM_NOT_READY;
    hm_zero(&slot,sizeof(slot));slot.iface=id;slot.length=len;slot.timestamp_ns=stamp;hm_copy(slot.payload,data,len);
    flush(s);
    if(!s->pending && (hm_u32)(s->head-s->tail)<HM_SLOTS) {write_slot(s,&slot);return HM_OK;}
    /* Ring full: keep the newest state per (interface, report ID). */
    s->overflow++;
    for(k=0;k<s->pending;k++)
        if(s->pend[k].iface==id && (!(s->ids&(1u<<i)) || s->pend[k].payload[0]==data[0])) {hm_copy(&s->pend[k],&slot,sizeof(slot));return HM_FULL;}
    if(s->pending<HM_COALESCE) hm_copy(&s->pend[s->pending++],&slot,sizeof(slot));
    return HM_FULL;
}
