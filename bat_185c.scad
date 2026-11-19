// Bat robot (Fledermaus 2.0) made to fit the Waveshare ESP32-S3-Touch-LCD-1.85C (d58 x 17).
// The original screen pocket is d49 x 25 deep (a smaller round display), so the model is
// scaled 1.2x (pocket d59 x 30), the narrow front lip is opened to the board size, a ring
// behind the board keeps its face flush with the front, and a channel lets the USB-C cable out.
// Model axes: y = front (-) to back (+), z = up. Pocket centre at x = 0, z = 3.6.
$fn = 160;

board_d = 58;
board_t = 17;
clear   = 0.4;                    // radius gap
pc_z    = 3.6;                    // pocket centre height
front_y = -33.8;                  // front face of the head
floor_y = -3.0;                   // back wall of the (scaled) pocket
in_r    = board_d / 2 + clear;    // 29.4
ledge   = 2.2;                    // how far the ring sticks in under the board's rim

module pocket_axis() translate([0, 0, pc_z]) rotate([-90, 0, 0]) children();   // cylinder along +y

difference() {
  union() {
    import("stl/bat_185c/_bat_scaled_120.stl");   // = Fledermaus_2.0.3mf scaled 1.2x
    // ring behind the board: the board's back rim rests on it, its face is level with the front
    translate([0, front_y + board_t + 0.3, 0]) pocket_axis()
      difference() {
        cylinder(r = in_r + 0.6, h = floor_y - (front_y + board_t + 0.3) + 0.5);
        translate([0, 0, -1]) cylinder(r = in_r - ledge, h = 50);
      }
  }
  // open the front lip to the board size (the rest of the pocket is already this wide)
  translate([0, front_y - 1, 0]) pocket_axis() cylinder(r = in_r, h = board_t + 1.3);
  // soft chamfer on the front edge
  translate([0, front_y - 0.01, 0]) pocket_axis() cylinder(r1 = in_r + 1.2, r2 = in_r, h = 1.2);
  // USB-C sits in the middle of the board's bottom edge: channel straight back and out
  // (starts 5 mm behind the face: below the head the chest is right at the front, so the
  //  channel stays inside it; the plug's middle is 8.5 mm behind the board's face)
  translate([-7, front_y + 5.2, pc_z - in_r - 5.2]) cube([14, 80, 7.6]);
}
