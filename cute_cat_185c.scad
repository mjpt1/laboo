// Cute cat desk buddy for Waveshare ESP32-S3-Touch-LCD-1.85C (Ø58 x 17 mm)
// Display = face. Parts: head (with ears), back cover, body (with paws).
// part = "head" | "back" | "body" | "all"

part = "all";

/* [Board] */
board_d   = 58;
board_t   = 17;
clear     = 0.4;
view_d    = 48;     // face window
usb_w     = 12;
usb_h     = 7;
usb_z     = 8.5;    // USB-C center from display face

/* [Head] */
wall      = 2.6;
bezel     = 1.8;
back_t    = 2.4;
round_r   = 5;      // soft edge radius
ear_angle = 38;     // ear position from top (deg)
ear_len   = 15;
tilt      = 12;     // head leans back

/* [Body] */
body_w    = 50;
body_h    = 40;
body_dp   = 42;

$fn = 64;

in_d   = board_d + 2*clear;
out_d  = in_d + 2*wall;
head_t = bezel + board_t + clear + back_t;
head_z = body_h + out_d/2 - 9;      // head center height

// ---------- helpers ----------
module soft_disc(d, h, r) {
    hull() for (z=[r, h-r])
        translate([0,0,z]) rotate_extrude() translate([d/2-r,0]) circle(r);
}

// Head local frame: face at z=0, back at z=head_t, up = -y
module ear(side) {
    a = 270 + side*ear_angle;
    rr = out_d/2 - 5;
    dz = head_t*0.55;
    hull() {
        for (t=[-9,9]) rotate([0,0,a]) translate([rr, t, dz]) sphere(d=7);
        rotate([0,0,a + side*6]) translate([rr + ear_len, 0, dz]) sphere(d=5);
    }
}
module ear_inner(side) {   // shallow recess on ear front
    a = 270 + side*ear_angle;
    rr = out_d/2 - 2;
    hull() {
        for (t=[-5,5]) rotate([0,0,a]) translate([rr, t, -2]) cylinder(d=3, h=2.6+2);
        rotate([0,0,a + side*6]) translate([rr + ear_len - 4, 0, -2]) cylinder(d=2, h=4.6);
    }
}

module head_solid() {
    soft_disc(out_d, head_t - back_t, round_r);
    ear(1); ear(-1);
}

module head() {
    difference() {
        head_solid();
        ear_inner(1); ear_inner(-1);
        translate([0,0,-0.01]) cylinder(d1=view_d+2*bezel, d2=view_d, h=bezel+0.02);
        translate([0,0,bezel]) cylinder(d=in_d, h=head_t);
        // back cover rabbet
        translate([0,0,head_t-back_t-1.5]) cylinder(d=in_d+wall, h=10);
        // USB-C at bottom (+y), opens down into body
        translate([0, out_d/2, bezel+usb_z]) cube([usb_w, wall*4, usb_h], center=true);
    }
}

module back_cover() {
    difference() {
        union() {
            translate([0,0,head_t-back_t-1.5]) cylinder(d=in_d+wall-0.3, h=1.5);
            translate([0,0,head_t-back_t]) soft_disc(out_d, back_t, 1.2);
        }
        // little heart vent
        translate([0,-6,head_t-back_t-3]) linear_extrude(10) scale(0.9) union() {
            translate([-4,0]) circle(5); translate([4,0]) circle(5);
            polygon([[-8.7,-2.5],[8.7,-2.5],[0,-12]]);
        }
        translate([0, in_d/2, head_t-back_t]) cube([usb_w, wall*4, 10], center=true);
    }
}

module head_place() {
    translate([0,0,head_z]) rotate([-tilt,0,0]) rotate([-90,0,0])
        translate([0,0,-head_t/2]) children();
}

module body() {
    difference() {
        union() {
            intersection() {
                translate([0,0,body_h*0.5]) scale([body_w, body_dp, body_h*1.25]/2) sphere(1);
                translate([-100,-100,0]) cube(200);
            }
            // front paws
            for (s=[-1,1]) translate([s*12, -body_dp/2+4, 0])
                intersection() {
                    scale([1,1.3,0.75]) sphere(d=15);
                    translate([-20,-20,0]) cube(40);
                }
            // tail curl at back
            translate([body_w/2-8, body_dp/2-4, 8]) rotate([0,0,30])
                rotate_extrude(angle=220) translate([8,0]) circle(3);
        }
        // saddle for head (glue seat)
        head_place() translate([0,0,-0.3]) soft_disc(out_d+0.6, head_t+0.6, round_r);
        // cable channel: from head USB down, out the back
        translate([0, 0, body_h*0.5]) cylinder(d=14, h=body_h);
        translate([0, 0, 9]) rotate([-90,0,0]) cylinder(d=14, h=body_dp);
        translate([0, 0, 9]) sphere(d=14);
        translate([0, 0, 9]) cylinder(d=14, h=body_h);
        // weight pocket underneath
        translate([0,0,-0.01]) cylinder(d=body_w-18, h=6);
    }
}

if (part == "head") head();
else if (part == "back") translate([0,0,head_t]) rotate([180,0,0]) back_cover();
else if (part == "body") body();
else {
    color("#f4e3c8") body();
    head_place() { color("#f4e3c8") head(); color("#e8b4a0") back_cover(); }
}
