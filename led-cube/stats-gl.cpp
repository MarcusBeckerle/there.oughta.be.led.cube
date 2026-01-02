/**
 * RGB Matrix Controller for Raspberry Pi 2
 * * ====================================================================
 * HARDWARE CONFIGURATION
 * ====================================================================
 * - Target: Raspberry Pi 2 (Model B)
 * - Panels: 3x 64x64 RGB LED Panels (FM6126A chips)
 * - Total Resolution: 192x64 pixels
 * - Orientation: 
 * - Panel 0 (Top): Coordinates locally flipped via map_xy to align circle flow.
 * - Panel 1 (Left): Standard alignment.
 * - Panel 2 (Right): Standard alignment.
 * - Hardware Mapping: adafruit-hat-pwm
 * - PWM Depth: 11-bit (for original high-quality color depth)
 * - GPIO Slowdown: 1 (Optimized for Pi 2 timing)
 *
 * ====================================================================
 * RENDERING ENGINE (GLES2 SHADERS)
 * ====================================================================
 * The system uses a procedural fragment shader to create a "Magic Shine" 
 * background with an interactive geometry element in front.
 *
 * BACKWARD COMPATIBILITY NOTE:
 * The "segments" array logic has been replaced by "percent" (arc coverage) 
 * and "width" (uniform thickness). While the API still parses segment data 
 * to avoid breaking old clients, it is no longer used by the shader rendering.
 *
 * GEOMETRY MODES (POST /update "geometry" field):
 * 0 - "ring"     : An organic, wobbling halo. Thickness via "width".
 * 1 - "circle"   : A solid glowing disc with shimmering edges.
 * 2 - "square"   : A geometric box outline. Thickness via "width".
 * 3 - "triangle" : An equilateral triangle. Thickness via "width".
 * 4 - "x"        : A cross shape. Diagonal thickness via "width".
 *
 * ====================================================================
 * API ENDPOINTS (cpp-httplib)
 * ====================================================================
 * Auth: Client must send header "X-API-Token: 1234567890"
 *
 * 1) POST /update
 * --------------------------------------------------------------------
 * Update the visual state. Supports both legacy "heat" and new "custom" modes.
 *
 * EXAMPLE: New "Custom" Payload
 * {
 * "mode": "custom",
 * "geometry": "square",
 * "width": 60,                // 0..100: How fat the line is
 * "percent": 0.5,             // 0..1:   How much of the shape is drawn fat
 * "elementColor": "#00FF00",  // The color of the square itself
 * "backgroundColor": "#110022"// The background shimmer tint
 * }
 *
 * EXAMPLE: Legacy "Heat" Payload (Auto-translated)
 * {
 * "mode": "heat",
 * "geometry": "ring",         // Forced to ring in heat mode
 * "colour": 15,               // 0-33: Teal/Blue, 34-66: Yellow, 67-100: Red
 * "elementColor": "#ffffff",  // Forced to white in heat mode
 * "width": 47,                // Becomes the "fat" part width
 * "percent": 0.74             // The coverage of the heat arc
 * }
 *
 * 2) GET /status
 * --------------------------------------------------------------------
 * Returns the current interpolated live values and signal age.
 * Response: { "colour": 15.0, "width": 47.0, "percent": 0.74, "age": 0.5, ... }
 *
 * 3) GET /config
 * --------------------------------------------------------------------
 * Static info: { "width": 192, "height": 64, "targetFps": 40, ... }
 *
 * ====================================================================
 * SHADER LOGIC & COLOR RULES
 * ====================================================================
 * - Magic Shine: The background uses a spatial shift (coords.x) to create 
 * a turquoise/blue transition. If "backgroundColor" is sent, it tints this.
 * - Double Base Line: Even if "percent" is low, a thin base line (width: 2) 
 * remains visible. The "fat" part is drawn over it with smooth tapering.
 * - Smooth 100%: At percent=1.0, the shader bypasses the arc-mask to ensure 
 * the ring/square connection is perfectly seamless.
 * - Grayscale Fade: If no API data is received, the BACKGROUND fades to 
 * grayscale (Signal-loss logic). The geometry element stays pure color.
 *
 * ====================================================================
 * COMPILATION & RESOURCES
 * ====================================================================
 * Linker flags: -lrgbmatrix -lEGL -lGLESv2 -lpthread
 * Dependencies:
 * - rpi-rgb-led-matrix (Henner Zeller)
 * - cpp-httplib (yhirose)
 * - nlohmann/json
 * ====================================================================
 */

#include "led-matrix.h"
#include "httplib.h"

#include <signal.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <math.h>

#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <sstream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <algorithm>

// =======================================================
// COMPATIBILITY FIXES
// =======================================================
// This MUST be defined before it is used in main()
namespace compat {
    template<class T>
    const T& clamp(const T& v, const T& lo, const T& hi) {
        return (v < lo) ? lo : (hi < v) ? hi : v;
    }
}

// =======================================================
// HARDWARE & API CONFIGURATION
// =======================================================
static const std::string API_TOKEN = "1234567890";
static const int API_PORT = 8080;

#define BLANKINTERVAL 0       // Seconds of inactivity before blanking the display
#define ANIMSTEP 40.0f        // Speed of color/segment transitions (units per second)
#define W 192                 // Total matrix width (e.g., 3x 64px panels)
#define H 64                  // Matrix height
#define SEGMENTS 10           // Number of interactive segments in the shader

static const int TARGET_FPS = 40;

#define CT1 40.0
#define CT2 60.0
#define CT3 80.0

using rgb_matrix::RGBMatrix;
using rgb_matrix::Canvas;
using rgb_matrix::FrameCanvas;

// Time (in seconds) after last update before graying starts (important: don't use XX.Xf here as the smoothstep in the shader has problems with that)
#define GRAY_START_TIME 60.0

// Time (in seconds) after last update before fully gray (important: don't use XX.Xf here as the smoothstep in the shader has problems with that)
#define GRAY_END_TIME 70.0

// =======================================================
// PANEL / ORIENTATION FIXES
// =======================================================
static const bool MAP_FLIP_X = false;
static const bool MAP_FLIP_Y = false;
static const bool MAP_REVERSE_PANELS = false;

static const int PANEL_W = 64;
static const int NUM_PANELS = W / PANEL_W;

static inline void map_xy(int x, int y, int &mx, int &my) {
    mx = x; 
    my = y;

    // Check if we  addressing the TOP panel (Panel 0)
    if (mx < 64) {
        // The top panel needs its X mirrored to align the 'arc' flow.
        mx = 63 - mx; 
    }

    // Standard global fixes (Keep these false as per your saved info)
    if (MAP_FLIP_X) mx = (W - 1) - mx;
    if (MAP_FLIP_Y) my = (H - 1) - my;
    
    if (MAP_REVERSE_PANELS) {
        int panel = mx / PANEL_W;
        int inpanel = mx % PANEL_W;
        int mapped_panel = (NUM_PANELS - 1) - panel;
        mx = mapped_panel * PANEL_W + inpanel;
    }
}

// =======================================================
// GLOBAL STATE & THREAD SAFETY
// =======================================================
float colourLevel = 30.f;
float segment[SEGMENTS];
int geometryMode = 0; // 0:ring, 1:circle, 2:square, 3:triangle, 4:x

float tcolourLevel = 30.f;
float tsegment[SEGMENTS];
int tgeometryMode = 0;
std::string tgeomStr = "ring";

// New (custom) controls
float elementColorRGB[3] = {1.0f, 1.0f, 1.0f};      // geometry color (in front)
float backgroundColorRGB[3] = {0.0f, 0.0f, 1.0f};   // background tint
float telementColorRGB[3] = {1.0f, 1.0f, 1.0f};
float tbackgroundColorRGB[3] = {0.0f, 0.0f, 1.0f};
bool  haveElementColor = false;
bool  haveBackgroundColor = false;
bool  thaveElementColor = false;
bool  thaveBackgroundColor = false;

float elementWidth = 20.0f;   // 0..100 thickness
float percent = 1.0f;         // 0..1 arc coverage
float telementWidth = 20.0f;
float tpercent = 1.0f;

std::string modeStr = "heat";   // "heat" or "custom"
std::string tmodeStr = "heat";

float t = 0.f;
float updateTime = -10.0f;
volatile bool interrupt_received = false;

static std::mutex state_mtx;
static httplib::Server *g_server = nullptr;
static auto start_time = std::chrono::steady_clock::now();

// =======================================================
// UTILITIES (Logging & Formatting)
// =======================================================
namespace std {
    template<class T>
    const T& clamp(const T& v, const T& lo, const T& hi) {
        return (v < lo) ? lo : (hi < v) ? hi : v;
    }
}

static std::string now_hms() {
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{}; localtime_r(&tt, &tm);
    char buf[32]; strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return std::string(buf);
}

static void log_ts(const std::string &msg) {
    fprintf(stderr, "[%s] %s\n", now_hms().c_str(), msg.c_str());
    fflush(stderr);
}

static std::string fmt_float(float v, int prec=3) {
    std::ostringstream ss; ss << std::fixed << std::setprecision(prec) << v;
    return ss.str();
}

static std::string segments_to_string(const float seg[]) {
    std::ostringstream ss; ss << "[";
    for (int i = 0; i < SEGMENTS; i++) {
        ss << fmt_float(seg[i], 2);
        if (i < SEGMENTS - 1) ss << ",";
    }
    ss << "]"; return ss.str();
}

static void InterruptHandler(int signo) {
    (void)signo; interrupt_received = true;
    log_ts("SIGNAL: interrupt received");
    if (g_server) g_server->stop();
}

// ---- Minimal JSON helpers (no external deps; keeps original structure) ----

static inline void skip_ws(const std::string &s, size_t &i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) i++;
}

static bool extract_json_string(const std::string &body, const std::string &key, std::string &out) {
    size_t k = body.find("\"" + key + "\"");
    if (k == std::string::npos) return false;
    size_t colon = body.find(':', k);
    if (colon == std::string::npos) return false;
    size_t i = colon + 1;
    skip_ws(body, i);
    if (i >= body.size() || body[i] != '"') return false;
    i++;
    size_t j = i;
    while (j < body.size() && body[j] != '"') j++;
    if (j >= body.size()) return false;
    out = body.substr(i, j - i);
    return true;
}

static bool extract_json_number(const std::string &body, const std::string &key, float &out) {
    size_t k = body.find("\"" + key + "\"");
    if (k == std::string::npos) return false;
    size_t colon = body.find(':', k);
    if (colon == std::string::npos) return false;
    size_t i = colon + 1;
    skip_ws(body, i);
    // read until comma or end brace
    size_t j = i;
    while (j < body.size() && body[j] != ',' && body[j] != '}' && body[j] != ']' && body[j] != '\n' && body[j] != '\r') j++;
    try {
        out = std::stof(body.substr(i, j - i));
        return true;
    } catch (...) {
        return false;
    }
}

static bool extract_json_array_floats(const std::string &body, const std::string &key, float *outArr, int maxN, int &outN) {
    outN = 0;
    size_t k = body.find("\"" + key + "\"");
    if (k == std::string::npos) return false;
    size_t lb = body.find('[', k);
    size_t rb = body.find(']', k);
    if (lb == std::string::npos || rb == std::string::npos || rb <= lb) return false;
    std::stringstream ss(body.substr(lb + 1, rb - lb - 1));
    std::string val;
    int idx = 0;
    try {
        while (std::getline(ss, val, ',') && idx < maxN) {
            // trim
            val.erase(val.begin(), std::find_if(val.begin(), val.end(), [](unsigned char ch){ return !std::isspace(ch); }));
            val.erase(std::find_if(val.rbegin(), val.rend(), [](unsigned char ch){ return !std::isspace(ch); }).base(), val.end());
            if (val.empty()) continue;
            outArr[idx++] = std::stof(val);
        }
    } catch (...) {
        return false;
    }
    outN = idx;
    return true;
}

static bool parse_hex_color(const std::string &hex, float rgb[3]) {
    // supports "#RRGGBB" or "RRGGBB"
    std::string h = hex;
    if (!h.empty() && h[0] == '#') h = h.substr(1);
    if (h.size() != 6) return false;
    auto hex2 = [](char c)->int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    int r1 = hex2(h[0]), r2 = hex2(h[1]);
    int g1 = hex2(h[2]), g2 = hex2(h[3]);
    int b1 = hex2(h[4]), b2 = hex2(h[5]);
    if (r1 < 0 || r2 < 0 || g1 < 0 || g2 < 0 || b1 < 0 || b2 < 0) return false;
    int R = (r1 << 4) | r2;
    int G = (g1 << 4) | g2;
    int B = (b1 << 4) | b2;
    rgb[0] = (float)R / 255.0f;
    rgb[1] = (float)G / 255.0f;
    rgb[2] = (float)B / 255.0f;
    return true;
}

/**
 * Maps a numerical input (0.0 - 100.0) to a specific background color gradient.
 * * The gradient follows a three-stage transition designed for the "heat" aesthetic:
 * 1. COLD (0-33): Deep Blue transitioning into Teal/Turquoise. This recreates 
 * the "magic shine" effect from the original shader by increasing the green channel.
 * 2. MEDIUM (33-66): Teal transitioning into Yellow.
 * 3. HOT (66-100): Yellow transitioning into pure Red.
 *
 * @param colour01_100 The input heat level, clamped between 0.0 and 100.0.
 * @param rgb An output array of 3 floats where the calculated R, G, and B 
 * values (0.0 to 1.0) will be stored.
 */
static void heat_colour_to_bg(float colour01_100, float rgb[3]) {
    float c = compat::clamp(colour01_100, 0.0f, 100.0f);
    
    if (c <= 33.0f) {
        // COLD: Deep Blue -> Teal (Your original favorite)
        float t = c / 33.0f;
        rgb[0] = 0.0f;          // No Red
        rgb[1] = t * 0.5f;          // Green starts at 0 and goes to 0.5
        rgb[2] = 0.4f + (t * 0.4f); // Blue starts dark (0.4) and brightens
    } else if (c <= 66.0f) {
        // MEDIUM: Teal -> Yellow
        float t = (c - 33.0f) / 33.0f;
        rgb[0] = t;             // Increasing Red
        rgb[1] = 0.6f + (t * 0.4f); 
        rgb[2] = 1.0f - t;      // Decreasing Blue
    } else {
        // HOT: Yellow -> Red
        float t = (c - 66.0f) / 34.0f;
        rgb[0] = 1.0f;
        rgb[1] = 1.0f - t;
        rgb[2] = 0.0f;
    }
}

// =======================================================
// SHADER SOURCE CODE
// =======================================================
#define STRINGIFY(x) #x
#define RESTRINGIFY(x) STRINGIFY(x)

static const char *vertexShaderCode = STRINGIFY(
    attribute vec3 pos;
    attribute vec2 coord;
    varying vec2 fragCoord;
    void main() {
        fragCoord = coord;
        gl_Position = vec4(pos, 1.0);
    }
);

static const char *fragmentShaderHeader = STRINGIFY(
    precision mediump float;
    const int SEGMENTS = ) RESTRINGIFY(SEGMENTS) STRINGIFY(;
    const float CT1 = ) RESTRINGIFY(CT1) STRINGIFY(;
    const float CT2 = ) RESTRINGIFY(CT2) STRINGIFY(;
    const float CT3 = ) RESTRINGIFY(CT3) STRINGIFY(;
    uniform float colourLevel;
    uniform float segment[SEGMENTS];
    uniform float age;
    uniform float time;
    uniform int u_geom;
    uniform vec3 u_bgColor;
    uniform vec3 u_elementColor;
    uniform float u_width;   // 0..100
    uniform float u_percent; // 0..1
    varying vec2 fragCoord;

    // Helper: Unified Wobble Calculation
    float getWobble(vec2 uv) {
        return (sin(normalize(uv).y * 5.0 + time * 2.0) - sin(normalize(uv).x * 5.0 + time * 2.0)) / 100.0;
    }

    // Updated Ring: Now uses the unified wobble
    float ring(vec2 uv, float w0, float width, float segf) {
        float f = length(uv) + getWobble(uv);
        float w = width + width * segf * 0.1; // Thickness logic
        return smoothstep(w0-w, w0, f) - smoothstep(w0, w0+w, f);
    }

    // Updated Box: Supports thickness and wobble
    float sdBox(vec2 p, float b, float width, float segf) {
        float wobble = (u_geom == 2) ? getWobble(p) : 0.0;
        vec2 d = abs(p) - b;
        float f = length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) + wobble;
        float w = width + width * segf * 0.1; // Thickness logic
        return smoothstep(w, 0.0, abs(f));
    }

    // Updated Triangle: Supports thickness and wobble
    float triangle(vec2 p, float r, float width, float segf) {
        const float k = sqrt(3.0);
        float wobble = (u_geom == 3) ? getWobble(p) : 0.0;
        p.x = abs(p.x) - r;
        p.y = p.y + r/k;
        if( p.x+k*p.y>0.0 ) p = vec2(p.x-k*p.y,-k*p.x-p.y)/2.0;
        p.x -= clamp( p.x, -2.0*r, 0.0 );
        float f = -length(p)*sign(p.y) + wobble;
        float w = width + width * segf * 0.1; // Thickness logic
        return smoothstep(w, 0.0, abs(f));
    }

	// Percent (0..1) arc mask with smooth edges
	float arcMask(vec2 uv, float pct) {
		// If percent is 100%, return 1.0 immediately to avoid the 'seam' gap
		if (pct >= 0.99) return 1.0;

		float angle = (atan(uv.y, uv.x) + 3.14159265) / 6.28318530;
		float feather = 0.03; 

		// Smoothly ramp up from the start and down at the percent mark
		float startRamp = smoothstep(0.0, feather, angle);
		float endRamp = smoothstep(pct + feather, pct - feather, angle);
		
		return startRamp * endRamp;
	}

    void main() {
        vec2 coords = fragCoord.xy * 0.5;
        float phi = (atan(coords.y, coords.x) + 3.14159) / 3.14159 * float(SEGMENTS) * 0.5;
        float segmentf = 0.0;
);

static const char *fragmentShaderFooter = STRINGIFY(
        // This is your original procedural background. We tint it with u_bgColor.
        vec2 p = fragCoord.xy * 0.5 * 10.0 - vec2(19.0);
        vec2 i = p; float c = 1.0; float inten = 0.05;
        for (int n = 0; n < 8; n++) {
            float t_inner = time * (0.7 - (0.2 / float(n+1)));
            i = p + vec2(cos(t_inner - i.x) + sin(t_inner + i.y), sin(t_inner - i.y) + cos(t_inner + i.x));
            c += 1.0 / length(vec2(p.x / (2.0 * sin(i.x + t_inner) / inten), p.y / (cos(i.y + t_inner) / inten)));
        }
        c /= 8.0; c = 1.5 - sqrt(c*c);

        // --- New Magic Shine Logic ---
        // Calculate a spatial shift based on X/Y position and time to create organic variation
        float shift = (coords.x + coords.y + sin(time * 0.5)) * 0.5;

        // Create a "shimmer" version of the background color by rotating RGB channels slightly
        vec3 shimmerColor = vec3(
            u_bgColor.r + (sin(shift * 3.14) * 0.10),
            u_bgColor.g + (cos(shift * 3.14) * 0.10),
            u_bgColor.b + (sin(shift * 6.28) * 0.10)
        );

        // Special logic for "Cold/Teal" magic: If primarily blue/teal, force the original horizontal green shift
        if (u_bgColor.b > 0.5 && u_bgColor.r < 0.3) {
            shimmerColor.g = clamp(coords.x + 0.4, 0.0, 1.0) * 1.1; // More Green
            shimmerColor.b *= 0.8; // Deepen the Blue
        }

        // Background: Apply the original "c" energy to the new dynamic shimmer color
        vec3 outcolor = shimmerColor * c * c * c * c;

        // Geometry thickness and mask logic
        float pmask = arcMask(coords, u_percent);

        // 1. Calculate the 'Active' width (from API u_width)
        float widthActive = mix(0.003, 0.08, clamp(u_width / 100.0, 0.0, 1.0));

        // 2. Calculate the 'Inactive' width (fixed at "4" which is approx 0.01 in shader units)
        float widthInactive = 0.01; 

        // 3. Blend the width based on the percent mask
        float baseWidth = mix(widthInactive, widthActive, pmask);

        // 4. Apply wobble ONLY to the active part so the base line stays stable
        float activeWobble = segmentf * pmask;

        float shape = 0.0;

        if (u_geom == 0) {
            // Ring now uses the variable baseWidth and activeWobble (no pmask multiplier so thin part shows)
            shape = ring(coords, 0.25, baseWidth, activeWobble);
        } else if (u_geom == 1) {
            // filled disc, "width" influences edge softness a little
            float r0 = 0.25;
            float edge = mix(0.01, 0.08, clamp(u_width / 100.0, 0.0, 1.0));
            // Circles remain special: we hide the inactive part for a true arc
            shape = (1.0 - smoothstep(r0-edge, r0+edge, length(coords) + getWobble(coords))) * pmask;
        } else if (u_geom == 2) {
            shape = sdBox(coords, 0.22, baseWidth, activeWobble);
        } else if (u_geom == 3) {
            shape = triangle(coords, 0.25, baseWidth, activeWobble);
        } else if (u_geom == 4) {
            // X-Shape with wobble and thickness
            vec2 d = abs(coords);
            float dist = abs(d.x - d.y) + getWobble(coords) * pmask;
            float w = baseWidth + baseWidth * activeWobble * 0.1;
            shape = ((dist < w && length(coords) < 0.3) ? 1.0 : 0.0);
        }

        // IMPORTANT: element color must be purely that color, IN FRONT.
        // So we do NOT multiply. We alpha-blend over the background.
        vec3 composed = mix(outcolor, u_elementColor, clamp(shape, 0.0, 1.0));

        /**
         * Signal-loss grayscale fade logic.
         *
         * The rendered output gradually transitions from full color to grayscale
         * when no new UDP data has been received for a configurable amount of time.
         *
         * Timing behavior:
         * - `age` represents the elapsed time (in seconds) since the last update.
         * - Grayscale blending starts after GRAY_START_TIME seconds.
         * - The image becomes fully grayscale after GRAY_END_TIME seconds.
         *
         * Implementation details:
         * - The fade is performed entirely in the fragment shader.
         * - A smoothstep() function is used to ensure a perceptually smooth
         * transition without abrupt visual changes.
         * - The grayscale value is computed using standard luminance coefficients
         * (ITU-R BT.601).
         *
         * Configuration:
         * - GRAY_START_TIME and GRAY_END_TIME are compile-time constants defined
         * at the top of this file.
         * - Changing these values requires recompilation but does not affect
         * runtime performance or OpenGL state.
         *
         * Note:
         * This visual fade is independent of BLANKINTERVAL.
         * BLANKINTERVAL controls full canvas blanking, while this logic only
         * provides visual feedback for short signal loss.
         *
         * IMPORTANT (requested):
         * Grayscale fade affects BACKGROUND ONLY. Element stays pure.
         */
        vec3 gray_bg = vec3(dot(vec3(0.3, 0.59, 0.11), outcolor));
        vec3 faded_bg = mix(outcolor, gray_bg, smoothstep(GRAY_START, GRAY_END, age));

        // Re-compose after grayscale so the element stays pure and in front.
        vec3 finalColor = mix(faded_bg, u_elementColor, clamp(shape, 0.0, 1.0));

        gl_FragColor = vec4(finalColor, 1.0);
    }
);

// =======================================================
// GL HELPERS
// =======================================================
static bool check_gl_shader(GLuint sh, const char *label) {
    GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0; glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> log(len > 1 ? len : 2, 0);
        glGetShaderInfoLog(sh, len, nullptr, log.data());
        log_ts("GL ERROR: " + std::string(label) + "\n" + log.data()); return false;
    }
    return true;
}

static bool check_gl_program(GLuint prog) {
    GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0; glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> log(len > 1 ? len : 2, 0);
        glGetProgramInfoLog(prog, len, nullptr, log.data());
        log_ts("GL LINK ERROR:\n" + std::string(log.data())); return false;
    }
    return true;
}

// =======================================================
// REST API
// =======================================================
void startRestApi() {
    httplib::Server svr; g_server = &svr;
    svr.new_task_queue = [] { return new httplib::ThreadPool(3); };

    auto set_cors = [](httplib::Response &res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "X-API-Token, Content-Type");
    };

    svr.Options(R"(.*)", [&](const httplib::Request&, httplib::Response& res) { set_cors(res); res.status = 204; });

    svr.Post("/update", [&](const httplib::Request& req, httplib::Response& res) {
        set_cors(res);
        if (req.get_header_value("X-API-Token") != API_TOKEN) { res.status = 401; return; }

        const std::string &b = req.body;
        std::lock_guard<std::mutex> lk(state_mtx);

        bool any = false;

        // Track presence of fields in THIS request (minimal fix: mode switching must not reuse old flags)
        bool gotColour = false;
        bool gotElementColor = false;
        bool gotBackgroundColor = false;

        try {
            // NEW: mode (heat/custom)
            std::string mode;
            if (extract_json_string(b, "mode", mode)) {
                tmodeStr = mode;
                any = true;
            }

            // OLD: colour (0..100)
            float col;
            if (extract_json_number(b, "colour", col)) {
                tcolourLevel = col;
                gotColour = true;
                any = true;
            }

            // Geometry (old/new)
            std::string geom;
            if (extract_json_string(b, "geometry", geom)) {
                if (geom == "ring")     { tgeometryMode = 0; tgeomStr = "ring"; }
                else if (geom == "circle")   { tgeometryMode = 1; tgeomStr = "circle"; }
                else if (geom == "square")   { tgeometryMode = 2; tgeomStr = "square"; }
                else if (geom == "triangle") { tgeometryMode = 3; tgeomStr = "triangle"; }
                else if (geom == "x")        { tgeometryMode = 4; tgeomStr = "x"; }
                any = true;
            }

            // Segments (old/new)
            int nseg = 0;
            if (extract_json_array_floats(b, "segments", tsegment, SEGMENTS, nseg)) {
                any = true;
            }

            // NEW: width (0..100) thickness
            float w;
            if (extract_json_number(b, "width", w)) {
                telementWidth = compat::clamp(w, 0.0f, 100.0f);
                any = true;
            }

            // NEW: percent (0..1) arc coverage
            float pct;
            if (extract_json_number(b, "percent", pct)) {
                tpercent = compat::clamp(pct, 0.0f, 1.0f);
                any = true;
            }

            // NEW: elementColor
            std::string ehex;
            if (extract_json_string(b, "elementColor", ehex)) {
                float rgb[3];
                if (parse_hex_color(ehex, rgb)) {
                    telementColorRGB[0] = rgb[0];
                    telementColorRGB[1] = rgb[1];
                    telementColorRGB[2] = rgb[2];
                    thaveElementColor = true;
                    gotElementColor = true;
                    any = true;
                }
            }

            // NEW: backgroundColor
            std::string bhex;
            if (extract_json_string(b, "backgroundColor", bhex)) {
                float rgb[3];
                if (parse_hex_color(bhex, rgb)) {
                    tbackgroundColorRGB[0] = rgb[0];
                    tbackgroundColorRGB[1] = rgb[1];
                    tbackgroundColorRGB[2] = rgb[2];
                    thaveBackgroundColor = true;
                    gotBackgroundColor = true;
                    any = true;
                }
            }

            // Apply requested "heat mode" enforcement:
            // - geometry forced to ring
            // - element color forced to white
            // - background uses translated colourLevel (unless explicit backgroundColor provided)
            if (tmodeStr == "heat") {
                tgeometryMode = 0; tgeomStr = "ring";
                telementColorRGB[0] = 1.0f; telementColorRGB[1] = 1.0f; telementColorRGB[2] = 1.0f;
                thaveElementColor = true;

                // Minimal fix: In heat mode, background MUST follow heat translation unless THIS request explicitly provides backgroundColor.
                if (!gotBackgroundColor) {
                    float rgb[3];
                    heat_colour_to_bg(tcolourLevel, rgb);
                    tbackgroundColorRGB[0] = rgb[0];
                    tbackgroundColorRGB[1] = rgb[1];
                    tbackgroundColorRGB[2] = rgb[2];
                    thaveBackgroundColor = true;
                }

                // percent/width are optional in heat mode; leave whatever was set.
            } else {
                // In custom mode: if legacy colour is present but backgroundColor isn't,
                // translate colour to backgroundColor (requested).
                if (!gotBackgroundColor && gotColour) {
                    float rgb[3];
                    heat_colour_to_bg(tcolourLevel, rgb);
                    tbackgroundColorRGB[0] = rgb[0];
                    tbackgroundColorRGB[1] = rgb[1];
                    tbackgroundColorRGB[2] = rgb[2];
                    thaveBackgroundColor = true;
                }
                // elementColor may or may not be present; if absent, keep previous.
            }

            if (!any) { res.status = 400; res.set_content("No valid fields", "text/plain"); return; }

            updateTime = t;
            log_ts("API: Updated Targets (Mode=" + tmodeStr + ", Color=" + fmt_float(tcolourLevel) + ", Geom=" + tgeomStr + ")");
        } catch (...) {
            res.status = 400; res.set_content("Invalid JSON", "text/plain"); return;
        }

        res.status = 200;
        res.set_content("OK", "text/plain");
    });

    svr.Get("/status", [&](const httplib::Request&, httplib::Response& res) {
        set_cors(res); std::lock_guard<std::mutex> lk(state_mtx);
        float age = t - updateTime;
        std::ostringstream json;
        json << "{"
             << "\"colour\":" << colourLevel
             << ",\"geometry\":\"" << tgeomStr << "\""
             << ",\"segments\":" << segments_to_string(segment)
             << ",\"age\":" << age
             << ",\"quiet\":" << ((BLANKINTERVAL != 0 && age > BLANKINTERVAL) ? "true" : "false")
             << ",\"mode\":\"" << modeStr << "\""
             << ",\"width\":" << elementWidth
             << ",\"percent\":" << percent
             << "}";
        res.set_content(json.str(), "application/json");
    });

    svr.Get("/health", [&](const httplib::Request&, httplib::Response& res) {
        set_cors(res);
        auto uptime = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_time).count();
        res.set_content("{\"ok\":true,\"uptime\":" + std::to_string(uptime) + "}", "application/json");
    });

    svr.Get("/config", [&](const httplib::Request&, httplib::Response& res) {
        set_cors(res);
        std::ostringstream json;
        json << "{\"width\":" << W << ",\"height\":" << H << ",\"segments\":" << SEGMENTS << ",\"blankInterval\":" << BLANKINTERVAL << ",\"animStep\":" << ANIMSTEP << ",\"targetFps\":" << TARGET_FPS << "}";
        res.set_content(json.str(), "application/json");
    });

    log_ts("API: Listening on port " + std::to_string(API_PORT));
    svr.listen("0.0.0.0", API_PORT);
}

// =======================================================
// MAIN LOOP
// =======================================================
int main(int argc, char *argv[]) {
    log_ts("INIT: Starting Matrix Controller");

    // EGL Setup
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(display, NULL, NULL);
    EGLConfig config; EGLint n;
    static const EGLint att[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    eglChooseConfig(display, att, &config, 1, &n);
    static const EGLint pAtt[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
    EGLSurface surface = eglCreatePbufferSurface(display, config, pAtt);
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, (const EGLint[]){EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE});
    eglMakeCurrent(display, surface, surface, context);

    // Shader Builder
    std::string fsSource = fragmentShaderHeader;

    /**
     * Build angular segment blending logic for the fragment shader.
     *
     * Each segment contributes to the ring thickness based on its angular
     * distance to the current fragment angle (phi).
     *
     * IMPORTANT:
     * The distance is computed in circular (wrap-around) space rather than
     * linear space. This avoids a visible seam and prevents alternating
     * thick/thin artifacts ("Perlenkette") at the 0 ↔ SEGMENTS boundary.
     *
     * Formula:
     *   d = min(|phi - i|, SEGMENTS - |phi - i|)
     *
     * This ensures smooth interpolation between neighboring segments and
     * guarantees consistent thickness modulation for all geometries
     * (circle, square, triangle, X), as they all derive their shape
     * modulation from the same angular parameter.
     */
    for (int i = 0; i < SEGMENTS; i++) {
        char buf[220];
        sprintf(
            buf,
            "float d%d = abs(phi - %d.0);"
            "d%d = min(d%d, float(SEGMENTS) - d%d);"
            "segmentf += smoothstep(1.0, 0.0, d%d) * segment[%d];",
            i, i,
            i, i, i,
            i, i
        );
        fsSource += buf;
    }

    // Minimal fix: normalize segmentf (segments are typically 0..100 from API)
    // This prevents geometry width from exploding (especially in custom mode) and
    // keeps thickness behavior consistent between heat and custom.
    fsSource += "segmentf = clamp(segmentf / 100.0, 0.0, 1.0);\n";

    // inject gray timing constants
    fsSource +=
        "const float GRAY_START = " RESTRINGIFY(GRAY_START_TIME) ";\n"
        "const float GRAY_END   = " RESTRINGIFY(GRAY_END_TIME) ";\n";

    fsSource += fragmentShaderFooter;

    GLuint prog = glCreateProgram();
    auto comp = [](GLenum type, const char* src) {
        GLuint s = glCreateShader(type); glShaderSource(s, 1, &src, NULL); glCompileShader(s); return s;
    };
    GLuint vsh = comp(GL_VERTEX_SHADER, vertexShaderCode);
    GLuint fsh = comp(GL_FRAGMENT_SHADER, fsSource.c_str());
    if(!check_gl_shader(vsh, "Vertex") || !check_gl_shader(fsh, "Fragment")) return 1;
    glAttachShader(prog, vsh); glAttachShader(prog, fsh);
    glLinkProgram(prog); if(!check_gl_program(prog)) return 1;
    glUseProgram(prog);

    // Quad
    static const GLfloat verts[] = { -1,-1,0, -1,1,0, -0.33,-1,0, -0.33,1,0, -0.33,-1,0, -0.33,1,0, 0.33,-1,0, 0.33,1,0, 0.33,-1,0, 0.33,1,0, 1,-1,0, 1,1,0 };
    static const GLfloat coords[] = { -0.866,-0.5, -0.866,0.5, 0,-1, 0,0, 0,-1, 0.866,-0.5, 0,0, 0.866,0.5, 0,0, 0.866,0.5, -0.866,0.5, 0,1 };
    GLuint vbo[2]; glGenBuffers(2, vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo[0]); glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glVertexAttribPointer(glGetAttribLocation(prog, "pos"), 3, GL_FLOAT, GL_FALSE, 0, 0); glEnableVertexAttribArray(glGetAttribLocation(prog, "pos"));
    glBindBuffer(GL_ARRAY_BUFFER, vbo[1]); glBufferData(GL_ARRAY_BUFFER, sizeof(coords), coords, GL_STATIC_DRAW);
    glVertexAttribPointer(glGetAttribLocation(prog, "coord"), 2, GL_FLOAT, GL_FALSE, 0, 0); glEnableVertexAttribArray(glGetAttribLocation(prog, "coord"));

    // Matrix
    //rgb_matrix::RGBMatrix::Options opt; opt.rows = 64; opt.cols = 192; opt.hardware_mapping = "adafruit-hat-pwm"; opt.panel_type = "FM6126A";
    //rgb_matrix::RuntimeOptions rOpt; rOpt.gpio_slowdown = 2;
    //RGBMatrix *matrix = rgb_matrix::CreateMatrixFromFlags(&argc, &argv, &opt, &rOpt);

    //LED Matrix settings
    rgb_matrix::RGBMatrix::Options defaults;
    defaults.hardware_mapping = "adafruit-hat-pwm";
    defaults.led_rgb_sequence = "RGB";
    defaults.pwm_bits = 11;
    //    defaults.pwm_lsb_nanoseconds = 50;
    defaults.panel_type = "FM6126A";
    defaults.rows = 64;
    defaults.cols = 192;
    //    defaults.chain_length = 1;
    //    defaults.parallel = 1;
    //  defaults.brightness = 60;

    rgb_matrix::RuntimeOptions runtime;
    //    runtime.drop_privileges = 0;
    runtime.gpio_slowdown = 2;

    // Matrix
    RGBMatrix *matrix = rgb_matrix::CreateMatrixFromFlags(&argc, &argv, &defaults, &runtime);
    if (matrix == NULL)
        return EXIT_FAILURE;

    FrameCanvas *canvas = matrix->CreateFrameCanvas();

    signal(SIGINT, InterruptHandler); signal(SIGTERM, InterruptHandler);
    std::thread apiThread(startRestApi);
    unsigned char *buffer = (unsigned char *)malloc(W * H * 3);
    auto last_time = std::chrono::steady_clock::now();

    log_ts("RENDER: Entering main loop");

    /**
     * Main render loop.
     *
     * Responsibilities:
     *  - Advances animation time using delta-time (dt) for frame-rate independence
     *  - Smoothly interpolates visual state (colour level, segments, geometry)
     *  - Renders the OpenGL scene into an offscreen framebuffer
     *  - Copies the framebuffer into the LED matrix with correct orientation
     *  - Handles signal-loss behavior (grayscale fade + eventual blanking)
     *
     * Signal-loss behavior:
     *  - Short-term loss:
     *      Handled in the fragment shader via the `age` uniform.
     *      The image gradually fades to grayscale after a configurable delay.
     *
     *  - Long-term loss (blanking):
     *      If no UDP update is received for BLANKINTERVAL seconds, the LED
     *      matrix is completely cleared (black).
     *
     * Disabling blanking:
     *  - Set BLANKINTERVAL to 0 to disable long-term blanking entirely.
     *    In this mode, the display will remain visible (including grayscale
     *    fade) indefinitely, even without incoming data.
     */

    // Cache uniform locations once (critical for performance on Raspberry Pi)
    GLint u_time        = glGetUniformLocation(prog, "time");
    GLint u_age         = glGetUniformLocation(prog, "age");
    GLint u_colourLevel = glGetUniformLocation(prog, "colourLevel");
    GLint u_segment     = glGetUniformLocation(prog, "segment");
    GLint u_geom        = glGetUniformLocation(prog, "u_geom");
    GLint u_bgColor     = glGetUniformLocation(prog, "u_bgColor");
    GLint u_elColor     = glGetUniformLocation(prog, "u_elementColor");
    GLint u_width       = glGetUniformLocation(prog, "u_width");
    GLint u_percent     = glGetUniformLocation(prog, "u_percent");

    while (!interrupt_received) {
        auto frame_start = std::chrono::steady_clock::now();
        float dt = compat::clamp(std::chrono::duration<float>(frame_start - last_time).count(), 0.0f, 0.1f);
        last_time = frame_start;
        t += dt;

        // --- Smooth state interpolation (thread-safe) ------------------------
        {
            std::lock_guard<std::mutex> lk(state_mtx);

            colourLevel += compat::clamp(tcolourLevel - colourLevel, -ANIMSTEP*dt, ANIMSTEP*dt);

            for(int i=0; i<SEGMENTS; i++)
                segment[i] += compat::clamp(tsegment[i] - segment[i], -ANIMSTEP*dt, ANIMSTEP*dt);

            geometryMode = tgeometryMode;
            modeStr = tmodeStr;

            // width/percent interpolate
            elementWidth += compat::clamp(telementWidth - elementWidth, -ANIMSTEP*dt, ANIMSTEP*dt);
            percent += compat::clamp(tpercent - percent, -ANIMSTEP*dt, ANIMSTEP*dt);

            // colors interpolate (fast, but still smooth)
            for (int k = 0; k < 3; k++) {
                elementColorRGB[k] += compat::clamp(telementColorRGB[k] - elementColorRGB[k], -2.0f*dt, 2.0f*dt);
                backgroundColorRGB[k] += compat::clamp(tbackgroundColorRGB[k] - backgroundColorRGB[k], -2.0f*dt, 2.0f*dt);
            }
            haveElementColor = thaveElementColor;
            haveBackgroundColor = thaveBackgroundColor;
        }

        // Time since last UDP update (seconds)
        float age = t - updateTime;

        // Freeze animation time during signal loss to reduce flicker and load
        float renderTime = (age < GRAY_START_TIME) ? t : updateTime;

        // --- Rendering / blanking decision -----------------------------------
        if (BLANKINTERVAL == 0 || age < BLANKINTERVAL) {
            // Normal rendering path (includes grayscale fade in shader)
            glUniform1f(u_time,        renderTime);
            glUniform1f(u_age,         age);
            glUniform1f(u_colourLevel, colourLevel);
            glUniform1fv(u_segment,    SEGMENTS, segment);
            glUniform1i(u_geom,        geometryMode);

            // Always send colors/width/percent (even in heat mode, because heat mode uses them too)
            glUniform3f(u_bgColor, backgroundColorRGB[0], backgroundColorRGB[1], backgroundColorRGB[2]);
            glUniform3f(u_elColor, elementColorRGB[0], elementColorRGB[1], elementColorRGB[2]);
            glUniform1f(u_width,   elementWidth);
            glUniform1f(u_percent, percent);

            glDrawArrays(GL_TRIANGLE_STRIP, 0, 12);
            glReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_BYTE, buffer);

            // OpenGL (0,0) is bottom-left → LED matrix expects top-left
            for (int y = 0; y < H; y++) {
                int gl_y = (H - 1) - y;
                for (int x = 0; x < W; x++) {
                    int mx, my; map_xy(x, y, mx, my);
                    int idx = gl_y*W*3 + x*3;
                    canvas->SetPixel(mx, my, buffer[idx], buffer[idx + 1], buffer[idx + 2]);
                }
            }
        } else {
            /**
             * Long-term signal loss:
             * The display is blanked completely after BLANKINTERVAL seconds
             * of inactivity to reduce visual noise and CPU/GPU load.
             *
             * Blanking can be disabled by setting BLANKINTERVAL to 0.
             */
            canvas->Clear();
        }

        canvas = matrix->SwapOnVSync(canvas);

        // --- Frame rate limiting ---------------------------------------------
        int el = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - frame_start).count();
        if (el < 1000000/TARGET_FPS) usleep(1000000/TARGET_FPS - el);
    }

    log_ts("EXIT: Shutting down");
    if(g_server) g_server->stop();
    apiThread.join();
    free(buffer);
    return 0;
}
