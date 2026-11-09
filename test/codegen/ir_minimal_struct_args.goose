// A flat int struct crosses a call by value: the callee gets its own copy,
// a struct result comes back whole, locals copy, and fields stay writable.
struct Pair { a: i64, b: i64 }
fn sum(p: Pair) -> i64 { p.a + p.b }
fn swap(p: Pair) -> Pair { Pair { a: p.b, b: p.a } }
recursive fn add(p: Pair, n: i64) -> Pair {
    if n == 0 { return p; }
    let q = add(p, n - 1);
    Pair { a: q.a + 1, b: q.b + 2 }
}
fn main() {
    let p = Pair { a: 3, b: 4 };
    print(sum(p));
    let q = swap(p);
    print(q.a, " ", q.b);
    let r = add(p, 3);
    print(r.a, " ", r.b);
    var s = p;
    s.a = 10;
    print(s.a, " ", s.b, " ", p.a);
    print(sum(swap(p)));
}
