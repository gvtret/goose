// An index assignment, plain and compound, into a fixed array local and a
// growable global; a store past the live count is the indexing abort.
var g: i64[>..] = [];
fn main() {
    var a: i64[3] = [1, 2, 3];
    a[1] = 20;
    a[2] += 5;
    g.push(7);
    g.push(8);
    g[0] = 70;
    g[1] *= 3;
    print(a[0]);
    print(a[1]);
    print(a[2]);
    print(g[0]);
    print(g[1]);
    print(9 + a[1] - a[1]);
    g[5] = 1;
    print("not reached");
}
