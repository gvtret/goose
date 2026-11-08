// A local struct of int fields: built from a literal (named fields in any
// order), read and written field by field, plain and compound.
struct Pt { x: i64, y: i64 }
fn main() {
    var p = Pt { x: 1, y: 2 };
    p.x = 5;
    p.y += 3;
    let q = Pt { y: 7, x: 9 };
    print(p.x);
    print(p.y);
    print(q.x * 10 + q.y);
}
