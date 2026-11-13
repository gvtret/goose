// A function may hand a reference back: a single i64& result comes back as
// the pointer itself, a value position reads through it, and a `var r = f()`
// binding takes a copy of the pointee (4.1).
var gi = 5;
var gj = 9;

fn get_i() -> i64& { gi }
fn get_p(x: i64&) -> i64& { x }
fn bump_to(r: i64&) -> i64& { r += 1; r }
fn pick(c: bool, a: i64&, b: i64&) -> i64& { if c { return a; } return b; }

fn main() {
    print(get_i(), " ", get_i());
    var a = get_i();
    a += 1;
    print(" ", gi, " ", a);
    var m = 10;
    var b = get_p(m);
    b *= 3;
    print(" ", m, " ", get_p(m));
    var c = bump_to(m);
    print(" ", m, " ", c);
    var p = 100;
    var q = 7;
    var w = pick(m > 5, p, q);
    w += 2;
    print(" ", p, " ", q, " ", get_i());
}
