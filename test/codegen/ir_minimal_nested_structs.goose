// A struct field may be a struct (section 4.4): a literal builds the inner
// value in its place, a read walks the field path to the leaf, a store
// writes the leaf's own width, and a nested value copies, passes and
// returns whole.
struct Inner { a: i64, b: u8 }
struct Mid { x: Inner, y: i32 }
struct Outer { m: Mid, n: i64 }

fn sum(i: Inner) -> i64 { return i.a + i.b; }

fn make(a: i64) -> Inner { return Inner { a: a, b: 3 }; }

fn main() {
    var o = Outer { m: Mid { x: Inner { a: 1, b: 2 }, y: 3 }, n: 4 };
    print(o.m.x.a, " ", o.m.x.b, " ", o.m.y, " ", o.n);
    o.m.x.a = 100;
    o.m.x.b += 5;
    o.m.y *= 7;
    o.n -= 1;
    print(o.m.x.a, " ", o.m.x.b, " ", o.m.y, " ", o.n);
    let i2 = o.m.x;
    print(i2.a, " ", i2.b, " ", sum(i2), " ", sum(make(40)));
    var m = o.m;
    m.x.a = m.x.a + 1;
    o.m = m;
    print(o.m.x.a, " ", make(9).a, " ", make(9).b);
}
