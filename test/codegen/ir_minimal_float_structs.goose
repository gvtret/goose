// A struct may hold float fields at their own width: literals narrow,
// reads load as floats, stores and compound forms run in float arithmetic,
// a copy moves the whole value, one crosses a call by value, and equality
// compares floats (bit-exact through the struct).
struct Floats { a: f64, b: f32 }
struct Wrap { f: Floats, tag: i64 }

fn show(f: Floats) -> f64 { return f.a; }

fn main() {
    let f = Floats { 1.5, 2.25 };
    print(f.a, " ", f.b);
    var g = f;
    g.a = 9.75;
    g.b += 0.75;
    print(" ", g.a, " ", g.b);
    var w = Wrap { Floats { 1.5, 2.25 }, 7 };
    print(" ", w.f.a, " ", w.f.b, " ", w.tag);
    print(" ", show(w.f));
    let q = w.f;
    w.f.a += 1.0;
    print(" ", w.f.a, " ", q.a);
    let p = Floats { 1.5, 2.25 };
    let r = Floats { 1.5, 2.5 };
    print(" ", p == q, " ", p != r);
}
