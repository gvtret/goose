// str(a, b, ...) renders its arguments into a fresh growable buffer and
// returns the view of it. format(out, a, ...) renders into an existing
// growable buffer -- both use the same renderer as print, but write no
// trailing newline.
struct Cell { v: i64 }

fn main() {
    print(str(42), " ", str(1.5), " ", str(true));
    print(" ", str("abc"), " ", str([1, 2, 3]));
    print(" ", str(Cell { 7 }));
    var out: u8[>..] = [];
    format(out, "n=", 9, " c=", Cell { 5 });
    print(out, " ", out.len);
    format(out, "!");
    print(out);
}
