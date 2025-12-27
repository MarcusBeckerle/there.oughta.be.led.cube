# Modifications to there.oughta.be/an/led-cube
In order to align the panels in a way that also the default matrix examples make sense and do not run from right to left on first panel, then on second panel from bottom to top and then on top panel from front to back, I had to rotate the panels and change the vcoords.

For one panel, the vcoords consist of 8 rows: [1..8]. You can calculate the rotations by using the following schema:
90 degree rotation orders:

1 2 3 4 5 6 7 8

5 6 1 2 7 8 3 4

7 8 5 6 3 4 1 2

3 4 7 8 1 2 5 6

1 2 3 4 5 6 7 8

Basically, you always chain the first operation.

I also renamed the elements as they no longer are used for displaying CPU stats in my project.

# Raspbian 11 (bullseye)
After installing Raspbian 11, make sure to install Raspberrypi Userland, to have the proper OpenGL files at hand. See https://github.com/matusnovak/rpi-opengl-without-x/issues/14
> git clone https://github.com/raspberrypi/userland.git
> cd userland
> sudo ./buildme


## 3dprint
This folder contains blend and STL files for the 3d printed case. The cube shown on there.oughta.be consists of three sides that hold a panel each (sides.stl), two sides holding the Raspberry Pi 2 (sides_pi_bottom.stl and sides_pi_side.stl) and one plain solid side (sides_filled.stl).

**Also checkout the README in the 3dprint folder for alternative designs.**

## php-data-sender
This is a very simple PHP script with a small HTML UI that sends a selection of color and ring thickness (and amount of segments) to be "filled" to the cube.

## led-cube
This also requires the appropriate OpenGL libraries to be present and linked against. On the Raspbian system on which this has been developed, compilation was done via g++ with the command `g++ -g -o stats-gl stats-gl.cpp -std=c++11 -lbrcmEGL -lbrcmGLESv2 -I/opt/vc/include -L/opt/vc/lib -Lrpi-rgb-led-matrix/lib -lrgbmatrix -lrt -lm -lpthread -lstdc++ -Irpi-rgb-led-matrix/include/`. (Obviously, the rpi-rgb-led-matrix library was just installed into a subdirectory.)

# Reset Raspberry Pi OS user password (after several years I actually did not remember, this is the generic Raspberry OS reset)

1) Power off the Pi, remove the SD card.
2) Insert SD card into another computer.
3) Open the **boot** partition and edit `cmdline.txt`.
   - It is **one single line**.
   - Add this to the very end (with a space before it):
     init=/bin/sh

4) Save `cmdline.txt`, eject SD, put it back into the Pi, boot.

5) At the shell, make the filesystem writable:
   mount -o remount,rw /

6) Set a new password (replace `<user>` with your username, e.g. `pi`):
   passwd <user>

7) Reboot / power off:
   sync
   reboot -f

8) Put SD back into the other computer, edit `cmdline.txt` again and REMOVE:
   init=/bin/sh

9) Boot normally and log in with the new password.

## Credits
The led-cube code is a slightly changed version of Sebastian Staacks cube. Please refer to https://there.oughta.be/an/led-cube to learn much more about this project.
