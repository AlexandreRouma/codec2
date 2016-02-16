/*---------------------------------------------------------------------------*\

  FILE........: fsk.c
  AUTHOR......: Brady O'Brien
  DATE CREATED: 11 February 2016

  Framer and deframer for VHF FreeDV modes 'A' and 'B'
  Currently designed for-
  * 40ms ota modem frames
  * 40ms Codec2 1300 frames
  * 52 bits of Codec2 per frame
  * 16 bits of unique word per frame
  * 28 'spare' bits per frame
  *  - 4 spare bits at front and end of frame (8 total) for padding
  *  - 20 'protocol' bits, either for higher layers of 'protocol' or
  *  - 18 'protocol' bits and 2 vericode sidechannel bits

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2016 David Rowe

  All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License version 2.1, as
  published by the Free Software Foundation.  This program is
  distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or
  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
  License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with this program; if not, see <http://www.gnu.org/licenses/>.
*/


#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "freedv_vhf_framing.h"

/* The UW of the VHF type A frame */
static const uint8_t A_uw[] =      {0,1,1,0,0,1,1,1,
                                    1,0,1,0,1,1,0,1};
/* Blank VHF type A frame */
static const uint8_t A_blank[] =   {1,0,1,0,0,1,1,1, /* Padding[0:3] Proto[0:3]   */
                                    1,0,1,0,0,1,1,1, /* Proto[4:11]               */
                                    0,0,0,0,0,0,0,0, /* Voice[0:7]                */
                                    0,0,0,0,0,0,0,0, /* Voice[8:15]               */
                                    0,0,0,0,0,0,0,0, /* Voice[16:23]              */
                                    0,1,1,0,0,1,1,1, /* UW[0:7]                   */
                                    1,0,1,0,1,1,0,1, /* UW[8:15]                  */
                                    0,0,0,0,0,0,0,0, /* Voice[24:31]              */
                                    0,0,0,0,0,0,0,0, /* Voice[32:39]              */
                                    0,0,0,0,0,0,0,0, /* Voice[40:47]              */
                                    0,0,0,0,0,0,1,0, /* Voice[48:51] Proto[12:15] */
                                    0,1,1,1,0,0,1,0};/* Proto[16:19] Padding[4:7] */

/* States */
#define ST_NOSYNC 0 /* Not synchronized */
#define ST_SYNC 1   /* Synchronized */

/* Get a single bit out of an MSB-first packed byte array */
#define UNPACK_BIT_MSBFIRST(bytes,bitidx) ((bytes)[(bitidx)>>3]>>(7-((bitidx)&0x7)))&0x1


/* Place codec and other bits into a frame */
void fvhff_frame_bits(  int frame_type,
                        uint8_t bits_out[],
                        uint8_t codec2_in[],
                        uint8_t proto_in[],
                        uint8_t vc_in[]){
    int i,ibit;
    if(frame_type == FREEDV_VHF_FRAME_A){
        /* Fill out frame with blank frame prototype */
        for(i=0; i<96; i++)
            bits_out[i] = A_blank[i];
        
        /* Fill in varicode bits, if present */
        if(vc_in!=NULL){
            bits_out[90] = vc_in[0];
            bits_out[91] = vc_in[1];
        }
        
        /* Fill in protocol bits, if present */
        if(proto_in!=NULL){
            ibit = 0;
            /* First half of protocol bits */
            /* Extract and place in frame, MSB first */
            for(i=4 ; i<16; i++){
                bits_out[i] = UNPACK_BIT_MSBFIRST(proto_in,ibit);
                ibit++;
            }
            /* Last set of protocol bits */
            for(i=84; i<92; i++){
                bits_out[i] = UNPACK_BIT_MSBFIRST(proto_in,ibit);
                ibit++;
            }
        }
        
        /* Fill in codec2 bits, present or not */
        ibit = 0;
        for(i=16; i<40; i++){   /* First half */
            bits_out[i] = UNPACK_BIT_MSBFIRST(codec2_in,ibit);
            ibit++;
        }
        for(i=56; i<84; i++){   /* Second half */
            bits_out[i] = UNPACK_BIT_MSBFIRST(codec2_in,ibit);
            ibit++;
        }
    }
}

/* Init and allocate memory for a freedv-vhf framer/deframer */
struct freedv_vhf_deframer * fvhff_create_deframer(uint8_t frame_type){
    struct freedv_vhf_deframer * deframer;
    uint8_t * bits;
    /* It's a Type A frame */
    if(frame_type == FREEDV_VHF_FRAME_A){
        /* Allocate memory for the thing */
        deframer = malloc(sizeof(struct freedv_vhf_deframer));
        if(deframer == NULL){
            return NULL;
        }
        
        /* Allocate the bit buffer */
        bits = malloc(sizeof(uint8_t)*96);
        if(bits == NULL){
            free(deframer);
            return NULL;
        }
        
        deframer->bits = bits;
        deframer->ftype = frame_type;
        deframer->state = ST_NOSYNC;
        deframer->bitptr = 0;
        deframer->last_uw = 0;
        deframer->miss_cnt = 0;
        deframer->frame_size = 96;
        
        return deframer;
    }
    return NULL;
}

void fvhff_destroy_deframer(struct freedv_vhf_deframer * def){
    free(def->bits);
    free(def);
}

int fvhff_synchronized(struct freedv_vhf_deframer * def){
    return (def->state) == ST_SYNC;
}

/* See if the UW is where it should be, to within a tolerance, in a bit buffer */
static int fvhff_match_uw(struct freedv_vhf_deframer * def,int tol){
    uint8_t * bits  = def->bits;
    int frame_type  = def->ftype;
    int bitptr      = def->bitptr;
    int frame_size  = def->frame_size;
    int iuw,ibit;
    const uint8_t * uw;
    int uw_len;
    int uw_offset;
    int diff = 0;
    
    /* Set up parameters for the standard type of frame */
    if(frame_type == FREEDV_VHF_FRAME_A){
        uw = A_uw;
        uw_len = 16;
        uw_offset = 40;
    } else {
        return 0;
    }
    
    /* Start bit pointer where UW should be */
    ibit = bitptr + uw_offset;
    if(ibit >= frame_size) ibit -= frame_size;
    /* Walk through and match bits in frame with bits of UW */
    for(iuw=0; iuw<uw_len; iuw++){
        if(bits[ibit] != uw[iuw]) diff++;
        ibit++;
        if(ibit >= frame_size) ibit = 0;
    }
    return diff <= tol;
}

static void fvhff_extract_frame(struct freedv_vhf_deframer * def,uint8_t codec2_out[],uint8_t proto_out[],uint8_t vc_out[]){
    uint8_t * bits  = def->bits;
    int frame_type  = def->ftype;
    int bitptr      = def->bitptr;
    int frame_size  = def->frame_size;
    int iframe,ibit;
    
    if(frame_type == FREEDV_VHF_FRAME_A){
        /* Extract codec2 bits */
        memset(codec2_out,0,7);
        ibit = 0;
        /* Extract and pack first half, MSB first */
        iframe = bitptr+16;
        if(iframe >= frame_size) iframe-=frame_size;
        for(;ibit<24;ibit++){
            codec2_out[ibit>>3] |= (bits[iframe]&0x1)<<(7-(ibit&0x7));
            iframe++;
            if(iframe >= frame_size) iframe=0;
        }
        
        /* Extract and pack last half, MSB first */
        iframe = bitptr+56;
        if(iframe >= frame_size) iframe-=frame_size;
        for(;ibit<52;ibit++){
            codec2_out[ibit>>3] |= (bits[iframe]&0x1)<<(7-(ibit&0x7));
            iframe++;
            if(iframe >= frame_size) iframe=0;
        }
        /* Extract varicode bits, if wanted */
        if(vc_out!=NULL){
            iframe = bitptr+90;
            if(iframe >= frame_size) iframe-=frame_size;
            vc_out[0] = bits[iframe];
            iframe++;
            vc_out[1] = bits[iframe];
        }
        /* Extract protocol bits, if proto is passed through */
        if(proto_out!=NULL){
            /* Clear protocol bit array */
            memset(proto_out,0,3);
            ibit = 0;
            /* Extract and pack first half, MSB first */
            iframe = bitptr+4;
            if(iframe >= frame_size) iframe-=frame_size;
            for(;ibit<12;ibit++){
                proto_out[ibit>>3] |= (bits[iframe]&0x1)<<(7-(ibit&0x7));
                iframe++;
                if(iframe >= frame_size) iframe=0;
            }
            
            /* Extract and pack last half, MSB first */
            iframe = bitptr+84;
            if(iframe >= frame_size) iframe-=frame_size;
            for(;ibit<20;ibit++){
                proto_out[ibit>>3] |= (bits[iframe]&0x1)<<(7-(ibit&0x7));
                iframe++;
                if(iframe >= frame_size) iframe=0;
            }
        }

    }
}

/*
 * Try to find the UW and extract codec/proto/vc bits in def->frame_size bits 
 */
int fvhff_deframe_bits(struct freedv_vhf_deframer * def,uint8_t codec2_out[],uint8_t proto_out[],uint8_t vc_out[],uint8_t bits_in[]){
    uint8_t * bits  = def->bits;
    int frame_type  = def->ftype;
    int state       = def->state;
    int bitptr      = def->bitptr;
    int last_uw     = def->last_uw;
    int miss_cnt    = def->miss_cnt;
    int frame_size  = def->frame_size;
    int i;
    int uw_first_tol;   
    int uw_sync_tol;
    int miss_tol;
    int extracted_frame = 0;
    
    /* Possibly set up frame-specific params here */
    if(frame_type == FREEDV_VHF_FRAME_A){
        uw_first_tol = 2;   /* The UW bit-error tolerance for the first frame */
        uw_sync_tol = 1;    /* The UW bit error tolerance for frames after sync */
        miss_tol = 2;       /* How many UWs may be missed before going into the de-synced state */
    }else{
        return 0;
    }
    for(i=0; i<frame_size; i++){
        /* Put a bit in the buffer */
        bits[bitptr] = bits_in[i];
        bitptr++;
        if(bitptr >= frame_size) bitptr = 0;
        def->bitptr = bitptr;
        /* Enter state machine */
        if(state==ST_SYNC){
            /* Already synchronized, just wait till UW is back where it should be */
            last_uw++;
            /* UW should be here. We're sunk, so deframe anyway */
            if(last_uw == frame_size){
                last_uw = 0;
                extracted_frame = 1;
                
                if(!fvhff_match_uw(def,uw_sync_tol))
                    miss_cnt++;
                else
                    miss_cnt=0;
                
                /* If we go over the miss tolerance, go into no-sync */
                if(miss_cnt>miss_tol)
                    state = ST_NOSYNC;
                /* Extract the bits */
                extracted_frame = 1;
                fvhff_extract_frame(def,codec2_out,proto_out,vc_out);
            }
        /* Not yet sunk */
        }else{
            /* It's a sync!*/
            if(fvhff_match_uw(def,uw_first_tol)){
                state = ST_SYNC;
                last_uw = 0;
                miss_cnt = 0;
                extracted_frame = 1;
                fvhff_extract_frame(def,codec2_out,proto_out,vc_out);
            }
        }
    }
    def->state = state;
    def->last_uw = last_uw;
    def->miss_cnt = miss_cnt;
    return extracted_frame;
}