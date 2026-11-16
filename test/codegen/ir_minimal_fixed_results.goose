// A fixed array crosses a call by value (the C backend's a2_i64): a
// parameter copies into the body's own storage, a result returns the
// whole array, and an inlined callee's parameter binding copies the run.
fn pair() -> i64[2] { [7, 8] }
fn id(a: i64[2]) -> i64[2] { a }
fn bump(a: i64[2]) -> i64 { a[0] + a[1]; return a.len; }

fn main() {
    let w = pair();
    print(w, " ", w.len, " ", w[1]);
    let x = id(w);
    print(" ", x[0], " ", x.len);
    print(" ", bump(x), " ", w[0], " ", x[0]);
}
