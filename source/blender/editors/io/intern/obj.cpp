extern "C" {

#include "BLI_sys_types.h"
#include "BLI_math.h"
#include "BLT_translation.h"

#include "BKE_mesh.h"
#include "BKE_mesh_runtime.h"
#include "BKE_global.h"
#include "BKE_context.h"
#include "BKE_scene.h"
#include "BKE_library.h"
#include "BKE_customdata.h"

#include "DNA_curve_types.h"
#include "DNA_ID.h"
#include "DNA_layer_types.h"
#include "DNA_material_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_space_types.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "WM_api.h"
#include "WM_types.h"

#ifdef WITH_PYTHON
#  include "BPY_extern.h"
#endif

#include "ED_object.h"
#include "bmesh.h"
#include "bmesh_tools.h"

#include "obj.h"
#include "../io_common.h"
#include "../io_obj.h"
}

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "common.hpp"
#include "iterators.hpp"

/*
  TODO someone: () not done, -- done, # maybe add, ? unsure
  --selection only
  --animation
  --apply modifiers
  --render modifiers -- mesh_create_derived_{view,render}, deg_get_mode
  --edges
  --normals
  --uvs
  --materials
  -- path mode
  -- triangulate
  -- nurbs
  --obj objects
  --obj groups
  --scale
  --units
  --removing duplicates with a threshold and as an option
  presets
  axis remap -- doesn't work. Does it need to update, somehow?
    DEG_id_tag_update(&mesh->id, 0); obedit->id.recalc & ID_RECALC_ALL
  smooth groups
  bitflag smooth groups?
  material groups
  polygroups?
  -?vertex order
 */

namespace {

using namespace common;

std::string get_path(const char *const original_path,
                     const char *const ext,
                     const bool export_animations = false,
                     const int frame = 0)
{
  std::string path = original_path;
  size_t start_pos = path.rfind(".obj");
  if (start_pos == std::string::npos) {
    std::cerr << "Invalid file path: " << original_path << '\n';
    return "";
  }
  path.erase(path.begin() + start_pos, path.end());
  if (export_animations) {
    char fr[8];
    std::snprintf(fr, 8, "_%06d", frame);
    path += fr;
  }
  path += ext;
  return path;
}

std::FILE *get_file(const char *const original_path,
                    const char *const ext,
                    const bool export_animations = false,
                    const size_t frame = 0)
{
  std::string path = get_path(original_path, ext, export_animations, frame);
  if (path == "")
    return nullptr;

  std::FILE *file = std::fopen(path.c_str(), "w");
  if (file == nullptr)
    std::cerr << "Couldn't open file: " << path << '\n';

  return file;
}

bool OBJ_export_materials(bContext *C,
                          ExportSettings *settings,
                          std::string mtl_path,
                          std::set<const Material *> materials)
{
#ifdef WITH_PYTHON
  const char *imports[] = {"obj_export", NULL};

  std::stringstream ss;
  ss << "obj_export.write_mtl(\"" << mtl_path << "\", [";
  bool first = true;
  for (const Material *const mat : materials) {
    if (first) {
      ss << '"';
      first = false;
    }
    else
      ss << ", \"";

    ss << (mat->id.name + 2) << '"';
  }
  ss << "], '"
     << path_reference_mode[((OBJExportSettings *)settings->format_specific)->path_mode].identifier
     << "')";
  std::cerr << "Running '" << ss.str() << "'\n";
  return BPY_execute_string(C, imports, ss.str().c_str());
#else
  return false;
#endif
}

bool OBJ_export_curve(bContext *UNUSED(C),
                      ExportSettings *UNUSED(settings),
                      std::FILE *file,
                      const Object *eob,
                      const Curve *cu)
{
  size_t totvert = 0;
  for (const Nurb &nurb : nurbs_of_curve_iter(cu)) {
    size_t deg_order_u = 0;
    if (nurb.type == CU_POLY) {
      deg_order_u = 1;
    }
    else {
      // Original OBJ exporter says this is tested to be correct...
      deg_order_u = nurb.orderu - 1;
    }

    if (nurb.type == CU_BEZIER) {
      std::cerr << "Warning: BEZIER curve '" << eob->id.name
                << "' skipped. Only Poly and NURBS curves supported\n";
      return true;  // Don't abort all exports, just skip
    }

    if (nurb.pntsv > 1) {
      std::cerr << "Warning: Surface curve '" << eob->id.name
                << "' skipped. Only Poly and NURBS curves supported\n";
      return true;  // Don't abort all exports, just skip
    }

    // If the length of the nurb.bp array is smaller than the degree, we'remissing points
    if ((nurb.pntsv > 0 ? nurb.pntsu * nurb.pntsv : nurb.pntsu) <= deg_order_u) {
      std::cerr << "Warning: Number of points in object '" << eob->id.name
                << "' is lower than it's degree. Skipped. \n";
      return true;  // Don't abort all exports, just skip
    }

    const bool closed = nurb.flagu & CU_NURB_CYCLIC;
    const bool endpoints = !closed && (nurb.flagu & CU_NURB_ENDPOINT);
    size_t num_points = 0;
    // TODO someone Transform, scale and such
    for (const std::array<float, 3> &point : points_of_nurbs_iter(&nurb)) {
      fprintf(file, "v %.6g %.6g %.6g\n", point[0], point[1], point[2]);
      ++num_points;
    }
    totvert += num_points;

    fprintf(file, "g %s\n", common::get_object_name(eob, cu).c_str());
    fprintf(file, "cstype bspline\ndeg %lu\n", deg_order_u);

    size_t real_num_points = num_points;
    if (closed) {
      num_points += deg_order_u;
    }

    fputs("curv 0.0 1.0", file);
    for (int i = 0; i < num_points; ++i) {
      // Relative indexes, reuse vertices if closed
      fprintf(file, " %ld", (long)(i < real_num_points ? -(i + 1) : -(i - real_num_points + 1)));
    }
    fputc('\n', file);

    fputs("parm u", file);
    size_t tot_parm = deg_order_u + 1 + num_points;
    float tot_norm_div = 1.0f / (tot_parm - 1);
    for (int i = 0; i < tot_parm; ++i) {
      float parm = i * tot_norm_div;
      if (endpoints) {
        if (i <= deg_order_u)
          parm = 0;
        else if (i >= num_points)
          parm = 1;
      }
      fprintf(file, " %g", parm);
    }
    fputs("\nend\n", file);
  }
  return true;
}

bool OBJ_export_mesh(bContext *UNUSED(C),
                     ExportSettings *settings,
                     std::FILE *file,
                     const Object *eob,
                     Mesh *mesh,
                     const float mat[4][4],
                     ulong &vertex_total,
                     ulong &uv_total,
                     ulong &no_total,
                     dedup_pair_t<uv_key_t> &uv_mapping_pair /* IN OUT */,
                     dedup_pair_t<no_key_t> &no_mapping_pair /* IN OUT */)
{

  auto &uv_mapping = uv_mapping_pair.second;
  auto &no_mapping = no_mapping_pair.second;

  ulong uv_initial_count = uv_mapping.size();
  ulong no_initial_count = no_mapping.size();

  if (mesh->totvert == 0)
    return true;

  const OBJExportSettings *format_specific = (OBJExportSettings *)settings->format_specific;

  if (format_specific->export_objects_as_objects || format_specific->export_objects_as_groups) {
    std::string name = common::get_object_name(eob, mesh);
    if (format_specific->export_objects_as_objects)
      fprintf(file, "o %s\n", name.c_str());
    else
      fprintf(file, "g %s\n", name.c_str());
  }

  for (const std::array<float, 3> &v : common::transformed_vertex_iter(mesh, mat)) {
    fprintf(file, "v %.6g %.6g %.6g\n", v[0], v[1], v[2]);
  }

  // handles non-existant uvs
  if (settings->export_uvs) {
    // TODO someone Is T47010 still relevant?
    if (format_specific->dedup_uvs)
      for (const std::array<float, 2> &uv :
           common::deduplicated_uv_iter(mesh, uv_total, uv_mapping_pair))
        fprintf(file, "vt %.6g %.6g\n", uv[0], uv[1]);
    else
      for (const std::array<float, 2> &uv : common::uv_iter{mesh})
        fprintf(file, "vt %.6g %.6g\n", uv[0], uv[1]);
  }

  if (settings->export_normals) {
    if (format_specific->dedup_normals)
      for (const std::array<float, 3> &no :
           common::deduplicated_normal_iter{mesh, no_total, no_mapping_pair})
        fprintf(file, "vn %.4g %.4g %.4g\n", no[0], no[1], no[2]);
    else
      for (const std::array<float, 3> &no : common::normal_iter{mesh}) {
        fprintf(file, "vn %.4g %.4g %.4g\n", no[0], no[1], no[2]);
      }
    // auto nos = common::get_normals(mesh);
    // for (const auto &no : nos)
    //   fs << "vn " << no[0] << ' ' << no[1] << ' ' << no[2] << '\n';
  }

  std::cerr << "Totals: " << uv_total << " " << no_total << "\nSizes: " << uv_mapping.size() << " "
            << no_mapping.size() << '\n';

  for (const MPoly &p : common::poly_iter(mesh)) {
    fputc('f', file);
    // Loop index
    int li = p.loopstart;
    for (const MLoop &l : common::loop_of_poly_iter(mesh, p)) {
      ulong vx = vertex_total + l.v + 1;
      ulong uv = 1;
      ulong no = 1;
      if (settings->export_uvs && mesh->mloopuv != nullptr) {
        if (format_specific->dedup_uvs)
          uv = uv_mapping[uv_initial_count + li]->second + 1;
        else
          uv = uv_initial_count + li + 1;
      }
      if (settings->export_normals) {
        if (format_specific->dedup_normals)
          no = no_mapping[no_initial_count + l.v]->second + 1;
        else
          no = no_initial_count + l.v + 1;
      }
      if (settings->export_uvs && settings->export_normals && mesh->mloopuv != nullptr)
        fprintf(file, " %lu/%lu/%lu", vx, uv, no);
      else if (settings->export_uvs && mesh->mloopuv != nullptr)
        fprintf(file, " %lu/%lu", vx, uv);
      else if (settings->export_normals)
        fprintf(file, " %lu//%lu", vx, no);
      else
        fprintf(file, " %lu", vx);
    }
    fputc('\n', file);
  }

  if (settings->export_edges) {
    for (const MEdge &e : common::loose_edge_iter{mesh})
      fprintf(file, "l %lu %lu\n", vertex_total + e.v1, vertex_total + e.v2);
  }

  vertex_total += mesh->totvert;
  uv_total += mesh->mloopuv ? mesh->totloop : 0;
  no_total += mesh->totvert;
  return true;
}

bool OBJ_export_object(bContext *C,
                       ExportSettings *const settings,
                       Scene *scene,
                       const Object *ob,
                       std::FILE *file,
                       ulong &vertex_total,
                       ulong &uv_total,
                       ulong &no_total,
                       dedup_pair_t<uv_key_t> &uv_mapping_pair,
                       dedup_pair_t<no_key_t> &no_mapping_pair)
{
  switch (ob->type) {
    case OB_MESH: {
      struct Mesh *mesh = nullptr;
      float mat[4][4];
      bool needs_free = false;
      needs_free = common::get_final_mesh(settings, scene, ob, &mesh /* OUT */, &mat /* OUT */);

      if (!OBJ_export_mesh(C,
                           settings,
                           file,
                           ob,
                           mesh,
                           mat,
                           vertex_total,
                           uv_total,
                           no_total,
                           uv_mapping_pair,
                           no_mapping_pair))
        return false;

      common::free_mesh(mesh, needs_free);
      return true;
    }
    case OB_CURVE:
    case OB_SURF:
      if (settings->export_curves)
        return OBJ_export_curve(C, settings, file, ob, (const Curve *)ob->data);
      else
        return true;  // Continue
    default:
      // TODO someone Probably abort, it shouldn't be possible to get here (once finished)
      std::cerr << "OBJ Export for the object \"" << ob->id.name << "\" is not implemented\n";
      // BLI_assert(false);
      return true;
  }
}

void OBJ_export_start(bContext *C, ExportSettings *const settings)
{
  common::export_start(C, settings);

  const OBJExportSettings *format_specific = (OBJExportSettings *)settings->format_specific;

  // If not exporting animations, the start and end are the same
  for (int frame = format_specific->start_frame; frame <= format_specific->end_frame; ++frame) {
    std::FILE *obj_file = get_file(
        settings->filepath, ".obj", format_specific->export_animations, frame);
    if (obj_file == nullptr)
      return;

    fprintf(obj_file, "# %s\n# www.blender.org\n", common::get_version_string().c_str());
    BKE_scene_frame_set(settings->scene, frame);
    BKE_scene_graph_update_for_newframe(settings->depsgraph, settings->main);
    Scene *escene = DEG_get_evaluated_scene(settings->depsgraph);
    ulong vertex_total = 0, uv_total = 0, no_total = 0;

    auto uv_mapping_pair = common::make_deduplicate_set<uv_key_t>(
        format_specific->dedup_uvs_threshold);
    auto no_mapping_pair = common::make_deduplicate_set<no_key_t>(
        format_specific->dedup_normals_threshold);

    std::string mtl_path = get_path(
        settings->filepath, ".mtl", format_specific->export_animations, frame);
    std::set<const Material *> materials;

    fprintf(obj_file, "mtllib %s\n", (mtl_path.c_str() + mtl_path.find_last_of("/\\") + 1));

    DEG_OBJECT_ITER_BEGIN (settings->depsgraph,
                           ob,
                           DEG_ITER_OBJECT_FLAG_LINKED_DIRECTLY |
                               DEG_ITER_OBJECT_FLAG_LINKED_VIA_SET | DEG_ITER_OBJECT_FLAG_DUPLI) {
      if (common::should_export_object(settings, ob)) {
        if (!OBJ_export_object(C,
                               settings,
                               escene,
                               ob,
                               obj_file,
                               vertex_total,
                               uv_total,
                               no_total,
                               uv_mapping_pair,
                               no_mapping_pair)) {
          return;
        }

        if (settings->export_materials) {
          auto mi = material_iter(ob);
          materials.insert(mi.begin(), mi.end());
        }
      }
    }
    DEG_OBJECT_ITER_FOR_RENDER_ENGINE_END;

    if (settings->export_materials) {
      if (!OBJ_export_materials(C, settings, mtl_path, materials)) {
        std::cerr << "Couldn't export materials\n";
      }
    }
    fclose(obj_file);
  }
}

bool OBJ_export_end(bContext *C, ExportSettings *const settings)
{
  return common::export_end(C, settings);
}
}  // namespace

extern "C" {
bool OBJ_export(bContext *C, ExportSettings *const settings)
{
  return common::time_export(C, settings, &OBJ_export_start, &OBJ_export_end);
}
}  // extern
