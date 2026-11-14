// A growable-returning function may hand back not only a name but a value
// built at the return site: an empty literal, a listed one, a string
// literal (byte elements) or a slice view, each built onto the caller's
// destination (the run form).
fn pairs(n: i64) -> i64[>..] {
    if n <= 0 { return []; }
    return [n, n * 2];
}

fn text(n: i64) -> u8[>..] {
    if n == 0 { return ""; }
    return "ab";
}

recursive fn pick(s: i64[:], k: i64) -> i64[>..] {
    if k <= 0 { return []; }
    return s[0..k];
}

fn main() {
    print(pairs(0), " ", pairs(3));
    print(text(0), " ", text(1), " ", text(1).len);
    let xs: i64[] = [5, 6, 7, 8];
    print(pick(xs[..], 0), " ", pick(xs[..], 3), " ", pick(xs[..], 3)[2]);
}
