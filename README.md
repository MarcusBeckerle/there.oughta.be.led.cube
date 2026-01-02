# LED Cube Controller: REST API & GLES2 Shader Overhaul

This project is a heavily modernized version of the [there.oughta.be](https://there.oughta.be/an/led-cube) cube. It has been transformed from a basic CPU monitor into a real-time, API-controllable art piece featuring custom GLES2 shaders and a smooth "Magic Shine" procedural background.

## Key Features
* **Modern REST API:** Replaced legacy scripts with a multi-threaded C++ API (`cpp-httplib`) allowing real-time JSON control.
* **Magic Shine Shaders:** A procedural background that isn't just a flat color; it features spatial color shifting (Teal/Blue transitions) and organic "plasma" movement.
* **Variable Geometry:** Supports Rings, Squares, Triangles, and X-shapes with dynamic thickness (`width`) and arc completion (`percent`).
* **Smooth Tapering:** Transitions between the "active" fat part of a shape and the "stable" thin base line (2px wide) are smoothly feathered.
* **Hardware Optimized:** Specifically tuned for Raspberry Pi 2 GPIO timings and FM6126A LED panels.

---

## Hardware Configuration & Mapping
The cube consists of three 64x64 panels chained together for a total resolution of **192x64**.

### Orientation Fixes
To ensure animations (like the circular `percent` arc) flow correctly across the Top, Left, and Right faces, the `map_xy` function handles local panel inversions.
* **Top Panel (0-63):** The X-axis is locally flipped (`63 - mx`) inside the `map_xy` logic. This ensures that as the `percent` increases, the arc flows seamlessly across physical edges rather than jumping or reversing.
* **Left/Right Panels:** Standard alignment.

**Rotation Schema for vcoords (8 rows):**
If you need to physically rotate panels, use this schema for the coordinate transformation:
1. `1 2 3 4 5 6 7 8` (Original)
2. `5 6 1 2 7 8 3 4` (90° CW)
3. `7 8 5 6 3 4 1 2` (180°)
4. `3 4 7 8 1 2 5 6` (270°)

---

## API Documentation
**Base URL:** `http://<pi-ip>:8080`  
**Auth:** Header `X-API-Token: 1234567890`

### 1. POST /update
Updates the visual state. Supports both legacy "heat" payloads and new "custom" modes.

**Payload Example:**
{
  "mode": "heat",
  "geometry": "ring",
  "colour": 15,
  "elementColor": "#ffffff",
  "width": 47,
  "percent": 0.74
}

| Field | Range | Description |
| :--- | :--- | :--- |
| `mode` | "heat" / "custom" | "heat" uses auto-gradients; "custom" uses your specific hex colors. |
| `geometry` | "ring", "circle", "square", "triangle", "x" | Sets the active shape. |
| `width` | 0 - 100 | The thickness of the "fat" segment (Active part). |
| `percent` | 0.0 - 1.0 | How much of the shape is "filled" with the active width. |
| `elementColor` | Hex String | The color of the geometry itself (rendered purely in front). |
| `backgroundColor`| Hex String | Tints the "Magic Shine" procedural background. |

### 2. GET /status
Returns current interpolated live values, signal age, and blanking status.

### 3) GET /health
**Purpose:** Lightweight liveness probe to verify the service is healthy.
**Response (JSON):**
```json
{
  "ok": true, 
  "uptime": 123456
}
```

* **ok**: Always `true` if the server is responding.
* **uptime**: Total seconds the C++ process has been active.

---

### 4) GET /config
**Purpose:** Returns the static hardware and software limits defined at compile-time.
**Response (JSON):**
```json
{
  "width": 192,
  "height": 64,
  "targetFps": 40,
  "blankInterval": 60,
  "animStep": 40.0
}
```

* **width/height**: The total resolution of the 3-panel array (192x64).
* **targetFps**: The internal frame-pacing goal for the Pi 2.
* **blankInterval**: Seconds of inactivity before the signal-loss (grayscale) logic triggers.


---

## Shader Logic
* **Double Base Line:** The shape never fully disappears. A stable "thin" version (approx. 2px wide) remains visible even where the `percent` hasn't reached.
* **Seamless 100%:** At `percent: 1.0`, the shader bypasses the start/end ramps to ensure the shape is perfectly continuous without a gap at the 12 o'clock join.
* **Stable Base:** The "wobble" (audio/segment movement) is only applied to the fat part (`activeWobble = segmentf * pmask`). This keeps the thin base line perfectly still for a high-quality look.
* **Signal Loss Fade:** If no API data is received for a set time, the **background only** fades to grayscale (Signal-loss logic), while the foreground element remains in pure color.

---

## Installation & Compilation

### OS Setup (Raspbian 11 Bullseye)
Install the Raspberry Pi Userland to provide the necessary OpenGL ES binaries:
```bash
git clone [https://github.com/raspberrypi/userland.git](https://github.com/raspberrypi/userland.git)
cd userland
sudo ./buildme
```

---

### Compilation

Ensure you link against the matrix and GL libraries. Use the following command to build the controller:

```bash
g++ -g -o led-controller main.cpp -std=c++11 \
-I/opt/vc/include -L/opt/vc/lib \
-Lrpi-rgb-led-matrix/lib -lrgbmatrix \
-lbrcmEGL -lbrcmGLESv2 -lrt -lm -lpthread -lstdc++
```

---

## Password Reset Tip

If you are locked out of your Raspberry Pi (as I was, after several years, when I came back to my cube), use this generic recovery method:

1.  **Mount the SD card** on another computer.
2.  **Edit `cmdline.txt`**: Add `init=/bin/sh` to the end of the single line of text.
3.  **Boot the Pi**: At the shell prompt, run `mount -o remount,rw /`.
4.  **Reset Password**: Type `passwd <user>` (e.g., `pi`) and enter your new password.
5.  **Cleanup**: Run `sync`, power off, and **remove** the `init=/bin/sh` string from `cmdline.txt` before booting normally.

## Credits

Originally based on the LED-Cube project by **Sebastian Staacks**. Modernized with high-performance GLES2 shaders, multi-panel coordinate mapping, and a multi-threaded C++ REST API.

For the original project inspiration and hardware build details, please refer to [there.oughta.be](https://there.oughta.be/an/led-cube).
