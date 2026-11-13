// A reference to a struct is the pointer to its body: a parameter passes the
// place it names, a `.=` binding keeps it, a field path walks through it at
// its own width, and a call may hand one back.
struct Cell { v: i64 }
struct Pair { a: i64, inner: Cell }

fn bump(c: Cell&) -> i64 { c.v += 1; c.v }
fn sum(p: Pair&) -> i64 { p.a + p.inner.v }
fn id(p: Pair&) -> Pair& { p }
fn inner(p: Pair&) -> Cell& { p.inner }

fn main() {
    var c = Cell { 5 };
    print(bump(c), " ", c.v);
    var p = Pair { 1, Cell { 10 } };
    print(" ", sum(p), " ", inner(p).v);
    inner(p).v += 5;
    print(" ", p.inner.v);
    var r .= id(p);
    r.a += 1;
    print(" ", p.a, " ", sum(p));
}
