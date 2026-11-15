// copy(x) makes a fresh copy of an array value: a growable builds onto a
// scratch run, a fixed array copies into a temporary, and the result is a
// view of the copy. A growable global's literal initializer lands on its
// region when the program starts.
var g: i64[>..] = [7, 8, 9];
var b: u8[>..]  = "abc";

fn main() {
    print(g, " ", g.len);
    print(b);
    let x = copy(g);
    print(x, " ", x.len);
    x[0] = 70;
    print(" ", g[0], " ", x[0]);
    var a = [1, 2, 3];
    print(" ", copy(a), " ", copy(a)[1]);
    print(" ", head(copy(g)));
}

fn head(s: i64[:]) -> i64 { return s[0]; }
