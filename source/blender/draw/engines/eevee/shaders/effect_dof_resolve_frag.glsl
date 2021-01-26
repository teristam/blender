
/* Recombine Pass
 * TODO decription
 */

#pragma BLENDER_REQUIRE(common_utiltex_lib.glsl)
#pragma BLENDER_REQUIRE(effect_dof_lib.glsl)

uniform sampler2D fullResColorBuffer;
uniform sampler2D fullResDepthBuffer;

uniform sampler2D bgColorBuffer;
uniform sampler2D bgWeightBuffer;
uniform sampler2D bgTileBuffer;

uniform sampler2D fgColorBuffer;
uniform sampler2D fgWeightBuffer;
uniform sampler2D fgTileBuffer;

uniform sampler2D holefillColorBuffer;
uniform sampler2D holefillWeightBuffer;

uniform float bokehMaxSize;

in vec4 uvcoordsvar;

out vec4 fragColor;

void dof_slight_focus_gather(float radius, vec4 noise, out vec4 out_color, out float out_weight)
{
  DofGatherData fg_accum = GATHER_DATA_INIT;
  DofGatherData bg_accum = GATHER_DATA_INIT;

  int i_radius = clamp(int(ceil(radius + 0.5)), 1, 4);
  const int resolve_ring_density = 2;
  ivec2 texel = ivec2(gl_FragCoord.xy);

  bool first_ring = true;

  for (int ring = i_radius - 1; ring >= 0; ring--) {
    DofGatherData fg_ring = GATHER_DATA_INIT;
    DofGatherData bg_ring = GATHER_DATA_INIT;

    int ring_distance = ring + 1;
    int ring_sample_count = resolve_ring_density * ring_distance;
    for (int sample_id = 0; sample_id < ring_sample_count; sample_id++) {
      int s = sample_id * (4 / resolve_ring_density) +
              int(noise.y * float((4 - resolve_ring_density) * ring_distance));

      ivec2 offset = dof_square_ring_sample_offset(ring_distance, s);
      float dist = length(vec2(offset));

      /* TODO(fclem) add Bokeh shape support here. */

      DofGatherData pair_data[2];
      for (int i = 0; i < 2; i++) {
        ivec2 sample_texel = texel + ((i == 0) ? offset : -offset);
        float depth = texelFetch(fullResDepthBuffer, sample_texel, 0).r;
        pair_data[i].color = safe_color(texelFetch(fullResColorBuffer, sample_texel, 0));
        pair_data[i].coc = dof_coc_from_zdepth(depth);
        pair_data[i].dist = dist;

        pair_data[i].coc = clamp(pair_data[i].coc, -bokehMaxSize, bokehMaxSize);
      }

      float bordering_radius = dist + 0.5;
      const float isect_mul = 1.0;
      dof_gather_accumulate_sample_pair(
          pair_data, bordering_radius, isect_mul, first_ring, false, false, bg_ring, bg_accum);
      dof_gather_accumulate_sample_pair(
          pair_data, bordering_radius, isect_mul, first_ring, false, true, fg_ring, fg_accum);
    }

    dof_gather_accumulate_sample_ring(
        bg_ring, ring_sample_count * 2, first_ring, false, false, bg_accum);
    dof_gather_accumulate_sample_ring(
        fg_ring, ring_sample_count * 2, first_ring, false, true, fg_accum);

    first_ring = false;
  }

  /* Center sample. */
  float depth = texelFetch(fullResDepthBuffer, texel, 0).r;
  DofGatherData center_data;
  center_data.color = safe_color(texelFetch(fullResColorBuffer, texel, 0));
  center_data.coc = dof_coc_from_zdepth(depth);
  center_data.dist = 0.0;

  /* Slide 38. */
  float bordering_radius = 0.5;

  dof_gather_accumulate_center_sample(center_data, bordering_radius, false, true, fg_accum);
  dof_gather_accumulate_center_sample(center_data, bordering_radius, false, false, bg_accum);

  vec4 bg_col, fg_col;
  float bg_weight, fg_weight;
  vec2 unused_occlusion;

  int total_sample_count = dof_gather_total_sample_count(i_radius, resolve_ring_density);
  dof_gather_accumulate_resolve(total_sample_count, bg_accum, bg_col, bg_weight, unused_occlusion);
  dof_gather_accumulate_resolve(total_sample_count, fg_accum, fg_col, fg_weight, unused_occlusion);

  /* Fix weighting issues on perfectly focus > slight focus transitionning areas. */
  if (abs(center_data.coc) < 0.5) {
    bg_col = center_data.color;
    bg_weight = 1.0;
  }

  /* Alpha Over */
  float alpha = 1.0 - fg_weight;
  out_weight = bg_weight * alpha + fg_weight;
  out_color = bg_col * bg_weight * alpha + fg_col * fg_weight;
  out_color *= safe_rcp(out_weight);
}

void dof_resolve_load_layer(sampler2D color_tex,
                            sampler2D weight_tex,
                            out vec4 out_color,
                            out float out_weight)
{
  ivec2 tx_size = textureSize(color_tex, 0).xy;

  vec2 pixel_co = gl_FragCoord.xy / 2.0;
  vec2 interp = fract(pixel_co);
  ivec2 texel = min(tx_size - 1, ivec2(pixel_co));

  /* Manual bilinear filtering with 0 weight handling. */
  vec4 c[2];
  float w[2];
  for (int i = 0; i < 2; i++) {
    ivec2 t0 = texel + ivec2(0, i);
    ivec2 t1 = texel + ivec2(1, i);
    vec4 c0 = texelFetch(color_tex, t0, 0);
    vec4 c1 = texelFetch(color_tex, t1, 0);
    float w0 = texelFetch(weight_tex, t0, 0).r;
    float w1 = texelFetch(weight_tex, t1, 0).r;

    if (w0 == 0.0) {
      c0 = c1;
      w0 = w1;
    }
    else if (w1 == 0.0) {
      c1 = c0;
      w1 = w0;
    }

    c[i] = mix(c0, c1, interp.x);
    w[i] = mix(w0, w1, interp.x);
  }

  if (w[0] == 0.0) {
    c[0] = c[1];
    w[0] = w[1];
  }
  else if (w[1] == 0.0) {
    c[1] = c[0];
    w[1] = w[0];
  }

  out_color = mix(c[0], c[1], interp.y);
  out_weight = mix(w[0], w[1], interp.y);
}

void main(void)
{
  /* offset coord to avoid correlation with sampling pattern.  */
  vec4 noise = texelfetch_noise_tex(gl_FragCoord.xy + 7.0);

  ivec2 tile_co = ivec2(gl_FragCoord.xy / 16.0);
  CocTile coc_tile = dof_coc_tile_load(fgTileBuffer, bgTileBuffer, tile_co);

  vec4 focus = vec4(0.0);
  float focus_w = 0.0;
  if (coc_tile.fg_slight_focus_max_coc >= 0.5) {
    dof_slight_focus_gather(coc_tile.fg_slight_focus_max_coc, noise, focus, focus_w);
  }
  else {
    focus = safe_color(textureLod(fullResColorBuffer, uvcoordsvar.xy, 0.0));
    if (coc_tile.fg_slight_focus_max_coc == DOF_TILE_FOCUS) {
      /* Tile is full in focus. */
      focus_w = 1.0;
    }
    else /* (coc_tile.fg_slight_focus_max_coc == DOF_TILE_DEFOCUS) */ {
      /* Tile is full in defocus. Use in focus to fill holes if there is no other options. */
      /* FIXME */
      focus_w = 0.0;
    }
  }

  fragColor = vec4(0.0);
  float weight = 0.0;
  vec4 layer_color;
  float layer_weight;

  /* TODO/OPTI(fclem): do not load uneeded layers based on tile prediction. */

  if (!no_holefill_pass) {
    dof_resolve_load_layer(holefillColorBuffer, holefillWeightBuffer, layer_color, layer_weight);
    fragColor = layer_color;
    weight = float(layer_weight > 0.0);
  }

  if (!no_background_pass) {
    dof_resolve_load_layer(bgColorBuffer, bgWeightBuffer, layer_color, layer_weight);
    /* Always prefer background to holefill pass. */
    layer_weight = float(layer_weight > 0.0);
    /* Composite background. */
    fragColor = fragColor * (1.0 - layer_weight) + layer_color * layer_weight;
    weight = weight * (1.0 - layer_weight) + layer_weight;
    fragColor *= safe_rcp(weight);
    /* Fill holes with the composited background. */
    weight = float(weight > 0.0);
  }

  if (!no_slight_focus_pass) {
    /* Composite in focus + slight defocus. */
    fragColor = fragColor * (1.0 - focus_w) + focus * focus_w;
    weight = weight * (1.0 - focus_w) + focus_w;
    fragColor *= safe_rcp(weight);
  }

  if (!no_foreground_pass) {
    dof_resolve_load_layer(fgColorBuffer, fgWeightBuffer, layer_color, layer_weight);
    /* Composite foreground. */
    fragColor = fragColor * (1.0 - layer_weight) + layer_color * layer_weight;
  }

  /* Fix float precision issue in alpha compositing.  */
  if (fragColor.a > 0.99) {
    fragColor.a = 1.0;
  }

#if 0 /* Debug */
  if (coc_tile.fg_slight_focus_max_coc >= 0.5) {
    fragColor.rgb *= vec3(1.0, 0.1, 0.1);
  }
#endif
}