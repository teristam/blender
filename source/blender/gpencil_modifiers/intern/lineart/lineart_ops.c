/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2019 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup editors
 */

#include <stdlib.h>

#include "MEM_guardedalloc.h"

#include "BKE_collection.h"
#include "BKE_context.h"
#include "BKE_global.h"
#include "BKE_gpencil.h"
#include "BKE_report.h"
#include "BKE_scene.h"

#include "DEG_depsgraph_query.h"

#include "BLI_utildefines.h"

#include "WM_api.h"
#include "WM_types.h"

#include "DNA_gpencil_modifier_types.h"
#include "DNA_gpencil_types.h"
#include "DNA_scene_types.h"

#include "UI_resources.h"

#include "ED_lineart.h"

#include "lineart_intern.h"

static void clear_strokes(Object *ob, GpencilModifierData *md, int frame)
{
  if (md->type != eGpencilModifierType_Lineart) {
    return;
  }
  LineartGpencilModifierData *lmd = (LineartGpencilModifierData *)md;
  bGPdata *gpd = ob->data;

  bGPDlayer *gpl = BKE_gpencil_layer_get_by_name(gpd, lmd->target_layer, 1);
  if (!gpl) {
    return;
  }
  bGPDframe *gpf = BKE_gpencil_layer_frame_find(gpl, frame);

  if (!gpf) {
    /* No greasepencil frame found. */
    return;
  }

  BKE_gpencil_layer_frame_delete(gpl, gpf);
}

static bool bake_strokes(Object *ob, Depsgraph *dg, GpencilModifierData *md, int frame)
{
  if (md->type != eGpencilModifierType_Lineart) {
    return false;
  }
  LineartGpencilModifierData *lmd = (LineartGpencilModifierData *)md;
  bGPdata *gpd = ob->data;

  bGPDlayer *gpl = BKE_gpencil_layer_get_by_name(gpd, lmd->target_layer, 1);
  if (!gpl) {
    return false;
  }
  bool only_use_existing_gp_frames = false;
  bGPDframe *gpf = (only_use_existing_gp_frames ?
                        BKE_gpencil_layer_frame_find(gpl, frame) :
                        BKE_gpencil_layer_frame_get(gpl, frame, GP_GETFRAME_ADD_NEW));

  if (!gpf) {
    /* No greasepencil frame created or found. */
    return false;
  }

  ED_lineart_compute_feature_lines(dg, lmd);

  ED_lineart_gpencil_generate_with_type(
      lmd->render_buffer,
      dg,
      ob,
      gpl,
      gpf,
      lmd->source_type,
      lmd->source_type == LRT_SOURCE_OBJECT ? (void *)lmd->source_object :
                                              (void *)lmd->source_collection,
      lmd->level_start,
      lmd->use_multiple_levels ? lmd->level_end : lmd->level_start,
      lmd->target_material ? BKE_gpencil_object_material_index_get(ob, lmd->target_material) : 0,
      lmd->line_types,
      lmd->transparency_flags,
      lmd->transparency_mask,
      lmd->thickness,
      lmd->opacity,
      lmd->pre_sample_length,
      lmd->source_vertex_group,
      lmd->vgname,
      lmd->flags);

  ED_lineart_destroy_render_data(lmd);

  md->mode &= ~(eGpencilModifierMode_Realtime | eGpencilModifierMode_Render);

  return true;
}

typedef struct LineartBakeJob {
  wmWindowManager *wm;
  void *owner;
  short *stop, *do_update;
  float *progress;

  /* C or ob must have one != NULL. */
  bContext *C;
  Object *ob;
  Scene *scene;
  Depsgraph *dg;
  int frame;
  int frame_begin;
  int frame_end;
  int frame_orig;
  int frame_increment;
  bool overwrite_frames;
} LineartBakeJob;

static bool lineart_gpencil_bake_single_target(LineartBakeJob *bj, Object *ob)
{
  bool touched = false;
  if (ob->type != OB_GPENCIL) {
    return false;
  }
  for (int frame = bj->frame_begin; frame <= bj->frame_end; frame += bj->frame_increment) {

    BKE_scene_frame_set(bj->scene, frame);
    BKE_scene_graph_update_for_newframe(bj->dg);

    if (bj->overwrite_frames) {
      LISTBASE_FOREACH (GpencilModifierData *, md, &ob->greasepencil_modifiers) {
        clear_strokes(ob, md, frame);
      }
    }

    LISTBASE_FOREACH (GpencilModifierData *, md, &ob->greasepencil_modifiers) {
      if (bake_strokes(ob, bj->dg, md, frame)) {
        touched = true;
      }
    }

    if (G.is_break) {
      return touched;
    }

    *bj->progress = (float)(frame - bj->frame_begin) / (bj->frame_end - bj->frame_begin);
  }
  return touched;
}

static void lineart_gpencil_bake_startjob(void *customdata,
                                          short *stop,
                                          short *do_update,
                                          float *progress)
{
  LineartBakeJob *bj = (LineartBakeJob *)customdata;
  bj->stop = stop;
  bj->do_update = do_update;
  bj->progress = progress;

  if (bj->ob) {
    /* Which means only bake one line art gpencil object, specified by bj->ob. */
    if (lineart_gpencil_bake_single_target(bj, bj->ob)) {
      DEG_id_tag_update((struct ID *)bj->ob->data, ID_RECALC_GEOMETRY);
      WM_event_add_notifier(bj->C, NC_GPENCIL | ND_DATA | NA_EDITED, bj->ob);
    }
  }
  else {
    CTX_DATA_BEGIN (bj->C, Object *, ob, visible_objects) {
      if (lineart_gpencil_bake_single_target(bj, ob)) {
        DEG_id_tag_update((struct ID *)ob->data, ID_RECALC_GEOMETRY);
        WM_event_add_notifier(bj->C, NC_GPENCIL | ND_DATA | NA_EDITED, ob);
      }
    }
    CTX_DATA_END;
  }

  /* Restore original frame. */
  BKE_scene_frame_set(bj->scene, bj->frame_orig);
  BKE_scene_graph_update_for_newframe(bj->dg);
}

static void lineart_gpencil_bake_endjob(void *customdata)
{
  LineartBakeJob *bj = customdata;

  WM_set_locked_interface(CTX_wm_manager(bj->C), false);

  WM_main_add_notifier(NC_SCENE | ND_FRAME, bj->scene);
  WM_main_add_notifier(NC_GPENCIL | ND_DATA | NA_EDITED, bj->ob);
}

static int lineart_gpencil_bake_common(bContext *C,
                                       wmOperator *op,
                                       bool bake_all_targets,
                                       bool do_background)
{
  LineartBakeJob *bj = MEM_callocN(sizeof(LineartBakeJob), "LineartBakeJob");

  bj->C = C;
  if (!bake_all_targets) {
    bj->ob = CTX_data_active_object(C);
    if (!bj->ob || bj->ob->type != OB_GPENCIL) {
      WM_report(RPT_ERROR, "No active object or active object isn't a GPencil object.");
      return OPERATOR_FINISHED;
    }
  }
  Scene *scene = CTX_data_scene(C);
  bj->scene = scene;
  bj->dg = CTX_data_depsgraph_pointer(C);
  bj->frame_begin = scene->r.sfra;
  bj->frame_end = scene->r.efra;
  bj->frame_orig = scene->r.cfra;
  bj->frame_increment = scene->r.frame_step;
  bj->overwrite_frames = true;

  if (do_background) {
    wmJob *wm_job = WM_jobs_get(CTX_wm_manager(C),
                                CTX_wm_window(C),
                                CTX_data_scene(C),
                                "Line Art",
                                WM_JOB_PROGRESS,
                                WM_JOB_TYPE_LINEART);

    WM_jobs_customdata_set(wm_job, bj, MEM_freeN);
    WM_jobs_timer(wm_job, 0.1, NC_GPENCIL | ND_DATA | NA_EDITED, NC_GPENCIL | ND_DATA | NA_EDITED);
    WM_jobs_callbacks(
        wm_job, lineart_gpencil_bake_startjob, NULL, NULL, lineart_gpencil_bake_endjob);

    WM_set_locked_interface(CTX_wm_manager(C), true);

    WM_jobs_start(CTX_wm_manager(C), wm_job);

    WM_event_add_modal_handler(C, op);

    return OPERATOR_RUNNING_MODAL;
  }
  else {
    float pseduo_progress;
    lineart_gpencil_bake_startjob(bj, NULL, NULL, &pseduo_progress);

    MEM_freeN(bj);

    return OPERATOR_FINISHED;
  }
}

static int lineart_gpencil_bake_strokes_all_targets_invoke(bContext *C,
                                                           wmOperator *op,
                                                           const wmEvent *UNUSED(event))
{
  return lineart_gpencil_bake_common(C, op, true, true);
}
static int lineart_gpencil_bake_strokes_all_targets_exec(bContext *C, wmOperator *op)
{
  return lineart_gpencil_bake_common(C, op, true, false);
}
static int lineart_gpencil_bake_strokes_invoke(bContext *C,
                                               wmOperator *op,
                                               const wmEvent *UNUSED(event))
{
  return lineart_gpencil_bake_common(C, op, false, true);
}
static int lineart_gpencil_bake_strokes_exec(bContext *C, wmOperator *op)
{
  return lineart_gpencil_bake_common(C, op, false, false);

  return OPERATOR_FINISHED;
}
static int lineart_gpencil_bake_strokes_commom_modal(bContext *C,
                                                     wmOperator *op,
                                                     const wmEvent *UNUSED(event))
{
  Scene *scene = (Scene *)op->customdata;

  /* no running blender, remove handler and pass through */
  if (0 == WM_jobs_test(CTX_wm_manager(C), scene, WM_JOB_TYPE_LINEART)) {
    return OPERATOR_FINISHED | OPERATOR_PASS_THROUGH;
  }

  return OPERATOR_PASS_THROUGH;
}

static int lineart_gpencil_clear_strokes_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  if (ob->type != OB_GPENCIL) {
    return OPERATOR_CANCELLED;
  }
  LISTBASE_FOREACH (GpencilModifierData *, md, &ob->greasepencil_modifiers) {
    if (md->type != eGpencilModifierType_Lineart) {
      continue;
    }
    LineartGpencilModifierData *lmd = (LineartGpencilModifierData *)md;
    bGPdata *gpd = ob->data;

    bGPDlayer *gpl = BKE_gpencil_layer_get_by_name(gpd, lmd->target_layer, 1);
    if (!gpl) {
      continue;
    }
    BKE_gpencil_free_frames(gpl);
  }

  BKE_report(op->reports, RPT_INFO, "Line art cleared for this target.");

  DEG_id_tag_update((struct ID *)ob->data, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, ob);
  return OPERATOR_FINISHED;
}
static int lineart_gpencil_clear_strokes_all_targets_exec(bContext *C, wmOperator *op)
{
  CTX_DATA_BEGIN (C, Object *, ob, visible_objects) {
    if (ob->type != OB_GPENCIL) {
      continue;
    }
    LISTBASE_FOREACH (GpencilModifierData *, md, &ob->greasepencil_modifiers) {
      if (md->type != eGpencilModifierType_Lineart) {
        continue;
      }
      LineartGpencilModifierData *lmd = (LineartGpencilModifierData *)md;
      bGPdata *gpd = ob->data;

      bGPDlayer *gpl = BKE_gpencil_layer_get_by_name(gpd, lmd->target_layer, 1);
      if (!gpl) {
        continue;
      }
      BKE_gpencil_free_frames(gpl);
    }
    DEG_id_tag_update((struct ID *)ob->data, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, ob);
  }
  CTX_DATA_END;

  BKE_report(op->reports, RPT_INFO, "All line art targets are now cleared.");
  return OPERATOR_FINISHED;
}

/* Bake all line art modifiers on the current object. */
void OBJECT_OT_lineart_bake_strokes(wmOperatorType *ot)
{
  ot->name = "Bake Line Art Strokes";
  ot->description = "Bake Line Art all line art modifier on this object";
  ot->idname = "OBJECT_OT_lineart_bake_strokes";

  ot->invoke = lineart_gpencil_bake_strokes_invoke;
  ot->exec = lineart_gpencil_bake_strokes_exec;
  ot->modal = lineart_gpencil_bake_strokes_commom_modal;
}

/* Bake all lineart objects in the scene. */
void OBJECT_OT_lineart_bake_strokes_all_targets(wmOperatorType *ot)
{
  ot->name = "Bake Line Art For All Targets";
  ot->description = "Bake all Line Art targets in the scene";
  ot->idname = "OBJECT_OT_lineart_bake_strokes_all_targets";

  ot->invoke = lineart_gpencil_bake_strokes_all_targets_invoke;
  ot->exec = lineart_gpencil_bake_strokes_all_targets_exec;
  ot->modal = lineart_gpencil_bake_strokes_commom_modal;
}

/* clear all line art modifiers on the current object. */
void OBJECT_OT_lineart_clear_strokes(wmOperatorType *ot)
{
  ot->name = "Clear Line Art Strokes";
  ot->description = "Clear Line Art grease pencil strokes for this target";
  ot->idname = "OBJECT_OT_lineart_clear_strokes";

  ot->exec = lineart_gpencil_clear_strokes_exec;
}

/* clear all lineart objects in the scene. */
void OBJECT_OT_lineart_clear_strokes_all_targets(wmOperatorType *ot)
{
  ot->name = "Clear All Line Art Strokes";
  ot->description = "Clear all Line Art targets in the scene";
  ot->idname = "OBJECT_OT_lineart_clear_strokes_all";

  ot->exec = lineart_gpencil_clear_strokes_all_targets_exec;
}

void ED_operatortypes_lineart(void)
{
  WM_operatortype_append(OBJECT_OT_lineart_bake_strokes);
  WM_operatortype_append(OBJECT_OT_lineart_bake_strokes_all_targets);
  WM_operatortype_append(OBJECT_OT_lineart_clear_strokes);
  WM_operatortype_append(OBJECT_OT_lineart_clear_strokes_all_targets);
}