// An append's source may be a run built at the site: a call returning a
// growable array, and an if of them -- the elements build onto a scratch
// destination, then join the receiver (only the chosen arm runs).
fn make(n: i64) -> i64[>..] {
    var a: i64[>..] = [];
    for i in n { a.push(i * 5); }
    return a;
}

fn main() {
    var out: i64[>..] = [];
    out.append(make(3));
    print(out, " ", out.len);
    for k in 0..2 {
        out.append(if k == 0 { make(1) } else { make(k + 1) });
    }
    print(out, " ", out.len);
}
