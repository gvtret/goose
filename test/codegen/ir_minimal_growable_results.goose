// A call may return a growable array: the callee builds the elements onto
// the destination the caller provides and writes its count (the run form).
// The result is a value of its own: bound to a let, measured by .len,
// indexed, printed in place, and appended from.
fn bytes(n: i64) -> u8[>..] {
    var a: u8[>..] = [];
    for i in n { a.push((i + 1) as u8); }
    return a;
}

recursive fn grow(n: i64) -> i64[>..] {
    var a: i64[>..] = [];
    for i in 0..n { a.push(i * 7); }
    return a;
}

fn main() {
    let s = bytes(3);
    print(s, " ", s.len);
    let v = grow(4);
    print(v, " ", v.len, " ", v[3]);
    print(bytes(2).len, " ", grow(2)[1]);
    var d: i64[>..] = [];
    d.append(grow(3));
    print(d, " ", d.len);
}
