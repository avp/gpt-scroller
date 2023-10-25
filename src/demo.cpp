#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "sokol_time.h"
#include "stb_image.h"

// Must be separate to avoid reordering.
#include "sokol_debugtext.h"

#include "soloud.h"
#include "soloud_wav.h"
#include "soloud_wavstream.h"

#include <deque>
#include <map>
#include <memory>
#include <vector>

#include "shaders.h"

class Sound {
  bool const enabled_;
  SoLoud::Soloud soloud;

 public:
  //  SoLoud::WavStream music;
  SoLoud::Wav explosion;
  SoLoud::Wav shot;

  explicit Sound(bool enabled) : enabled_(enabled) {
    if (!enabled_)
      return;
    this->soloud.init();
    //    this->music.load("/Users/tmikov/prog/media/fun2.mp3");
    //    this->music.setLooping(true);
    //    this->play(this->music);

    this->explosion.load("explosion-6055.mp3");
    this->explosion.setVolume(0.5);
    this->shot.load("laser_gun_sound-40813.mp3");
    this->shot.setVolume(0.2);
  }

  void play(SoLoud::AudioSource &sound) {
    if (!enabled_)
      return;
    soloud.play(sound);
  }
};
//

static const float ASSUMED_W = 800;
static const float INV_ASSUMED_W = 1.0f / ASSUMED_W;
static const float ASSUMED_H = 600;
static const float INV_ASSUMED_H = 1.0f / ASSUMED_H;

static double mathRandom(double range) {
  return rand() * (1.0 / (RAND_MAX + 1.0)) * range;
}

/// Transform a unit rect centered around origin.
static void transformRect(float x, float y, float w, float h, float *outMatrix) {
  // Initialize to identity matrix
  for (int i = 0; i < 16; ++i) {
    outMatrix[i] = 0.0f;
  }
  outMatrix[0] = 1.0f;
  outMatrix[5] = 1.0f;
  outMatrix[10] = 1.0f;
  outMatrix[15] = 1.0f;

  // Apply scaling
  outMatrix[0] = w; // Scale X
  outMatrix[5] = h; // Scale Y

  // Apply translation
  outMatrix[12] = x + w / 2; // Translate X
  outMatrix[13] = y + h / 2; // Translate Y
}

void transformRectPx(float x, float y, float width, float height, float *out_transform) {
  // Normalize pixel coordinates to [-1, 1]
  //  float norm_x = (x / (window_width * 0.5f)) - 1.0f;
  //  float norm_y = 1.0f - (y / (window_height * 0.5f));
  //  float norm_width = width / (window_width * 0.5f);
  //  float norm_height = height / (window_height * 0.5f);
  float norm_x = (x * INV_ASSUMED_W * 2) - 1.0f;
  float norm_y = 1.0f - (y * INV_ASSUMED_H * 2);
  float norm_width = width * INV_ASSUMED_W * 2;
  float norm_height = height * INV_ASSUMED_H * 2;

  // Generate transform matrix for a rectangle at normalized coordinates
  // 4x4 matrix represented in column-major order
  // [ sx,  0,  0, tx]
  // [  0, sy,  0, ty]
  // [  0,  0,  1,  0]
  // [  0,  0,  0,  1]
  out_transform[0] = norm_width; // sx
  out_transform[1] = 0.0f; // 0
  out_transform[2] = 0.0f; // 0
  out_transform[3] = 0.0f; // 0

  out_transform[4] = 0.0f; // 0
  out_transform[5] = norm_height; // sy
  out_transform[6] = 0.0f; // 0
  out_transform[7] = 0.0f; // 0

  out_transform[8] = 0.0f; // 0
  out_transform[9] = 0.0f; // 0
  out_transform[10] = 1.0f; // 1
  out_transform[11] = 0.0f; // 0

  out_transform[12] = norm_x + norm_width * 0.5f; // tx
  out_transform[13] = norm_y - norm_height * 0.5f; // ty
  out_transform[14] = 0.0f; // 0
  out_transform[15] = 1.0f; // 1
}

class Image {
 public:
  int w_ = 0, h_ = 0;
  sg_image image_ = {};

  explicit Image(const char *path) {
    int n;
    unsigned char *data = stbi_load(path, &w_, &h_, &n, 4);
    if (!data)
      abort();

    image_ = sg_make_image(sg_image_desc{
        .width = w_,
        .height = h_,
        .data{.subimage[0][0] = {.ptr = data, .size = (size_t)w_ * h_ * 4}},
    });

    stbi_image_free(data);
  }

  ~Image() {
    sg_destroy_image(image_);
  }
};

static sg_shader s_fill_sh = {};
static sg_shader s_blit_sh = {};

static sg_pipeline s_fill_pip = {};
static sg_pipeline s_blit_pip = {};
static sg_sampler s_sampler = {};

/// Map from an image id to an index in s_img_list.
static std::map<uint32_t, unsigned> s_img_index{};
/// List of vertices in the vertex pool associated with an image.
static std::vector<std::pair<sg_image, std::vector<float> *>> s_img_list{};
/// Lists of triangle vertices. {x, y, u, v}
static std::deque<std::vector<float>> s_vert_pool{};

/// List of fill triangle vertices. {x, y, r, g, b, a};
static std::vector<float> s_fill_verts{};

static int s_frame_count = 0;
static double s_fps = 0;
static uint64_t s_last_fps_time;

static std::unique_ptr<Image> s_ship_image;
static std::unique_ptr<Image> s_enemy_image;
static std::unique_ptr<Image> s_background_image;
static std::unique_ptr<Sound> s_sound;

static int s_enemySpawnCounter = 0;
static const int s_enemySpawnRate = 120;

static float s_backgroundX = 0;
static float s_backgroundSpeed = 1;

static bool s_keys[512];

static void load_images() {
  s_ship_image = std::make_unique<Image>("ship.png");
  s_enemy_image = std::make_unique<Image>("enemy.png");
  s_background_image = std::make_unique<Image>("background.png");
}

static void push_rect(std::vector<float> &vec, float x, float y, float w, float h) {
  /*
   * Triangle strip:
   *    2  |  0
   * ------+------
   *    3  |  1
   */
  vec.insert(vec.end(), {x + w, y + h, 1, 1});
  vec.insert(vec.end(), {x + w, y, 1, 0});
  vec.insert(vec.end(), {x, y + h, 0, 1});

  vec.insert(vec.end(), {x + w, y, 1, 0});
  vec.insert(vec.end(), {x, y + h, 0, 1});
  vec.insert(vec.end(), {x, y, 0, 0});
}

static void
push_rect_with_color(std::vector<float> &vec, float x, float y, float w, float h, sg_color color) {
  /*
   * Triangle strip:
   *    2  |  0
   * ------+------
   *    3  |  1
   */
  vec.insert(vec.end(), {x + w, y + h});
  vec.insert(vec.end(), {color.r, color.g, color.b, color.a});
  vec.insert(vec.end(), {x + w, y});
  vec.insert(vec.end(), {color.r, color.g, color.b, color.a});
  vec.insert(vec.end(), {x, y + h});
  vec.insert(vec.end(), {color.r, color.g, color.b, color.a});

  vec.insert(vec.end(), {x + w, y});
  vec.insert(vec.end(), {color.r, color.g, color.b, color.a});
  vec.insert(vec.end(), {x, y + h});
  vec.insert(vec.end(), {color.r, color.g, color.b, color.a});
  vec.insert(vec.end(), {x, y});
  vec.insert(vec.end(), {color.r, color.g, color.b, color.a});
}

static void draw_fill_px(float x, float y, float w, float h, sg_color color) {
  //-1, 1       1, 1
  //
  //       0,0
  //
  //-1,-1       1, -1
  x = x * INV_ASSUMED_W * 2 - 1;
  y = -(y * INV_ASSUMED_H * 2 - 1);
  w = w * INV_ASSUMED_W * 2;
  h = -(h * INV_ASSUMED_H * 2);

  push_rect_with_color(s_fill_verts, x, y, w, h, color);
}

static void draw_blit_px(Image *image, float x, float y, float w, float h) {
  // If this is a new image, add it to the image list and create a new vertex list.
  auto [it, inserted] = s_img_index.try_emplace(image->image_.id, 0);
  if (inserted) {
    s_vert_pool.emplace_back();
    s_img_list.emplace_back(image->image_, &s_vert_pool.back());
    it->second = s_img_list.size() - 1;
  }

  //-1, 1       1, 1
  //
  //       0,0
  //
  //-1,-1       1, -1
  x = x * INV_ASSUMED_W * 2 - 1;
  y = -(y * INV_ASSUMED_H * 2 - 1);
  w = w * INV_ASSUMED_W * 2;
  h = -(h * INV_ASSUMED_H * 2);

  push_rect(*s_img_list[it->second].second, x, y, w, h);
}

static void reset_blits() {
  for (auto &verts : s_vert_pool)
    verts.clear();
  s_fill_verts.clear();
}

static void render_blits() {
  {
    sg_apply_pipeline(s_blit_pip);

    sg_bindings blit_bind = {};
    blit_bind.fs.samplers[SLOT_samp] = s_sampler;

    blit_vs_params_t blit_vs_params;
    //  transformRectPx(0, 0, 800, 600, blit_vs_params.transform);
    for (int y = 0; y < 4; ++y)
      for (int x = 0; x < 4; ++x)
        blit_vs_params.transform[y * 4 + x] = x == y ? 1 : 0;

    sg_apply_uniforms(SG_SHADERSTAGE_VS, SLOT_blit_vs_params, SG_RANGE(blit_vs_params));

    //  printf("\n");

    for (auto &img : s_img_list) {
      if (img.second->empty())
        continue;
      //    printf("img: %d", img.first.id);
      //    for (size_t i = 0; i < img.second->size(); i += 12) {
      //      printf(
      //          "  [(%.3f, %.3f), (%.3f, %.3f), (%.3f, %.3f)]\n",
      //          img.second->at(i),
      //          img.second->at(i + 1),
      //          img.second->at(i + 4),
      //          img.second->at(i + 5),
      //          img.second->at(i + 8),
      //          img.second->at(i + 9));
      //    }
      //    printf("\n");
      blit_bind.fs.images[SLOT_tex] = img.first;
      blit_bind.vertex_buffers[0] = sg_make_buffer(sg_buffer_desc{
          .data = {.ptr = img.second->data(), .size = img.second->size() * sizeof(float)},
          .label = "blit vertices",
      });
      sg_apply_bindings(&blit_bind);
      sg_draw(0, img.second->size() / 4, 1);
      sg_destroy_buffer(blit_bind.vertex_buffers[0]);
    }
  }

  if (!s_fill_verts.empty()) {
    sg_apply_pipeline(s_fill_pip);

    fill_vs_params_t fill_vs_params;
    for (int y = 0; y < 4; ++y)
      for (int x = 0; x < 4; ++x)
        fill_vs_params.transform[y * 4 + x] = x == y ? 1 : 0;

    sg_apply_uniforms(SG_SHADERSTAGE_VS, SLOT_fill_vs_params, SG_RANGE(fill_vs_params));

    sg_bindings fill_bind = {};
    fill_bind.vertex_buffers[0] = sg_make_buffer(sg_buffer_desc{
        .data = {.ptr = s_fill_verts.data(), .size = s_fill_verts.size() * sizeof(float)},
        .label = "fill vertices",
    });
    sg_apply_bindings(&fill_bind);

    sg_draw(0, s_fill_verts.size() / 6, 1);
    sg_destroy_buffer(fill_bind.vertex_buffers[0]);
  }
}

static void reset_frame() {
  reset_blits();
}

class Actor {
 public:
  float x, y, width, height, vel;

  explicit Actor(float x, float y, float width, float height, float vel)
      : x(x), y(y), width(width), height(height), vel(vel) {}
};

class Ship : public Actor {
 public:
  explicit Ship(float x, float y) : Actor(x, y, s_ship_image->w_, s_ship_image->h_, 5) {}

  void update() {
    if (s_keys[SAPP_KEYCODE_LEFT])
      x -= vel;
    if (s_keys[SAPP_KEYCODE_RIGHT])
      x += vel;
    if (s_keys[SAPP_KEYCODE_UP])
      y -= vel;
    if (s_keys[SAPP_KEYCODE_DOWN])
      y += vel;
  }

  void draw() const {
    draw_blit_px(s_ship_image.get(), x, y, width, height);
  }
};

class Enemy : public Actor {
 public:
  explicit Enemy(float x, float y) : Actor(x, y, 64, 64, 2) {}

  void update() {
    x -= vel;
  }

  void draw() const {
    draw_blit_px(s_enemy_image.get(), x, y, width, height);
  }
};

class Bullet : public Actor {
 public:
  explicit Bullet(float x, float y) : Actor(x, y, 5, 5, 8) {}

  void update() {
    x += vel;
  }

  void draw() const {
    draw_fill_px(x, y, width, height, {1, 1, 0, 1});
  }
};

class Particle {
 public:
  float x, y, size, speedX, speedY, life, maxLife, alpha;

  explicit Particle(float x, float y)
      : x(x),
        y(y),
        size(mathRandom(2) + 1),
        speedX(mathRandom(4) - 2),
        speedY(mathRandom(4) - 2),
        life(0),
        maxLife(mathRandom(30) + 50),
        alpha(1) {}

  void update() {
    x += speedX;
    y += speedY;
    ++life;
    alpha = 1 - (life / maxLife);
  }

  void draw() {
    draw_fill_px(x - size / 2, y - size / 2, size, size, {1, 0.5, 0, alpha});
  }

  bool isAlive() const {
    return life < maxLife;
  }
};

class Explosion {
 public:
  float x, y;
  std::vector<Particle> particles;

  explicit Explosion(float x, float y) : x(x), y(y) {
    for (int i = 0; i < 50; ++i) {
      particles.emplace_back(x, y);
    }
  }

  void update() {
    for (long i = 0; i < particles.size();) {
      particles[i].update();
      if (!particles[i].isAlive()) {
        particles.erase(particles.begin() + i);
        continue;
      }
      ++i;
    }
  }

  void draw() {
    for (auto &particle : particles) {
      particle.draw();
    }
  }

  bool isAlive() const {
    return !particles.empty();
  }
};

static std::unique_ptr<Ship> s_ship;
static std::vector<Bullet> s_bullets;
static std::vector<Enemy> s_enemies;
static std::vector<Explosion> s_explosions;
static bool s_pause = false;

void app_init() {
  stm_setup();
  s_last_fps_time = stm_now();

  sg_desc desc = {.context = sapp_sgcontext(), .logger.func = slog_func};
  sg_setup(&desc);

  load_images();
  s_sound = std::make_unique<Sound>(false);

  s_fill_sh = sg_make_shader(fill_shader_desc(sg_query_backend()));

  s_fill_pip = sg_make_pipeline(sg_pipeline_desc{
      .shader = s_fill_sh,
      .layout =
          {
              .attrs =
                  {
                      [ATTR_vs_fill_position].format = SG_VERTEXFORMAT_FLOAT2,
                      [ATTR_vs_fill_color].format = SG_VERTEXFORMAT_FLOAT4,
                  },
          },
      .colors[0].blend =
          {
              .enabled = true,
              .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
              .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
          },
      .primitive_type = SG_PRIMITIVETYPE_TRIANGLES,
      .label = "fill-pipeline",
  });

  s_blit_sh = sg_make_shader(blit_shader_desc(sg_query_backend()));

  s_blit_pip = sg_make_pipeline(sg_pipeline_desc{
      .shader = s_blit_sh,
      .layout =
          {
              .attrs =
                  {
                      [ATTR_vs_blit_pos].format = SG_VERTEXFORMAT_FLOAT2,
                      [ATTR_vs_blit_texcoord0].format = SG_VERTEXFORMAT_FLOAT2,
                  },
          },
      .colors[0].blend =
          {
              .enabled = true,
              .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
              .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
          },
      .primitive_type = SG_PRIMITIVETYPE_TRIANGLES,
      .label = "blit-pipeline",
  });

  s_sampler = sg_make_sampler(sg_sampler_desc{
      .min_filter = SG_FILTER_LINEAR,
      .mag_filter = SG_FILTER_LINEAR,
  });

  sdtx_desc_t sdtx_desc = {.fonts = {sdtx_font_kc854()}, .logger.func = slog_func};
  sdtx_setup(&sdtx_desc);

  s_ship = std::make_unique<Ship>(800.0f / 2, 600.0f / 2);
}

void app_cleanup() {
  s_sound.reset();
  sdtx_shutdown();
  s_ship_image.reset();
  s_enemy_image.reset();
  s_background_image.reset();
  sg_destroy_shader(s_fill_sh);
  sg_destroy_pipeline(s_fill_pip);
  sg_shutdown();
}

void app_event(const sapp_event *ev) {
  if (ev->type == SAPP_EVENTTYPE_KEY_DOWN) {
    if (ev->key_code == SAPP_KEYCODE_Q && (ev->modifiers & SAPP_MODIFIER_SUPER)) {
      sapp_request_quit();
      return;
    }
    s_keys[ev->key_code] = true;
    if (ev->key_code == SAPP_KEYCODE_SPACE) {
      s_bullets.emplace_back(s_ship->x + s_ship->width, s_ship->y + s_ship->height / 2.0 - 2.5);
      s_sound->play(s_sound->shot);
    }
  } else if (ev->type == SAPP_EVENTTYPE_KEY_UP) {
    s_keys[ev->key_code] = false;
    if (ev->key_code == SAPP_KEYCODE_P)
      s_pause = !s_pause;
  }
}

static bool checkCollision(Actor &a, Actor &b) {
  return a.x < b.x + b.width && a.x + a.width > b.x && a.y < b.y + b.height && a.y + a.height > b.y;
}

static void createExplosion(float x, float y) {
  s_explosions.emplace_back(x, y);
  s_sound->play(s_sound->explosion);
}

void app_frame() {
  reset_frame();

  uint64_t now = stm_now();
  ++s_frame_count;

  // Update FPS every second
  uint64_t diff = stm_diff(now, s_last_fps_time);
  if (diff > 1000000000) {
    s_fps = s_frame_count / stm_sec(diff);
    s_frame_count = 0;
    s_last_fps_time = now;
  }

  // Setup pass action to clear the framebuffer with yellow color
  sg_pass_action pass_action = {
      .colors[0] = {.load_action = SG_LOADACTION_CLEAR, .clear_value = {0, 0, 0, 0}}};

  // Begin and end pass
  sg_begin_default_pass(&pass_action, sapp_width(), sapp_height());

  draw_blit_px(s_background_image.get(), 0 + s_backgroundX, 0, s_background_image->w_, ASSUMED_H);
  draw_blit_px(
      s_background_image.get(),
      s_backgroundX + s_background_image->w_,
      0,
      s_background_image->w_,
      ASSUMED_H);

  if (!s_pause) {
    s_backgroundX -= s_backgroundSpeed;
    if (s_backgroundX <= -s_background_image->w_)
      s_backgroundX = 0;
  }

  s_ship->update();
  s_ship->draw();

  for (long i = 0; i < s_bullets.size();) {
    if (!s_pause) {
      s_bullets[i].update();
      if (s_bullets[i].x > ASSUMED_W) {
        s_bullets.erase(s_bullets.begin() + i);
        continue;
      }
    }
    s_bullets[i].draw();
    ++i;
  }

  if (!s_pause) {
    ++s_enemySpawnCounter;
    if (s_enemySpawnCounter >= s_enemySpawnRate) {
      float y = mathRandom(ASSUMED_H - 64);
      s_enemies.emplace_back(ASSUMED_W, y);
      s_enemySpawnCounter = 0;
    }
  }

  for (long i = 0; i < s_enemies.size();) {
    if (s_pause) {
      s_enemies[i++].draw();
      continue;
    }

    s_enemies[i].update();
    if (s_enemies[i].x < -s_enemies[i].width) {
      s_enemies.erase(s_enemies.begin() + i);
      continue;
    }
    s_enemies[i].draw();

    bool destroy = false;
    if (checkCollision(*s_ship, s_enemies[i])) {
      createExplosion(
          s_enemies[i].x + s_enemies[i].width / 2, s_enemies[i].y + s_enemies[i].height / 2);
      destroy = true;
    } else {
      for (long j = 0; j < s_bullets.size();) {
        if (checkCollision(s_bullets[j], s_enemies[i])) {
          if (!destroy) {
            createExplosion(
                s_enemies[i].x + s_enemies[i].width / 2, s_enemies[i].y + s_enemies[i].height / 2);
          }
          s_bullets.erase(s_bullets.begin() + j);
          destroy = true;
          continue;
        }
        ++j;
      }
    }
    if (destroy) {
      s_enemies.erase(s_enemies.begin() + i);
      continue;
    }

    ++i;
  }

  for (long i = 0; i < s_explosions.size();) {
    if (s_pause) {
      s_explosions[i++].draw();
      continue;
    }
    s_explosions[i].update();
    s_explosions[i].draw();
    if (!s_explosions[i].isAlive()) {
      s_explosions.erase(s_explosions.begin() + i);
      continue;
    }
    ++i;
  }

  render_blits();

  sdtx_canvas((float)sapp_width(), (float)sapp_height());
  sdtx_printf("FPS: %d", (int)(s_fps + 0.5));
  sdtx_draw();

  sg_end_pass();

  // Commit the frame
  sg_commit();
}

int main() {
  sapp_desc desc = {};
  desc.init_cb = app_init;
  desc.frame_cb = app_frame;
  desc.cleanup_cb = app_cleanup;
  desc.event_cb = app_event;
  desc.width = 800;
  desc.height = 600;
  desc.window_title = "C GPT Scroller";
  desc.logger.func = slog_func;

  sapp_run(&desc);
  return 0;
}
