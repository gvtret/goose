// A growable array of float elements works like one of ints: literals,
// push, index, .len, element iteration, and print through the runtime's
// float hooks.
fn main() {
    var f: f64[>..] = [0.25, 1.5];
    f.push(300.75);
    print(f, " ", f.len);
    print(f[1], " ", f[2]);
    var g: f32[>..] = [];
    g.push(0.5);
    g.push(2.25);
    print(g);
    for x in f { print(x); }
}
