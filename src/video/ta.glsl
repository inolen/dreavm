static const char *ta_vp =
"uniform mat4 u_mvp;\n"

"layout(location = 0) in vec3 attr_xyz;\n"
"layout(location = 1) in vec2 attr_texcoord;\n"
"layout(location = 2) in vec4 attr_color;\n"
"layout(location = 3) in vec4 attr_offset_color;\n"

"out vec4 var_color;\n"
"out vec4 var_offset_color;\n"
"out vec2 var_texcoord;\n"

"void main() {\n"
"  var_color = attr_color;\n"
"  var_offset_color = attr_offset_color;\n"
"  var_texcoord = attr_texcoord;\n"

"  // convert from window space back into ndc space\n"
"  gl_Position = u_mvp * vec4(attr_xyz, 1.0);\n"

"  // specify w so OpenGL applies perspective corrected texture mapping, but\n"
"  // cancel the perspective divide on the xyz, they're already in ndc space\n"
"  float w = 1.0 / attr_xyz.z;\n"
"  gl_Position.xyz *= w;\n"
"  gl_Position.w = w;\n"
"}";

static const char *ta_fp =
"uniform sampler2D u_diffuse;\n"

"in vec4 var_color;\n"
"in vec4 var_offset_color;\n"
"in vec2 var_texcoord;\n"

"layout(location = 0) out vec4 fragcolor;\n"

"void main() {\n"
"  vec4 col = var_color;\n"
"  #ifdef IGNORE_ALPHA\n"
"    col.a = 1.0;\n"
"  #endif\n"
"  #ifdef TEXTURE\n"
"    vec4 tex = texture(u_diffuse, var_texcoord);\n"
"    #ifdef IGNORE_TEXTURE_ALPHA\n"
"      tex.a = 1.0;\n"
"    #endif\n"
"    #ifdef SHADE_DECAL\n"
"      fragcolor = tex;\n"
"    #endif\n"
"    #ifdef SHADE_MODULATE\n"
"      fragcolor.rgb = tex.rgb * col.rgb;\n"
"      fragcolor.a = tex.a;\n"
"    #endif\n"
"    #ifdef SHADE_DECAL_ALPHA\n"
"      fragcolor.rgb = tex.rgb * tex.a + col.rgb * (1 - tex.a);\n"
"      fragcolor.a = col.a;\n"
"    #endif\n"
"    #ifdef SHADE_MODULATE_ALPHA\n"
"      fragcolor = tex * col;\n"
"    #endif\n"
"  #else\n"
"    fragcolor = col;\n"
"  #endif\n"
"  #ifdef OFFSET_COLOR\n"
"    fragcolor.rgb += var_offset_color.rgb;\n"
"  #endif\n"
"}";
