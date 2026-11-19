// Dotii-style head shell for Waveshare ESP32-S3-Touch-LCD-1.85C
// Board numbers from the official drawing ESP32-S3-Touch-LCD-1_85C-dimension.pdf:
//   body d58 x 17, touch/view area d46.08
//   back: 3 case screws at (19.18, +-14) and (-23.75, 0)   [front view, mm from centre]
//   right side: 2 buttons at y = +-7.5
//   left side: TF slot at y = +13, slide switch at y = -11
//   bottom edge: USB-C at x = 0, connectors at x = +-16.5, mic holes
//   back: pin-header connector at (16.2, 0), about 5.5 x 21
//   back: speaker openings near the bottom
// Front view coordinates: +x right, +y up, z = 0 at the front face, +z towards the back.
$fn = 128;

board_d   = 58;
board_t   = 17;
clear     = 0.3;          // gap around the board (radius)
wall      = 2.65;         // same outer size as the scaled Dotii: d63.9
back_t    = 2.4;
lip       = 1.0;          // small front lip that stops the board
lip_d     = 52;           // > 46.08 view area, so nothing covers the screen
fillet    = 2.2;

in_d  = board_d + 2 * clear;
out_d = in_d + 2 * wall;
depth = lip + board_t + 0.4 + back_t;          // 20.8
mid   = lip + board_t / 2;                       // middle of the board's side

module rounded_cyl(d, h, r) {
  hull() {
    translate([0, 0, r]) rotate_extrude() translate([d/2 - r, 0]) circle(r);
    translate([0, 0, h - r]) rotate_extrude() translate([d/2 - r, 0]) circle(r);
  }
}
// a rounded slot through the side wall, at angle a (0 = right, 90 = top), height z
module side_slot(a, along, w, h, z = mid) {
  rotate([0, 0, a]) translate([out_d/2 - wall/2, 0, z])
    hull() for (s = [-1, 1]) translate([0, s * (along - h)/2, 0]) rotate([0, 90, 0]) cylinder(d = h, h = wall * 3, center = true);
}
// a hole through the side wall at a point (x, y) on the board's edge
module edge_hole(x, y, along, h, z = mid) side_slot(atan2(y, x), along, 0, h, z);
module edge_round(x, y, d, z = mid) rotate([0, 0, atan2(y, x)]) translate([out_d/2 - wall/2, 0, z]) rotate([0, 90, 0]) cylinder(d = d, h = wall * 3, center = true);

screws = [[19.18, 14], [19.18, -14], [-23.75, 0]];

// seen from the front (looking towards +z) the +x axis points left, so mirror:
// the numbers above stay "as on the drawing's front view"
mirror([1, 0, 0]) difference() {
  rounded_cyl(out_d, depth, fillet);
  // board pocket, open at the front
  translate([0, 0, lip]) cylinder(d = in_d, h = board_t + 0.4);
  translate([0, 0, -1]) cylinder(d = lip_d, h = lip + 2);
  // soft chamfer on the front opening
  translate([0, 0, -0.01]) cylinder(d1 = lip_d + 1.6, d2 = lip_d, h = 0.8);

  // right side: two buttons (y = +-7.5)
  for (y = [7.5, -7.5]) edge_hole(sqrt(29*29 - y*y), y, 8.5, 7.5);
  // left side: TF card slot (y = +13) and power switch (y = -11)
  edge_hole(-sqrt(29*29 - 169), 13, 15, 4.5);
  edge_hole(-sqrt(29*29 - 121), -11, 10, 6);
  // bottom edge: USB-C in the middle, connectors at +-16.5
  edge_hole(0, -29, 13, 6.5);
  for (x = [16.5, -16.5]) edge_hole(x, -sqrt(29*29 - x*x), 9.5, 6);
  // bottom edge: microphone holes
  for (x = [-23, -10, 10]) edge_round(x, -sqrt(29*29 - x*x), 1.6);

  // back: screw holes (replace the case screws with longer M2 ones) + head recess
  for (p = screws) translate([p[0], p[1], depth - back_t - 1]) {
    cylinder(d = 2.4, h = back_t + 2);
    translate([0, 0, back_t + 1 - 1.4]) cylinder(d = 4.4, h = 2);   // outside: new screw head
    translate([0, 0, 0.5]) cylinder(d = 5.5, h = 1.5);              // inside: room for the case screw head
  }
  // back: opening for the pin-header connector between the two screws (about 5.5 x 21 on the board)
  translate([16.2, 0, depth - back_t - 1])
    hull() for (s = [-1, 1]) translate([0, s * (22 - 7) / 2, 0]) cylinder(d = 7, h = back_t + 2);
  // back: speaker openings near the bottom
  for (x = [-6.5, 6.5]) translate([x, -22, depth - back_t - 1])
    hull() for (s = [-3, 3]) translate([s, 0, 0]) cylinder(d = 4, h = back_t + 2);
  // back: small vents (heat from the ESP32 and the panel)
  for (x = [-9, -3, 3, 9]) translate([x, 6, depth - back_t - 1])
    hull() for (s = [-5, 5]) translate([0, s, 0]) cylinder(d = 2.2, h = back_t + 2);
}
