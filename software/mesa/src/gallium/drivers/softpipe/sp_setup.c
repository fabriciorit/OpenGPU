/*
Copyright 2017 Fabricio Ribeiro Toloczko 

OpenGPU

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

/**************************************************************************
 *
 * Copyright 2007 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

/**
 * \brief  Primitive rasterization/rendering (points, lines, triangles)
 *
 * \author  Keith Whitwell <keithw@vmware.com>
 * \author  Brian Paul
 */

#include "sp_context.h"
#include "sp_quad.h"
#include "sp_quad_pipe.h"
#include "sp_setup.h"
#include "sp_state.h"
#include "draw/draw_context.h"
#include "pipe/p_shader_tokens.h"
#include "util/u_math.h"
#include "util/u_memory.h"

//OGPU
#include "hps_0.h" //HPS FPGA DE1 SoC Board definitions for this project

#define soc_cv_av //TODO: remove this and do cross compiling in altera environment
#include "hwlib.h"//TODO: remove this and do cross compiling in altera environment
#include "socal/socal.h"//TODO: remove this and do cross compiling in altera environment
#include "socal/hps.h"//TODO: remove this and do cross compiling in altera environment
#include "socal/alt_gpio.h"//TODO: remove this and do cross compiling in altera environment


#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define HW_REGS_BASE ( ALT_STM_OFST )
#define HW_REGS_SPAN ( 0x04000000 )
#define HW_REGS_MASK ( HW_REGS_SPAN - 1 )

#define ALT_AXI_FPGASLVS_OFST (0xC0000000) // axi_master
#define HW_FPGA_AXI_SPAN ( 0xFBFFFFFF-ALT_AXI_FPGASLVS_OFST+1 ) // Bridge span
#define HW_FPGA_AXI_MASK ( HW_FPGA_AXI_SPAN - 1 )
//OGPU end

#define DEBUG_VERTS 0
#define DEBUG_FRAGS 0


/**
 * Triangle edge info
 */
struct edge {
   float dx;		/**< X(v1) - X(v0), used only during setup */
   float dy;		/**< Y(v1) - Y(v0), used only during setup */
   float dxdy;		/**< dx/dy */
   float sx, sy;	/**< first sample point coord */
   int lines;		/**< number of lines on this edge */
};


/**
 * Max number of quads (2x2 pixel blocks) to process per batch.
 * This can't be arbitrarily increased since we depend on some 32-bit
 * bitmasks (two bits per quad).
 */
#define MAX_QUADS 16


/**
 * Triangle setup info.
 * Also used for line drawing (taking some liberties).
 */
struct setup_context {
   struct softpipe_context *softpipe;

   /* Vertices are just an array of floats making up each attribute in
    * turn.  Currently fixed at 4 floats, but should change in time.
    * Codegen will help cope with this.
    */
   const float (*vmax)[4];
   const float (*vmid)[4];
   const float (*vmin)[4];
   const float (*vprovoke)[4];

   struct edge ebot;
   struct edge etop;
   struct edge emaj;

   float oneoverarea;
   int facing;

   float pixel_offset;
   unsigned max_layer;

   struct quad_header quad[MAX_QUADS];
   struct quad_header *quad_ptrs[MAX_QUADS];
   unsigned count;

   struct tgsi_interp_coef coef[PIPE_MAX_SHADER_INPUTS];
   struct tgsi_interp_coef posCoef;  /* For Z, W */

   struct {
      int left[2];   /**< [0] = row0, [1] = row1 */
      int right[2];
      int y;
   } span;

#if DEBUG_FRAGS
   uint numFragsEmitted;  /**< per primitive */
   uint numFragsWritten;  /**< per primitive */
#endif

   unsigned cull_face;		/* which faces cull */
   unsigned nr_vertex_attrs;
};







/**
 * Clip setup->quad against the scissor/surface bounds.
 */
static inline void
quad_clip(struct setup_context *setup, struct quad_header *quad)
{
   unsigned viewport_index = quad[0].input.viewport_index;
   const struct pipe_scissor_state *cliprect = &setup->softpipe->cliprect[viewport_index];
   const int minx = (int) cliprect->minx;
   const int maxx = (int) cliprect->maxx;
   const int miny = (int) cliprect->miny;
   const int maxy = (int) cliprect->maxy;

   if (quad->input.x0 >= maxx ||
       quad->input.y0 >= maxy ||
       quad->input.x0 + 1 < minx ||
       quad->input.y0 + 1 < miny) {
      /* totally clipped */
      quad->inout.mask = 0x0;
      return;
   }
   if (quad->input.x0 < minx)
      quad->inout.mask &= (MASK_BOTTOM_RIGHT | MASK_TOP_RIGHT);
   if (quad->input.y0 < miny)
      quad->inout.mask &= (MASK_BOTTOM_LEFT | MASK_BOTTOM_RIGHT);
   if (quad->input.x0 == maxx - 1)
      quad->inout.mask &= (MASK_BOTTOM_LEFT | MASK_TOP_LEFT);
   if (quad->input.y0 == maxy - 1)
      quad->inout.mask &= (MASK_TOP_LEFT | MASK_TOP_RIGHT);
}


/**
 * Emit a quad (pass to next stage) with clipping.
 */
static inline void
clip_emit_quad(struct setup_context *setup, struct quad_header *quad)
{
   quad_clip(setup, quad);

   if (quad->inout.mask) {
      struct softpipe_context *sp = setup->softpipe;

#if DEBUG_FRAGS
      setup->numFragsEmitted += util_bitcount(quad->inout.mask);
#endif

      sp->quad.first->run( sp->quad.first, &quad, 1 );
   }
}



/**
 * Given an X or Y coordinate, return the block/quad coordinate that it
 * belongs to.
 */
static inline int
block(int x)
{
   return x & ~(2-1);
}


static inline int
block_x(int x)
{
   return x & ~(16-1);
}


/**
 * Render a horizontal span of quads
 */
static void
flush_spans(struct setup_context *setup)
{
   const int step = MAX_QUADS;
   const int xleft0 = setup->span.left[0];
   const int xleft1 = setup->span.left[1];
   const int xright0 = setup->span.right[0];
   const int xright1 = setup->span.right[1];
   struct quad_stage *pipe = setup->softpipe->quad.first;

   const int minleft = block_x(MIN2(xleft0, xleft1));
   const int maxright = MAX2(xright0, xright1);
   int x;

   /* process quads in horizontal chunks of 16 */
   for (x = minleft; x < maxright; x += step) {
      unsigned skip_left0 = CLAMP(xleft0 - x, 0, step);
      unsigned skip_left1 = CLAMP(xleft1 - x, 0, step);
      unsigned skip_right0 = CLAMP(x + step - xright0, 0, step);
      unsigned skip_right1 = CLAMP(x + step - xright1, 0, step);
      unsigned lx = x;
      unsigned q = 0;

      unsigned skipmask_left0 = (1U << skip_left0) - 1U;
      unsigned skipmask_left1 = (1U << skip_left1) - 1U;

      /* These calculations fail when step == 32 and skip_right == 0.
       */
      unsigned skipmask_right0 = ~0U << (unsigned)(step - skip_right0);
      unsigned skipmask_right1 = ~0U << (unsigned)(step - skip_right1);

      unsigned mask0 = ~skipmask_left0 & ~skipmask_right0;
      unsigned mask1 = ~skipmask_left1 & ~skipmask_right1;

      if (mask0 | mask1) {
         do {
            unsigned quadmask = (mask0 & 3) | ((mask1 & 3) << 2);
            if (quadmask) {
               setup->quad[q].input.x0 = lx;
               setup->quad[q].input.y0 = setup->span.y;
               setup->quad[q].input.facing = setup->facing;
               setup->quad[q].inout.mask = quadmask;
               setup->quad_ptrs[q] = &setup->quad[q];
               q++;
#if DEBUG_FRAGS
               setup->numFragsEmitted += util_bitcount(quadmask);
#endif
            }
            mask0 >>= 2;
            mask1 >>= 2;
            lx += 2;
         } while (mask0 | mask1);

         pipe->run( pipe, setup->quad_ptrs, q );
      }
   }


   setup->span.y = 0;
   setup->span.right[0] = 0;
   setup->span.right[1] = 0;
   setup->span.left[0] = 1000000;     /* greater than right[0] */
   setup->span.left[1] = 1000000;     /* greater than right[1] */
}


#if DEBUG_VERTS
static void
print_vertex(const struct setup_context *setup,
             const float (*v)[4])
{
   int i;
   debug_printf("   Vertex: (%p)\n", (void *) v);
   for (i = 0; i < setup->nr_vertex_attrs; i++) {
      debug_printf("     %d: %f %f %f %f\n",  i,
              v[i][0], v[i][1], v[i][2], v[i][3]);
      if (util_is_inf_or_nan(v[i][0])) {
         debug_printf("   NaN!\n");
      }
   }
}
#endif


/**
 * Sort the vertices from top to bottom order, setting up the triangle
 * edge fields (ebot, emaj, etop).
 * \return FALSE if coords are inf/nan (cull the tri), TRUE otherwise
 */
static boolean
setup_sort_vertices(struct setup_context *setup,
                    float det,
                    const float (*v0)[4],
                    const float (*v1)[4],
                    const float (*v2)[4])
{
   if (setup->softpipe->rasterizer->flatshade_first)
      setup->vprovoke = v0;
   else
      setup->vprovoke = v2;

   /* determine bottom to top order of vertices */
   {
      float y0 = v0[0][1];
      float y1 = v1[0][1];
      float y2 = v2[0][1];
      if (y0 <= y1) {
	 if (y1 <= y2) {
	    /* y0<=y1<=y2 */
	    setup->vmin = v0;
	    setup->vmid = v1;
	    setup->vmax = v2;
	 }
	 else if (y2 <= y0) {
	    /* y2<=y0<=y1 */
	    setup->vmin = v2;
	    setup->vmid = v0;
	    setup->vmax = v1;
	 }
	 else {
	    /* y0<=y2<=y1 */
	    setup->vmin = v0;
	    setup->vmid = v2;
	    setup->vmax = v1;
	 }
      }
      else {
	 if (y0 <= y2) {
	    /* y1<=y0<=y2 */
	    setup->vmin = v1;
	    setup->vmid = v0;
	    setup->vmax = v2;
	 }
	 else if (y2 <= y1) {
	    /* y2<=y1<=y0 */
	    setup->vmin = v2;
	    setup->vmid = v1;
	    setup->vmax = v0;
	 }
	 else {
	    /* y1<=y2<=y0 */
	    setup->vmin = v1;
	    setup->vmid = v2;
	    setup->vmax = v0;
	 }
      }
   }

   setup->ebot.dx = setup->vmid[0][0] - setup->vmin[0][0];
   setup->ebot.dy = setup->vmid[0][1] - setup->vmin[0][1];
   setup->emaj.dx = setup->vmax[0][0] - setup->vmin[0][0];
   setup->emaj.dy = setup->vmax[0][1] - setup->vmin[0][1];
   setup->etop.dx = setup->vmax[0][0] - setup->vmid[0][0];
   setup->etop.dy = setup->vmax[0][1] - setup->vmid[0][1];

   /*
    * Compute triangle's area.  Use 1/area to compute partial
    * derivatives of attributes later.
    *
    * The area will be the same as prim->det, but the sign may be
    * different depending on how the vertices get sorted above.
    *
    * To determine whether the primitive is front or back facing we
    * use the prim->det value because its sign is correct.
    */
   {
      const float area = (setup->emaj.dx * setup->ebot.dy -
			    setup->ebot.dx * setup->emaj.dy);

      setup->oneoverarea = 1.0f / area;

      /*
      debug_printf("%s one-over-area %f  area %f  det %f\n",
                   __FUNCTION__, setup->oneoverarea, area, det );
      */
      if (util_is_inf_or_nan(setup->oneoverarea))
         return FALSE;
   }

   /* We need to know if this is a front or back-facing triangle for:
    *  - the GLSL gl_FrontFacing fragment attribute (bool)
    *  - two-sided stencil test
    * 0 = front-facing, 1 = back-facing
    */
   setup->facing =
      ((det < 0.0) ^
       (setup->softpipe->rasterizer->front_ccw));

   {
      unsigned face = setup->facing == 0 ? PIPE_FACE_FRONT : PIPE_FACE_BACK;

      if (face & setup->cull_face)
	 return FALSE;
   }


   /* Prepare pixel offset for rasterisation:
    *  - pixel center (0.5, 0.5) for GL, or
    *  - assume (0.0, 0.0) for other APIs.
    */
   if (setup->softpipe->rasterizer->half_pixel_center) {
      setup->pixel_offset = 0.5f;
   } else {
      setup->pixel_offset = 0.0f;
   }

   return TRUE;
}


/* Apply cylindrical wrapping to v0, v1, v2 coordinates, if enabled.
 * Input coordinates must be in [0, 1] range, otherwise results are undefined.
 * Some combinations of coordinates produce invalid results,
 * but this behaviour is acceptable.
 */
static void
tri_apply_cylindrical_wrap(float v0,
                           float v1,
                           float v2,
                           uint cylindrical_wrap,
                           float output[3])
{
   if (cylindrical_wrap) {
      float delta;

      delta = v1 - v0;
      if (delta > 0.5f) {
         v0 += 1.0f;
      }
      else if (delta < -0.5f) {
         v1 += 1.0f;
      }

      delta = v2 - v1;
      if (delta > 0.5f) {
         v1 += 1.0f;
      }
      else if (delta < -0.5f) {
         v2 += 1.0f;
      }

      delta = v0 - v2;
      if (delta > 0.5f) {
         v2 += 1.0f;
      }
      else if (delta < -0.5f) {
         v0 += 1.0f;
      }
   }

   output[0] = v0;
   output[1] = v1;
   output[2] = v2;
}


/**
 * Compute a0 for a constant-valued coefficient (GL_FLAT shading).
 * The value value comes from vertex[slot][i].
 * The result will be put into setup->coef[slot].a0[i].
 * \param slot  which attribute slot
 * \param i  which component of the slot (0..3)
 */
static void
const_coeff(struct setup_context *setup,
            struct tgsi_interp_coef *coef,
            uint vertSlot, uint i)
{
   assert(i <= 3);

   coef->dadx[i] = 0;
   coef->dady[i] = 0;

   /* need provoking vertex info!
    */
   coef->a0[i] = setup->vprovoke[vertSlot][i];
}


/**
 * Compute a0, dadx and dady for a linearly interpolated coefficient,
 * for a triangle.
 * v[0], v[1] and v[2] are vmin, vmid and vmax, respectively.
 */
static void
tri_linear_coeff(struct setup_context *setup,
                 struct tgsi_interp_coef *coef,
                 uint i,
                 const float v[3])
{
   float botda = v[1] - v[0];
   float majda = v[2] - v[0];
   float a = setup->ebot.dy * majda - botda * setup->emaj.dy;
   float b = setup->emaj.dx * botda - majda * setup->ebot.dx;
   float dadx = a * setup->oneoverarea;
   float dady = b * setup->oneoverarea;

   assert(i <= 3);

   coef->dadx[i] = dadx;
   coef->dady[i] = dady;

   /* calculate a0 as the value which would be sampled for the
    * fragment at (0,0), taking into account that we want to sample at
    * pixel centers, in other words (pixel_offset, pixel_offset).
    *
    * this is neat but unfortunately not a good way to do things for
    * triangles with very large values of dadx or dady as it will
    * result in the subtraction and re-addition from a0 of a very
    * large number, which means we'll end up loosing a lot of the
    * fractional bits and precision from a0.  the way to fix this is
    * to define a0 as the sample at a pixel center somewhere near vmin
    * instead - i'll switch to this later.
    */
   coef->a0[i] = (v[0] -
                  (dadx * (setup->vmin[0][0] - setup->pixel_offset) +
                   dady * (setup->vmin[0][1] - setup->pixel_offset)));
}


/**
 * Compute a0, dadx and dady for a perspective-corrected interpolant,
 * for a triangle.
 * We basically multiply the vertex value by 1/w before computing
 * the plane coefficients (a0, dadx, dady).
 * Later, when we compute the value at a particular fragment position we'll
 * divide the interpolated value by the interpolated W at that fragment.
 * v[0], v[1] and v[2] are vmin, vmid and vmax, respectively.
 */
static void
tri_persp_coeff(struct setup_context *setup,
                struct tgsi_interp_coef *coef,
                uint i,
                const float v[3])
{
   /* premultiply by 1/w  (v[0][3] is always W):
    */
   float mina = v[0] * setup->vmin[0][3];
   float mida = v[1] * setup->vmid[0][3];
   float maxa = v[2] * setup->vmax[0][3];
   float botda = mida - mina;
   float majda = maxa - mina;
   float a = setup->ebot.dy * majda - botda * setup->emaj.dy;
   float b = setup->emaj.dx * botda - majda * setup->ebot.dx;
   float dadx = a * setup->oneoverarea;
   float dady = b * setup->oneoverarea;

   assert(i <= 3);

   coef->dadx[i] = dadx;
   coef->dady[i] = dady;
   coef->a0[i] = (mina -
                  (dadx * (setup->vmin[0][0] - setup->pixel_offset) +
                   dady * (setup->vmin[0][1] - setup->pixel_offset)));
}


/**
 * Special coefficient setup for gl_FragCoord.
 * X and Y are trivial, though Y may have to be inverted for OpenGL.
 * Z and W are copied from posCoef which should have already been computed.
 * We could do a bit less work if we'd examine gl_FragCoord's swizzle mask.
 */
static void
setup_fragcoord_coeff(struct setup_context *setup, uint slot)
{
   const struct tgsi_shader_info *fsInfo = &setup->softpipe->fs_variant->info;
   boolean origin_lower_left =
         fsInfo->properties[TGSI_PROPERTY_FS_COORD_ORIGIN];
   boolean pixel_center_integer =
         fsInfo->properties[TGSI_PROPERTY_FS_COORD_PIXEL_CENTER];

   /*X*/
   setup->coef[slot].a0[0] = pixel_center_integer ? 0.0f : 0.5f;
   setup->coef[slot].dadx[0] = 1.0f;
   setup->coef[slot].dady[0] = 0.0f;
   /*Y*/
   setup->coef[slot].a0[1] =
		   (origin_lower_left ? setup->softpipe->framebuffer.height-1 : 0)
		   + (pixel_center_integer ? 0.0f : 0.5f);
   setup->coef[slot].dadx[1] = 0.0f;
   setup->coef[slot].dady[1] = origin_lower_left ? -1.0f : 1.0f;
   /*Z*/
   setup->coef[slot].a0[2] = setup->posCoef.a0[2];
   setup->coef[slot].dadx[2] = setup->posCoef.dadx[2];
   setup->coef[slot].dady[2] = setup->posCoef.dady[2];
   /*W*/
   setup->coef[slot].a0[3] = setup->posCoef.a0[3];
   setup->coef[slot].dadx[3] = setup->posCoef.dadx[3];
   setup->coef[slot].dady[3] = setup->posCoef.dady[3];
}



/**
 * Compute the setup->coef[] array dadx, dady, a0 values.
 * Must be called after setup->vmin,vmid,vmax,vprovoke are initialized.
 */
static void
setup_tri_coefficients(struct setup_context *setup)
{
   struct softpipe_context *softpipe = setup->softpipe;
   const struct tgsi_shader_info *fsInfo = &setup->softpipe->fs_variant->info;
   const struct sp_setup_info *sinfo = &softpipe->setup_info;
   uint fragSlot;
   float v[3];

   assert(sinfo->valid);

   /* z and w are done by linear interpolation:
    */
   v[0] = setup->vmin[0][2];
   v[1] = setup->vmid[0][2];
   v[2] = setup->vmax[0][2];
   tri_linear_coeff(setup, &setup->posCoef, 2, v);

   v[0] = setup->vmin[0][3];
   v[1] = setup->vmid[0][3];
   v[2] = setup->vmax[0][3];
   tri_linear_coeff(setup, &setup->posCoef, 3, v);

   /* setup interpolation for all the remaining attributes:
    */
   for (fragSlot = 0; fragSlot < fsInfo->num_inputs; fragSlot++) {
      const uint vertSlot = sinfo->attrib[fragSlot].src_index;
      uint j;

      switch (sinfo->attrib[fragSlot].interp) {
      case SP_INTERP_CONSTANT:
         for (j = 0; j < TGSI_NUM_CHANNELS; j++) {
            const_coeff(setup, &setup->coef[fragSlot], vertSlot, j);
         }
         break;
      case SP_INTERP_LINEAR:
         for (j = 0; j < TGSI_NUM_CHANNELS; j++) {
            tri_apply_cylindrical_wrap(setup->vmin[vertSlot][j],
                                       setup->vmid[vertSlot][j],
                                       setup->vmax[vertSlot][j],
                                       fsInfo->input_cylindrical_wrap[fragSlot] & (1 << j),
                                       v);
            tri_linear_coeff(setup, &setup->coef[fragSlot], j, v);
         }
         break;
      case SP_INTERP_PERSPECTIVE:
         for (j = 0; j < TGSI_NUM_CHANNELS; j++) {
            tri_apply_cylindrical_wrap(setup->vmin[vertSlot][j],
                                       setup->vmid[vertSlot][j],
                                       setup->vmax[vertSlot][j],
                                       fsInfo->input_cylindrical_wrap[fragSlot] & (1 << j),
                                       v);
            tri_persp_coeff(setup, &setup->coef[fragSlot], j, v);
         }
         break;
      case SP_INTERP_POS:
         setup_fragcoord_coeff(setup, fragSlot);
         break;
      default:
         assert(0);
      }

      if (fsInfo->input_semantic_name[fragSlot] == TGSI_SEMANTIC_FACE) {
         /* convert 0 to 1.0 and 1 to -1.0 */
         setup->coef[fragSlot].a0[0] = setup->facing * -2.0f + 1.0f;
         setup->coef[fragSlot].dadx[0] = 0.0;
         setup->coef[fragSlot].dady[0] = 0.0;
      }

      if (0) {
         for (j = 0; j < TGSI_NUM_CHANNELS; j++) {
            debug_printf("attr[%d].%c: a0:%f dx:%f dy:%f\n",
                         fragSlot, "xyzw"[j],
                         setup->coef[fragSlot].a0[j],
                         setup->coef[fragSlot].dadx[j],
                         setup->coef[fragSlot].dady[j]);
         }
      }
   }
}


static void
setup_tri_edges(struct setup_context *setup)
{
   float vmin_x = setup->vmin[0][0] + setup->pixel_offset;
   float vmid_x = setup->vmid[0][0] + setup->pixel_offset;

   float vmin_y = setup->vmin[0][1] - setup->pixel_offset;
   float vmid_y = setup->vmid[0][1] - setup->pixel_offset;
   float vmax_y = setup->vmax[0][1] - setup->pixel_offset;

   setup->emaj.sy = ceilf(vmin_y);
   setup->emaj.lines = (int) ceilf(vmax_y - setup->emaj.sy);
   setup->emaj.dxdy = setup->emaj.dy ? setup->emaj.dx / setup->emaj.dy : .0f;
   setup->emaj.sx = vmin_x + (setup->emaj.sy - vmin_y) * setup->emaj.dxdy;

   setup->etop.sy = ceilf(vmid_y);
   setup->etop.lines = (int) ceilf(vmax_y - setup->etop.sy);
   setup->etop.dxdy = setup->etop.dy ? setup->etop.dx / setup->etop.dy : .0f;
   setup->etop.sx = vmid_x + (setup->etop.sy - vmid_y) * setup->etop.dxdy;

   setup->ebot.sy = ceilf(vmin_y);
   setup->ebot.lines = (int) ceilf(vmid_y - setup->ebot.sy);
   setup->ebot.dxdy = setup->ebot.dy ? setup->ebot.dx / setup->ebot.dy : .0f;
   setup->ebot.sx = vmin_x + (setup->ebot.sy - vmin_y) * setup->ebot.dxdy;
}


/**
 * Render the upper or lower half of a triangle.
 * Scissoring/cliprect is applied here too.
 */
static void
subtriangle(struct setup_context *setup,
            struct edge *eleft,
            struct edge *eright,
            int lines,
            unsigned viewport_index)
{
   const struct pipe_scissor_state *cliprect = &setup->softpipe->cliprect[viewport_index];
   const int minx = (int) cliprect->minx;
   const int maxx = (int) cliprect->maxx;
   const int miny = (int) cliprect->miny;
   const int maxy = (int) cliprect->maxy;
   int y, start_y, finish_y;
   int sy = (int)eleft->sy;

   assert((int)eleft->sy == (int) eright->sy);
   assert(lines >= 0);

   /* clip top/bottom */
   start_y = sy;
   if (start_y < miny)
      start_y = miny;

   finish_y = sy + lines;
   if (finish_y > maxy)
      finish_y = maxy;

   start_y -= sy;
   finish_y -= sy;

   /*
   debug_printf("%s %d %d\n", __FUNCTION__, start_y, finish_y);
   */

   for (y = start_y; y < finish_y; y++) {

      /* avoid accumulating adds as floats don't have the precision to
       * accurately iterate large triangle edges that way.  luckily we
       * can just multiply these days.
       *
       * this is all drowned out by the attribute interpolation anyway.
       */
      int left = (int)(eleft->sx + y * eleft->dxdy);
      int right = (int)(eright->sx + y * eright->dxdy);

      /* clip left/right */
      if (left < minx)
         left = minx;
      if (right > maxx)
         right = maxx;

      if (left < right) {
         int _y = sy + y;
         if (block(_y) != setup->span.y) {
            flush_spans(setup);
            setup->span.y = block(_y);
         }

         setup->span.left[_y&1] = left;
         setup->span.right[_y&1] = right;
      }
   }


   /* save the values so that emaj can be restarted:
    */
   eleft->sx += lines * eleft->dxdy;
   eright->sx += lines * eright->dxdy;
   eleft->sy += lines;
   eright->sy += lines;
}


/**
 * Recalculate prim's determinant.  This is needed as we don't have
 * get this information through the vbuf_render interface & we must
 * calculate it here.
 */
static float
calc_det(const float (*v0)[4],
         const float (*v1)[4],
         const float (*v2)[4])
{
   /* edge vectors e = v0 - v2, f = v1 - v2 */
   const float ex = v0[0][0] - v2[0][0];
   const float ey = v0[0][1] - v2[0][1];
   const float fx = v1[0][0] - v2[0][0];
   const float fy = v1[0][1] - v2[0][1];

   /* det = cross(e,f).z */
   return ex * fy - ey * fx;
}


/**
 * Do setup for triangle rasterization, then render the triangle.
 */
void
sp_setup_tri(struct setup_context *setup,
             const float (*v0)[4],
             const float (*v1)[4],
             const float (*v2)[4])
{
   float det;
   uint layer = 0;
   unsigned viewport_index = 0;
#if DEBUG_VERTS
   debug_printf("Setup triangle:\n");
   print_vertex(setup, v0);
   print_vertex(setup, v1);
   print_vertex(setup, v2);
#endif

   if (setup->softpipe->no_rast || setup->softpipe->rasterizer->rasterizer_discard)
      return;

   det = calc_det(v0, v1, v2);
   /*
   debug_printf("%s\n", __FUNCTION__ );
   */

#if DEBUG_FRAGS
   setup->numFragsEmitted = 0;
   setup->numFragsWritten = 0;
#endif

   if (!setup_sort_vertices( setup, det, v0, v1, v2 ))
      return;

   setup_tri_coefficients( setup );
   setup_tri_edges( setup );

   assert(setup->softpipe->reduced_prim == PIPE_PRIM_TRIANGLES);

   setup->span.y = 0;
   setup->span.right[0] = 0;
   setup->span.right[1] = 0;
   /*   setup->span.z_mode = tri_z_mode( setup->ctx ); */
   if (setup->softpipe->layer_slot > 0) {
      layer = *(unsigned *)setup->vprovoke[setup->softpipe->layer_slot];
      layer = MIN2(layer, setup->max_layer);
   }
   setup->quad[0].input.layer = layer;

   if (setup->softpipe->viewport_index_slot > 0) {
      unsigned *udata = (unsigned*)v0[setup->softpipe->viewport_index_slot];
      viewport_index = sp_clamp_viewport_idx(*udata);
   }
   setup->quad[0].input.viewport_index = viewport_index;

   /*   init_constant_attribs( setup ); */

   if (setup->oneoverarea < 0.0) {
      /* emaj on left:
       */
      subtriangle(setup, &setup->emaj, &setup->ebot, setup->ebot.lines, viewport_index);
      subtriangle(setup, &setup->emaj, &setup->etop, setup->etop.lines, viewport_index);
   }
   else {
      /* emaj on right:
       */
      subtriangle(setup, &setup->ebot, &setup->emaj, setup->ebot.lines, viewport_index);
      subtriangle(setup, &setup->etop, &setup->emaj, setup->etop.lines, viewport_index);
   }

   flush_spans( setup );

   if (setup->softpipe->active_statistics_queries) {
      setup->softpipe->pipeline_statistics.c_primitives++;
   }

#if DEBUG_FRAGS
   printf("Tri: %u frags emitted, %u written\n",
          setup->numFragsEmitted,
          setup->numFragsWritten);
#endif
}


/* Apply cylindrical wrapping to v0, v1 coordinates, if enabled.
 * Input coordinates must be in [0, 1] range, otherwise results are undefined.
 */
static void
line_apply_cylindrical_wrap(float v0,
                            float v1,
                            uint cylindrical_wrap,
                            float output[2])
{
   if (cylindrical_wrap) {
      float delta;

      delta = v1 - v0;
      if (delta > 0.5f) {
         v0 += 1.0f;
      }
      else if (delta < -0.5f) {
         v1 += 1.0f;
      }
   }

   output[0] = v0;
   output[1] = v1;
}


/**
 * Compute a0, dadx and dady for a linearly interpolated coefficient,
 * for a line.
 * v[0] and v[1] are vmin and vmax, respectively.
 */
static void
line_linear_coeff(const struct setup_context *setup,
                  struct tgsi_interp_coef *coef,
                  uint i,
                  const float v[2])
{
   const float da = v[1] - v[0];
   const float dadx = da * setup->emaj.dx * setup->oneoverarea;
   const float dady = da * setup->emaj.dy * setup->oneoverarea;
   coef->dadx[i] = dadx;
   coef->dady[i] = dady;
   coef->a0[i] = (v[0] -
                  (dadx * (setup->vmin[0][0] - setup->pixel_offset) +
                   dady * (setup->vmin[0][1] - setup->pixel_offset)));
}


/**
 * Compute a0, dadx and dady for a perspective-corrected interpolant,
 * for a line.
 * v[0] and v[1] are vmin and vmax, respectively.
 */
static void
line_persp_coeff(const struct setup_context *setup,
                 struct tgsi_interp_coef *coef,
                 uint i,
                 const float v[2])
{
   const float a0 = v[0] * setup->vmin[0][3];
   const float a1 = v[1] * setup->vmax[0][3];
   const float da = a1 - a0;
   const float dadx = da * setup->emaj.dx * setup->oneoverarea;
   const float dady = da * setup->emaj.dy * setup->oneoverarea;
   coef->dadx[i] = dadx;
   coef->dady[i] = dady;
   coef->a0[i] = (a0 -
                  (dadx * (setup->vmin[0][0] - setup->pixel_offset) +
                   dady * (setup->vmin[0][1] - setup->pixel_offset)));
}


/**
 * Compute the setup->coef[] array dadx, dady, a0 values.
 * Must be called after setup->vmin,vmax are initialized.
 */
static boolean
setup_line_coefficients(struct setup_context *setup,
                        const float (*v0)[4],
                        const float (*v1)[4])
{
   struct softpipe_context *softpipe = setup->softpipe;
   const struct tgsi_shader_info *fsInfo = &setup->softpipe->fs_variant->info;
   const struct sp_setup_info *sinfo = &softpipe->setup_info;
   uint fragSlot;
   float area;
   float v[2];

   assert(sinfo->valid);

   /* use setup->vmin, vmax to point to vertices */
   if (softpipe->rasterizer->flatshade_first)
      setup->vprovoke = v0;
   else
      setup->vprovoke = v1;
   setup->vmin = v0;
   setup->vmax = v1;

   setup->emaj.dx = setup->vmax[0][0] - setup->vmin[0][0];
   setup->emaj.dy = setup->vmax[0][1] - setup->vmin[0][1];

   /* NOTE: this is not really area but something proportional to it */
   area = setup->emaj.dx * setup->emaj.dx + setup->emaj.dy * setup->emaj.dy;
   if (area == 0.0f || util_is_inf_or_nan(area))
      return FALSE;
   setup->oneoverarea = 1.0f / area;

   /* z and w are done by linear interpolation:
    */
   v[0] = setup->vmin[0][2];
   v[1] = setup->vmax[0][2];
   line_linear_coeff(setup, &setup->posCoef, 2, v);

   v[0] = setup->vmin[0][3];
   v[1] = setup->vmax[0][3];
   line_linear_coeff(setup, &setup->posCoef, 3, v);

   /* setup interpolation for all the remaining attributes:
    */
   for (fragSlot = 0; fragSlot < fsInfo->num_inputs; fragSlot++) {
      const uint vertSlot = sinfo->attrib[fragSlot].src_index;
      uint j;

      switch (sinfo->attrib[fragSlot].interp) {
      case SP_INTERP_CONSTANT:
         for (j = 0; j < TGSI_NUM_CHANNELS; j++)
            const_coeff(setup, &setup->coef[fragSlot], vertSlot, j);
         break;
      case SP_INTERP_LINEAR:
         for (j = 0; j < TGSI_NUM_CHANNELS; j++) {
            line_apply_cylindrical_wrap(setup->vmin[vertSlot][j],
                                        setup->vmax[vertSlot][j],
                                        fsInfo->input_cylindrical_wrap[fragSlot] & (1 << j),
                                        v);
            line_linear_coeff(setup, &setup->coef[fragSlot], j, v);
         }
         break;
      case SP_INTERP_PERSPECTIVE:
         for (j = 0; j < TGSI_NUM_CHANNELS; j++) {
            line_apply_cylindrical_wrap(setup->vmin[vertSlot][j],
                                        setup->vmax[vertSlot][j],
                                        fsInfo->input_cylindrical_wrap[fragSlot] & (1 << j),
                                        v);
            line_persp_coeff(setup, &setup->coef[fragSlot], j, v);
         }
         break;
      case SP_INTERP_POS:
         setup_fragcoord_coeff(setup, fragSlot);
         break;
      default:
         assert(0);
      }

      if (fsInfo->input_semantic_name[fragSlot] == TGSI_SEMANTIC_FACE) {
         /* convert 0 to 1.0 and 1 to -1.0 */
         setup->coef[fragSlot].a0[0] = setup->facing * -2.0f + 1.0f;
         setup->coef[fragSlot].dadx[0] = 0.0;
         setup->coef[fragSlot].dady[0] = 0.0;
      }
   }
   return TRUE;
}


/**
 * Plot a pixel in a line segment.
 */
static inline void
plot(struct setup_context *setup, int x, int y)
{
   const int iy = y & 1;
   const int ix = x & 1;
   const int quadX = x - ix;
   const int quadY = y - iy;
   const int mask = (1 << ix) << (2 * iy);

   if (quadX != setup->quad[0].input.x0 ||
       quadY != setup->quad[0].input.y0)
   {
      /* flush prev quad, start new quad */

      if (setup->quad[0].input.x0 != -1)
         clip_emit_quad(setup, &setup->quad[0]);

      setup->quad[0].input.x0 = quadX;
      setup->quad[0].input.y0 = quadY;
      setup->quad[0].inout.mask = 0x0;
   }

   setup->quad[0].inout.mask |= mask;
}


/**
 * Do setup for line rasterization, then render the line.
 * Single-pixel width, no stipple, etc.  We rely on the 'draw' module
 * to handle stippling and wide lines.
 */
void
sp_setup_line(struct setup_context *setup,
              const float (*v0)[4],
              const float (*v1)[4])
{
   int x0 = (int) v0[0][0];
   int x1 = (int) v1[0][0];
   int y0 = (int) v0[0][1];
   int y1 = (int) v1[0][1];
   int dx = x1 - x0;
   int dy = y1 - y0;
   int xstep, ystep;
   uint layer = 0;
   unsigned viewport_index = 0;

#if DEBUG_VERTS
   debug_printf("Setup line:\n");
   print_vertex(setup, v0);
   print_vertex(setup, v1);
#endif

   if (setup->softpipe->no_rast || setup->softpipe->rasterizer->rasterizer_discard)
      return;

   if (dx == 0 && dy == 0)
      return;

   if (!setup_line_coefficients(setup, v0, v1))
      return;

   assert(v0[0][0] < 1.0e9);
   assert(v0[0][1] < 1.0e9);
   assert(v1[0][0] < 1.0e9);
   assert(v1[0][1] < 1.0e9);

   if (dx < 0) {
      dx = -dx;   /* make positive */
      xstep = -1;
   }
   else {
      xstep = 1;
   }

   if (dy < 0) {
      dy = -dy;   /* make positive */
      ystep = -1;
   }
   else {
      ystep = 1;
   }

   assert(dx >= 0);
   assert(dy >= 0);
   assert(setup->softpipe->reduced_prim == PIPE_PRIM_LINES);

   setup->quad[0].input.x0 = setup->quad[0].input.y0 = -1;
   setup->quad[0].inout.mask = 0x0;
   if (setup->softpipe->layer_slot > 0) {
      layer = *(unsigned *)setup->vprovoke[setup->softpipe->layer_slot];
      layer = MIN2(layer, setup->max_layer);
   }
   setup->quad[0].input.layer = layer;

   if (setup->softpipe->viewport_index_slot > 0) {
      unsigned *udata = (unsigned*)setup->vprovoke[setup->softpipe->viewport_index_slot];
      viewport_index = sp_clamp_viewport_idx(*udata);
   }
   setup->quad[0].input.viewport_index = viewport_index;

   /* XXX temporary: set coverage to 1.0 so the line appears
    * if AA mode happens to be enabled.
    */
   setup->quad[0].input.coverage[0] =
   setup->quad[0].input.coverage[1] =
   setup->quad[0].input.coverage[2] =
   setup->quad[0].input.coverage[3] = 1.0;

   if (dx > dy) {
      /*** X-major line ***/
      int i;
      const int errorInc = dy + dy;
      int error = errorInc - dx;
      const int errorDec = error - dx;

      for (i = 0; i < dx; i++) {
         plot(setup, x0, y0);

         x0 += xstep;
         if (error < 0) {
            error += errorInc;
         }
         else {
            error += errorDec;
            y0 += ystep;
         }
      }
   }
   else {
      /*** Y-major line ***/
      int i;
      const int errorInc = dx + dx;
      int error = errorInc - dy;
      const int errorDec = error - dy;

      for (i = 0; i < dy; i++) {
         plot(setup, x0, y0);

         y0 += ystep;
         if (error < 0) {
            error += errorInc;
         }
         else {
            error += errorDec;
            x0 += xstep;
         }
      }
   }

   /* draw final quad */
   if (setup->quad[0].inout.mask) {
      clip_emit_quad(setup, &setup->quad[0]);
   }
}


static void
point_persp_coeff(const struct setup_context *setup,
                  const float (*vert)[4],
                  struct tgsi_interp_coef *coef,
                  uint vertSlot, uint i)
{
   assert(i <= 3);
   coef->dadx[i] = 0.0F;
   coef->dady[i] = 0.0F;
   coef->a0[i] = vert[vertSlot][i] * vert[0][3];
}


/**
 * Do setup for point rasterization, then render the point.
 * Round or square points...
 * XXX could optimize a lot for 1-pixel points.
 */
void
sp_setup_point(struct setup_context *setup,
               const float (*v0)[4])
{
   struct softpipe_context *softpipe = setup->softpipe;
   const struct tgsi_shader_info *fsInfo = &setup->softpipe->fs_variant->info;
   const int sizeAttr = setup->softpipe->psize_slot;
   const float size
      = sizeAttr > 0 ? v0[sizeAttr][0]
      : setup->softpipe->rasterizer->point_size;
   const float halfSize = 0.5F * size;
   const boolean round = (boolean) setup->softpipe->rasterizer->point_smooth;
   const float x = v0[0][0];  /* Note: data[0] is always position */
   const float y = v0[0][1];
   const struct sp_setup_info *sinfo = &softpipe->setup_info;
   uint fragSlot;
   uint layer = 0;
   unsigned viewport_index = 0;
#if DEBUG_VERTS
   debug_printf("Setup point:\n");
   print_vertex(setup, v0);
#endif

   assert(sinfo->valid);

   if (setup->softpipe->no_rast || setup->softpipe->rasterizer->rasterizer_discard)
      return;

   assert(setup->softpipe->reduced_prim == PIPE_PRIM_POINTS);

   if (setup->softpipe->layer_slot > 0) {
      layer = *(unsigned *)v0[setup->softpipe->layer_slot];
      layer = MIN2(layer, setup->max_layer);
   }
   setup->quad[0].input.layer = layer;

   if (setup->softpipe->viewport_index_slot > 0) {
      unsigned *udata = (unsigned*)v0[setup->softpipe->viewport_index_slot];
      viewport_index = sp_clamp_viewport_idx(*udata);
   }
   setup->quad[0].input.viewport_index = viewport_index;

   /* For points, all interpolants are constant-valued.
    * However, for point sprites, we'll need to setup texcoords appropriately.
    * XXX: which coefficients are the texcoords???
    * We may do point sprites as textured quads...
    *
    * KW: We don't know which coefficients are texcoords - ultimately
    * the choice of what interpolation mode to use for each attribute
    * should be determined by the fragment program, using
    * per-attribute declaration statements that include interpolation
    * mode as a parameter.  So either the fragment program will have
    * to be adjusted for pointsprite vs normal point behaviour, or
    * otherwise a special interpolation mode will have to be defined
    * which matches the required behaviour for point sprites.  But -
    * the latter is not a feature of normal hardware, and as such
    * probably should be ruled out on that basis.
    */
   setup->vprovoke = v0;

   /* setup Z, W */
   const_coeff(setup, &setup->posCoef, 0, 2);
   const_coeff(setup, &setup->posCoef, 0, 3);

   for (fragSlot = 0; fragSlot < fsInfo->num_inputs; fragSlot++) {
      const uint vertSlot = sinfo->attrib[fragSlot].src_index;
      uint j;

      switch (sinfo->attrib[fragSlot].interp) {
      case SP_INTERP_CONSTANT:
         /* fall-through */
      case SP_INTERP_LINEAR:
         for (j = 0; j < TGSI_NUM_CHANNELS; j++)
            const_coeff(setup, &setup->coef[fragSlot], vertSlot, j);
         break;
      case SP_INTERP_PERSPECTIVE:
         for (j = 0; j < TGSI_NUM_CHANNELS; j++)
            point_persp_coeff(setup, setup->vprovoke,
                              &setup->coef[fragSlot], vertSlot, j);
         break;
      case SP_INTERP_POS:
         setup_fragcoord_coeff(setup, fragSlot);
         break;
      default:
         assert(0);
      }

      if (fsInfo->input_semantic_name[fragSlot] == TGSI_SEMANTIC_FACE) {
         /* convert 0 to 1.0 and 1 to -1.0 */
         setup->coef[fragSlot].a0[0] = setup->facing * -2.0f + 1.0f;
         setup->coef[fragSlot].dadx[0] = 0.0;
         setup->coef[fragSlot].dady[0] = 0.0;
      }
   }


   if (halfSize <= 0.5 && !round) {
      /* special case for 1-pixel points */
      const int ix = ((int) x) & 1;
      const int iy = ((int) y) & 1;
      setup->quad[0].input.x0 = (int) x - ix;
      setup->quad[0].input.y0 = (int) y - iy;
      setup->quad[0].inout.mask = (1 << ix) << (2 * iy);
      clip_emit_quad(setup, &setup->quad[0]);
   }
   else {
      if (round) {
         /* rounded points */
         const int ixmin = block((int) (x - halfSize));
         const int ixmax = block((int) (x + halfSize));
         const int iymin = block((int) (y - halfSize));
         const int iymax = block((int) (y + halfSize));
         const float rmin = halfSize - 0.7071F;  /* 0.7071 = sqrt(2)/2 */
         const float rmax = halfSize + 0.7071F;
         const float rmin2 = MAX2(0.0F, rmin * rmin);
         const float rmax2 = rmax * rmax;
         const float cscale = 1.0F / (rmax2 - rmin2);
         int ix, iy;

         for (iy = iymin; iy <= iymax; iy += 2) {
            for (ix = ixmin; ix <= ixmax; ix += 2) {
               float dx, dy, dist2, cover;

               setup->quad[0].inout.mask = 0x0;

               dx = (ix + 0.5f) - x;
               dy = (iy + 0.5f) - y;
               dist2 = dx * dx + dy * dy;
               if (dist2 <= rmax2) {
                  cover = 1.0F - (dist2 - rmin2) * cscale;
                  setup->quad[0].input.coverage[QUAD_TOP_LEFT] = MIN2(cover, 1.0f);
                  setup->quad[0].inout.mask |= MASK_TOP_LEFT;
               }

               dx = (ix + 1.5f) - x;
               dy = (iy + 0.5f) - y;
               dist2 = dx * dx + dy * dy;
               if (dist2 <= rmax2) {
                  cover = 1.0F - (dist2 - rmin2) * cscale;
                  setup->quad[0].input.coverage[QUAD_TOP_RIGHT] = MIN2(cover, 1.0f);
                  setup->quad[0].inout.mask |= MASK_TOP_RIGHT;
               }

               dx = (ix + 0.5f) - x;
               dy = (iy + 1.5f) - y;
               dist2 = dx * dx + dy * dy;
               if (dist2 <= rmax2) {
                  cover = 1.0F - (dist2 - rmin2) * cscale;
                  setup->quad[0].input.coverage[QUAD_BOTTOM_LEFT] = MIN2(cover, 1.0f);
                  setup->quad[0].inout.mask |= MASK_BOTTOM_LEFT;
               }

               dx = (ix + 1.5f) - x;
               dy = (iy + 1.5f) - y;
               dist2 = dx * dx + dy * dy;
               if (dist2 <= rmax2) {
                  cover = 1.0F - (dist2 - rmin2) * cscale;
                  setup->quad[0].input.coverage[QUAD_BOTTOM_RIGHT] = MIN2(cover, 1.0f);
                  setup->quad[0].inout.mask |= MASK_BOTTOM_RIGHT;
               }

               if (setup->quad[0].inout.mask) {
                  setup->quad[0].input.x0 = ix;
                  setup->quad[0].input.y0 = iy;
                  clip_emit_quad(setup, &setup->quad[0]);
               }
            }
         }
      }
      else {
         /* square points */
         const int xmin = (int) (x + 0.75 - halfSize);
         const int ymin = (int) (y + 0.25 - halfSize);
         const int xmax = xmin + (int) size;
         const int ymax = ymin + (int) size;
         /* XXX could apply scissor to xmin,ymin,xmax,ymax now */
         const int ixmin = block(xmin);
         const int ixmax = block(xmax - 1);
         const int iymin = block(ymin);
         const int iymax = block(ymax - 1);
         int ix, iy;

         /*
         debug_printf("(%f, %f) -> X:%d..%d Y:%d..%d\n", x, y, xmin, xmax,ymin,ymax);
         */
         for (iy = iymin; iy <= iymax; iy += 2) {
            uint rowMask = 0xf;
            if (iy < ymin) {
               /* above the top edge */
               rowMask &= (MASK_BOTTOM_LEFT | MASK_BOTTOM_RIGHT);
            }
            if (iy + 1 >= ymax) {
               /* below the bottom edge */
               rowMask &= (MASK_TOP_LEFT | MASK_TOP_RIGHT);
            }

            for (ix = ixmin; ix <= ixmax; ix += 2) {
               uint mask = rowMask;

               if (ix < xmin) {
                  /* fragment is past left edge of point, turn off left bits */
                  mask &= (MASK_BOTTOM_RIGHT | MASK_TOP_RIGHT);
               }
               if (ix + 1 >= xmax) {
                  /* past the right edge */
                  mask &= (MASK_BOTTOM_LEFT | MASK_TOP_LEFT);
               }

               setup->quad[0].inout.mask = mask;
               setup->quad[0].input.x0 = ix;
               setup->quad[0].input.y0 = iy;
               clip_emit_quad(setup, &setup->quad[0]);
            }
         }
      }
   }
}


/**
 * Called by vbuf code just before we start buffering primitives.
 */
void
sp_setup_prepare(struct setup_context *setup)
{
   struct softpipe_context *sp = setup->softpipe;
   int i;
   unsigned max_layer = ~0;
   if (sp->dirty) {
      softpipe_update_derived(sp, sp->reduced_api_prim);
   }

   /* Note: nr_attrs is only used for debugging (vertex printing) */
   setup->nr_vertex_attrs = draw_num_shader_outputs(sp->draw);

   /*
    * Determine how many layers the fb has (used for clamping layer value).
    * OpenGL (but not d3d10) permits different amount of layers per rt, however
    * results are undefined if layer exceeds the amount of layers of ANY
    * attachment hence don't need separate per cbuf and zsbuf max.
    */
   for (i = 0; i < setup->softpipe->framebuffer.nr_cbufs; i++) {
      struct pipe_surface *cbuf = setup->softpipe->framebuffer.cbufs[i];
      if (cbuf) {
         max_layer = MIN2(max_layer,
                          cbuf->u.tex.last_layer - cbuf->u.tex.first_layer);

      }
   }

   setup->max_layer = max_layer;

   sp->quad.first->begin( sp->quad.first );

   if (sp->reduced_api_prim == PIPE_PRIM_TRIANGLES &&
       sp->rasterizer->fill_front == PIPE_POLYGON_MODE_FILL &&
       sp->rasterizer->fill_back == PIPE_POLYGON_MODE_FILL) {
      /* we'll do culling */
      setup->cull_face = sp->rasterizer->cull_face;
   }
   else {
      /* 'draw' will do culling */
      setup->cull_face = PIPE_FACE_NONE;
   }
}


void
sp_setup_destroy_context(struct setup_context *setup)
{
   FREE( setup );
}


/**
 * Create a new primitive setup/render stage.
 */
struct setup_context *
sp_setup_create_context(struct softpipe_context *softpipe)
{
   struct setup_context *setup = CALLOC_STRUCT(setup_context);
   unsigned i;

   setup->softpipe = softpipe;

   for (i = 0; i < MAX_QUADS; i++) {
      setup->quad[i].coef = setup->coef;
      setup->quad[i].posCoef = &setup->posCoef;
   }

   setup->span.left[0] = 1000000;     /* greater than right[0] */
   setup->span.left[1] = 1000000;     /* greater than right[1] */

   return setup;
}

//--OPENGPU
typedef uint8_t ogpu_bit;
typedef uint8_t ogpu_command;

enum OGPU_COMMAND
{
    OGPU_CMD_NOP=0,
    OGPU_CMD_PREPARE=0xA5,
    OGPU_CMD_RASTER=0xAA
};

struct ogpu_edge
{
    uint32_t x0,y0;
    uint32_t x1,y1;
};

struct ogpu_box
{
    float x0,y0;
    float x1,y1;
};

struct ogpu_quad
{
    //Fragment-vector element correspondence
    //  #===#===#
    //  | 0 | 1 |
    //  #===#===#
    //  | 2 | 3 |
    //  #===#===#
    uint16_t m[4][2];
};

struct ogpu_tile
{
    uint16_t x0,y0;
    uint16_t x1,y1;
};

struct ogpu_depth_coef
{
    int32_t a,b,c;
};

struct ogpu_depth_quad
{
    int32_t m[4];
};

struct ogpu_quad_buffer_cell
{
    uint16_t x,y; //quad (X,Y) top left coord
    uint8_t mask:4; //quad mask on same quad order referenced above
    uint8_t stencil[4]; //RESERVED to stencil for future implementation
    float depth[4];
};

struct ogpu_quad_buffer
{
    uint16_t n; //number of elements
    struct ogpu_quad_buffer_cell *b; //buffer pointer
    struct ogpu_tile tile;
};

#define OGPU_DEPTH_DEPTH ((1<<30)-1) //depth buffer precision

static inline int32_t ogpu_fix_float(float f)
{
    return (int32_t)f;
}

static inline uint32_t ogpu_ufix_float(float f)
{
    return (uint32_t)f;
}

static void ogpu_setup(ogpu_bit clock,
                       //INPUTS
                       const ogpu_bit start_raster,
                       const float (*v0)[2],const float (*v1)[2],const float (*v2)[2], //v*: input vertices
                       //OUTPUTS
                       //struct ogpu_box *box, //bound box
                       struct ogpu_edge *e0,struct ogpu_edge *e1,struct ogpu_edge *e2, //e*: edges
                       ogpu_bit *setup_done)
{
    static ogpu_bit _clock=0;
    //////////////////
    //     COMB     //
    //////////////////
    //Edges data allocating and fixing floating points
    e0->x0=ogpu_ufix_float(v0[0][0]);    e0->y0=ogpu_ufix_float(v0[0][1]);
    e0->x1=ogpu_ufix_float(v1[0][0]);    e0->y1=ogpu_ufix_float(v1[0][1]);

    e1->x0=ogpu_ufix_float(v1[0][0]);    e1->y0=ogpu_ufix_float(v1[0][1]);
    e1->x1=ogpu_ufix_float(v2[0][0]);    e1->y1=ogpu_ufix_float(v2[0][1]);

    e2->x0=ogpu_ufix_float(v2[0][0]);    e2->y0=ogpu_ufix_float(v2[0][1]);
    e2->x1=ogpu_ufix_float(v0[0][0]);    e2->y1=ogpu_ufix_float(v0[0][1]);

    //Define triangle bound box //SOFTPIPE'S CLIPRECT DEFINES BOUND BOX BEFORE RASTERIZER STAGE
//    ogpu_bit c1,c2,c3;
//    c1=v0[0][0]<=v1[0][0];
//    c2=v0[0][0]<=v2[0][0];
//    c3=v1[0][0]<=v2[0][0];
//    if(c1)
//    {
//        if(c3)
//        {
//            box->x0=v0[0][0];
//            box->x1=v2[0][0];
//        }
//        else
//        {
//            if(c2)
//            {
//                box->x0=v0[0][0];
//                box->x1=v1[0][0];
//            }
//            else
//            {
//                box->x0=v2[0][0];
//                box->x1=v1[0][0];
//            }
//        }
//    }
//    else
//    {
//        if(c2)
//        {
//            box->x0=v1[0][0];
//            box->x1=v2[0][0];
//        }
//        else
//        {
//            if(c3)
//            {
//                box->x0=v1[0][0];
//                box->x1=v0[0][0];
//            }
//            else
//            {
//                box->x0=v2[0][0];
//                box->x1=v0[0][0];
//            }
//        }
//    }
//    c1=v0[0][1]<=v1[0][1];
//    c2=v0[0][1]<=v2[0][1];
//    c3=v1[0][1]<=v2[0][1];
//    if(c1)
//    {
//        if(c3)
//        {
//            box->y0=v0[0][1];
//            box->y1=v2[0][1];
//        }
//        else
//        {
//            if(c2)
//            {
//                box->y0=v0[0][1];
//                box->y1=v1[0][1];
//            }
//            else
//            {
//                box->y0=v2[0][1];
//                box->y1=v1[0][1];
//            }
//        }
//    }
//    else
//    {
//        if(c2)
//        {
//            box->y0=v1[0][1];
//            box->y1=v2[0][1];
//        }
//        else
//        {
//            if(c3)
//            {
//                box->y0=v1[0][1];
//                box->y1=v0[0][1];
//            }
//            else
//            {
//                box->y0=v2[0][1];
//                box->y1=v0[0][1];
//            }
//        }
//    }
    //////////////////
    //     SEQ      //
    //////////////////
    if(clock!=_clock)
    {
        _clock=clock;
        if(clock)      //CLOCK RISING EDGE
        {
            if(start_raster)
            {
                *setup_done=1;
            }
            else
            {
                *setup_done=0;
            }
        }
        else      //CLOCK FALLING EDGE
        {
        }
    }
}

static void ogpu_quad_generator(ogpu_bit clock,
                                //INPUTS
                                const ogpu_bit next_quad,
                                const struct ogpu_box box, //(x0,y0) must be at left and at upper side of (x1,y1)
                                const struct ogpu_tile tile, //(x0,y0) must be at left and at upper side of (x1,y1)
                                //OUTPUTS
                                ogpu_bit *quad_ready,
                                ogpu_bit *end_tile,
                                struct ogpu_quad *quad)
{
    static ogpu_bit generate_quads=0;
    static ogpu_bit _clock=0;

    static uint16_t i=0,j=0,x0=0,y0=0,x1=0,y1=0;
    //////////////////
    //     COMB     //
    //////////////////


    //////////////////
    //     SEQ      //
    //////////////////
    if(clock!=_clock)
    {
        _clock=clock;
        if(clock)      //CLOCK RISING EDGE
        {
            if(next_quad)
            {
                if(!generate_quads)
                {
                    *end_tile=0;
                    if((tile.x1 < box.x0) || (tile.x0 > box.x1)) {*quad_ready=0;*end_tile=1;return;} //checks if box is not
                    if((tile.y1 < box.y0) || (tile.y0 > box.y1)) {*quad_ready=0;*end_tile=1;return;} //intersecting tile
                    generate_quads=1; //else generate quads
                    //clip tile: this helps discard void areas
                    i=y0=(tile.y0 <= box.y0)?(uint16_t)box.y0:tile.y0; //y0 is not used?
                    j=x0=(tile.x0 <= box.x0)?(uint16_t)box.x0:tile.x0;
                    x1=(tile.x1 >= box.x1)?(uint16_t)box.x1:tile.x1;
                    y1=(tile.y1 >= box.y1)?(uint16_t)box.y1:tile.y1;
                    //end clip tile
                    i&=~(1<<0);    //guarantee even number clearing first bit, this
                    x0=j&=~(1<<0); //is important to maintain quads at same place
                                   //independently of clipping
                }
                //generate quad coords
                quad->m[0][0]=j;     quad->m[1][0]=j+1;
                quad->m[0][1]=i;     quad->m[1][1]=i;
                quad->m[2][0]=j;     quad->m[3][0]=j+1;
                quad->m[2][1]=i+1;   quad->m[3][1]=i+1;
                j+=2;
                if(j>x1)
                {
                    j=x0;
                    i+=2;
                    if(i>y1)
                    {
                        *end_tile=1;
                        generate_quads=0;
                    }
                }
                *quad_ready=1;
            }
            else
            {
                *quad_ready=0;
            }
        }
        else      //CLOCK FALLING EDGE
        {
        }
    }
}

static inline ogpu_bit ogpu_edge_test(struct ogpu_edge e,uint32_t x,uint32_t y)
{
    //TODO: implement hardware oriented solution(fast fix point computing)
    if(
       (((int32_t)x-(int32_t)(e.x0))*((int32_t)(e.y1)-(int32_t)(e.y0)) -
       ((int32_t)(e.x1)-(int32_t)(e.x0))*((int32_t)y-(int32_t)(e.y0)))>=0 ) return 1;
    else return 0;
}

static void ogpu_quad_edge_test(ogpu_bit clock,
                                //INPUTS
                                struct ogpu_edge e,struct ogpu_quad quad,
                                ogpu_bit edge_test,
                                //OUTPUTS
                                ogpu_bit (*edge_mask)[4],
                                ogpu_bit *edge_ready)
{
    //static ogpu_bit _clock=0;
    //////////////////
    //     COMB     //
    //////////////////
    edge_mask[0][0]=ogpu_edge_test(e,quad.m[0][0],quad.m[0][1]);
    edge_mask[0][1]=ogpu_edge_test(e,quad.m[1][0],quad.m[1][1]);
    edge_mask[0][2]=ogpu_edge_test(e,quad.m[2][0],quad.m[2][1]);
    edge_mask[0][3]=ogpu_edge_test(e,quad.m[3][0],quad.m[3][1]);
    //////////////////
    //     SEQ      //
    //////////////////
    //if(clock!=_clock)
    {
        //_clock=clock;
        //if(clock)      //CLOCK RISING EDGE
        {
            *edge_ready=edge_test;
        }
        //else      //CLOCK FALLING EDGE
        {
        }
    }
}

static void ogpu_triangle_edge_test(ogpu_bit clock,
                                    //INPUTS
                                    ogpu_bit (*edge_ready)[3],ogpu_bit (*edge_mask0)[4],
                                    ogpu_bit (*edge_mask1)[4],ogpu_bit (*edge_mask2)[4],
                                    //OUTPUTS
                                    ogpu_bit (*quad_mask)[4],
                                    ogpu_bit *draw_quad,ogpu_bit *discard_quad)
{
    static ogpu_bit _clock=0;
    ogpu_bit _quad_mask[4];
    ogpu_bit _draw_quad;
    //////////////////
    //     COMB     //
    //////////////////
    //if edge bits from one fragment are 1,1,1 or 0,0,0, draw fragment
    _quad_mask[0]=(edge_mask0[0][0]&&edge_mask1[0][0]&&edge_mask2[0][0])||
                    (!(edge_mask0[0][0]||edge_mask1[0][0]||edge_mask2[0][0]));
    _quad_mask[1]=(edge_mask0[0][1]&&edge_mask1[0][1]&&edge_mask2[0][1])||
                    (!(edge_mask0[0][1]||edge_mask1[0][1]||edge_mask2[0][1]));
    _quad_mask[2]=(edge_mask0[0][2]&&edge_mask1[0][2]&&edge_mask2[0][2])||
                    (!(edge_mask0[0][2]||edge_mask1[0][2]||edge_mask2[0][2]));
    _quad_mask[3]=(edge_mask0[0][3]&&edge_mask1[0][3]&&edge_mask2[0][3])||
                    (!(edge_mask0[0][3]||edge_mask1[0][3]||edge_mask2[0][3]));
    _draw_quad=_quad_mask[0]||_quad_mask[1]||_quad_mask[2]||_quad_mask[3];

    //////////////////
    //     SEQ      //
    //////////////////
    if(clock!=_clock)
    {
        _clock=clock;
        if(clock)      //CLOCK RISING EDGE
        {
            if(edge_ready[0][0]&&edge_ready[0][1]&&edge_ready[0][2])
            {
                if(_draw_quad)
                {
                    *draw_quad=1;
                    *discard_quad=0;
                    quad_mask[0][0]=_quad_mask[0];
                    quad_mask[0][1]=_quad_mask[1];
                    quad_mask[0][2]=_quad_mask[2];
                    quad_mask[0][3]=_quad_mask[3];
                }
                else
                {
                    *draw_quad=0;
                    *discard_quad=1;
                }
            }
            else
            {
                *draw_quad=0;
                *discard_quad=0;
            }
        }
        else      //CLOCK FALLING EDGE
        {
        }
    }
}

static void ogpu_depth_coef(const float (*v0)[4],const float (*v1)[4],const float (*v2)[4],
                            struct ogpu_depth_coef *coef)
{
    float A,B,C; //Cross product components
    float vx,vy,vz,wx,wy,wz;
    vx=(v1[0][0]-v0[0][0]); //V vector
    vy=(v1[0][1]-v0[0][1]);
    vz=(v1[0][2]-v0[0][2]);
    wx=(v2[0][0]-v0[0][0]); //W vector
    wy=(v2[0][1]-v0[0][1]);
    wz=(v2[0][2]-v0[0][2]);
    //This is a simple algorithm for calculate the plane equation from three points.
    //Calculating cross product VxW to obtain normal vector.
    //The normal vector and a point(v0) define a plane
    //Then we calculate the z function -- that returns z value for any x,y
    //Here, we calculate the coefficients a,b,c for function z=a*x+b*y+c
    //The final products and sums are performed inside hardware, while this
    //pre-calculations are performed in software, because they are constant for each
    //triangle. More details, consult OpenGPU project documentation.
    A=vy*wz-vz*wy;
    B=vz*wx-vx*wz;
    C=vx*wy-vy*wx;
    coef->a=ogpu_fix_float((-A/C)*(OGPU_DEPTH_DEPTH));
    coef->b=ogpu_fix_float((-B/C)*(OGPU_DEPTH_DEPTH));
    coef->c=ogpu_fix_float((v0[0][2]+A/C*v0[0][0]+B/C*v0[0][1])*(OGPU_DEPTH_DEPTH)); //c=Pz+A/C*Px+B/C*Py , where P=v0
}

static void ogpu_quad_depth_test(ogpu_bit clock,
                            //INPUTS
                            struct ogpu_quad quad,
                            ogpu_bit depth_test,
                            struct ogpu_depth_coef coef,
                            //OUTPUTS
                             ogpu_bit *depth_ready,
                             struct ogpu_depth_quad *depth_quad
                                 )
{
    static ogpu_bit _clock=0;
    static ogpu_bit _depth_test=0;
    //////////////////
    //     COMB     //
    //////////////////

    //////////////////
    //     SEQ      //
    //////////////////
    if(clock!=_clock)
    {
        _clock=clock;
        if(clock)      //CLOCK RISING EDGE
        {
            if(depth_test != _depth_test) // RISING EDGE
            {
                _depth_test = depth_test;
                if(depth_test)
                {
                    depth_quad->m[0]=coef.a*quad.m[0][0]+coef.b*quad.m[0][1]+coef.c;
                    depth_quad->m[1]=coef.a*quad.m[1][0]+coef.b*quad.m[1][1]+coef.c;
                    depth_quad->m[2]=coef.a*quad.m[2][0]+coef.b*quad.m[2][1]+coef.c;
                    depth_quad->m[3]=coef.a*quad.m[3][0]+coef.b*quad.m[3][1]+coef.c;
                    *depth_ready=1;
                }
                else
                {
                    *depth_ready=0;
                }
            }
        }
        else      //CLOCK FALLING EDGE
        {
        }
    }
}

static int ogpu_quad_buffer_alloc(struct ogpu_quad_buffer *quad_buffer,const struct ogpu_tile tile)
{
//    static uint32_t _size = 0;
//    uint32_t size=((tile.x1-tile.x0)/2+1)*((tile.y1-tile.y0)/2+1);
//    if(_size==0 || quad_buffer->b==0)
//    {
//        quad_buffer->b = (struct ogpu_quad_buffer_cell*)malloc(sizeof(struct ogpu_quad_buffer_cell)*size);
//        if(quad_buffer->b == 0) return -1; //memory allocation failed
//    }
//    else
//    {
//        if(size>_size)
//        {
//            _size=size;
//            quad_buffer->b = (struct ogpu_quad_buffer_cell*)realloc(quad_buffer->b,sizeof(struct ogpu_quad_buffer_cell)*size);
//            if(quad_buffer->b == 0) return -2; //memory allocation failed
//        }
//
//    }
//    quad_buffer->n=0;
//    quad_buffer->tile=tile;
//    return 0;
    return -1;
}

static void ogpu_quad_buffer_free(struct ogpu_quad_buffer *quad_buffer)
{
    //if(quad_buffer->b) free(quad_buffer->b);
    //quad_buffer->b=0;
}

static inline float ogpu_float_fix(int32_t x)
{
    return ((float)x)/((long)(1<<31)-2);
}

static void ogpu_quad_store(ogpu_bit clock,
                                 //INPUTS
                                 ogpu_bit (*quad_mask)[4],
                                 struct ogpu_quad quad,
                                 ogpu_bit start_raster,
                                 ogpu_bit store_quad,
                                 struct ogpu_tile tile,
                                 struct ogpu_depth_quad depth_quad,
                                 //OUTPUTS
                                 ogpu_bit *quad_stored,
                                 struct ogpu_quad_buffer *quad_buffer
                                 )
{
    static ogpu_bit _clock=0;
    static ogpu_bit _start_raster=0;
    static ogpu_bit _store_quad=0;
    static uint16_t _quad_counter=0;
    //////////////////
    //     COMB     //
    //////////////////

    //////////////////
    //     SEQ      //
    //////////////////
    if(start_raster!=_start_raster)
    {
        _start_raster=start_raster;
        if(start_raster) // Rising edge
        {
            _quad_counter=0;
        }
    }
    if(clock!=_clock)
    {
        _clock=clock;
        if(clock)      //CLOCK RISING EDGE
        {
            if(store_quad != _store_quad)
            {
                _store_quad = store_quad;
                if(store_quad) // RISING EDGE
                {
                    quad_buffer->b[_quad_counter].x=quad.m[0][0];
                    quad_buffer->b[_quad_counter].y=quad.m[0][1];
                    quad_buffer->b[_quad_counter].mask=
                        quad_mask[0][0]<<0|quad_mask[0][1]<<1|
                        quad_mask[0][2]<<2|quad_mask[0][3]<<3;
                    quad_buffer->b[_quad_counter].stencil[0]=0;
                    quad_buffer->b[_quad_counter].stencil[1]=0;
                    quad_buffer->b[_quad_counter].stencil[2]=0;
                    quad_buffer->b[_quad_counter].stencil[3]=0;
                    quad_buffer->b[_quad_counter].depth[0]=ogpu_float_fix(depth_quad.m[0]);
                    quad_buffer->b[_quad_counter].depth[1]=ogpu_float_fix(depth_quad.m[1]);
                    quad_buffer->b[_quad_counter].depth[2]=ogpu_float_fix(depth_quad.m[2]);
                    quad_buffer->b[_quad_counter].depth[3]=ogpu_float_fix(depth_quad.m[3]);
                    quad_buffer->tile=tile;

                    _quad_counter++;
                    quad_buffer->n=_quad_counter;
                    *quad_stored=1;
                }
                else
                {
                    *quad_stored=0;
                }
            }
        }
        else      //CLOCK FALLING EDGE
        {
        }
    }
}

enum OGPU_RASTER_CONTROL_STATE
{
    OGPU_RASTER_CONTROL_IDLE=0,
    OGPU_RASTER_CONTROL_DONE,
    OGPU_RASTER_CONTROL_SETUP,
    OGPU_RASTER_CONTROL_QUAD_GEN,
    OGPU_RASTER_CONTROL_QUAD_TEST,
    OGPU_RASTER_CONTROL_STORE_QUAD
};

static void ogpu_raster_control(ogpu_bit clock,
                                //INPUTS
                                ogpu_command cmd,
                                ogpu_bit setup_done,
                                ogpu_bit end_tile,
                                ogpu_bit quad_ready,
                                ogpu_bit depth_ready,
                                ogpu_bit quad_stored,
                                ogpu_bit draw_quad,
                                ogpu_bit discard_quad,
                                //OUTPUTS
                                ogpu_bit *start_raster,
                                ogpu_bit *next_quad,
                                ogpu_bit *edge_test,
                                ogpu_bit *depth_test,
                                ogpu_bit *store_quad,
                                ogpu_bit *busy,
                                ogpu_bit *done
                                )
{
    static unsigned _state=OGPU_RASTER_CONTROL_IDLE;
    static ogpu_bit _clock=0;
    if(clock!=_clock)
    {
        _clock=clock;
        if(clock)      //CLOCK RISING EDGE
        {
            // //!//
            // CAUTION: 'if' order matters, because it does part of logic in some state transitions
            switch(_state)
            {
            default:
            case OGPU_RASTER_CONTROL_IDLE:
                if(cmd==OGPU_CMD_RASTER)
                {
                    _state=OGPU_RASTER_CONTROL_SETUP;
                    *busy=1;
                    *done=0;
                    *start_raster=1;
                    break;
                }
                break;

            case OGPU_RASTER_CONTROL_DONE:
                if(cmd==OGPU_CMD_PREPARE)
                {
                    _state=OGPU_RASTER_CONTROL_IDLE;
                    *done=0;
                    *busy=0;
                    *start_raster=0;
                    *next_quad=0;
                    *edge_test=0;
                    *depth_test=0;
                    *store_quad=0;
                    break;
                }
                break;

            case OGPU_RASTER_CONTROL_SETUP:
                if(setup_done)
                {
                    _state=OGPU_RASTER_CONTROL_QUAD_GEN;
                    *next_quad=1;
                    *store_quad=0;
                    *edge_test=0;
                    *depth_test=0;
                    break;
                }
                break;

            case OGPU_RASTER_CONTROL_QUAD_GEN:
                if(quad_ready)
                {
                    _state=OGPU_RASTER_CONTROL_QUAD_TEST;
                    *next_quad=0;
                    *edge_test=1;
                    *depth_test=1;
                    break;
                }
                if(end_tile)
                {
                    _state=OGPU_RASTER_CONTROL_DONE;
                    *done=1;
                    *busy=0;
                    *start_raster=0;
                    *next_quad=0;
                    *edge_test=0;
                    *depth_test=0;
                    *store_quad=0;
                    break;
                }
                break;

            case OGPU_RASTER_CONTROL_QUAD_TEST:
                if(draw_quad && depth_ready)
                {
                    _state=OGPU_RASTER_CONTROL_STORE_QUAD;
                    *store_quad=1;
                    break;
                }
                if(end_tile)
                {
                    _state=OGPU_RASTER_CONTROL_DONE;
                    *done=1;
                    *busy=0;
                    *start_raster=0;
                    *next_quad=0;
                    *edge_test=0;
                    *depth_test=0;
                    *store_quad=0;
                    break;
                }
                if(discard_quad)
                {
                    _state=OGPU_RASTER_CONTROL_QUAD_GEN;
                    *next_quad=1;
                    *store_quad=0;
                    *edge_test=0;
                    *depth_test=0;
                    break;
                }
                break;

            case OGPU_RASTER_CONTROL_STORE_QUAD:
                if(end_tile)
                {
                    _state=OGPU_RASTER_CONTROL_DONE;
                    *done=1;
                    *busy=0;
                    *start_raster=0;
                    *next_quad=0;
                    *edge_test=0;
                    *depth_test=0;
                    *store_quad=0;
                    break;
                }
                if(quad_stored)
                {
                    _state=OGPU_RASTER_CONTROL_QUAD_GEN;
                    *next_quad=1;
                    *store_quad=0;
                    *edge_test=0;
                    *depth_test=0;
                    break;
                }
                break;
            }
        }
        else      //CLOCK FALLING EDGE
        {
        }
    }
}

//HARDWARE FUNCTIONS FOR FPGA DE1 SoC
static int ipow(int base, int exp)
{
    int result = 1;
    while (exp)
    {
        if (exp & 1)
            result *= base;
        exp >>= 1;
        base *= base;
    }

    return result;
}

//Seven Seg display: 0,1,2,3,4,5,6,7,8,9,A,B,C,D,E,F,-, .
uint8_t _seven_mask[]={
		0x3F, //0
		0x06, //1
		0x5B, //2
		0x4F, //3
		0x66, //4
		0x6D, //5
		0x7D, //6
		0x07, //7
		0x7F, //8
		0x6F, //9
		0x77, //A
		0x7c, //B
		0x39, //C
		0x5E, //D
		0x79, //E
		0x71, //F
		0x40, //-
		0x00};//

static uint8_t d2ss(int value,int digit) //returns seven segment 'digit' configuration according entered 'value'
{
	uint8_t res=0xFF;
	if(digit<0)
	{
		if(value<0) res=_seven_mask[16];
		else res=_seven_mask[17];
	}
	else if(digit<100)
	{
		uint8_t n;
		if(value>0) n=(value%ipow(10,digit+1))/ipow(10,digit);
		else n=17;

		res=_seven_mask[n];
	}
	res=~res;
	return res;
}

static uint8_t h2ss(int value,int digit) //returns seven segment 'digit' configuration according entered hexadecimal 'value'
{
	uint8_t res=0xFF;
	if(digit<0)
	{
		if(value<0) res=_seven_mask[16];
		else res=_seven_mask[17];
	}
	else if(digit<100)
	{
		uint8_t n;
		if(value>0) n=(value%ipow(10,digit+1))/ipow(10,digit);
		else n=17;

		res=_seven_mask[n];
	}
	res=~res;
	return res;
}
//

/**
 * Do triangle rasterization using OPENGPU VIRTUAL RASTERIZER.
 */
void
ogpu_raster_tri(struct setup_context *setup,
             const float (*v0)[4],
             const float (*v1)[4],
             const float (*v2)[4])
{
	////SOFTPIPE RASTERIZER SETUP FUNCTIONS
	//TODO: REPLACE THIS FUNCTIONS WITH CUSTOM ONES
	struct quad_stage *pipe = setup->softpipe->quad.first;

	float det;

	uint layer = 0;
	unsigned viewport_index = 0;

	if (setup->softpipe->no_rast || setup->softpipe->rasterizer->rasterizer_discard)
	  return;

	det = calc_det(v0, v1, v2);

	if (!setup_sort_vertices( setup, det, v0, v1, v2 ))
	  return;

	setup_tri_coefficients( setup );

	if (setup->softpipe->layer_slot > 0) {
	  layer = *(unsigned *)setup->vprovoke[setup->softpipe->layer_slot];
	  layer = MIN2(layer, setup->max_layer);
	}

	if (setup->softpipe->viewport_index_slot > 0) {
	  unsigned *udata = (unsigned*)v0[setup->softpipe->viewport_index_slot];
	  viewport_index = sp_clamp_viewport_idx(*udata);
	}
	////
//TEST DE1
	void *virtual_base,*h2f_virtual_base;
	int fd;
	//int loop_count;
	static int led_direction=0;
	static int led_mask=0x1;
	void *h2p_lw_led_addr,*h2p_lw_sw_addr;
	void *seven_seg_addr[6];
	void *r1_req,*r1_data_high,*r1_data_low,*r1_ack;
	void *r1_reset;
	void *r1_command;
	void *r1_v0x,*r1_v0y,*r1_v0z;
	void *r1_v1x,*r1_v1y,*r1_v1z;
	void *r1_v2x,*r1_v2y,*r1_v2z;
	void *r1_clip_rect0,*r1_clip_rect1;
	void *r1_tile0,*r1_tile1,*r1_depth_coef_a,*r1_depth_coef_b,*r1_depth_coef_c;
	void *r1_quad_buffer_addr_high,*r1_quad_buffer_addr_low;
	void *r1_status;

	// map the address space for the LED registers into user space so we can interact with them.
	// we'll actually map in the entire CSR span of the HPS since we want to access various registers within that span

	if( ( fd = open( "/dev/mem", ( O_RDWR | O_SYNC ) ) ) == -1 ) {
		printf( "ERROR: could not open \"/dev/mem\"...\n" );
		return;
	}

	virtual_base = mmap( NULL, HW_REGS_SPAN, ( PROT_READ | PROT_WRITE ), MAP_SHARED, fd, HW_REGS_BASE );

	if( virtual_base == MAP_FAILED ) {
		printf( "ERROR: mmap() failed...\n" );
		close( fd );
		return;
	}

	h2p_lw_led_addr=( unsigned long  )virtual_base + ( ( unsigned long  )( ALT_LWFPGASLVS_OFST + LED_PIO_BASE ) & ( unsigned long)( HW_REGS_MASK ) );
	h2p_lw_sw_addr=( unsigned long  )virtual_base + ( ( unsigned long  )( ALT_LWFPGASLVS_OFST + DIPSW_PIO_BASE ) & ( unsigned long)( HW_REGS_MASK ) );
	seven_seg_addr[0]=( unsigned long  )virtual_base + ( ( unsigned long  )( ALT_LWFPGASLVS_OFST + SEVEN_SEG_0_BASE ) & ( unsigned long)( HW_REGS_MASK ) );
	seven_seg_addr[1]=( unsigned long  )virtual_base + ( ( unsigned long  )( ALT_LWFPGASLVS_OFST + SEVEN_SEG_1_BASE ) & ( unsigned long)( HW_REGS_MASK ) );
	seven_seg_addr[2]=( unsigned long  )virtual_base + ( ( unsigned long  )( ALT_LWFPGASLVS_OFST + SEVEN_SEG_2_BASE ) & ( unsigned long)( HW_REGS_MASK ) );
	seven_seg_addr[3]=( unsigned long  )virtual_base + ( ( unsigned long  )( ALT_LWFPGASLVS_OFST + SEVEN_SEG_3_BASE ) & ( unsigned long)( HW_REGS_MASK ) );
	seven_seg_addr[4]=( unsigned long  )virtual_base + ( ( unsigned long  )( ALT_LWFPGASLVS_OFST + SEVEN_SEG_4_BASE ) & ( unsigned long)( HW_REGS_MASK ) );
	seven_seg_addr[5]=( unsigned long  )virtual_base + ( ( unsigned long  )( ALT_LWFPGASLVS_OFST + SEVEN_SEG_5_BASE ) & ( unsigned long)( HW_REGS_MASK ) );


	h2f_virtual_base = mmap( NULL, HW_FPGA_AXI_SPAN, ( PROT_READ | PROT_WRITE ), MAP_SHARED, fd,ALT_AXI_FPGASLVS_OFST );
	if( h2f_virtual_base == MAP_FAILED ) {
		if( munmap( h2f_virtual_base, HW_FPGA_AXI_SPAN ) != 0 ) {
				printf( "ERROR: munmap() failed...\n" );
				close( fd );
				return;
		}
		printf( "ERROR: mmap() failed for h2f mapping...\n" );
		close( fd );
		return;
	}

	r1_reset=( unsigned long  )h2f_virtual_base + ( ( unsigned long  )( OGPU_RESET_BASE ));
	r1_req=( unsigned long  )h2f_virtual_base + ( ( unsigned long  )( OGPU_QUAD_STORE_REQ_BASE ));
	r1_data_high=( unsigned long  )h2f_virtual_base + ( ( unsigned long  )( OGPU_QUAD_STORE_DATA_HIGH_BASE ));
	r1_data_low=( unsigned long  )h2f_virtual_base + ( ( unsigned long  )( OGPU_QUAD_STORE_DATA_LOW_BASE ));
	r1_ack=( unsigned long  )h2f_virtual_base + ( ( unsigned long  )( OGPU_QUAD_STORE_ACK_BASE ));

	r1_command=( unsigned long  )h2f_virtual_base + ( ( unsigned long  )( OGPU_RASTER_UNIT_COMMAND_BASE ));
	r1_v0x=( unsigned long  )h2f_virtual_base + ( ( unsigned long  )( OGPU_RASTER_UNIT_V0X_BASE ));
	r1_v0y=( unsigned long  )h2f_virtual_base + ( ( unsigned long  )( OGPU_RASTER_UNIT_V0Y_BASE ));
	r1_v0z=( unsigned long  )h2f_virtual_base + ( ( unsigned long  )( OGPU_RASTER_UNIT_V0Z_BASE ));
	r1_v1x=( unsigned long  )h2f_virtual_base + ( ( unsigned long  )( OGPU_RASTER_UNIT_V1X_BASE ));
	r1_v1y=( unsigned long  )h2f_virtual_base + ( ( unsigned long  )( OGPU_RASTER_UNIT_V1Y_BASE ));
	r1_v1z=( unsigned long  )h2f_virtual_base + ( ( unsigned long  )( OGPU_RASTER_UNIT_V1Z_BASE ));
	r1_v2x=( unsigned long  )h2f_virtual_base + ( ( unsigned long  )( OGPU_RASTER_UNIT_V2X_BASE ));
	r1_v2y=( unsigned long  )h2f_virtual_base + ( ( unsigned long  )( OGPU_RASTER_UNIT_V2Y_BASE ));
	r1_v2z=( unsigned long  )h2f_virtual_base + ( ( unsigned long  )( OGPU_RASTER_UNIT_V2Z_BASE ));
	r1_clip_rect0=( unsigned long  )h2f_virtual_base + ( ( unsigned long  )( OGPU_RASTER_UNIT_CLIP_RECT0_BASE ));
	r1_clip_rect1=( unsigned long  )h2f_virtual_base + ( ( unsigned long  )( OGPU_RASTER_UNIT_CLIP_RECT1_BASE ));
	r1_tile0=( unsigned long  )h2f_virtual_base + ( ( unsigned long  )( OGPU_RASTER_UNIT_TILE0_BASE ));
	r1_tile1=( unsigned long  )h2f_virtual_base + ( ( unsigned long  )( OGPU_RASTER_UNIT_TILE1_BASE ));
	r1_depth_coef_a=( unsigned long  )h2f_virtual_base + ( ( unsigned long  )( OGPU_RASTER_UNIT_DEPTH_COEF_A_BASE ));
	r1_depth_coef_b=( unsigned long  )h2f_virtual_base + ( ( unsigned long  )( OGPU_RASTER_UNIT_DEPTH_COEF_B_BASE ));
	r1_depth_coef_c=( unsigned long  )h2f_virtual_base + ( ( unsigned long  )( OGPU_RASTER_UNIT_DEPTH_COEF_C_BASE ));
	r1_quad_buffer_addr_high=( unsigned long  )h2f_virtual_base + ( ( unsigned long  )( OGPU_RASTER_UNIT_QUAD_BUFFER_ADDR_HIGH_BASE ));
	r1_quad_buffer_addr_low=( unsigned long  )h2f_virtual_base + ( ( unsigned long  )( OGPU_RASTER_UNIT_QUAD_BUFFER_ADDR_LOW_BASE ));
	r1_status=( unsigned long  )h2f_virtual_base + ( ( unsigned long  )( OGPU_RASTER_UNIT_STATUS_BASE ));



	int sw = alt_read_hword(h2p_lw_sw_addr) & 0x3FF;
	static unsigned f_counter = 0;

	f_counter++;

	if(sw&4) // information ON
	{
		static clock_t t0=0;
		double seconds = ((double)(clock()-t0))/CLOCKS_PER_SEC;
		if(seconds>0.25)
		{
			unsigned fps=(unsigned)((f_counter/seconds)*1000);
			t0=clock();

			if(sw&2) //Total frame number after switching to this mode
			{
				alt_write_word(seven_seg_addr[0],d2ss(f_counter,0));
				alt_write_word(seven_seg_addr[1],d2ss(f_counter,1));
				alt_write_word(seven_seg_addr[2],d2ss(f_counter,2));
				alt_write_word(seven_seg_addr[3],d2ss(f_counter,3));
				alt_write_word(seven_seg_addr[4],d2ss(f_counter,4));
				alt_write_word(seven_seg_addr[5],d2ss(f_counter,5));
			}
			else //FPS
			{

				alt_write_word(seven_seg_addr[0],d2ss(fps,0));
				alt_write_word(seven_seg_addr[1],d2ss(fps,1));
				alt_write_word(seven_seg_addr[2],d2ss(fps,2));
				alt_write_word(seven_seg_addr[3],d2ss(-1,0));
				alt_write_word(seven_seg_addr[4],d2ss(fps,3));
				alt_write_word(seven_seg_addr[5],d2ss(fps,4));
				f_counter=0;
			}
		}

		// control led
		if(sw&1) *(uint32_t *)h2p_lw_led_addr = led_mask;
		else *(uint32_t *)h2p_lw_led_addr = ~led_mask;

		// update led mask
		if (led_direction == 0){
			led_mask <<= 1;
			if (led_mask == (0x01 << (LED_PIO_DATA_WIDTH-1)))
				 led_direction = 1;
		}else{
			led_mask >>= 1;
			if (led_mask == 0x01){
				led_direction = 0;
			}
		}
	}


	if(sw&1) //if sw0 is one, do OGPU HARDWARE APPROACH
	{
		static struct ogpu_box box;
		static struct ogpu_tile tile;

		uint8_t command;
		uint16_t v0x=ogpu_ufix_float(v0[0][0]),
				 v0y=ogpu_ufix_float(v0[0][1]),
				 v0z=ogpu_ufix_float(v0[0][2]);
		uint16_t v1x=ogpu_ufix_float(v1[0][0]),
				 v1y=ogpu_ufix_float(v1[0][1]),
				 v1z=ogpu_ufix_float(v1[0][2]);
		uint16_t v2x=ogpu_ufix_float(v2[0][0]),
				 v2y=ogpu_ufix_float(v2[0][1]),
				 v2z=ogpu_ufix_float(v2[0][2]);
		uint32_t clip_rect0,clip_rect1,tile0,tile1;
		box.x0=setup->softpipe->cliprect[viewport_index].minx;
		box.y0=setup->softpipe->cliprect[viewport_index].miny;
		box.x1=setup->softpipe->cliprect[viewport_index].maxx;
		box.y1=setup->softpipe->cliprect[viewport_index].maxy;
		//printf("clip_rect: p0(%d,%d) p1(%d,%d)\n",x0,y0,x1,y1);
		clip_rect0=(ogpu_fix_float(box.x0)<<16)|(ogpu_fix_float(box.y0)&0xFFFF);//x0|y0
		clip_rect1=(ogpu_fix_float(box.x1)<<16)|(ogpu_fix_float(box.y1)&0xFFFF);//x1|y1

		tile.x0=setup->softpipe->cliprect[viewport_index].minx;
		tile.y0=setup->softpipe->cliprect[viewport_index].miny;
		tile.x1=tile.x0+62;
		tile.y1=tile.y0+62;
		tile0=(tile.x0<<16)|(tile.y0&0xFFFF);//x0|y0 tile of 64x64 pixels
		tile1=(tile.x1<<16)|(tile.y1&0xFFFF);//x1|y1
		//printf("tile: p0(%x) p1(%x)\n",tile0,tile1);

		alt_write_word(r1_reset,0); // reset gpu(active low)
		alt_write_word(r1_reset,1); // active gpu
		//printf("status: %x\n",alt_read_word(r1_status));
		alt_write_word(r1_v0x,v0x); alt_write_word(r1_v0y,v0y); alt_write_word(r1_v0z,v0z);
		alt_write_word(r1_v1x,v1x); alt_write_word(r1_v1y,v1y); alt_write_word(r1_v1z,v1z);
		alt_write_word(r1_v2x,v2x); alt_write_word(r1_v2y,v2y); alt_write_word(r1_v2z,v2z);
		alt_write_word(r1_clip_rect0,clip_rect0); alt_write_word(r1_clip_rect1,clip_rect1);
		alt_write_word(r1_tile0,tile0); alt_write_word(r1_tile1,tile1);
		//ogpu_depth_coef(v0,v1,v2,&coef);
		alt_write_word(r1_depth_coef_a,0);
		alt_write_word(r1_depth_coef_b,0);
		alt_write_word(r1_depth_coef_c,0);
		alt_write_word(r1_quad_buffer_addr_high,0); alt_write_word(r1_quad_buffer_addr_low,0);

		static struct ogpu_quad_buffer quad_buffer;
	#define OGPU_HW_TILE_SIZE 64 //by now, it's limited to TILE_SIZE in sp_tile_cache.h
		struct ogpu_quad_buffer_cell __qb[OGPU_HW_TILE_SIZE/2*OGPU_HW_TILE_SIZE/2];
		memset((void*)__qb,0,sizeof(struct ogpu_quad_buffer_cell)*OGPU_HW_TILE_SIZE/2*OGPU_HW_TILE_SIZE/2);

		quad_buffer.b=__qb;

		do//TILE LOOP
		{
			//alt_write_word(r1_reset,0); // reset gpu(active low)
			//alt_write_word(r1_reset,1); // active gpu
			quad_buffer.n=0;
			quad_buffer.tile=tile;

			//if(ogpu_quad_buffer_alloc(&quad_buffer,tile)<0){ printf("Memory allocation failed\n"); return;}

			unsigned ibuf=0;
			uint32_t dataH,dataL,st,rt;
			command=0xAA;
			alt_write_word(r1_command,command);
			st=alt_read_word(r1_status);
			while((st&1)==0) //while DONE bit is zero
			{
				st=alt_read_word(r1_status);
				rt=alt_read_word(r1_req);
				if(rt)
				{
					dataH=alt_read_word(r1_data_high);
					dataL=alt_read_word(r1_data_low);
					alt_write_word(r1_ack,1);
					//usleep(10000);
					while(alt_read_word(r1_req)); // while req signal is high
					alt_write_word(r1_ack,0);
					//mem_buf[ibuf++]=(uint64_t)((((uint64_t)dataH)<<32)|(dataL));
					quad_buffer.b[ibuf].x=(uint16_t)(dataH>>16);
					quad_buffer.b[ibuf].y=(uint16_t)(dataH&0xFFFF);
					quad_buffer.b[ibuf].mask=(uint8_t)(dataL&0x0F);
					ibuf++;
					if(ibuf>(OGPU_HW_TILE_SIZE/2*OGPU_HW_TILE_SIZE/2))
					{
						ibuf=0;
						printf("OPENGPU: SETUP.C: hardware approach: '__qb': BUFFER OVERFLOW");
					}
				}
			}
			st=alt_read_word(r1_status);

			quad_buffer.n=ibuf;

			command=0xA5; //PREPARE FOR NEXT RASTER
			alt_write_word(r1_command,command);
			do
			{
				st=alt_read_word(r1_status);
				//printf("status(prepare) (%d us): %x\n",i,s);
				//usleep(i);
				//i+=100;
			}while((st&1)!=0); //while DONE bit is one

		//    printf("e0\tx0:%d y0:%d\tx1:%d y1:%d\n"
		//           "e1\tx0:%d y0:%d\tx1:%d y1:%d\n"
		//           "e2\tx0:%d y0:%d\tx1:%d y1:%d\n",e0.x0,e0.y0,e0.x1,e0.y1,
		//                                            e1.x0,e1.y0,e1.x1,e1.y1,
		//                                            e2.x0,e2.y0,e2.x1,e2.y1);
		//    printf("B0(%.1f,%.1f)\n"
		//           "B1(%.1f,%.1f)\n",box.x0,box.y0,box.x1,box.y1);

			unsigned q,s,m,c;
			#define OGPU_HW_QUAD_SIZE MAX_QUADS
		//    struct quad_header sp_quad[OGPU_SOFT_QUAD_SIZE];
		//    struct quad_header *sp_quad_ptrs[OGPU_SOFT_QUAD_SIZE];
			m=quad_buffer.n;
			c=0;
			s=OGPU_HW_QUAD_SIZE;

			do//MEMORY LOOP -- QUAD BUFFER READING AND SOFTPIPE NEXT STAGE INTERFACING
			{
				if(m<s) s=m;
				for(q=0;q<s;q++,c++)
				{
					setup->quad[q].input.x0=quad_buffer.b[c].x;
					setup->quad[q].input.y0=quad_buffer.b[c].y;

					setup->quad[q].input.layer=layer;
					setup->quad[q].input.viewport_index=viewport_index;
					setup->quad[q].input.coverage[0]=0;
					setup->quad[q].input.coverage[1]=0;
					setup->quad[q].input.coverage[2]=0;
					setup->quad[q].input.coverage[3]=0;
					setup->quad[q].input.facing=setup->facing;
					setup->quad[q].inout.mask=quad_buffer.b[c].mask;
					setup->quad[q].output.depth[0]=0;//quad_buffer.b[c].depth[0];
					setup->quad[q].output.depth[1]=0;//quad_buffer.b[c].depth[1];
					setup->quad[q].output.depth[2]=0;//quad_buffer.b[c].depth[2];
					setup->quad[q].output.depth[3]=0;//quad_buffer.b[c].depth[3];
	//                printf("D%d\t%f\t%f\n\t%f\t%f\n\n",c,quad_buffer.b[c].depth[0],
	//                       quad_buffer.b[c].depth[1],quad_buffer.b[c].depth[2],
	//                       quad_buffer.b[c].depth[3]);
					setup->quad[q].output.stencil[0]=0;//quad_buffer.b[c].stencil[0];
					setup->quad[q].output.stencil[1]=0;//quad_buffer.b[c].stencil[0];
					setup->quad[q].output.stencil[2]=0;//quad_buffer.b[c].stencil[0];
					setup->quad[q].output.stencil[3]=0;//quad_buffer.b[c].stencil[0];
					setup->quad[q].coef=setup->coef;
					setup->quad[q].posCoef=&setup->posCoef;

					setup->quad_ptrs[q]=&setup->quad[q];
				}
				if(s) pipe->run( pipe, setup->quad_ptrs, s );
				m-=s;
			}while(m);
		tile.x0+=64;
		tile.x1=tile.x0+62;
		if(tile.x0>setup->softpipe->cliprect[viewport_index].maxx)
		{
			tile.x0=setup->softpipe->cliprect[viewport_index].minx;
			tile.x1=tile.x0+62;
			tile.y0+=64;
			tile.y1=tile.y0+62;
		}
		tile0=(tile.x0<<16)|(tile.y0&0xFFFF);//x0|y0 tile of 64x64 pixels
		tile1=(tile.x1<<16)|(tile.y1&0xFFFF);//x1|y1
		alt_write_word(r1_tile0,tile0); alt_write_word(r1_tile1,tile1);

		}while(tile.y0<=setup->softpipe->cliprect[viewport_index].maxy);
	}
	else // if sw0 is zero, do software approach
	{
		if(sw&(1<<9)) // if one, ogpu software model
		{


			//    static unsigned counter=0;
			//    printf("Tri %d\nv0\tx:%.1f\ty:%.1f\tz:%.1f\tw:%.1f\n"
			//           "v1\tx:%.1f\ty:%.1f\tz:%.1f\tw:%.1f\n"
			//           "v2\tx:%.1f\ty:%.1f\tz:%.1f\tw:%.1f\n\n",counter++,
			//                                                    v0[0][0],v0[0][1],v0[0][2],v0[0][3],
			//                                                    v1[0][0],v1[0][1],v1[0][2],v1[0][3],
			//                                                    v2[0][0],v2[0][1],v2[0][2],v2[0][3]);
			    static ogpu_bit start_raster=0,next_quad=0,quad_ready=0,end_tile=0,setup_done=0;
			    static ogpu_bit edge_ready[]={0,0,0},edge_test=0;
			    static ogpu_bit edge_mask0[]={0,0,0,0};
			    static ogpu_bit edge_mask1[]={0,0,0,0};
			    static ogpu_bit edge_mask2[]={0,0,0,0};
			    static ogpu_bit clock=0;
			    static ogpu_bit draw_quad=0;
			    static ogpu_bit discard_quad=0;
			    static ogpu_bit quad_mask[]={0,0,0,0};
			    static ogpu_bit depth_ready=0;
			    static ogpu_bit depth_test=0;
			    static ogpu_bit store_quad=0;
			    static ogpu_bit quad_stored=0;
			    static ogpu_bit done=0;
			    static ogpu_bit busy=0;
			    static ogpu_command cmd=OGPU_CMD_RASTER;
			    static struct ogpu_edge e0,e1,e2;
			    static struct ogpu_box box;
			    static struct ogpu_quad quad;
			    static struct ogpu_tile tile;
			    static struct ogpu_depth_coef coef;
			    static struct ogpu_depth_quad depth_quad;
			    static struct ogpu_quad_buffer quad_buffer;
			#define OGPU_TILE_SIZE 64 //by now, it's limited to TILE_SIZE in sp_tile_cache.h
			    struct ogpu_quad_buffer_cell __qb[OGPU_TILE_SIZE/2*OGPU_TILE_SIZE/2];

			    ogpu_depth_coef(v0,v1,v2,&coef);
			    tile.x0=setup->softpipe->cliprect[viewport_index].minx;
			    tile.y0=setup->softpipe->cliprect[viewport_index].miny;

			    box.x0=setup->softpipe->cliprect[viewport_index].minx;
			    box.y0=setup->softpipe->cliprect[viewport_index].miny;
			    box.x1=setup->softpipe->cliprect[viewport_index].maxx;
			    box.y1=setup->softpipe->cliprect[viewport_index].maxy;

			    tile.x1=tile.x0+62; tile.y1=tile.y0+62;

			    quad_buffer.b=__qb;
			    do//TILE LOOP
			    {
			        clock=0;
			        quad_buffer.n=0;
			        quad_buffer.tile=tile;

			        unsigned _next_raster=1;

			        //if(ogpu_quad_buffer_alloc(&quad_buffer,tile)<0){ printf("Memory allocation failed\n"); return;}
			        do//OGPU LOOP -- behavior algorithm implementation
			        {
			            if(_next_raster)
			            {
			                if(!done)
			                {
			                    cmd=OGPU_CMD_RASTER;
			                    _next_raster=0;
			                }
			                else
			                {
			                    cmd=OGPU_CMD_PREPARE;
			                }
			            }
			            ogpu_raster_control(clock,cmd,setup_done,end_tile,quad_ready,depth_ready,quad_stored,
			                                    draw_quad,discard_quad,
			                                &start_raster,&next_quad,&edge_test,&depth_test,&store_quad,
			                                    &busy,&done);

			            ogpu_setup(clock,start_raster,(const float (*)[2])v0,(const float (*)[2])v1,(const float (*)[2])v2,
			                       &e0,&e1,&e2,&setup_done);
			            ogpu_quad_generator(clock,next_quad,box,tile,
			                                &quad_ready,&end_tile,&quad);

			            ogpu_quad_edge_test(clock,e0,quad,edge_test,&edge_mask0,&edge_ready[0]);
			            ogpu_quad_edge_test(clock,e1,quad,edge_test,&edge_mask1,&edge_ready[1]);
			            ogpu_quad_edge_test(clock,e2,quad,edge_test,&edge_mask2,&edge_ready[2]);
			            ogpu_triangle_edge_test(clock,&edge_ready,&edge_mask0,&edge_mask1,&edge_mask2,
			                                        &quad_mask,&draw_quad,&discard_quad);
			            ogpu_quad_depth_test(clock,quad,depth_test,coef,
			                                 &depth_ready,&depth_quad);

			            ogpu_quad_store(clock,&quad_mask,quad,start_raster,store_quad,tile,depth_quad,
			                            &quad_stored,&quad_buffer);
			            clock^=1;
			        }while(!done || _next_raster);

			    //    printf("e0\tx0:%d y0:%d\tx1:%d y1:%d\n"
			    //           "e1\tx0:%d y0:%d\tx1:%d y1:%d\n"
			    //           "e2\tx0:%d y0:%d\tx1:%d y1:%d\n",e0.x0,e0.y0,e0.x1,e0.y1,
			    //                                            e1.x0,e1.y0,e1.x1,e1.y1,
			    //                                            e2.x0,e2.y0,e2.x1,e2.y1);
			    //    printf("B0(%.1f,%.1f)\n"
			    //           "B1(%.1f,%.1f)\n",box.x0,box.y0,box.x1,box.y1);

			        unsigned q,s,m,c;
			        #define OGPU_SOFT_QUAD_SIZE MAX_QUADS
			    //    struct quad_header sp_quad[OGPU_SOFT_QUAD_SIZE];
			    //    struct quad_header *sp_quad_ptrs[OGPU_SOFT_QUAD_SIZE];
			        m=quad_buffer.n;
			        c=0;
			        s=OGPU_SOFT_QUAD_SIZE;

			        do//MEMORY LOOP -- QUAD BUFFER READING AND SOFTPIPE NEXT STAGE INTERFACING
			        {
			            if(m<s) s=m;
			            for(q=0;q<s;q++,c++)
			            {
			                setup->quad[q].input.x0=quad_buffer.b[c].x;
			                setup->quad[q].input.y0=quad_buffer.b[c].y;

			                setup->quad[q].input.layer=layer;
			                setup->quad[q].input.viewport_index=viewport_index;
			                setup->quad[q].input.coverage[0]=0;
			                setup->quad[q].input.coverage[1]=0;
			                setup->quad[q].input.coverage[2]=0;
			                setup->quad[q].input.coverage[3]=0;
			                setup->quad[q].input.facing=setup->facing;
			                setup->quad[q].inout.mask=quad_buffer.b[c].mask;
			                setup->quad[q].output.depth[0]=0;//quad_buffer.b[c].depth[0];
			                setup->quad[q].output.depth[1]=0;//quad_buffer.b[c].depth[1];
			                setup->quad[q].output.depth[2]=0;//quad_buffer.b[c].depth[2];
			                setup->quad[q].output.depth[3]=0;//quad_buffer.b[c].depth[3];
			//                printf("D%d\t%f\t%f\n\t%f\t%f\n\n",c,quad_buffer.b[c].depth[0],
			//                       quad_buffer.b[c].depth[1],quad_buffer.b[c].depth[2],
			//                       quad_buffer.b[c].depth[3]);
			                setup->quad[q].output.stencil[0]=0;//quad_buffer.b[c].stencil[0];
			                setup->quad[q].output.stencil[1]=0;//quad_buffer.b[c].stencil[0];
			                setup->quad[q].output.stencil[2]=0;//quad_buffer.b[c].stencil[0];
			                setup->quad[q].output.stencil[3]=0;//quad_buffer.b[c].stencil[0];
			                setup->quad[q].coef=setup->coef;
			                setup->quad[q].posCoef=&setup->posCoef;

			                setup->quad_ptrs[q]=&setup->quad[q];
			            }
			            if(s) pipe->run( pipe, setup->quad_ptrs, s );
			            m-=s;
			        }while(m);
			    tile.x0+=64;
			    tile.x1=tile.x0+62;
			    if(tile.x0>=setup->softpipe->cliprect[viewport_index].maxx)
			    {
			        tile.x0=0;
			        tile.x1=tile.x0+62;
			        tile.y0+=64;
			        tile.y1=tile.y0+62;
			    }
			    }while(tile.y0<setup->softpipe->cliprect[viewport_index].maxy);

			    //ogpu_quad_buffer_free(&quad_buffer);
		}
		else sp_setup_tri(setup,v0,v1,v2); // if sw9 is zero, use softpipe original function
	}
	if( munmap( virtual_base, HW_REGS_SPAN ) != 0 ) {
			if( munmap( h2f_virtual_base, HW_FPGA_AXI_SPAN ) != 0 ) {
					printf( "ERROR: h2f munmap() failed...\n" );
					close( fd );
					return;
			}
			printf( "ERROR: munmap() failed...\n" );
			close( fd );
			return;
		}
		else
		{
			if( munmap( h2f_virtual_base, HW_FPGA_AXI_SPAN ) != 0 ) {
					printf( "ERROR: h2f munmap() failed...\n" );
					close( fd );
					return;
			}
		}

		close( fd );
	//TEST DE1 END-----
}
//--OPENGPU
