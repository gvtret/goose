// Zero-length fixed arrays are values of their own: an empty literal is a
// [0 x T] with no storage, it crosses a call (inline and direct) as that
// value, and .len is zero.
fn none() -> i64[0] { [] }
fn same(a: i64[0]) -> i64[0] { a }
recursive fn echo(a: i64[0]) -> i64[0] { return a; }

fn main() {
    let e = none();
    print(e.len);
    let f = same(e);
    print(" ", f.len);
    let g = echo(e);
    print(" ", g.len);
}
