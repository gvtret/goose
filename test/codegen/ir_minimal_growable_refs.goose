// A reference to a growable array (section 3.3) names the header and the
// stack the array lives on: an `i64[>..]&` parameter pushes onto and reads
// the caller's array, a `let r .= a` binding keeps naming it while a push
// moves its length and top, and a recursive call passes the reference on.

fn push_two(a: i64[>..]&) -> i64 {
    a.push(10);
    a.push(20);
    return a.len;
}

recursive fn fill(a: i64[>..]&, n: i64) {
    if n == 0 { return; }
    a.push(a.len + n);
    fill(a, n - 1);
}

fn main() {
    var a: i64[>..] = [1, 2];
    print(push_two(a), " ", a, " ", a.len);
    let r .= a;
    r.push(3);
    print(r.len, " ", a.len, " ", a[3], " ", r[1]);
    fill(r, 2);
    print(a);
    var b: i64[>..] = [7];
    print(push_two(b), " ", b);
}
