# Modifications to there.oughta.be/an/led-cube
In order to align the panels in a way that also the default matrix examples make sense and do not run from right to left on first panel, then on second panel from bottom to top and then on top panel from front to back, I had to rotate the pabels and change the vcoords.

For one panel, the vcoords consist of 8 rows: [1..8]. You can calculate the rotations by using the following schema:
90 degree rotation orders:

1 2 3 4 5 6 7 8

5 6 1 2 7 8 3 4

7 8 5 6 3 4 1 2

3 4 7 8 1 2 5 6

1 2 3 4 5 6 7 8

Basically, you always chain the first operation.


## 3dprint
This folder contains blend and STL files for the 3d printed case. The cube shown on there.oughta.be consists of three sides that hold a panel each (sides.stl), two sides holding the Raspberry Pi 2 (sides_pi_bottom.stl and sides_pi_side.stl) and one plain solid side (sides_filled.stl).

**Also checkout the README in the 3dprint folder for alternative designs.**

## cpu-udp-sender
This is the Python 3 script running on the PC that sends its CPU status to the cube to be visualized.

**Also checkout the README in the cpu-udp-sender folder for alternative solutions.**

## led-cube
This also requires the appropriate OpenGL libraries to be present and linked against. On the Raspbian system on which this has been developed, compilation was done via g++ with the command `g++ -g -o cpu-stats-gl cpu-stats-gl.cpp -std=c++11 -lbrcmEGL -lbrcmGLESv2 -I/opt/vc/include -L/opt/vc/lib -Lrpi-rgb-led-matrix/lib -lrgbmatrix -lrt -lm -lpthread -lstdc++ -Irpi-rgb-led-matrix/include/`. (Obviously, the rpi-rgb-led-matrix library was just installed into a subdirectory.)

## Credits
The led-cube code is a slightly changed version of Sebastian Staacks cube. Please refer to https://there.oughta.be/an/led-cube to learn much more about this project.
