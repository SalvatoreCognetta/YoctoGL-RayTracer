//
// Implementation for Yocto/RayTrace.
//

//
// LICENSE:
//
// Copyright (c) 2016 -- 2020 Fabio Pellacini
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//

#include "yocto_raytrace.h"

#include <atomic>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
using namespace std::string_literals;

// -----------------------------------------------------------------------------
// MATH FUNCTIONS
// -----------------------------------------------------------------------------
namespace yocto::raytrace {

// import math symbols for use
using math::abs;
using math::acos;
using math::atan2;
using math::clamp;
using math::cos;
using math::exp;
using math::flt_max;
using math::fmod;
using math::fresnel_conductor;
using math::fresnel_dielectric;
using math::identity3x3f;
using math::invalidb3f;
using math::log;
using math::make_rng;
using math::max;
using math::min;
using math::pif;
using math::pow;
using math::sample_discrete;
using math::sample_discrete_pdf;
using math::sample_uniform;
using math::sample_uniform_pdf;
using math::sin;
using math::sqrt;
using math::zero2f;
using math::zero2i;
using math::zero3f;
using math::zero3i;
using math::zero4f;
using math::zero4i;

}  // namespace yocto::raytrace

// -----------------------------------------------------------------------------
// IMPLEMENTATION FOR SCENE EVALUATION
// -----------------------------------------------------------------------------
namespace yocto::raytrace {

// Check texture size
static vec2i texture_size(const rtr::texture* texture) {
  if (!texture->colorf.empty()) {
    return texture->colorf.size();
  } else if (!texture->colorb.empty()) {
    return texture->colorb.size();
  } else if (!texture->scalarf.empty()) {
    return texture->scalarf.size();
  } else if (!texture->scalarb.empty()) {
    return texture->scalarb.size();
  } else {
    return zero2i;
  }
}

// Evaluate a texture
static vec3f lookup_texture(
    const rtr::texture* texture, const vec2i& ij, bool ldr_as_linear = false) {
  if (!texture->colorf.empty()) {
    return texture->colorf[ij];
  } else if (!texture->colorb.empty()) {
    return ldr_as_linear ? byte_to_float(texture->colorb[ij])
                         : srgb_to_rgb(byte_to_float(texture->colorb[ij]));
  } else if (!texture->scalarf.empty()) {
    return vec3f{texture->scalarf[ij]};
  } else if (!texture->scalarb.empty()) {
    return ldr_as_linear
               ? byte_to_float(vec3b{texture->scalarb[ij]})
               : srgb_to_rgb(byte_to_float(vec3b{texture->scalarb[ij]}));
  } else {
    return {1, 1, 1};
  }
}

// Evaluate a texture
static vec3f eval_texture(const rtr::texture* texture, const vec2f& uv,
    bool ldr_as_linear = false) {
  // get texture
  if (!texture) return {1, 1, 1};

  // get yimg::image width/height
  auto size = texture_size(texture);

  // YOUR CODE GOES HERE
  // get coordinates normalized for tiling
  auto s = fmod(uv.x, 1) * size.x, t = fmod(uv.y, 1) * size.y;
  if (s < 0) s += size.x; if (t < 0) t += size.y;
  // get image coordinates and residuals
  auto i=clamp((int)s,0,size.x-1), j=clamp((int)t,0,size.y-1);
  auto ii = (i + 1) % size.x, jj = (j + 1) % size.y;
  auto u = s - i, v = t - j;
  // handle interpolation
  return lookup_texture(texture, { i, j},ldr_as_linear) * (1 - u) * (1 - v) +
          lookup_texture(texture,{ i, jj},ldr_as_linear) * (1 - u) * v +
          lookup_texture(texture,{ii, j},ldr_as_linear) * u * (1 - v) +
          lookup_texture(texture,{ii, jj},ldr_as_linear) * u * v;
}
static float eval_texturef(const rtr::texture* texture, const vec2f& uv,
    bool ldr_as_linear = false) {
  return eval_texture(texture, uv, ldr_as_linear).x;
}

// Generates a ray from a camera for yimg::image plane coordinate uv and
// the lens coordinates luv.
static ray3f eval_camera(const rtr::camera* camera, const vec2f& image_uv) {
  // YOUR CODE GOES HERE
  auto q = vec3f{camera-> film.x * (0.5f -image_uv.x), 
      camera->film.y *(image_uv.y - 0.5f), camera->lens};
  auto e = vec3f{0}; // setting e to zero because we are creating the origin
  auto d = normalize(-q - e);
  // evaluate the camera ray as per slides
  return ray3f{transform_point(camera->frame, e), transform_direction(camera->frame, d)};
}

// Eval position
static vec3f eval_position(
    const rtr::shape* shape, int element, const vec2f& uv) {
  // YOUR CODE GOES HERE
  if (!shape->triangles.empty()) {
    auto t = shape->triangles[element];
    // interpolate_line
    return interpolate_triangle(shape->positions[t.x],
            shape->positions[t.y], shape->positions[t.z], uv);
  } else if (!shape->lines.empty()) {
    auto l = shape->lines[element];
    return interpolate_line(
      shape->positions[l.x], shape->positions[l.y], uv.x);
  } else if (!shape->points.empty()) {
    return {0, 0, 1};
  } else {
    return {0, 0, 0};
  }
}

// Shape element normal.
static vec3f eval_element_normal(const rtr::shape* shape, int element) {
  if (!shape->triangles.empty()) {
    auto t = shape->triangles[element];
    return triangle_normal(
        shape->positions[t.x], shape->positions[t.y], shape->positions[t.z]);
  } else if (!shape->lines.empty()) {
    auto l = shape->lines[element];
    return line_tangent(shape->positions[l.x], shape->positions[l.y]);
  } else if (!shape->points.empty()) {
    return {0, 0, 1};
  } else {
    return {0, 0, 0};
  }
}

// Eval normal
static vec3f eval_normal(
    const rtr::shape* shape, int element, const vec2f& uv) {
  if (shape->normals.empty()) return eval_element_normal(shape, element);
  // YOUR CODE GOES HERE
  if (!shape->triangles.empty()) {
    auto t = shape->triangles[element];
    // interpolate_linetangent
    return normalize(interpolate_triangle(
      shape->normals[t.x], shape->normals[t.y],
      shape->normals[t.z], uv));
  } else if (!shape->lines.empty()) {
    auto l = shape->lines[element];
    return normalize(interpolate_line(
      shape->normals[l.x], shape->normals[l.y], uv.x));
    // return line_tangent(shape->positions[l.x], shape->positions[l.y]);
  } else if (!shape->points.empty()) {
    return {0, 0, 1};
  } else {
    return {0, 0, 0};
  }
}

// Eval texcoord
static vec2f eval_texcoord(
    const rtr::shape* shape, int element, const vec2f& uv) {
  if (shape->texcoords.empty()) return uv;
  // YOUR CODE GOES HERE
  if (!shape->triangles.empty()) {
    
    auto t = shape->triangles[element];
    // interpolate_line
    return interpolate_triangle(
      shape->texcoords[t.x], shape->texcoords[t.y], shape->texcoords[t.z], uv);
  } else if (!shape->lines.empty()) {
    auto l = shape->lines[element];
    return interpolate_line(
      shape->texcoords[l.x], shape->texcoords[l.y], uv.x);
    // return line_tangent(shape->positions[l.x], shape->positions[l.y]);
  } else if (!shape->points.empty()) {
    return {0, 1};
  } else {
    return {0, 0};
  }
}

// Evaluate all environment color.
static vec3f eval_environment(const rtr::scene* scene, const ray3f& ray) {
  // YOUR CODE GOES HERE
  vec3f ret = vec3f{0,0,0};
  for (auto environment : scene->environments) {
    auto local_dir = transform_direction(
                        inverse(environment->frame), ray.d);
    auto texcoord = vec2f{
                      static_cast<float>(atan2(local_dir.z, local_dir.x) / (2 * yocto::math::pi)),
                      static_cast<float>(acos(clamp(local_dir.y, -1.f, 1.f)) / yocto::math::pi)};
    if (texcoord.x < 0) texcoord.x += 1;
    ret += environment->emission *
            eval_texture(environment->emission_tex, texcoord);
  }
  return ret;
}

}  // namespace yocto::raytrace

// -----------------------------------------------------------------------------
// IMPLEMENTATION FOR SHAPE/SCENE BVH
// -----------------------------------------------------------------------------
namespace yocto::raytrace {

// primitive used to sort bvh entries
struct bvh_primitive {
  bbox3f bbox      = invalidb3f;
  vec3f  center    = zero3f;
  int    primitive = 0;
};

// Splits a BVH node. Returns split position and axis.
static std::pair<int, int> split_middle(
    std::vector<bvh_primitive>& primitives, int start, int end) {
  // initialize split axis and position
  auto axis = 0;
  auto mid  = (start + end) / 2;

  // compute primintive bounds and size
  auto cbbox = invalidb3f;
  for (auto i = start; i < end; i++) cbbox = merge(cbbox, primitives[i].center);
  auto csize = cbbox.max - cbbox.min;
  if (csize == zero3f) return {mid, axis};

  // split along largest
  if (csize.x >= csize.y && csize.x >= csize.z) axis = 0;
  if (csize.y >= csize.x && csize.y >= csize.z) axis = 1;
  if (csize.z >= csize.x && csize.z >= csize.y) axis = 2;

  // split the space in the middle along the largest axis
  mid = (int)(std::partition(primitives.data() + start, primitives.data() + end,
                  [axis, middle = center(cbbox)[axis]](auto& primitive) {
                    return primitive.center[axis] < middle;
                  }) -
              primitives.data());

  // if we were not able to split, just break the primitives in half
  if (mid == start || mid == end) {
    // throw std::runtime_error("bad bvh split");
    mid = (start + end) / 2;
  }

  return {mid, axis};
}

// Maximum number of primitives per BVH node.
const int bvh_max_prims = 4;

// Build BVH nodes
static void build_bvh(
    std::vector<bvh_node>& nodes, std::vector<bvh_primitive>& primitives) {
  // prepare to build nodes
  nodes.clear();
  nodes.reserve(primitives.size() * 2);

  // queue up first node
  auto queue = std::deque<vec3i>{{0, 0, (int)primitives.size()}};
  nodes.emplace_back();

  // create nodes until the queue is empty
  while (!queue.empty()) {
    // grab node to work on
    auto next = queue.front();
    queue.pop_front();
    auto nodeid = next.x, start = next.y, end = next.z;

    // grab node
    auto& node = nodes[nodeid];

    // compute bounds
    node.bbox = invalidb3f;
    for (auto i = start; i < end; i++)
      node.bbox = merge(node.bbox, primitives[i].bbox);

    // split into two children
    if (end - start > bvh_max_prims) {
      // get split
      auto [mid, axis] = split_middle(primitives, start, end);

      // make an internal node
      node.internal = true;
      node.axis     = axis;
      node.num      = 2;
      node.start    = (int)nodes.size();
      nodes.emplace_back();
      nodes.emplace_back();
      queue.push_back({node.start + 0, start, mid});
      queue.push_back({node.start + 1, mid, end});
    } else {
      // Make a leaf node
      node.internal = false;
      node.num      = end - start;
      node.start    = start;
    }
  }

  // cleanup
  nodes.shrink_to_fit();
}

static void init_bvh(rtr::shape* shape, const trace_params& params) {
  // build primitives
  auto primitives = std::vector<bvh_primitive>{};
  if (!shape->points.empty()) {
    for (auto idx = 0; idx < shape->points.size(); idx++) {
      auto& p             = shape->points[idx];
      auto& primitive     = primitives.emplace_back();
      primitive.bbox      = point_bounds(shape->positions[p], shape->radius[p]);
      primitive.center    = center(primitive.bbox);
      primitive.primitive = idx;
    }
  } else if (!shape->lines.empty()) {
    for (auto idx = 0; idx < shape->lines.size(); idx++) {
      auto& l         = shape->lines[idx];
      auto& primitive = primitives.emplace_back();
      primitive.bbox = line_bounds(shape->positions[l.x], shape->positions[l.y],
          shape->radius[l.x], shape->radius[l.y]);
      primitive.center    = center(primitive.bbox);
      primitive.primitive = idx;
    }
  } else if (!shape->triangles.empty()) {
    for (auto idx = 0; idx < shape->triangles.size(); idx++) {
      auto& primitive = primitives.emplace_back();
      auto& t         = shape->triangles[idx];
      primitive.bbox  = triangle_bounds(
          shape->positions[t.x], shape->positions[t.y], shape->positions[t.z]);
      primitive.center    = center(primitive.bbox);
      primitive.primitive = idx;
    }
  }

  // build nodes
  if (shape->bvh) delete shape->bvh;
  shape->bvh = new bvh_tree{};
  build_bvh(shape->bvh->nodes, primitives);

  // set bvh primitives
  shape->bvh->primitives.reserve(primitives.size());
  for (auto& primitive : primitives) {
    shape->bvh->primitives.push_back(primitive.primitive);
  }
}

void init_bvh(rtr::scene* scene, const trace_params& params,
    progress_callback progress_cb) {
  // handle progress
  auto progress = vec2i{0, 1 + (int)scene->shapes.size()};

  // shapes
  for (auto idx = 0; idx < scene->shapes.size(); idx++) {
    if (progress_cb) progress_cb("build shape bvh", progress.x++, progress.y);
    init_bvh(scene->shapes[idx], params);
  }

  // handle progress
  if (progress_cb) progress_cb("build scene bvh", progress.x++, progress.y);

  // instance bboxes
  auto primitives = std::vector<bvh_primitive>{};
  auto object_id  = 0;
  for (auto object : scene->objects) {
    auto& primitive = primitives.emplace_back();
    primitive.bbox =
        object->shape->bvh->nodes.empty()
            ? invalidb3f
            : transform_bbox(object->frame, object->shape->bvh->nodes[0].bbox);
    primitive.center    = center(primitive.bbox);
    primitive.primitive = object_id++;
  }

  // build nodes
  if (scene->bvh) delete scene->bvh;
  scene->bvh = new bvh_tree{};
  build_bvh(scene->bvh->nodes, primitives);

  // set bvh primitives
  scene->bvh->primitives.reserve(primitives.size());
  for (auto& primitive : primitives) {
    scene->bvh->primitives.push_back(primitive.primitive);
  }

  // handle progress
  if (progress_cb) progress_cb("build bvh", progress.x++, progress.y);
}

// Intersect ray with a bvh->
static bool intersect_shape_bvh(rtr::shape* shape, const ray3f& ray_,
    int& element, vec2f& uv, float& distance, bool find_any) {
  // get bvh and shape pointers for fast access
  auto bvh = shape->bvh;

  // check empty
  if (bvh->nodes.empty()) return false;

  // node stack
  int  node_stack[128];
  auto node_cur          = 0;
  node_stack[node_cur++] = 0;

  // shared variables
  auto hit = false;

  // copy ray to modify it
  auto ray = ray_;

  // prepare ray for fast queries
  auto ray_dinv  = vec3f{1 / ray.d.x, 1 / ray.d.y, 1 / ray.d.z};
  auto ray_dsign = vec3i{(ray_dinv.x < 0) ? 1 : 0, (ray_dinv.y < 0) ? 1 : 0,
      (ray_dinv.z < 0) ? 1 : 0};

  // walking stack
  while (node_cur) {
    // grab node
    auto& node = bvh->nodes[node_stack[--node_cur]];

    // intersect bbox
    // if (!intersect_bbox(ray, ray_dinv, ray_dsign, node.bbox)) continue;
    if (!intersect_bbox(ray, ray_dinv, node.bbox)) continue;

    // intersect node, switching based on node type
    // for each type, iterate over the the primitive list
    if (node.internal) {
      // for internal nodes, attempts to proceed along the
      // split axis from smallest to largest nodes
      if (ray_dsign[node.axis]) {
        node_stack[node_cur++] = node.start + 0;
        node_stack[node_cur++] = node.start + 1;
      } else {
        node_stack[node_cur++] = node.start + 1;
        node_stack[node_cur++] = node.start + 0;
      }
    } else if (!shape->points.empty()) {
      for (auto idx = node.start; idx < node.start + node.num; idx++) {
        auto& p = shape->points[shape->bvh->primitives[idx]];
        if (intersect_point(
                ray, shape->positions[p], shape->radius[p], uv, distance)) {
          hit      = true;
          element  = shape->bvh->primitives[idx];
          ray.tmax = distance;
        }
      }
    } else if (!shape->lines.empty()) {
      for (auto idx = node.start; idx < node.start + node.num; idx++) {
        auto& l = shape->lines[shape->bvh->primitives[idx]];
        if (intersect_line(ray, shape->positions[l.x], shape->positions[l.y],
                shape->radius[l.x], shape->radius[l.y], uv, distance)) {
          hit      = true;
          element  = shape->bvh->primitives[idx];
          ray.tmax = distance;
        }
      }
    } else if (!shape->triangles.empty()) {
      for (auto idx = node.start; idx < node.start + node.num; idx++) {
        auto& t = shape->triangles[shape->bvh->primitives[idx]];
        if (intersect_triangle(ray, shape->positions[t.x],
                shape->positions[t.y], shape->positions[t.z], uv, distance)) {
          hit      = true;
          element  = shape->bvh->primitives[idx];
          ray.tmax = distance;
        }
      }
    }

    // check for early exit
    if (find_any && hit) return hit;
  }

  return hit;
}

// Intersect ray with a bvh->
static bool intersect_scene_bvh(const rtr::scene* scene, const ray3f& ray_,
    int& object, int& element, vec2f& uv, float& distance, bool find_any,
    bool non_rigid_frames) {
  // get bvh and scene pointers for fast access
  auto bvh = scene->bvh;

  // check empty
  if (bvh->nodes.empty()) return false;

  // node stack
  int  node_stack[128];
  auto node_cur          = 0;
  node_stack[node_cur++] = 0;

  // shared variables
  auto hit = false;

  // copy ray to modify it
  auto ray = ray_;

  // prepare ray for fast queries
  auto ray_dinv  = vec3f{1 / ray.d.x, 1 / ray.d.y, 1 / ray.d.z};
  auto ray_dsign = vec3i{(ray_dinv.x < 0) ? 1 : 0, (ray_dinv.y < 0) ? 1 : 0,
      (ray_dinv.z < 0) ? 1 : 0};

  // walking stack
  while (node_cur) {
    // grab node
    auto& node = bvh->nodes[node_stack[--node_cur]];

    // intersect bbox
    // if (!intersect_bbox(ray, ray_dinv, ray_dsign, node.bbox)) continue;
    if (!intersect_bbox(ray, ray_dinv, node.bbox)) continue;

    // intersect node, switching based on node type
    // for each type, iterate over the the primitive list
    if (node.internal) {
      // for internal nodes, attempts to proceed along the
      // split axis from smallest to largest nodes
      if (ray_dsign[node.axis]) {
        node_stack[node_cur++] = node.start + 0;
        node_stack[node_cur++] = node.start + 1;
      } else {
        node_stack[node_cur++] = node.start + 1;
        node_stack[node_cur++] = node.start + 0;
      }
    } else {
      for (auto idx = node.start; idx < node.start + node.num; idx++) {
        auto object_ = scene->objects[scene->bvh->primitives[idx]];
        auto inv_ray = transform_ray(
            inverse(object_->frame, non_rigid_frames), ray);
        if (intersect_shape_bvh(
                object_->shape, inv_ray, element, uv, distance, find_any)) {
          hit      = true;
          object   = scene->bvh->primitives[idx];
          ray.tmax = distance;
        }
      }
    }

    // check for early exit
    if (find_any && hit) return hit;
  }

  return hit;
}

// Intersect ray with a bvh->
static bool intersect_instance_bvh(const rtr::object* object, const ray3f& ray,
    int& element, vec2f& uv, float& distance, bool find_any,
    bool non_rigid_frames) {
  auto inv_ray = transform_ray(inverse(object->frame, non_rigid_frames), ray);
  return intersect_shape_bvh(
      object->shape, inv_ray, element, uv, distance, find_any);
}

intersection3f intersect_scene_bvh(const rtr::scene* scene, const ray3f& ray,
    bool find_any, bool non_rigid_frames) {
  auto intersection = intersection3f{};
  intersection.hit  = intersect_scene_bvh(scene, ray, intersection.object,
      intersection.element, intersection.uv, intersection.distance, find_any,
      non_rigid_frames);
  return intersection;
}
intersection3f intersect_instance_bvh(const rtr::object* object,
    const ray3f& ray, bool find_any, bool non_rigid_frames) {
  auto intersection = intersection3f{};
  intersection.hit  = intersect_instance_bvh(object, ray, intersection.element,
      intersection.uv, intersection.distance, find_any, non_rigid_frames);
  return intersection;
}

}  // namespace yocto::raytrace

// -----------------------------------------------------------------------------
// IMPLEMENTATION FOR PATH TRACING
// -----------------------------------------------------------------------------
namespace yocto::raytrace {

// Raytrace renderer.
static vec4f trace_raytrace(const rtr::scene* scene, const ray3f& ray,
    int bounce, rng_state& rng, const trace_params& params) {
  // YOUR CODE GOES HERE

  // intersect next point
  auto intersection = intersect_scene_bvh(scene, ray);
  if(!intersection.hit)
    return {eval_environment(scene, ray),1};

  // evaluate geometry
  auto object = scene->objects[intersection.object];
  // get position
  auto position = transform_point(object->frame,
                   eval_position(object->shape, intersection.element, intersection.uv));
  // get normal
  auto normal = transform_direction(object->frame,
    eval_normal(object->shape, intersection.element, intersection.uv));
  // get textcoords
  auto texcoord = eval_texcoord(object->shape, intersection.element, intersection.uv);

  texcoord = {fmod(texcoord.x,1), fmod(texcoord.y,1)};

  // normal corrections
  auto outgoing = -ray.d;
  if (dot(normal,outgoing) < 0)
    normal = -normal;

  // evaluate material
  auto emission = object->material->emission *
                  eval_texture(object->material->emission_tex, texcoord, false);
  auto specular = object->material->specular *
                  eval_texture(object->material->specular_tex, texcoord, true).x;
  auto metallic = object->material->metallic *
                  eval_texture(object->material->metallic_tex, texcoord, true).x;
  auto roughness = object->material->roughness *
                   eval_texture(object->material->roughness_tex, texcoord, true).x;
                   
  auto color = object->material->color *
                eval_texture(object->material->color_tex, texcoord, true);

  // auto ior  = object->material->ior;
  // auto coat = object->material->coat *
  //             eval_texture(object->material->coat_tex, texcoord, true).x;
  auto transmission = object->material->transmission *
                      eval_texture(object->material->emission_tex, texcoord, true).x;
  auto opacity = object->material->opacity *
                 mean(eval_texture(object->material->opacity_tex, texcoord, true));
  auto thin = object->material->thin || !object->material->transmission;

  // handle opacity
  if(rand1f(rng) > opacity) {
    position = normalize(position);

    return trace_raytrace(scene, ray3f{position, outgoing},
                          bounce+1, rng, params);
  }

  // accumulate emission
  auto radiance = emission; //object->material->emission;

  // exit if enough bounces are done
  if(bounce >= params.bounces) return {radiance,1};

  // compute indirect illumination
  // transmission -> polished dielectric
  if (object->material->transmission) { 
    // <handle polished dielectrics> 
    if(rand1f(rng) < fresnel_schlick(vec3f{0.04}, normal, outgoing).x) {
      auto incoming = reflect(outgoing, normal);
      auto ray_temp = trace_raytrace(scene, ray3f{position, incoming},
                    bounce+1, rng, params);
      vec3f shade = {ray_temp.x, ray_temp.y, ray_temp.z};
    
      radiance += shade;
    } else {
      auto incoming = -outgoing;
      auto ray_temp = trace_raytrace(scene, ray3f{position, incoming},
                                    bounce+1, rng, params);
      vec3f shade = {ray_temp.x, ray_temp.y, ray_temp.z};
      radiance += color *shade;
                    
    }
  }
  // metallic && !roughness -> polished metal
  else if (object->material->metallic && !object->material->roughness) {     
    // <handle polished metals> 
    auto incoming = reflect(outgoing, normal);
    auto ray_temp = trace_raytrace(scene, ray3f{position, incoming},
                                bounce+1, rng, params);
    vec3f shade = {ray_temp.x, ray_temp.y, ray_temp.z};
    radiance += fresnel_schlick(color, normal, outgoing)*
                  shade;
  }  
  // metallic &&  roughness -> rough metal 
  // please update roughness as roughness = roughness * roughness;
  else if (object->material->metallic && object->material->roughness) { 
    // <handle rough metals> 
    auto incoming = sample_hemisphere(normal, rand2f(rng));
    auto halfway = normalize(outgoing + incoming);
    auto ray_temp = trace_raytrace(scene, ray3f{position, incoming},
                              bounce+1, rng, params);
    vec3f shade = {ray_temp.x, ray_temp.y, ray_temp.z};
    radiance += (2 * yocto::math::pi) *
              fresnel_schlick(color, halfway, outgoing) *
              microfacet_distribution(roughness, normal, halfway) *
              microfacet_shadowing(roughness,normal,halfway,outgoing,incoming) /
              (4 * dot(normal, outgoing) * dot(normal, incoming)) *
              shade * dot(normal, incoming);
  }
  // specular -> rough plastic 
  else if (object->material->specular) { 
    // <handle rough plastic> 
    auto incoming = sample_hemisphere(normal, rand2f(rng));
    auto halfway = normalize(outgoing + incoming);
    auto ray_temp = trace_raytrace(scene, ray3f{position, incoming},
                                bounce+1, rng, params);
    vec3f shade = {ray_temp.x, ray_temp.y, ray_temp.z};
    radiance += (2 * yocto::math::pi) * (
                color / yocto::math::pi * (1 - fresnel_schlick(vec3f{0.04},halfway,outgoing)) +
                fresnel_schlick(vec3f{0.04}, halfway, outgoing) *
                microfacet_distribution(roughness, normal, halfway) *
                microfacet_shadowing(roughness,normal,halfway,outgoing,incoming) /
                (4 * dot(normal, outgoing) * dot(normal, incoming))) *
                shade * dot(normal, incoming);
  } else { 
    // <handle diffuse> 
    auto incoming = sample_hemisphere(normal, rand2f(rng));
    auto ray_temp = trace_raytrace(scene, ray3f{position, incoming},bounce+1, rng, params);
    vec3f shade = {ray_temp.x, ray_temp.y, ray_temp.z};
    radiance += (2 * math::pi) * (color / math::pi) *
                shade * dot(normal, incoming);
  }

  // done
  return {radiance, 1};
}

// Eyelight for quick previewing.
static vec4f trace_eyelight(const rtr::scene* scene, const ray3f& ray,
    int bounce, rng_state& rng, const trace_params& params) {
  // YOUR CODE GOES HERE
  // intersect next point
  auto intersection = intersect_scene_bvh(scene, ray);
  if(!intersection.hit)
    return {zero3f, 1};
  // evaluate geometry
  auto object = scene->objects[intersection.object];
  auto normal = transform_direction(object->frame,
    eval_normal(object->shape, intersection.element, intersection.uv));
  // evaluate material
  // add simple shading
  return {object->material->color * dot(normal, -ray.d),1};
}

static vec4f trace_normal(const rtr::scene* scene, const ray3f& ray, 
    int bounce, rng_state& rng, const trace_params& params) {
  // YOUR CODE GOES HERE
  // intersect next point
  auto intersection = intersect_scene_bvh(scene, ray);
  if(!intersection.hit)
    return {zero3f, 1};
  // prepare shading point
  auto object = scene->objects[intersection.object];
  auto normal = transform_direction(object->frame,
    eval_normal(object->shape, intersection.element, intersection.uv));
  // return normal as color  
  return {normal * 0.5 + 0.5, 1};
}

static vec4f trace_texcoord(const rtr::scene* scene, const ray3f& ray,
    int bounce, rng_state& rng, const trace_params& params) {
  // YOUR CODE GOES HERE
  // intersect next point
  auto intersection = intersect_scene_bvh(scene, ray);
  if(!intersection.hit)
    return {zero3f, 1};
  // prepare shading point
  auto object = scene->objects[intersection.object];
  // {fmod(ret.x, 1), fmod(ret.y, 1)};
  vec3f tex_coord = {eval_texcoord(object->shape, intersection.element, intersection.uv),0};
  // auto normal = transform_direction(object->frame,tex_coord);
  // return texcoord in xy
  return {fmod(tex_coord.x,1), fmod(tex_coord.y,1),0, 1};
}

static vec4f trace_color(const rtr::scene* scene, const ray3f& ray, 
    int bounce, rng_state& rng, const trace_params& params) {
  // YOUR CODE GOES HERE
  // intersect next point
  auto intersection = intersect_scene_bvh(scene, ray);
  if(!intersection.hit)
    return {zero3f, 1};
  // prepare shading point
  auto object = scene->objects[intersection.object];
  // return color
  return {object->material->color, 1};
}

// Trace a single ray from the camera using the given algorithm.
using shader_func = vec4f (*)(const rtr::scene* scene, const ray3f& ray,
    int bounce, rng_state& rng, const trace_params& params);
static shader_func get_trace_shader_func(const trace_params& params) {
  switch (params.shader) {
    case shader_type::raytrace: return trace_raytrace;
    case shader_type::eyelight: return trace_eyelight;
    case shader_type::normal: return trace_normal;
    case shader_type::texcoord: return trace_texcoord;
    case shader_type::color: return trace_color;
    default: {
      throw std::runtime_error("sampler unknown");
      return nullptr;
    }
  }
}

// Init a sequence of random number generators.
void init_state(rtr::state* state, const rtr::scene* scene,
    const rtr::camera* camera, const trace_params& params) {
  auto image_size =
      (camera->film.x > camera->film.y)
          ? vec2i{params.resolution,
                (int)round(params.resolution * camera->film.y / camera->film.x)}
          : vec2i{
                (int)round(params.resolution * camera->film.x / camera->film.y),
                params.resolution};
  state->pixels.assign(image_size, pixel{});
  state->render.assign(image_size, zero4f);
  auto rng = make_rng(1301081);
  for (auto& pixel : state->pixels) {
    pixel.rng = make_rng(params.seed, rand1i(rng, 1 << 31) / 2 + 1);
  }
}

using std::atomic;
using std::deque;
using std::future;

// Simple parallel for used since our target platforms do not yet support
// parallel algorithms. `Func` takes the integer index.
template <typename Func>
inline void parallel_for(const vec2i& size, Func&& func) {
  auto             futures  = std::vector<std::future<void>>{};
  auto             nthreads = std::thread::hardware_concurrency();
  std::atomic<int> next_idx(0);
  for (auto thread_id = 0; thread_id < nthreads; thread_id++) {
    futures.emplace_back(
        std::async(std::launch::async, [&func, &next_idx, size]() {
          while (true) {
            auto j = next_idx.fetch_add(1);
            if (j >= size.y) break;
            for (auto i = 0; i < size.x; i++) func({i, j});
          }
        }));
  }
  for (auto& f : futures) f.get();
}
template <typename Func>
inline void parallel_for(
    const vec2i& size, std::atomic<bool>* stop, Func&& func) {
  auto             futures  = std::vector<std::future<void>>{};
  auto             nthreads = std::thread::hardware_concurrency();
  std::atomic<int> next_idx(0);
  for (auto thread_id = 0; thread_id < nthreads; thread_id++) {
    futures.emplace_back(
        std::async(std::launch::async, [&func, &next_idx, size, stop]() {
          while (true) {
            if (stop && *stop) return;
            auto j = next_idx.fetch_add(1);
            if (j >= size.y) break;
            for (auto i = 0; i < size.x; i++) func({i, j});
          }
        }));
  }
  for (auto& f : futures) f.get();
}

/**
 * Main Rendering Loop in function trace_samples():
 * implement the main rendering loop considering only 1 sample (the loop over samples in done in the apps)
 * from the slides this is like the progressive rendering loop but only one sample
 * update the accumulation buffer, number of samples and final image for each pixel of the state object
 * use get_trace_shader_func() to get the shader from the options
 * implement both a simple loop over pixel and a parallel one, as shown in code
 * for each returns value from the shader, clamp its color if above params.clamp
 * 
**/

void loop_over_pixel(rtr::state* state, const rtr::scene* scene,
    const rtr::camera* camera, const trace_params& params) {
      // get current shader
  auto shader = get_trace_shader_func(params);

  auto img = state->render.size();
  for (auto j : common::range(img.y)) {
    for (auto i : common::range(img.x)) {
      auto index = vec2i{i,j};
      // get pixel uv from rng
      auto puv = math::rand2f(state->pixels[index].rng);
      auto uv = (vec2f{index} +  puv) / vec2f(img);
      // get camera ray
      // auto ray = camera_ray(camera->frame, camera->lens, camera->film, uv);
      auto ray = eval_camera(camera, uv);
      // call shader
      auto shaded = shader(scene, ray, 0, state->pixels[index].rng, params);
      // clamp to max value
      shaded = clamp(shaded, 0.f, params.clamp);
      // update state accumulation, samples and render
      state->pixels[index].accumulated += shaded;
      state->pixels[index].samples     += 1;
      state->render[index] = state->pixels[index].accumulated / state->pixels[index].samples;
    }
  }
}

// Progressively compute an image by calling trace_samples multiple times.
void trace_samples(rtr::state* state, const rtr::scene* scene,
    const rtr::camera* camera, const trace_params& params) {
  
  // check if we run in parallel or not
  if (params.noparallel) {
    // loop over image pixels   
    loop_over_pixel(state, scene, camera, params);
  } else {
    parallel_for(
      state->render.size(), [state, scene, camera, &params](const vec2i& ij) {
        loop_over_pixel(state, scene, camera, params);
      });
  }
}

void trace_samples(rtr::state* state, const rtr::scene* scene,
    const rtr::camera* camera, const trace_params& params,
    std::atomic<bool>* stop) {
  // If you want, you can implement it here and make it stop when the stop flag is set
  return trace_samples(state, scene, camera, params);
}

}  // namespace yocto::raytrace

// -----------------------------------------------------------------------------
// SCENE CREATION
// -----------------------------------------------------------------------------
namespace yocto::raytrace {

// cleanup
shape::~shape() {
  if (bvh) delete bvh;
}

// cleanup
scene::~scene() {
  if (bvh) delete bvh;
  for (auto camera : cameras) delete camera;
  for (auto object : objects) delete object;
  for (auto shape : shapes) delete shape;
  for (auto material : materials) delete material;
  for (auto texture : textures) delete texture;
  for (auto environment : environments) delete environment;
}

// Add element
rtr::camera* add_camera(rtr::scene* scene) {
  return scene->cameras.emplace_back(new camera{});
}
rtr::texture* add_texture(rtr::scene* scene) {
  return scene->textures.emplace_back(new texture{});
}
rtr::shape* add_shape(rtr::scene* scene) {
  return scene->shapes.emplace_back(new shape{});
}
rtr::material* add_material(rtr::scene* scene) {
  return scene->materials.emplace_back(new material{});
}
rtr::object* add_object(rtr::scene* scene) {
  return scene->objects.emplace_back(new object{});
}
rtr::environment* add_environment(rtr::scene* scene) {
  return scene->environments.emplace_back(new environment{});
}

// Set cameras
void set_frame(rtr::camera* camera, const frame3f& frame) {
  camera->frame = frame;
}
void set_lens(rtr::camera* camera, float lens, float aspect, float film) {
  camera->lens = lens;
  camera->film = aspect >= 1 ? vec2f{film, film / aspect}
                             : vec2f{film * aspect, film};
}
void set_focus(rtr::camera* camera, float aperture, float focus) {
  camera->aperture = aperture;
  camera->focus    = focus;
}

// Add texture
void set_texture(rtr::texture* texture, const img::image<vec3b>& img) {
  texture->colorb  = img;
  texture->colorf  = {};
  texture->scalarb = {};
  texture->scalarf = {};
}
void set_texture(rtr::texture* texture, const img::image<vec3f>& img) {
  texture->colorb  = {};
  texture->colorf  = img;
  texture->scalarb = {};
  texture->scalarf = {};
}
void set_texture(rtr::texture* texture, const img::image<byte>& img) {
  texture->colorb  = {};
  texture->colorf  = {};
  texture->scalarb = img;
  texture->scalarf = {};
}
void set_texture(rtr::texture* texture, const img::image<float>& img) {
  texture->colorb  = {};
  texture->colorf  = {};
  texture->scalarb = {};
  texture->scalarf = img;
}

// Add shape
void set_points(rtr::shape* shape, const std::vector<int>& points) {
  shape->points = points;
}
void set_lines(rtr::shape* shape, const std::vector<vec2i>& lines) {
  shape->lines = lines;
}
void set_triangles(rtr::shape* shape, const std::vector<vec3i>& triangles) {
  shape->triangles = triangles;
}
void set_positions(rtr::shape* shape, const std::vector<vec3f>& positions) {
  shape->positions = positions;
}
void set_normals(rtr::shape* shape, const std::vector<vec3f>& normals) {
  shape->normals = normals;
}
void set_texcoords(rtr::shape* shape, const std::vector<vec2f>& texcoords) {
  shape->texcoords = texcoords;
}
void set_radius(rtr::shape* shape, const std::vector<float>& radius) {
  shape->radius = radius;
}

// Add object
void set_frame(rtr::object* object, const frame3f& frame) {
  object->frame = frame;
}
void set_shape(rtr::object* object, rtr::shape* shape) {
  object->shape = shape;
}
void set_material(rtr::object* object, rtr::material* material) {
  object->material = material;
}

// Add material
void set_emission(rtr::material* material, const vec3f& emission,
    rtr::texture* emission_tex) {
  material->emission     = emission;
  material->emission_tex = emission_tex;
}
void set_color(
    rtr::material* material, const vec3f& color, rtr::texture* color_tex) {
  material->color     = color;
  material->color_tex = color_tex;
}
void set_specular(
    rtr::material* material, float specular, rtr::texture* specular_tex) {
  material->specular     = specular;
  material->specular_tex = specular_tex;
}
void set_metallic(
    rtr::material* material, float metallic, rtr::texture* metallic_tex) {
  material->metallic     = metallic;
  material->metallic_tex = metallic_tex;
}
void set_ior(rtr::material* material, float ior) { material->ior = ior; }
void set_transmission(rtr::material* material, float transmission, bool thin,
    float trdepth, rtr::texture* transmission_tex) {
  material->transmission     = transmission;
  material->thin             = thin;
  material->trdepth          = trdepth;
  material->transmission_tex = transmission_tex;
}
void set_thin(rtr::material* material, bool thin) { material->thin = thin; }
void set_roughness(
    rtr::material* material, float roughness, rtr::texture* roughness_tex) {
  material->roughness     = roughness * roughness;
  material->roughness_tex = roughness_tex;
}
void set_opacity(
    rtr::material* material, float opacity, rtr::texture* opacity_tex) {
  material->opacity     = opacity;
  material->opacity_tex = opacity_tex;
}
void set_scattering(rtr::material* material, const vec3f& scattering,
    float scanisotropy, rtr::texture* scattering_tex) {
  material->scattering     = scattering;
  material->scanisotropy   = scanisotropy;
  material->scattering_tex = scattering_tex;
}

// Add environment
void set_frame(rtr::environment* environment, const frame3f& frame) {
  environment->frame = frame;
}
void set_emission(rtr::environment* environment, const vec3f& emission,
    rtr::texture* emission_tex) {
  environment->emission     = emission;
  environment->emission_tex = emission_tex;
}

}  // namespace yocto::raytrace
