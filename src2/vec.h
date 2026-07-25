#ifndef VEC_H
#define VEC_H
typedef struct Vec2f {
  float x;
  float y;
} Vec2f;
typedef struct Vec3f {
  float x;
  float y;
  float z;
} Vec3f;
typedef struct Vec4f {
  float x;
  float y;
  float z;
  float w;
} Vec4f;
Vec2f vec2f_add(Vec2f a, Vec2f b) { return (Vec2f){a.x + b.x, a.y + b.y}; }
Vec3f vec3f_add(Vec3f a, Vec3f b) {
  return (Vec3f){a.x + b.x, a.y + b.y, a.z + b.z};
}
Vec4f vec4f_add(Vec4f a, Vec4f b) {
  return (Vec4f){a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}
Vec2f vec2f_scale(Vec2f a, float x) { return (Vec2f){a.x * x, a.y * x}; }
Vec3f vec3f_scale(Vec3f a, float x) {
  return (Vec3f){a.x * x, a.y * x, a.z * x};
}
Vec4f vec4f_scale(Vec4f a, float x) {
  return (Vec4f){a.x * x, a.y * x, a.z * x, a.w * x};
}
float vec3f_dot(Vec3f a, Vec3f b){
	return (a.x*b.x+a.y*b.y+a.z*b.z);
}
Vec3f vec3f_sub(Vec3f a, Vec3f b) {
  return (Vec3f){a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec3f vec3f_cross(Vec3f a, Vec3f b) {
  return (Vec3f){a.y * b.z - a.z * b.y,
                a.z * b.x - a.x * b.z,
                a.x * b.y - a.y * b.x};
}
typedef struct Vec2d {
  double x;
  double y;
} Vec2d;
typedef struct Vec3d {
  double x;
  double y;
  double z;
} Vec3d;
typedef struct Vec4d {
  double x;
  double y;
  double z;
  double w;
} Vec4d;
Vec2d vec2d_add(Vec2d a, Vec2d b) { return (Vec2d){a.x + b.x, a.y + b.y}; }
Vec3d vec3d_add(Vec3d a, Vec3d b) {
  return (Vec3d){a.x + b.x, a.y + b.y, a.z + b.z};
}
Vec4d vec4d_add(Vec4d a, Vec4d b) {
  return (Vec4d){a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}
Vec2d vec2d_scale(Vec2d a, double x) { return (Vec2d){a.x * x, a.y * x}; }
Vec3d vec3d_scale(Vec3d a, double x) {
  return (Vec3d){a.x * x, a.y * x, a.z * x};
}
Vec4d vec4d_scale(Vec4d a, double x) {
  return (Vec4d){a.x * x, a.y * x, a.z * x, a.w * x};
}
double vec3d_dot(Vec3d a, Vec3d b){
	return (a.x*b.x+a.y*b.y+a.z*b.z);
}
Vec3d vec3d_sub(Vec3d a, Vec3d b) {
  return (Vec3d){a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec3d vec3d_cross(Vec3d a, Vec3d b) {
  return (Vec3d){a.y * b.z - a.z * b.y,
                a.z * b.x - a.x * b.z,
                a.x * b.y - a.y * b.x};
}
#endif
