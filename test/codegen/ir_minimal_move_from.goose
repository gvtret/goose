// move_from replaces a view's elements with another's, in place: sources
// and destinations may overlap (a right-moving memmove), a growable may be
// the destination, and mismatched lengths abort (covered by
// slice_move_length.goose).
fn main() {
    var a = [1, 2, 3, 4, 5];
    a[1..].move_from(a[..4]);
    print(a);
    var b: i64[3] = [0, 0, 0];
    b[..].move_from(a[2..5]);
    print(b);
    a[..].move_from(a[..]);
    print(a);
    var g: i64[>..] = [7, 8, 9];
    g[..].move_from(a[1..4]);
    print(g);
    var s: i64[5] = [9, 9, 9, 9, 9];
    s[..2].move_from(a[..2]);
    print(s);
}
