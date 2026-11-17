// A reference may bind an array element's storage: `let f .= fixed[0]`
// reaches the element, so a write through the reference is a write to the
// array. Both a struct element and a scalar one work.
struct Cell { v: i64 }

fn main() {
    var fixed: Cell[2] = [Cell { 5 }, Cell { 6 }];
    let f .= fixed[0];
    f.v += 10;
    print(fixed[0].v, " ", f.v);
    var ints = [1, 2, 3];
    let ri .= ints[2];
    ri = 30;
    print(" ", ints[2], " ", ri);
}
