// A slice crosses a call as a fat value: the callee gets a { data, len }
// view it can index, measure and re-slice, and a view comes back whole.
fn total(s: i64[:]) -> i64 {
    var t = 0;
    for x in s { t += x; }
    t
}
fn tail(s: i64[:]) -> i64[:] { s[1..] }
fn grew(v: i64[:], n: i64) -> i64 { total(v) + n }
// Recursion keeps a real call (the base-case pass only rewrites the
// recursing arm), so these cross the slice ABI as a frame does.
recursive fn sumfrom(s: i64[:], i: i64) -> i64 {
    if i >= s.len { return 0; }
    s[i] + sumfrom(s, i + 1)
}
recursive fn drop(s: i64[:], n: i64) -> i64[:] {
    if n == 0 { return s; }
    drop(s[1..], n - 1)
}
fn main() {
    var a: i64[5] = [1, 2, 3, 4, 5];
    print(total(a[..]));
    print(total(a[1..3]));
    let t = tail(a[..]);
    print(t.len, " ", t[0], " ", t[3]);
    print(total(tail(a[2..])));
    print(grew(tail(a[..2]), 10));
    print(tail(a)[0]);
    print(sumfrom(a[..], 0));
    print(sumfrom(a[1..], 1));
    let d = drop(a[..], 3);
    print(d.len, " ", d[0]);
    print(sumfrom(drop(a[..], 2), 0));
}
