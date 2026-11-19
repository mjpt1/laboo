// Mini desk robot for Waveshare ESP32-S3-Touch-LCD-1.85C (Ø58 x 17 mm)
// - Round head = display, with robot "ear pods", leans back on a neck
// - Pear-shaped hollow body with chest badge, little feet
// - Magnet sockets (6x2 mm) for snap-on accessories: head top x3, ear pods x2, body sides x2
// - Hollow body = extension bay; removable back hatch with cable port
// part = "head" | "back" | "body" | "hatch" | "antenna" | "arm" | "all"

part = "all";

/* [Board] */
board_d   = 58;
board_t   = 17;
clear     = 0.4;
view_d    = 48;
usb_w     = 12;
usb_h     = 7;
usb_z     = 8.5;     // USB-C center from display face

/* [Head] */
wall      = 3.2;
bezel     = 1.8;
back_t    = 2.4;
round_r   = 6;
tilt      = 12;
pod_d     = 18;      // ear pod diameter
pod_h     = 6;       // ear pod height out of head

/* [Body] */
body_w    = 50;
body_dp   = 40;
body_h    = 46;
bwall     = 2.2;
neck_h    = 3;
base_d    = 60;      // anti-tip base width
base_dp   = 54;      // base depth
base_h    = 8;
base_off  = 3;       // base shifted back (head leans back)
ballast_h = 5;       // weight pocket depth
hatch_w   = 26;
hatch_h   = 22;
hatch_fit = 0.15;
port_w    = 13;
port_h    = 7;

/* [Magnets] */
mag_d     = 6.3;
mag_t     = 2.2;

$fn = 64;

in_d    = board_d + 2*clear;
out_d   = in_d + 2*wall;
head_t  = bezel + board_t + clear + back_t;
front_t = head_t - back_t;
body_c  = body_h/2;
head_z  = body_h + neck_h + out_d/2 - 8;
side_z  = body_c - 8;          // widest part of body

// ---------- helpers ----------
module soft_disc(d, h, r) {
    hull() for (z=[r, h-r])
        translate([0,0,z]) rotate_extrude() translate([d/2-r,0]) circle(r);
}
// pear body, centred at body_c, s = inward offset
module egg(s) hull() {
    translate([0,0,-8]) scale([body_w/2 - s, body_dp/2 - s, 15 - s]) sphere(1);
    translate([0,0,10]) scale([body_w/2 - 6 - s, body_dp/2 - 4 - s, 13 - s]) sphere(1);
}

// ---------- head (local: face z=0, back z=head_t, up = -y) ----------
module radial(a, r, z=front_t/2) rotate([0,0,a]) translate([r,0,z]) rotate([0,90,0]) children();

module head() {
    difference() {
        union() {
            soft_disc(out_d, front_t, round_r);
            for (a=[0,180]) radial(a, out_d/2 - 3) {       // ear pods
                soft_disc(pod_d, pod_h + 3, 2.5);
                cylinder(d=pod_d - 6, h=pod_h + 4.2);        // small cap ring
            }
        }
        translate([0,0,-0.01]) cylinder(d1=view_d+2*bezel, d2=view_d, h=bezel+0.02);
        // accent groove around the face
        translate([0,0,-0.01]) difference() { cylinder(d=view_d+9, h=0.7); cylinder(d=view_d+7.4, h=0.7); }
        translate([0,0,bezel]) cylinder(d=in_d, h=head_t);
        translate([0,0,front_t-1.5]) cylinder(d=in_d+wall, h=10);
        translate([0, out_d/2, bezel+usb_z]) cube([usb_w, wall*4, usb_h], center=true);
        // magnet sockets
        for (a=[270, 230, 310]) radial(a, out_d/2 - mag_t) cylinder(d=mag_d, h=mag_t+1);
        for (a=[0,180])         radial(a, out_d/2 - 3 + pod_h + 4.2 - mag_t) cylinder(d=mag_d, h=mag_t+1);
        // ear pod rings
        for (a=[0,180]) radial(a, out_d/2 - 3 + pod_h + 2.4)
            difference() { cylinder(d=pod_d - 2, h=0.7); cylinder(d=pod_d - 3.4, h=0.7); }
    }
}

module back_cover() {
    difference() {
        union() {
            translate([0,0,front_t-1.5]) cylinder(d=in_d+wall-0.3, h=1.5);
            translate([0,0,front_t]) soft_disc(out_d, back_t, 1.2);
        }
        // radial vent pattern
        for (a=[0:30:330]) rotate([0,0,a]) translate([12, 0, front_t-3])
            hull() { cylinder(d=2.2, h=10, $fn=16); translate([9,0,0]) cylinder(d=2.2, h=10, $fn=16); }
        translate([0, in_d/2, front_t]) cube([usb_w, wall*4, 10], center=true);
    }
}

module head_place() {
    translate([0,0,head_z]) rotate([-tilt,0,0]) rotate([-90,0,0])
        translate([0,0,-head_t/2]) children();
}

// ---------- body ----------
module hatch_box(g) translate([0, body_dp/2, side_z + 4])
    cube([hatch_w - 2*g, body_dp, hatch_h - 2*g], center=true);

module body_shell() {
    difference() {
        intersection() { translate([0,0,body_c]) egg(0); translate([-100,-100,0]) cube(200); }
        intersection() { translate([0,0,body_c]) egg(bwall); translate([-100,-100,ballast_h + 1.6]) cube(200); }
    }
}

module body() {
    difference() {
        union() {
            body_shell();
            for (s=[-1,1]) translate([s*(body_w/2 - bwall - 1.0), 0, side_z])
                rotate([0,90,0]) cylinder(d=mag_d+4, h=4, center=true);
            // wide anti-tip base, shifted back under the leaning head
            translate([0, base_off, 0]) scale([1, base_dp/base_d, 1]) soft_disc(base_d, base_h, 3);
            for (s=[-1,1]) translate([s*11, -body_dp/2 + 3, 0])  // feet
                intersection() { scale([1,1.4,0.55]) sphere(d=15); translate([-20,-20,0]) cube(40); }
            // neck with collar ring
            translate([0,0,body_h - 6]) cylinder(d=22, h=neck_h + 8);
            translate([0,0,body_h + neck_h - 2]) cylinder(d=27, h=2.4);
        }
        head_place() translate([0,0,-0.3]) soft_disc(out_d+0.6, head_t+0.6, round_r);
        translate([0,0,body_h-16]) cylinder(d=16, h=40);              // cable path
        hatch_box(0);
        // chest badge: ring groove + speaker holes
        translate([0, -body_dp/2 + 2.5, body_c - 2]) rotate([90,0,0]) {
            difference() { cylinder(d=22, h=10); cylinder(d=19, h=10); }
            for (p=[[0,0],[4,0],[-4,0],[2,3.5],[-2,3.5],[2,-3.5],[-2,-3.5]])
                translate([p[0],p[1],-6]) cylinder(d=2.2, h=20, $fn=16);
        }
        for (s=[-1,1]) translate([s*(body_w/2 - mag_t + 0.01), 0, side_z])
            rotate([0, s*90, 0]) cylinder(d=mag_d, h=mag_t+1);
        // ballast pocket: fill with M8 nuts / coins / steel washers, then glue a felt pad
        translate([0, base_off, -0.01]) scale([1, base_dp/base_d, 1]) cylinder(d=base_d - 2*bwall - 4, h=ballast_h);
    }
}

module hatch() {
    difference() {
        union() {
            intersection() { body_shell(); hatch_box(hatch_fit); }
            intersection() {
                difference() { translate([0,0,body_c]) egg(bwall - 0.5); translate([0,0,body_c]) egg(bwall + 1.6); }
                hatch_box(hatch_fit + 1.0);
            }
        }
        translate([0, body_dp/2, side_z + 1]) cube([port_w, 20, port_h], center=true);
        translate([0, body_dp/2 + 1, side_z + 4 + hatch_h/2 - 3]) scale([1,0.5,0.5]) sphere(d=8);
    }
}

// ---------- accessories ----------
module base_puck() difference() {
    soft_disc(12, 4, 1.5);
    translate([0,0,-0.01]) cylinder(d=mag_d, h=mag_t);
}
module antenna() {
    base_puck();
    translate([0,0,3]) cylinder(d=3, h=12);
    translate([0,0,17]) sphere(d=8);
}
module arm() {   // stubby arm, sticks on body side magnets
    base_puck();
    hull() {
        translate([0,0,3]) sphere(d=10);
        translate([0,-9,9]) sphere(d=9);
    }
}

if (part == "head") head();
else if (part == "back") translate([0,0,head_t]) rotate([180,0,0]) back_cover();
else if (part == "body") body();
else if (part == "hatch") rotate([90,0,0]) translate([0,-body_dp/2,-body_c]) hatch();
else if (part == "antenna") antenna();
else if (part == "arm") arm();
else {
    shell = "#f3f1ec"; accent = "#5b8fb0"; pop = "#f2a65a";
    color(shell) body();
    color(accent) hatch();
    head_place() { color(shell) head(); color(accent) back_cover(); }
    head_place() radial(270, out_d/2 - mag_t) color(pop) antenna();
    for (s=[-1,1]) translate([s*(body_w/2), 0, side_z]) rotate([0, s*90, 0])
        rotate([0,0, s>0 ? 0 : 180]) color(accent) arm();
}
