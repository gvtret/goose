// .len on expressions that are not simple identifiers: a call returning a
// slice (a recursive one, so it is not inlined), and an inlined body doing
// the same.
fn view(s: i64[:]) -> i64[:] { s[1..] }

recursive fn tail(s: i64[:], n: i64) -> i64[:] {
    if n <= 1 { return s[..1]; }
    return tail(s[1..], n - 1);
}

fn main() {
    let xs: i64[] = [1, 2, 3, 4];
    print(view(xs[..]).len);
    print(" ", tail(xs[..], 3).len);
    print(" ", view(xs[..]).len);
}
