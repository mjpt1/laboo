// Dotii-style desk stand for Waveshare ESP32-S3-Touch-LCD-1.85C
// Board: Ø58 mm, 17 mm thick. Parametric; print 3 parts.
// part = "shell" | "back" | "base" | "all" (assembled preview)

part = "all";

/* [Board] */
board_d     = 58;     // board diameter
board_t     = 17;     // board thickness
clear       = 0.4;    // fit clearance (radial)
view_d      = 48;     // bezel window (active display ~1.85")
usb_angle   = 270;    // USB-C position on rim (270 = bottom)
usb_w       = 12;     // USB-C cutout width
usb_h       = 7;      // USB-C cutout height
usb_z       = 8.5;    // USB-C center from display face

/* [Shell] */
wall        = 2.4;    // side wall
bezel       = 1.6;    // front lip thickness
fillet      = 3;      // outer edge rounding
back_t      = 2.4;    // back cover thickness

/* [Stand] */
tilt        = 15;     // backward tilt (deg)
base_d      = 56;     // base puck diameter
base_h      = 10;     // base height
neck_w      = 22;     // neck width

$fn = 128;

in_d   = board_d + 2*clear;
out_d  = in_d + 2*wall;
body_h = bezel + board_t + clear + back_t;

module rounded_puck(d, h, r) {
    hull() {
        translate([0,0,r]) rotate_extrude() translate([d/2 - r, 0]) circle(r);
        translate([0,0,h-r]) rotate_extrude() translate([d/2 - r, 0]) circle(r);
    }
}

// Front at z=0, back at z=body_h
module shell() {
    difference() {
        rounded_puck(out_d, body_h - back_t, fillet);
        // window, chamfered
        translate([0,0,-0.01]) cylinder(d1=view_d+2*bezel, d2=view_d, h=bezel+0.02);
        // board pocket
        translate([0,0,bezel]) cylinder(d=in_d, h=body_h);
        // USB-C
        rotate([0,0,usb_angle]) translate([out_d/2, 0, bezel+usb_z])
            cube([wall*4, usb_w, usb_h], center=true);
        // back cover rabbet
        translate([0,0,body_h-back_t-1.5]) cylinder(d=in_d+wall, h=10);
    }
}

module back_cover() {
    difference() {
        union() {
            translate([0,0,body_h-back_t-1.5]) cylinder(d=in_d+wall-0.3, h=1.5);
            translate([0,0,body_h-back_t]) cylinder(d=out_d - 2*0.01, h=back_t);
        }
        // vent slots
        for (i=[-2:2]) translate([i*6, 8, body_h-back_t-2]) hull() {
            cylinder(d=2.5, h=10, $fn=24);
            translate([0,14,0]) cylinder(d=2.5, h=10, $fn=24);
        }
        // USB clearance notch
        rotate([0,0,usb_angle]) translate([in_d/2, 0, body_h-back_t])
            cube([wall*4, usb_w, 10], center=true);
    }
}

// Base: weighted puck + angled cradle that grips the bottom of the body
module base() {
    cradle_r = out_d/2 + 0.3;
    difference() {
        union() {
            rounded_puck(base_d, base_h, 3);
            // neck
            hull() {
                translate([-neck_w/2, -8, base_h-1]) cube([neck_w, 16, 1]);
                translate([0, 0, base_h + 6]) rotate([-tilt-90,0,0])
                    translate([0,0,-body_h/2]) cylinder(d=neck_w, h=body_h - 2, center=false);
            }
        }
        // cradle cut: the body itself
        translate([0, 0, base_h + 6 + cradle_r - 6]) rotate([90 - tilt, 0, 0])
            translate([0,0,-body_h/2]) cylinder(r=cradle_r, h=body_h + 0.6, center=false);
        // USB cable channel through neck
        translate([0, 0, base_h + 2]) rotate([-tilt,0,0]) cube([usb_w+2, 60, usb_h+2], center=true);
        // weight pocket (coins / steel nuts) underneath
        translate([0,0,-0.01]) cylinder(d=base_d-10, h=base_h-3);
    }
}

module body_assembled() { shell(); color("gray") back_cover(); }

if (part == "shell") shell();
else if (part == "back") translate([0,0,body_h]) rotate([180,0,0]) back_cover();
else if (part == "base") base();
else {
    color("tan") base();
    translate([0, 0, base_h + 6 + out_d/2 - 6]) rotate([90 - tilt, 0, 0])
        translate([0,0,-body_h/2]) body_assembled();
}
