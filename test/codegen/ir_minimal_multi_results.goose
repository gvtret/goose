// A call with several results: the first comes back as the call's value,
// each other through an out-pointer the callee writes. A bool result slots
// in beside ints, and `a, b = f()` assigns each target in turn.
fn two() -> i64, bool { return 7, true; }
fn three(n: i64) -> i64, i64, bool { return n, n * 2, n > 0; }
fn flip(b: bool, n: i64) -> bool, i64 { return !b, n + 1; }

fn main() {
    let a, b = two();
    print(a, " ", b);
    var x = 0;
    var y = 0;
    var ok = false;
    x, y, ok = three(4);
    print(x, " ", y, " ", ok);
    let p, q, r = three(-3);
    print(p, " ", q, " ", r);
    x, ok = two();
    print(x, " ", ok, " ", a, " ", b);
    let t, u = flip(true, 41);
    print(t, " ", u, " ", flip(false, 0));
}
