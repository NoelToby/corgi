// Copyright 2015 Google Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "component_library/rendermesh.h"
#include "component_library/animation.h"
#include "component_library/common_services.h"
#include "component_library/transform.h"
#include "library_components_generated.h"
#include "fplbase/mesh.h"
#include "fplbase/utilities.h"

using mathfu::vec3;
using mathfu::mat4;

FPL_ENTITY_DEFINE_COMPONENT(fpl::component_library::RenderMeshComponent,
                            fpl::component_library::RenderMeshData)

namespace fpl {
namespace component_library {

// Offset the frustrum by this many world-units.  As long as no objects are
// larger than this number, they should still all draw, even if their
// registration points technically fall outside our frustrum.
static const float kFrustrumOffset = 10.0f;

void RenderMeshComponent::Init() {
  asset_manager_ =
      entity_manager_->GetComponent<CommonServicesComponent>()->asset_manager();
}

// Rendermesh depends on transform:
void RenderMeshComponent::InitEntity(entity::EntityRef& entity) {
  entity_manager_->AddEntityToComponent<TransformComponent>(entity);
}

void RenderMeshComponent::RenderPrep(const CameraInterface& camera) {
  for (int pass = 0; pass < RenderPass_Count; pass++) {
    pass_render_list_[pass].clear();
  }
  for (auto iter = component_data_.begin(); iter != component_data_.end();
       ++iter) {
    RenderMeshData* rendermesh_data = GetComponentData(iter->entity);
    TransformData* transform_data = Data<TransformData>(iter->entity);

    float max_cos = cos(camera.viewport_angle());
    vec3 camera_facing = camera.facing();
    vec3 camera_position = camera.position();

    // Put each entity into the list for each render pass it is
    // planning on participating in.
    for (int pass = 0; pass < RenderPass_Count; pass++) {
      if (rendermesh_data->pass_mask & (1 << pass)) {
        if (!rendermesh_data->visible) continue;

        // Check to make sure objects are inside the frustrum of our
        // view-cone before we draw:
        vec3 entity_position =
            transform_data->world_transform.TranslationVector3D();
        vec3 pos_relative_to_camera = (entity_position - camera_position) +
                                      camera_facing * kFrustrumOffset;

        // Cache off the distance from the camera because we'll use it
        // later as a depth aproxamation.
        rendermesh_data->z_depth =
            (entity_position - camera.position()).LengthSquared();

        // Are we culling this object based on the view angle?
        // If so, does this lie outside of our view frustrum?
        if ((rendermesh_data->culling_mask & (1 << CullingTest_ViewAngle)) &&
            (vec3::DotProduct(pos_relative_to_camera.Normalized(),
                              camera_facing.Normalized()) < max_cos)) {
          // The origin point for this mesh is not in our field of view.  Cut
          // out early, and don't bother rendering it.
          continue;
        }

        // Are we culling this object based on view distance?  If so,
        // is it far enough away that we should skip it?
        if ((rendermesh_data->culling_mask & (1 << CullingTest_Distance)) &&
            rendermesh_data->z_depth > culling_distance_squared()) {
          continue;
        }

        pass_render_list_[pass]
            .push_back(RenderlistEntry(iter->entity, &iter->data));
      }
    }
  }
  std::sort(pass_render_list_[RenderPass_Opaque].begin(),
            pass_render_list_[RenderPass_Opaque].end());
  std::sort(pass_render_list_[RenderPass_Alpha].begin(),
            pass_render_list_[RenderPass_Alpha].end(),
            std::greater<RenderlistEntry>());
}

void RenderMeshComponent::RenderAllEntities(Renderer& renderer,
                                            const CameraInterface& camera) {
  // Make sure we only draw the front-facing polygons:
  renderer.SetCulling(Renderer::kCullBack);

  // Render the actual game:
  for (int pass = 0; pass < RenderPass_Count; pass++) {
    RenderPass(pass, camera, renderer);
  }
}

// Render a pass.
void RenderMeshComponent::RenderPass(int pass_id, const CameraInterface& camera,
                                     Renderer& renderer) {
  RenderPass(pass_id, camera, renderer, nullptr);
}

// Render a single render-pass, by ID.
void RenderMeshComponent::RenderPass(int pass_id, const CameraInterface& camera,
                                     Renderer& renderer,
                                     const Shader* shader_override) {
  mat4 camera_vp = camera.GetTransformMatrix();

  for (size_t i = 0; i < pass_render_list_[pass_id].size(); i++) {
    entity::EntityRef& entity = pass_render_list_[pass_id][i].entity;

    RenderMeshData* rendermesh_data = Data<RenderMeshData>(entity);

    TransformData* transform_data = Data<TransformData>(entity);

    AnimationData* anim_data = Data<AnimationData>(entity);

    // TODO: anim_data will set uniforms for an array of matricies. Each
    //       matrix represents one bone position.
    const bool has_anim = anim_data != nullptr && anim_data->motivator.Valid();
    const int num_mesh_bones = rendermesh_data->mesh->num_bones();
    const int num_anim_bones =
        has_anim ? anim_data->motivator.DefiningAnim()->NumBones() : 0;
    const bool has_one_bone_anim =
        has_anim && (num_mesh_bones <= 1 || num_anim_bones == 1);
    const mat4 world_transform =
        has_one_bone_anim
            ? transform_data->world_transform *
                  mat4(anim_data->motivator.GlobalTransforms()[0])
            : transform_data->world_transform;

    const mat4 mvp = camera_vp * world_transform;
    const mat4 world_matrix_inverse = world_transform.Inverse();
    renderer.set_light_pos(world_matrix_inverse * light_position_);
    renderer.set_color(rendermesh_data->tint);
    renderer.set_model(world_transform);

    // If the mesh has a skeleton, we need to update the bone positions.
    // The positions are normally supplied by the animation, but if they are
    // not, use the default pose in the RenderMesh.
    if (num_mesh_bones > 1) {
      const bool use_default_pose =
          num_anim_bones != num_mesh_bones || rendermesh_data->default_pose;
      const mathfu::AffineTransform* bone_transforms =
          use_default_pose ? rendermesh_data->mesh->bone_global_transforms()
                           : anim_data->motivator.GlobalTransforms();
      rendermesh_data->mesh->GatherShaderTransforms(
          bone_transforms, rendermesh_data->shader_transforms);
      renderer.SetBoneTransforms(rendermesh_data->shader_transforms,
                                 rendermesh_data->num_shader_transforms);
    }

    if (!camera.IsStereo()) {
      renderer.set_camera_pos(world_matrix_inverse * camera.position());
      renderer.set_model_view_projection(mvp);

      if (!shader_override && rendermesh_data->shader) {
        rendermesh_data->shader->Set(renderer);
      } else {
        shader_override->Set(renderer);
      }

      rendermesh_data->mesh->Render(renderer);
    } else {
      const Shader* shader = nullptr;
      if (!shader_override && rendermesh_data->shader) {
        shader = rendermesh_data->shader;
      } else {
        shader = shader_override;
      }
      vec4i viewport[2] = {camera.viewport(0), camera.viewport(1)};
      mat4 camera_vp_stereo = camera.GetTransformMatrix(1);
      mat4 mvp_matrices[2] = {mvp, camera_vp_stereo * world_transform};
      vec3 camera_positions[2] = {world_matrix_inverse * camera.position(0),
                                  world_matrix_inverse * camera.position(1)};
      rendermesh_data->mesh->RenderStereo(renderer, shader, viewport,
                                          mvp_matrices, camera_positions);
    }
  }
}

void RenderMeshComponent::SetVisibilityRecursively(
    const entity::EntityRef& entity, bool visible) {
  RenderMeshData* rendermesh_data = Data<RenderMeshData>(entity);
  TransformData* transform_data = Data<TransformData>(entity);
  if (transform_data) {
    if (rendermesh_data) {
      rendermesh_data->visible = visible;
    }
    for (auto iter = transform_data->children.begin();
         iter != transform_data->children.end(); ++iter) {
      SetVisibilityRecursively(iter->owner, visible);
    }
  }
}

void RenderMeshComponent::AddFromRawData(entity::EntityRef& entity,
                                         const void* raw_data) {
  auto rendermesh_def = static_cast<const RenderMeshDef*>(raw_data);

  // You need to call asset_manager before you can add from raw data,
  // otherwise it can't load up new meshes!
  assert(asset_manager_ != nullptr);
  assert(rendermesh_def->source_file() != nullptr);
  assert(rendermesh_def->shader() != nullptr);

  RenderMeshData* rendermesh_data = AddEntity(entity);

  rendermesh_data->mesh_filename = rendermesh_def->source_file()->c_str();
  rendermesh_data->shader_filename = rendermesh_def->shader()->c_str();

  rendermesh_data->mesh =
      asset_manager_->LoadMesh(rendermesh_def->source_file()->c_str());
  assert(rendermesh_data->mesh != nullptr);

  // Allocate the array to hold shader default pose's transforms.
  assert(rendermesh_data->shader_transforms == nullptr);
  const uint8_t num_shader_transforms =
      rendermesh_data->mesh->num_shader_bones();
  rendermesh_data->num_shader_transforms = num_shader_transforms;
  if (num_shader_transforms > 0) {
    rendermesh_data->shader_transforms =
        new mathfu::AffineTransform[num_shader_transforms];
    for (uint8_t i = 0; i < num_shader_transforms; ++i) {
      rendermesh_data->shader_transforms[i] = mathfu::kAffineIdentity;
    }
  }

  rendermesh_data->shader =
      asset_manager_->LoadShader(rendermesh_def->shader()->c_str());
  assert(rendermesh_data->shader != nullptr);

  rendermesh_data->visible = rendermesh_def->visible();
  rendermesh_data->default_pose = rendermesh_def->default_pose();

  rendermesh_data->pass_mask = 0;
  if (rendermesh_def->render_pass() != nullptr) {
    for (size_t i = 0; i < rendermesh_def->render_pass()->size(); i++) {
      int render_pass = rendermesh_def->render_pass()->Get(i);
      assert(render_pass < RenderPass_Count);
      rendermesh_data->pass_mask |= 1 << render_pass;
    }
  } else {
    // Anything unspecified is assumed to be opaque.
    rendermesh_data->pass_mask = (1 << RenderPass_Opaque);
  }

  if (rendermesh_def->culling() != nullptr) {
    for (size_t i = 0; i < rendermesh_def->culling()->size(); i++) {
      int culling_test = rendermesh_def->culling()->Get(i);
      assert(culling_test < CullingTest_Count);
      rendermesh_data->culling_mask |= 1 << culling_test;
    }
  }

  // TODO: Load this from a flatbuffer file instead of setting it.
  rendermesh_data->tint = mathfu::kOnes4f;
}

entity::ComponentInterface::RawDataUniquePtr RenderMeshComponent::ExportRawData(
    const entity::EntityRef& entity) const {
  const RenderMeshData* data = GetComponentData(entity);
  if (data == nullptr) return nullptr;

  if (data->mesh_filename == "" || data->shader_filename == "") {
    // If we don't have a mesh filename or a shader, we can't be exported;
    // we were obviously created programatically.
    return nullptr;
  }

  flatbuffers::FlatBufferBuilder fbb;
  bool defaults = entity_manager_->GetComponent<CommonServicesComponent>()
                      ->export_force_defaults();
  fbb.ForceDefaults(defaults);

  auto source_file =
      (data->mesh_filename != "") ? fbb.CreateString(data->mesh_filename) : 0;
  auto shader = (data->shader_filename != "")
                    ? fbb.CreateString(data->shader_filename)
                    : 0;
  std::vector<unsigned char> render_pass_vec;
  for (int i = 0; i < RenderPass_Count; i++) {
    if (data->pass_mask & (1 << i)) {
      render_pass_vec.push_back(i);
    }
  }
  auto render_pass = fbb.CreateVector(render_pass_vec);

  std::vector<unsigned char> culling_mask_vec;
  for (int i = 0; i < CullingTest_Count; i++) {
    if (data->culling_mask & (1 << i)) {
      culling_mask_vec.push_back(i);
    }
  }
  auto culling_mask =
      data->culling_mask ? fbb.CreateVector(culling_mask_vec) : 0;

  RenderMeshDefBuilder builder(fbb);
  if (defaults || source_file.o != 0) {
    builder.add_source_file(source_file);
  }
  if (defaults || shader.o != 0) {
    builder.add_shader(shader);
  }
  if (defaults || render_pass.o != 0) {
    builder.add_render_pass(render_pass);
  }

  if (defaults || culling_mask.o != 0) {
    builder.add_culling(culling_mask);
  }

  fbb.Finish(builder.Finish());
  return fbb.ReleaseBufferPointer();
}

}  // component_library
}  // fpl
