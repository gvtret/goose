// An optional reference to a struct (`Cell&?`) is a pointer that may be
// null: `null` and `default<T&?>()` are the null pointer, `== null`,
// `!= null` and a bare `if o` test it, a call may pass null to one, and a
// read through a non-null one reaches the struct body.
struct Cell { v: i64 }

fn show(r: Cell&?) -> i64 { if r { return r.v; } return -1; }
fn bump(r: Cell&?) -> i64 {
    if r { r.v += 1; return r.v; }
    return -1;
}

fn main() {
    print(show(null), " ", show(default<Cell?>()));
    var c = Cell { 42 };
    let o: Cell&? = c;
    if o != null { print(" ", show(o), " ", o.v); }
    print(" ", bump(o), " ", c.v);
    var d = Cell { 10 };
    var p: Cell&? = null;
    if p == null { print(" null"); }
    p .= d;
    if p { print(" ", p.v); }
    p.v = 99;
    print(" ", d.v);
}
