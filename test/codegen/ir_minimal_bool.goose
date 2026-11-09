// A bool lives in an i64 slot (0/1): comparisons assign to locals, bool
// parameters and results cross calls, print renders true/false, and ! is
// the logical not.
fn is_even(n: i64) -> bool { n % 2 == 0 }
fn pick(c: bool) -> i64 { if c { 1 } else { 2 } }
fn main() {
    let t = 3 > 2;
    let f = 3 < 2;
    print(t, " ", f, " ", is_even(4), " ", is_even(5));
    print(pick(t), " ", pick(f));
    var b = true;
    b = !b;
    print(b);
    b = 1 == 1;
    print(b);
}
