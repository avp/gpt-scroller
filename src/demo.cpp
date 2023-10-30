#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "sokol_time.h"
#include "stb_image.h"

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"
#include "sokol_imgui.h"

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

static const float PHYS_FPS = 60;
static const float PHYS_DT = 1.0f / PHYS_FPS;
static const float ASSUMED_W = 800;
static const float INV_ASSUMED_W = 1.0f / ASSUMED_W;
static const float ASSUMED_H = 600;
static const float INV_ASSUMED_H = 1.0f / ASSUMED_H;

static double mathRandom(double range) {
  return rand() * (1.0 / (RAND_MAX + 1.0)) * range;
}

static sg_sampler s_sampler = {};

class Image {
 public:
  int w_ = 0, h_ = 0;
  sg_image image_ = {};
  simgui_image_t simguiImage_ = {};

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

    simguiImage_ = simgui_make_image(simgui_image_desc_t{image_, s_sampler});
  }

  ~Image() {
    simgui_destroy_image(simguiImage_);
    sg_destroy_image(image_);
  }
};

static std::unique_ptr<Image> s_ship_image;
static std::unique_ptr<Image> s_enemy_image;
static std::unique_ptr<Image> s_background_image;
static std::unique_ptr<Sound> s_sound;

static int s_enemySpawnCounter = 0;
static const int s_enemySpawnRate = 120;

static float s_oldBackgroundX = 0;
static float s_backgroundX = 0;
static float s_backgroundSpeed = 2;

static bool s_keys[512];

static ImVec2 s_winOrg;
static ImVec2 s_winSize;
static ImVec2 s_scale;

static void load_images() {
  s_ship_image = std::make_unique<Image>("ship.png");
  s_enemy_image = std::make_unique<Image>("enemy.png");
  s_background_image = std::make_unique<Image>("background.png");
}

#define IM_COL32(r, g, b, a)                                                                 \
  ((ImU32)(((ImU32)(a)&0xFF) << 24) | (((ImU32)(b)&0xFF) << 16) | (((ImU32)(g)&0xFF) << 8) | \
   (((ImU32)(r)&0xFF) << 0))

static void push_rect_image(float x, float y, float w, float h, simgui_image_t img) {
  x = x * s_scale.x + s_winOrg.x;
  y = y * s_scale.y + s_winOrg.y;
  w *= s_scale.x;
  h *= s_scale.y;

  ImDrawList_AddImage(
      igGetWindowDrawList(),
      simgui_imtextureid(img),
      ImVec2{x, y},
      ImVec2{x + w, y + h},
      ImVec2{0, 0},
      ImVec2{1, 1},
      IM_COL32(255, 255, 255, 255));
}

static void push_rect_with_color(float x, float y, float w, float h, sg_color color) {
  x = x * s_scale.x + s_winOrg.x;
  y = y * s_scale.y + s_winOrg.y;
  w *= s_scale.x;
  h *= s_scale.y;

  ImDrawList_AddRectFilled(
      igGetWindowDrawList(),
      ImVec2{x, y},
      ImVec2{x + w, y + h},
      IM_COL32(255 * color.r, 255 * color.g, 255 * color.b, 255 * color.a),
      0.0f,
      0);
}

static void draw_fill_px(float x, float y, float w, float h, sg_color color) {
  push_rect_with_color(x, y, w, h, color);
}

static void draw_blit_px(Image *image, float x, float y, float w, float h) {
  push_rect_image(x, y, w, h, image->simguiImage_);
}

class Actor {
 public:
  float oldX, oldY;
  float x, y, width, height, velX, velY;

  explicit Actor(float x, float y, float width, float height, float velX, float velY)
      : oldX(x), oldY(y), x(x), y(y), width(width), height(height), velX(velX), velY(velY) {}

  virtual void update(bool save) {
    if (save) {
      oldX = x;
      oldY = y;
    }
    x += velX;
    y += velY;
  }

  float curX(float dt) const {
    return x + (x - oldX) * dt;
  }
  float curY(float dt) const {
    return y + (y - oldY) * dt;
  }
};

class Ship : public Actor {
 public:
  float speed = 5 * 2;
  explicit Ship(float x, float y) : Actor(x, y, s_ship_image->w_, s_ship_image->h_, 0, 0) {}

  virtual void update(bool save) {
    if (s_keys[SAPP_KEYCODE_LEFT])
      velX = -speed;
    else if (s_keys[SAPP_KEYCODE_RIGHT])
      velX = speed;
    else
      velX = 0;
    if (s_keys[SAPP_KEYCODE_UP])
      velY = -speed;
    else if (s_keys[SAPP_KEYCODE_DOWN])
      velY = speed;
    else
      velY = 0;
    Actor::update(save);
  }

  void draw(float dt) const {
    draw_blit_px(s_ship_image.get(), curX(dt), curY(dt), width, height);
  }
};

class Enemy : public Actor {
 public:
  explicit Enemy(float x, float y) : Actor(x, y, 64, 64, -2 * 2, 0) {}

  void draw(float dt) const {
    draw_blit_px(s_enemy_image.get(), curX(dt), curY(dt), width, height);
  }
};

class Bullet : public Actor {
 public:
  explicit Bullet(float x, float y) : Actor(x, y, 5, 5, 8 * 2, 0) {}

  void draw(float dt) const {
    draw_fill_px(curX(dt), curY(dt), width, height, {1, 1, 0, 1});
  }
};

class Particle : public Actor {
 public:
  float life, maxLife, alpha;

  explicit Particle(float x, float y)
      : Actor(x, y, 0, 0, (mathRandom(4) - 2) * 2, ((mathRandom(4) - 2) * 2)),
        life(0),
        maxLife((mathRandom(30) + 50) / 2),
        alpha(1) {
    width = height = mathRandom(2) + 1;
  }

  virtual void update(bool save) {
    Actor::update(save);
    ++life;
    alpha = 1 - (life / maxLife);
  }

  void draw(float dt) const {
    draw_fill_px(curX(dt) - width / 2, curY(dt) - height / 2, width, height, {1, 0.5, 0, alpha});
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

  void update(bool save) {
    for (long i = 0; i < particles.size();) {
      particles[i].update(save);
      if (!particles[i].isAlive()) {
        particles.erase(particles.begin() + i);
        continue;
      }
      ++i;
    }
  }

  void draw(float dt) const {
    for (auto &particle : particles) {
      particle.draw(dt);
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

  sg_desc desc = {.context = sapp_sgcontext(), .logger.func = slog_func};
  sg_setup(&desc);
  simgui_setup(simgui_desc_t{});

  s_sound = std::make_unique<Sound>(getenv("NOSOUND") == nullptr);

  s_sampler = sg_make_sampler(sg_sampler_desc{
      .min_filter = SG_FILTER_LINEAR,
      .mag_filter = SG_FILTER_LINEAR,
  });
  load_images();

  sdtx_desc_t sdtx_desc = {.fonts = {sdtx_font_kc854()}, .logger.func = slog_func};
  sdtx_setup(&sdtx_desc);

  s_ship = std::make_unique<Ship>(800.0f / 2, 600.0f / 2);
}

void app_cleanup() {
  s_ship_image.reset();
  s_enemy_image.reset();
  s_background_image.reset();
  s_sound.reset();
  simgui_shutdown();
  sdtx_shutdown();
  sg_shutdown();
}

void app_event(const sapp_event *ev) {
  if (ev->type == SAPP_EVENTTYPE_KEY_DOWN && ev->key_code == SAPP_KEYCODE_Q &&
      (ev->modifiers & SAPP_MODIFIER_SUPER)) {
    sapp_request_quit();
    return;
  }

  // For now game keys are handled outside of Imgui.
  if (ev->type == SAPP_EVENTTYPE_KEY_DOWN) {
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

  if (simgui_handle_event(ev))
    return;
}

static bool checkCollision(Actor &a, Actor &b) {
  return a.x < b.x + b.width && a.x + a.width > b.x && a.y < b.y + b.height && a.y + a.height > b.y;
}

static void createExplosion(float x, float y) {
  s_explosions.emplace_back(x, y);
  s_sound->play(s_sound->explosion);
}

// Update game state
static void update_game_state(bool save) {
  if (save)
    s_oldBackgroundX = s_backgroundX;
  s_backgroundX -= s_backgroundSpeed;
  if (s_backgroundX <= -s_background_image->w_) {
    s_backgroundX += s_background_image->w_;
    s_oldBackgroundX += s_background_image->w_;
  }

  s_ship->update(save);

  for (long i = 0; i < s_bullets.size();) {
    s_bullets[i].update(save);
    if (s_bullets[i].x > ASSUMED_W) {
      s_bullets.erase(s_bullets.begin() + i);
      continue;
    }
    ++i;
  }

  ++s_enemySpawnCounter;
  if (s_enemySpawnCounter >= s_enemySpawnRate) {
    float y = mathRandom(ASSUMED_H - 64);
    s_enemies.emplace_back(ASSUMED_W, y);
    s_enemySpawnCounter = 0;
  }

  for (long i = 0; i < s_enemies.size();) {
    s_enemies[i].update(save);

    if (s_enemies[i].x < -s_enemies[i].width) {
      s_enemies.erase(s_enemies.begin() + i);
      continue;
    }

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
    s_explosions[i].update(save);
    if (!s_explosions[i].isAlive()) {
      s_explosions.erase(s_explosions.begin() + i);
      continue;
    }
    ++i;
  }
}

// Render game frame
static void render_game_frame(float dt) {
  float bkgX = s_oldBackgroundX + (s_backgroundX - s_oldBackgroundX) * dt;
  draw_blit_px(s_background_image.get(), bkgX, 0, s_background_image->w_, ASSUMED_H);
  draw_blit_px(
      s_background_image.get(),
      bkgX + s_background_image->w_,
      0,
      s_background_image->w_,
      ASSUMED_H);

  s_ship->draw(dt);

  for (const auto &bullet : s_bullets) {
    bullet.draw(dt);
  }

  for (const auto &enemy : s_enemies) {
    enemy.draw(dt);
  }

  for (const auto &explosion : s_explosions) {
    explosion.draw(dt);
  }
}

static bool s_started = false;
static uint64_t s_start_time = 0;
static double s_last_game_time = 0;
static double s_game_time = 0;
static int s_frame_count = 0;
static uint64_t s_last_fps_time = 0;
static uint64_t s_fps = 0;

static void gameWindow(uint64_t now) {
  double render_time = stm_sec(stm_diff(now, s_start_time));
  bool save = true;
  while (s_game_time <= render_time) {
    if (save)
      s_last_game_time = s_game_time;
    s_game_time += PHYS_DT;
    if (!s_pause)
      update_game_state(save);
    save = false;
  }

  // s_last_game_time ... render_time ... s_game_time
  float renderDT = render_time >= s_last_game_time && s_game_time > s_last_game_time
      ? (render_time - s_last_game_time) / (s_game_time - s_last_game_time)
      : 0;

  float app_w = sapp_widthf();
  float app_h = sapp_heightf();
  igSetNextWindowPos((ImVec2){app_w * 0.10f, app_h * 0.10f}, ImGuiCond_Once, (ImVec2){0, 0});
  igSetNextWindowSize((ImVec2){app_w * 0.8f, app_h * 0.8f}, ImGuiCond_Once);
  if (igBegin("Game", NULL, 0)) {
    // Get the top-left corner and size of the window
    igGetCursorScreenPos(&s_winOrg);
    igGetContentRegionAvail(&s_winSize);

    s_scale.x = s_winSize.x * INV_ASSUMED_W;
    s_scale.y = s_winSize.y * INV_ASSUMED_H;

    render_game_frame(renderDT);
  }
  igEnd();
}

static sg_pass_action s_pass_action = {
    .colors[0] = {.load_action = SG_LOADACTION_CLEAR, .clear_value = {0, 0, 0, 0}}};

static void chooseColorWindow() {
  igSetNextWindowPos((ImVec2){10, 10}, ImGuiCond_Once, (ImVec2){0, 0});
  igSetNextWindowSize((ImVec2){400, 100}, ImGuiCond_Once);
  igBegin("Hello Dear ImGui!", 0, ImGuiWindowFlags_None);
  igColorEdit3("Background", &s_pass_action.colors[0].clear_value.r, ImGuiColorEditFlags_None);
  igEnd();
}

static void bouncingBallWindow() {
  // Global variables for ball position and velocity
  static float ball_x = 0.0f, ball_y = 0.0f;
  static float velocity_x = 2, velocity_y = 1.5;

  float app_w = sapp_widthf();
  float app_h = sapp_heightf();
  igSetNextWindowPos((ImVec2){app_w * 0.7f, app_h * 0.05f}, ImGuiCond_Once, (ImVec2){0, 0});
  igSetNextWindowSize((ImVec2){app_w * 0.3f, app_h * 0.3f}, ImGuiCond_Once);
  if (igBegin("Bouncing Ball", NULL, 0)) {
    // Get the window's draw list
    ImDrawList *draw_list = igGetWindowDrawList();

    // Get the top-left corner and size of the window
    ImVec2 p;
    igGetCursorScreenPos(&p);
    ImVec2 win_size;
    igGetContentRegionAvail(&win_size);

    // Draw white borders (4 rectangles)
    float border_thickness = 4.0f;
    ImDrawList_AddRectFilled(
        draw_list,
        p,
        (ImVec2){p.x + win_size.x, p.y + border_thickness},
        IM_COL32(255, 255, 255, 255),
        0.0f,
        0); // Top
    ImDrawList_AddRectFilled(
        draw_list,
        p,
        (ImVec2){p.x + border_thickness, p.y + win_size.y},
        IM_COL32(255, 255, 255, 255),
        0.0f,
        0); // Left
    ImDrawList_AddRectFilled(
        draw_list,
        (ImVec2){p.x, p.y + win_size.y - border_thickness},
        (ImVec2){p.x + win_size.x, p.y + win_size.y},
        IM_COL32(255, 255, 255, 255),
        0.0f,
        0); // Bottom
    ImDrawList_AddRectFilled(
        draw_list,
        (ImVec2){p.x + win_size.x - border_thickness, p.y},
        (ImVec2){p.x + win_size.x, p.y + win_size.y},
        IM_COL32(255, 255, 255, 255),
        0.0f,
        0); // Right

    // Update ball position
    ball_x += velocity_x;
    ball_y += velocity_y;

    // Ball radius
    float radius = 10.0f;

    // Bounce logic for X
    if (ball_x - radius <= 0 || ball_x + radius >= win_size.x) {
      velocity_x *= -1;
      ball_x = (ball_x - radius <= 0) ? radius : win_size.x - radius;
    }

    // Bounce logic for Y
    if (ball_y - radius <= 0 || ball_y + radius >= win_size.y) {
      velocity_y *= -1;
      ball_y = (ball_y - radius <= 0) ? radius : win_size.y - radius;
    }

    // Draw the ball
    ImDrawList_AddCircleFilled(
        draw_list, (ImVec2){p.x + ball_x, p.y + ball_y}, radius, IM_COL32(0, 255, 0, 255), 12);
  }
  igEnd();
}

void app_frame() {
  uint64_t now = stm_now();

  if (!s_started) {
    s_started = true;
    s_start_time = now;
    s_last_fps_time = now;
    s_frame_count = 0;
  } else {
    ++s_frame_count;
    // Update FPS every second
    uint64_t diff = stm_diff(now, s_last_fps_time);
    if (diff > 1000000000) {
      s_fps = s_frame_count / stm_sec(diff);
      s_frame_count = 0;
      s_last_fps_time = now;
    }
  }

  simgui_new_frame({
      .width = sapp_width(),
      .height = sapp_height(),
      .delta_time = sapp_frame_duration(),
      .dpi_scale = sapp_dpi_scale(),
  });
  chooseColorWindow();
  gameWindow(now);
  bouncingBallWindow();

  if (s_fps) {
    sdtx_canvas((float)sapp_width(), (float)sapp_height());
    sdtx_printf("FPS: %d", (int)(s_fps + 0.5));
  }

  // Begin and end pass
  sg_begin_default_pass(&s_pass_action, sapp_width(), sapp_height());
  simgui_render();
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
