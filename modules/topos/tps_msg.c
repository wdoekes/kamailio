/**
 * Copyright (C) 2016 Daniel-Constantin Mierla (asipto.com)
 *
 * This file is part of Kamailio, a free SIP server.
 *
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 *
 * This file is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

/*!
 * \file
 * \brief SIP-router topoh ::
 * \ingroup topoh
 * Module: \ref topoh
 */

#include <string.h>

#include "../../dprint.h"
#include "../../mem/mem.h"
#include "../../data_lump.h"
#include "../../forward.h"
#include "../../trim.h"
#include "../../msg_translator.h"
#include "../../parser/parse_rr.h"
#include "../../parser/parse_uri.h"
#include "../../parser/parse_param.h"
#include "../../parser/parse_from.h"
#include "../../parser/parse_to.h"
#include "../../parser/parse_via.h"
#include "../../parser/contact/parse_contact.h"
#include "../../parser/parse_refer_to.h"
#include "tps_msg.h"
#include "tps_storage.h"

extern int _tps_param_mask_callid;

/**
 *
 */
int tps_skip_rw(char *s, int len)
{
	while(len>0)
	{
		if(s[len-1]==' ' || s[len-1]=='\t' || s[len-1]=='\n' || s[len-1]=='\r'
				|| s[len-1]==',')
			len--;
		else return len;
	}
	return 0;
}

/**
 *
 */
struct via_param *tps_get_via_param(struct via_body *via, str *name)
{
	struct via_param *p;
	for(p=via->param_lst; p; p=p->next)
	{
		if(p->name.len==name->len
				&& strncasecmp(p->name.s, name->s, name->len)==0)
			return p;
	}
	return NULL;
}

/**
 *
 */
int tps_get_param_value(str *in, str *name, str *value)
{
	param_t* params = NULL;
	param_t* p = NULL;
	param_hooks_t phooks;
	if (parse_params(in, CLASS_ANY, &phooks, &params)<0)
		return -1;
	for (p = params; p; p=p->next)
	{
		if (p->name.len==name->len
				&& strncasecmp(p->name.s, name->s, name->len)==0)
		{
			*value = p->body;
			free_params(params);
			return 0;
		}
	}
	
	if(params) free_params(params);
	return 1;

}

/**
 *
 */
int tps_remove_headers(sip_msg_t *msg, uint32_t hdr)
{
	struct hdr_field *hf;
	struct lump* l;

	parse_headers(msg, HDR_EOH_F, 0);
	for (hf=msg->headers; hf; hf=hf->next) {
		if (hdr!=hf->type)
			continue;
		l=del_lump(msg, hf->name.s-msg->buf, hf->len, 0);
		if (l==0) {
			LM_ERR("no memory\n");
			return -1;
		}
	}
	return 0;
}

/**
 *
 */
int tps_add_headers(sip_msg_t *msg, str *hname, str *hbody, int hpos)
{
	struct lump* anchor;
	str hs;

	parse_headers(msg, HDR_EOH_F, 0);
	if(hpos == 0) { /* append */
		/* after last header */
		anchor = anchor_lump(msg, msg->unparsed - msg->buf, 0, 0);
	} else { /* insert */
		/* before first header */
		anchor = anchor_lump(msg, msg->headers->name.s - msg->buf, 0, 0);
	}

	if(anchor == 0) {
		LM_ERR("can't get anchor\n");
		return -1;
	}

	hs.len = hname->len + 2 + hbody->len;
	hs.s  = (char*)pkg_malloc(hs.len + 3);
	if (hs.s==NULL) {
		LM_ERR("no pkg memory left\n");
		return -1;
	}
	memcpy(hs.s, hname->s, hname->len);
	hs.s[hname->len] = ':';
	hs.s[hname->len+1] = ' ';
	memcpy(hs.s + hname->len + 2, hbody->s, hbody->len);

	/* add end of header if not present */
	if(hs.s[hname->len + 2 + hbody->len]!='\n') {
		hs.s[hname->len + 2 + hbody->len] = '\r';
		hs.s[hname->len + 2 + hbody->len+1] = '\n';
		hs.len += 2;
	}

	if (insert_new_lump_before(anchor, hs.s, hs.len, 0) == 0) {
		LM_ERR("can't insert lump\n");
		pkg_free(hs.s);
		return -1;
	}

	return 0;
}

/**
 *
 */
int tps_get_uri_param_value(str *uri, str *name, str *value)
{
	struct sip_uri puri;

	memset(value, 0, sizeof(str));
	if(parse_uri(uri->s, uri->len, &puri)<0)
		return -1;
	return tps_get_param_value(&puri.params, name, value);
}

/**
 *
 */
int tps_get_uri_type(str *uri, int *mode, str *value)
{
	struct sip_uri puri;
	int ret;
	str r2 = {"r2", 2};

	memset(value, 0, sizeof(str));
	*mode = 0;
	if(parse_uri(uri->s, uri->len, &puri)<0)
		return -1;

	LM_DBG("PARAMS [%.*s]\n", puri.params.len, puri.params.s);

	if(check_self(&puri.host, puri.port_no, 0)==1)
	{
		/* myself -- matched on all protos */
		ret = tps_get_param_value(&puri.params, &r2, value);
		if(ret<0)
			return -1;
		if(ret==1) /* not found */
			return 0; /* skip */
		LM_DBG("VALUE [%.*s]\n",
				value->len, value->s);
		if(value->len==2 && strncasecmp(value->s, "on", 2)==0)
			*mode = 1;
		memset(value, 0, sizeof(str));
		return 0; /* skip */
	}
	/* not myself & not mask ip */
	return 1; /* encode */
}

/**
 *
 */
char* tps_msg_update(sip_msg_t *msg, unsigned int *olen)
{
	struct dest_info dst;

	init_dest_info(&dst);
	dst.proto = PROTO_UDP;
	return build_req_buf_from_sip_req(msg,
			olen, &dst, BUILD_NO_LOCAL_VIA|BUILD_NO_VIA1_UPDATE);
}

/**
 *
 */
int tps_route_direction(sip_msg_t *msg)
{
	rr_t *rr;
	struct sip_uri puri;
	str ftn = {"ftag", 4};
	str ftv = {0, 0};

	if(get_from(msg)->tag_value.len<=0)
	{
		LM_ERR("failed to get from header tag\n");
		return -1;
	}
	if(msg->route==NULL)
	{
		LM_DBG("no route header - downstream\n");
		return 0;
	}
	if (parse_rr(msg->route) < 0) 
	{
		LM_ERR("failed to parse route header\n");
		return -1;
	}

	rr =(rr_t*)msg->route->parsed;

	if (parse_uri(rr->nameaddr.uri.s, rr->nameaddr.uri.len, &puri) < 0) {
		LM_ERR("failed to parse the first route URI\n");
		return -1;
	}
	if(tps_get_param_value(&puri.params, &ftn, &ftv)!=0)
		return 0;

	if(get_from(msg)->tag_value.len!=ftv.len
			|| strncmp(get_from(msg)->tag_value.s, ftv.s, ftv.len)!=0)
	{
		LM_DBG("ftag mismatch\n");
		return 1;
	}
	LM_DBG("ftag match\n");
	return 0;
}

/**
 *
 */
int tps_skip_msg(sip_msg_t *msg)
{
	if (msg->cseq==NULL || get_cseq(msg)==NULL) {
		LM_WARN("Invalid/Unparsed CSeq in message. Skipping.");
		return 1;
	}

	if((get_cseq(msg)->method_id)&(METHOD_REGISTER|METHOD_PUBLISH))
		return 1;

	return 0;
}

/**
 *
 */
int tps_request_received(sip_msg_t *msg, int dialog, int direction)
{
	if(dialog==0) {
		/* nothing to do for initial request */
		return 0;
	}
	return 0;
}

/**
 *
 */
int tps_response_received(sip_msg_t *msg)
{
	return 0;
}

/**
 *
 */
int tps_pack_request(sip_msg_t *msg, tps_data_t *ptsd)
{
	hdr_field_t *hdr;
	via_body_t *via;
	rr_t *rr;
	int i;
	int vlen;

	if(ptsd->cp==NULL) {
		ptsd->cp = ptsd->cbuf;
	}
	i = 0;
	for(hdr=msg->h_via1; hdr; hdr=next_sibling_hdr(hdr)) {
		for(via=(struct via_body*)hdr->parsed; via; via=via->next) {
			i++;
			vlen = tps_skip_rw(via->name.s, via->bsize);
			if(ptsd->cp + vlen + 2 >= ptsd->cbuf + TPS_DATA_SIZE) {
				LM_ERR("no more spage to pack via headers\n");
				return -1;
			}
			if(i>1) {
				*ptsd->cp = ',';
				ptsd->cp++;
				if(i>2) {
					ptsd->x_via2.len++;
				}
			}
			memcpy(ptsd->cp, via->name.s, vlen);
			if(i==1) {
				ptsd->x_via1.s = ptsd->cp;
				ptsd->x_via1.len = vlen;
				if(via->branch!=NULL) {
					ptsd->x_vbranch1.s = ptsd->x_via1.s + (via->branch->value.s - via->name.s);
					ptsd->x_vbranch1.len = via->branch->value.len;
				}
			} else {
				if(i==2) {
					ptsd->x_via2.s = ptsd->cp;
				}
				ptsd->x_via2.len += vlen;
			}
			ptsd->cp += vlen;
		}
	}
	LM_DBG("compacted headers - x_via1: [%.*s](%d) - x_via2: [%.*s](%d)"
			" - x_vbranch1: [%.*s](%d)\n",
			ptsd->x_via1.len, ZSW(ptsd->x_via1.s), ptsd->x_via1.len,
			ptsd->x_via2.len, ZSW(ptsd->x_via2.s), ptsd->x_via2.len,
			ptsd->x_vbranch1.len, ZSW(ptsd->x_vbranch1.s), ptsd->x_vbranch1.len);

	i = 0;
	ptsd->a_rr.len = 0;
	for(hdr=msg->record_route; hdr; hdr=next_sibling_hdr(hdr)) {
		if (parse_rr(hdr) < 0) {
			LM_ERR("failed to parse RR\n");
			return -1;
		}

		for(rr =(rr_t*)hdr->parsed; rr; rr=rr->next) {
			i++;
			if(ptsd->cp + rr->nameaddr.uri.len + 4 >= ptsd->cbuf + TPS_DATA_SIZE) {
				LM_ERR("no more spage to pack rr headers\n");
				return -1;
			}
			if(i>1) {
				*ptsd->cp = ',';
				ptsd->cp++;
				ptsd->a_rr.len++;
			}
			*ptsd->cp = '<';
			if(i==1) {
				ptsd->a_rr.s = ptsd->cp;
			}
			ptsd->cp++;
			ptsd->a_rr.len++;

			memcpy(ptsd->cp, rr->nameaddr.uri.s, rr->nameaddr.uri.len);
			if(i==1) {
				ptsd->bs_contact.s = ptsd->cp;
				ptsd->bs_contact.len = rr->nameaddr.uri.len;
				if(strnstr(ptsd->bs_contact.s, ";r2=on",
							ptsd->bs_contact.len)==NULL) {
					LM_DBG("single record routing by proxy\n");
					ptsd->as_contact.s = ptsd->cp;
					ptsd->as_contact.len = rr->nameaddr.uri.len;
				}
			} else {
				if(i==2 && ptsd->as_contact.len==0) {
					LM_DBG("double record routing by proxy\n");
					ptsd->as_contact.s = ptsd->cp;
					ptsd->as_contact.len = rr->nameaddr.uri.len;
				}
			}
			ptsd->a_rr.len += rr->nameaddr.uri.len;
			ptsd->cp += rr->nameaddr.uri.len;
			*ptsd->cp = '>';
			ptsd->cp++;
			ptsd->a_rr.len++;
		}

	}
	LM_DBG("compacted headers - a_rr: [%.*s](%d) - b_rr: [%.*s](%d)\n",
			ptsd->a_rr.len, ZSW(ptsd->a_rr.s), ptsd->a_rr.len,
			ptsd->b_rr.len, ZSW(ptsd->b_rr.s), ptsd->b_rr.len);
	LM_DBG("compacted headers - as_contact: [%.*s](%d) - bs_contact: [%.*s](%d)\n",
			ptsd->as_contact.len, ZSW(ptsd->as_contact.s), ptsd->as_contact.len,
			ptsd->bs_contact.len, ZSW(ptsd->bs_contact.s), ptsd->bs_contact.len);
	return 0;
}

/**
 *
 */
int tps_reinsert_via(sip_msg_t *msg, tps_data_t *ptsd, str *hbody)
{
	str hname = str_init("Via");
	
	if(tps_add_headers(msg, &hname, hbody, 1)<0) {
		return -1;
	}

	return 0;
}

/**
 *
 */
int tps_reinsert_contact(sip_msg_t *msg, tps_data_t *ptsd, str *hbody)
{
	str hname = str_init("Contact");
	
	if(tps_add_headers(msg, &hname, hbody, 0)<0) {
		return -1;
	}

	return 0;
}


/**
 *
 */
int tps_request_sent(sip_msg_t *msg, int dialog, int direction, int local)
{
	tps_data_t mtsd;
	tps_data_t stsd;
	tps_data_t *ptsd;
	str lkey;

	memset(&mtsd, 0, sizeof(tps_data_t));
	memset(&stsd, 0, sizeof(tps_data_t));
	ptsd = &mtsd;

	if(tps_pack_request(msg, &mtsd)<0) {
		LM_ERR("failed to extract and pack the headers\n");
		return -1;
	}

	if(direction==TPS_DIR_DOWNSTREAM) {
		lkey = get_from(msg)->tag_value;
	} else {
		lkey = get_to(msg)->tag_value;
	}
	tps_storage_lock_get(&lkey);
	if(dialog==0) {
		if(tps_storage_record(msg, ptsd)<0) {
			goto error;
		}
	}

	/* local generated requests */
	if(local) {
		/* ACK and CANCEL go downstream */
		if(get_cseq(msg)->method_id==METHOD_ACK
				|| get_cseq(msg)->method_id==METHOD_CANCEL
				|| local==2) {
			// th_mask_callid(&msg);
			goto done;
		} else {
			/* should be for upstream */
			goto done;
		}
	}

	tps_remove_headers(msg, HDR_RECORDROUTE_T);
	tps_remove_headers(msg, HDR_CONTACT_T);
	tps_remove_headers(msg, HDR_VIA_T);

	tps_reinsert_via(msg, ptsd, &ptsd->x_via1);
	if(direction==TPS_DIR_UPSTREAM) {
		tps_reinsert_contact(msg, ptsd, &ptsd->as_contact);
	} else {
		tps_reinsert_contact(msg, ptsd, &ptsd->bs_contact);
	}

done:
	tps_storage_lock_release(&lkey);
	return 0;

error:
	tps_storage_lock_release(&lkey);
	return -1;
}

/**
 *
 */
int tps_response_sent(sip_msg_t *msg)
{
	return 0;
}
