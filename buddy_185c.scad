// Desk buddy for Waveshare ESP32-S3-Touch-LCD-1.85C (Ã˜58 x 17 mm)
// One round base body + swappable magnetic accessory kits (cat / bat).
// Single-colour printing: coloured details are separate inlays to glue in.
//
// part = "all" (preview, choose kit) | "head" | "back" | "body" | "hatch"
//        | "paw_pad" | "cat_ear" | "cat_ear_inlay" | "cat_tail"
//        | "bat_ear" | "bat_ear_inlay" | "bat_wing_L" | "bat_wing_R"
//        | "bat_wing_inlay_L" | "bat_wing_inlay_R" | "bat_hand"
// kit  = "cat" | "bat" | "none"   (preview only)

part = "all";
kit  = "cat";

/* [Board] */
board_d   = 58;
board_t   = 17;
clear     = 0.4;
view_d    = 48;
usb_w     = 12;
usb_h     = 7;
usb_z     = 8.5;      // USB-C centre from display face

/* [Head] */
wall      = 3.2;
bezel     = 1.8;
back_t    = 2.4;
round_r   = 6;
tilt      = 10;
head_y    = -4;
head_z    = 54;

/* [Body] */
bwall     = 2.2;
ballast_h = 5;        // weight pocket under body (M8 nuts / coins)
hatch_w   = 26;
hatch_h   = 18;
hatch_fit = 0.15;
port_w    = 13;
port_h    = 7;

/* [Magnets 6x2] */
mag_d     = 6.3;
mag_t     = 2.2;

/* [Inlays] */
inlay_t   = 1.0;      // inlay thickness
recess_t  = 1.2;      // recess depth
inlay_gap = 0.15;

$fn = 64;

in_d    = board_d + 2*clear;
out_d   = in_d + 2*wall;
head_t  = bezel + board_t + clear + back_t;
front_t = head_t - back_t;
floor_z = ballast_h + 1.6;

// accessory socket positions on body
wing_p  = [28, 10, 30];
tail_p  = [0, 32.8, 10];

// ---------------- helpers ----------------
module soft_disc(d, h, r) {
    hull() for (z=[r, h-r])
        translate([0,0,z]) rotate_extrude() translate([d/2-r,0]) circle(r);
}
module radial(a, r, z=front_t/2) rotate([0,0,a]) translate([r,0,z]) rotate([0,90,0]) children();
module mag_pocket() translate([0,0,-0.01]) cylinder(d=mag_d, h=mag_t);
module x_extrude(t) rotate([90,0,90]) linear_extrude(t) children();   // 2D(x,y) -> (y,z), thickness along +x

// ---------------- head (local: face z=0, up = -y) ----------------
head_socks = [270, 230, 310, 0, 180];

module head() {
    difference() {
        soft_disc(out_d, front_t, round_r);
        translate([0,0,-0.01]) cylinder(d1=view_d+2*bezel, d2=view_d, h=bezel+0.02);
        translate([0,0,-0.01]) difference() { cylinder(d=view_d+9, h=0.7); cylinder(d=view_d+7.4, h=0.7); }
        translate([0,0,bezel]) cylinder(d=in_d, h=head_t);
        translate([0,0,front_t-1.5]) cylinder(d=in_d+wall, h=10);
        translate([0, out_d/2, bezel+usb_z]) cube([usb_w, wall*4, usb_h], center=true);
        for (a=head_socks) radial(a, out_d/2 - mag_t) cylinder(d=mag_d, h=mag_t+5);
    }
}

module back_cover() {
    difference() {
        union() {
            translate([0,0,front_t-1.5]) cylinder(d=in_d+wall-0.3, h=1.5);
            translate([0,0,front_t]) soft_disc(out_d, back_t, 1.2);
        }
        for (a=[0:30:330]) rotate([0,0,a]) translate([12, 0, front_t-3])
            hull() { cylinder(d=2.2, h=10, $fn=16); translate([9,0,0]) cylinder(d=2.2, h=10, $fn=16); }
        translate([0, in_d/2, front_t]) cube([usb_w, wall*4, 10], center=true);
    }
}

module head_place() {
    translate([0,head_y,head_z]) rotate([-tilt,0,0]) rotate([-90,0,0])
        translate([0,0,-head_t/2]) children();
}

// ---------------- body ----------------
module body_shape(s) hull() {
    translate([0,6,8])  scale([31-s, 27-s, 16-s]) sphere(1);
    translate([0,10,30]) scale([28-s, 24-s, 15-s]) sphere(1);
}
module above(z) intersection() { children(); translate([-200,-200,z]) cube(400); }

module body_shell() difference() {
    above(0) body_shape(0);
    above(floor_z) body_shape(bwall);
}

module hatch_box(g) translate([0, 40, 32]) cube([hatch_w-2*g, 30, hatch_h-2*g], center=true);

module legs() for (s=[-1,1]) above(0) hull() {
    translate([s*13, -6, 16]) sphere(d=16);
    translate([s*13, -24, 8]) sphere(d=17);
}

// toe-bean layout (2D)
module pad2d(g=0) offset(delta=g) {
    translate([0,-1.2]) scale([1,0.8]) circle(d=7);
    for (p=[[-3.4,2.8],[0,4],[3.4,2.8]]) translate(p) circle(d=3);
}
module paw_frame(s) translate([s*13, -24, 8]) rotate([55,0,0]) translate([0,0,8.5 - recess_t]) children();

module body() {
    difference() {
        union() {
            body_shell();
            legs();
            // magnet bosses behind sockets
            for (s=[-1,1]) translate([s*(wing_p[0]-mag_t-2), wing_p[1], wing_p[2]])
                rotate([0,90,0]) cylinder(d=mag_d+4, h=4, center=true);
            translate([0, tail_p[1]-mag_t-2, tail_p[2]+1]) rotate([90,0,0]) cylinder(d=mag_d+4, h=4, center=true);
        }
        head_place() translate([0,0,-0.3]) soft_disc(out_d+0.6, head_t+0.6, round_r);
        translate([0, head_y+2, floor_z]) cylinder(d=18, h=head_z);       // cable path into head
        hatch_box(0);
        // accessory sockets
        for (s=[-1,1]) translate([s*(wing_p[0]-mag_t), wing_p[1], wing_p[2]])
            rotate([0, s*90, 0]) cylinder(d=mag_d, h=mag_t+5);
        translate([0, tail_p[1]-mag_t, tail_p[2]]) rotate([-90,0,0]) cylinder(d=mag_d, h=mag_t+5);
        // paw pad recesses
        for (s=[-1,1]) paw_frame(s) linear_extrude(5) pad2d(inlay_gap);
        // ballast pocket
        translate([0,6,-0.01]) scale([1,0.85,1]) cylinder(d=40, h=ballast_h);
    }
}

module hatch() difference() {
    union() {
        intersection() { body_shell(); hatch_box(hatch_fit); }
        intersection() {
            difference() { body_shape(bwall - 0.5); body_shape(bwall + 1.6); }
            hatch_box(hatch_fit + 1.0);
        }
    }
    translate([0, 40, 29]) cube([port_w, 30, port_h], center=true);
    translate([0, 31, 32 + hatch_h/2 - 3]) scale([1,0.6,0.5]) sphere(d=8);
}

// ---------------- accessories ----------------
// all accessories: magnet face at z=0, outward = +z, toward viewer = +x
// rounded magnet puck
module mount_block(h=5) difference() {
    translate([1, 0, 0]) soft_disc(13, h, min(2, h/2));
    translate([1, 0, 0]) mag_pocket();
}

// concave seat so accessories hug the round head rim
module head_seat() translate([1,0,-out_d/2]) rotate([0,90,0]) cylinder(r=out_d/2 + 0.1, h=60, center=true, $fn=128);

// ears: soft pillow shapes, flat back (x=-2.5) for printing, inlay on front
ear_rx = 3.2;   // recess floor (x)
module cat_ear_body() hull() {
    for (y=[-12,12]) translate([1.5, y, 2]) scale([1,1,0.9]) sphere(d=10);
    translate([1.5, 0, 6]) sphere(d=11);
    translate([1, 0, 23]) sphere(d=6);
}
module bat_ear_body() hull() {
    for (y=[-11,11]) translate([1.5, y, 2]) sphere(d=9);
    translate([1.5, 0, 10]) sphere(d=9);
    translate([1, 0, 30]) sphere(d=3.2);
}
module cat_in2d() offset(r=2.5) polygon([[-6,4],[6,4],[0,16]]);
module bat_in2d() offset(r=1.5) polygon([[-5,5],[5,5],[0,22]]);

module ear() difference() {
    intersection() { children(0); translate([-2.5,-50,-50]) cube(100); }
    head_seat();
    translate([ear_rx, 0, 0]) x_extrude(10) offset(delta=inlay_gap) children(1);
    translate([1, 0, 0]) mag_pocket();
}
module cat_ear() ear() { cat_ear_body(); cat_in2d(); }
module bat_ear() ear() { bat_ear_body(); bat_in2d(); }

module cat_tail() {
    mount_block();
    pts = [[1,0,4],[1,0,10],[1,-6,15],[1,-14,17],[1,-22,15],[1,-28,18],[1,-31,24]];
    for (i=[0:len(pts)-2]) hull() {
        translate(pts[i])   sphere(d=10 - i*0.6);
        translate(pts[i+1]) sphere(d=10 - (i+1)*0.6);
    }
}

module bat_hand() {
    mount_block(4);
    hull() { translate([1,0,5]) sphere(d=11); translate([9,0,5]) sphere(d=10); }
    for (y=[-3.2,0,3.2]) hull() { translate([9,y,5]) sphere(d=5); translate([14,y,3]) sphere(d=4.6); }
}

// wing: 2D in (X out, Y up), thickness Z; front face Z=-2 has inlay recess
wr = 1.6;   // wing edge rounding
module wing2d() offset(r=3, $fn=24) offset(delta=-3) difference() {
    hull() {
        translate([6,0]) circle(6, $fn=32);
        translate([30,30]) circle(3, $fn=24);
        translate([46,10]) circle(3, $fn=24);
        translate([31,-9]) circle(3, $fn=24);
    }
    translate([42,-8]) circle(9, $fn=48);
    translate([17,-16]) circle(9, $fn=48);
}
module ribs2d() for (p=[[30,30],[46,10],[31,-9]]) hull() { translate([6,2]) circle(1.2, $fn=16); translate(p) circle(1, $fn=16); }
module wing_in2d() offset(r=1.5, $fn=16) offset(delta=-1.5) difference() { offset(delta=-3) wing2d(); offset(delta=1.4) ribs2d(); }

module wing3d() difference() {
    intersection() {
        union() {
            minkowski() {   // soft rounded membrane, Z -2.1..2.1
                linear_extrude(4.2 - 2*wr, center=true) offset(delta=-wr) wing2d();
                sphere(r=wr, $fn=12);
            }
            hull() { translate([4,0,-1.2]) sphere(d=12, $fn=32); translate([11,2,-0.5]) sphere(d=6, $fn=24); }   // root
        }
        translate([0,-60,-60]) cube([120,120,60 + 2.1]);   // flat back + flat magnet face
    }
    translate([-0.01,0,-1.2]) rotate([0,90,0]) cylinder(d=mag_d, h=mag_t);
    translate([0,0,-2.2]) linear_extrude(recess_t + 0.1) offset(delta=inlay_gap) wing_in2d();
}
// mount transform: X -> outward, Y -> up, Z -> front/back
module wing_mount(s) multmatrix([[0,-s,0,0],[0,0,1,0],[1,0,0,0]]) children();
module wing_part(s) if (s>0) mirror([0,1,0]) wing3d(); else wing3d();

// ---------------- placement (preview) ----------------
module on_head(a) head_place() radial(a, out_d/2) children();
module on_wing(s) translate([s*wing_p[0], wing_p[1], wing_p[2]]) rotate([0, s*90, 0]) children();
module on_tail() translate(tail_p) rotate([-90,0,0]) children();

shell  = "#f4f2ee";
accent = kit == "bat" ? "#3b3366" : "#8fd3e0";
inlayc = kit == "bat" ? "#3fd0c0" : "#ffffff";

if (part == "head") head();
else if (part == "back") translate([0,0,head_t]) rotate([180,0,0]) back_cover();
else if (part == "body") body();
else if (part == "hatch") rotate([-90,0,0]) translate([0,-40,-32]) hatch();
else if (part == "paw_pad") linear_extrude(inlay_t) pad2d();
else if (part == "cat_ear") rotate([0,-90,0]) cat_ear();
else if (part == "cat_ear_inlay") linear_extrude(inlay_t) cat_in2d();
else if (part == "cat_tail") cat_tail();
else if (part == "bat_ear") rotate([0,-90,0]) bat_ear();
else if (part == "bat_ear_inlay") linear_extrude(inlay_t) bat_in2d();
else if (part == "bat_wing_L") rotate([180,0,0]) wing_part(-1);
else if (part == "bat_wing_R") rotate([180,0,0]) wing_part(1);
else if (part == "bat_wing_inlay_L") linear_extrude(inlay_t) wing_in2d();
else if (part == "bat_wing_inlay_R") mirror([0,1,0]) linear_extrude(inlay_t) wing_in2d();
else if (part == "bat_hand") bat_hand();
else {
    color(kit == "bat" ? accent : shell) body();
    color(accent) hatch();
    for (s=[-1,1]) color(inlayc) paw_frame(s) translate([0,0,recess_t - inlay_t]) linear_extrude(inlay_t + 0.05) pad2d();
    head_place() { color(shell) head(); color(accent) back_cover(); }
    if (kit == "cat") {
        for (a=[230,310]) on_head(a) { color(accent) cat_ear();
            color(inlayc) translate([ear_rx + 0.05, 0, 0]) x_extrude(inlay_t) cat_in2d(); }
        color(accent) on_tail() cat_tail();
    }
    if (kit == "bat") {
        for (a=[230,310]) on_head(a) { color(accent) bat_ear();
            color(inlayc) translate([ear_rx + 0.05, 0, 0]) x_extrude(inlay_t) bat_in2d(); }
        for (s=[-1,1]) on_wing(s) wing_mount(s) { color(accent) wing3d();
            color(inlayc) translate([0,0,-2 + recess_t - inlay_t - 0.05]) linear_extrude(inlay_t) wing_in2d(); }
        for (a=[0,180]) on_head(a) color(accent) bat_hand();
    }
}
