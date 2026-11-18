// A flat struct may hold a growable array field: the field's { base, len }
// header lives in the struct body, push and .len reach it through a field
// path, print renders the run, and the field binds as a fat-reference
// argument (b.items).
struct Bag { tag: i64, items: i64[>..] }

fn fill(b: Bag&, v: i64) { b.items.push(v); }

fn main() {
    var b = Bag { tag: 2, items: [] };
    b.items.push(7);
    b.items.push(9);
    print(b.items, " ", b.items.len, " ", b.items[1]);
    fill(b, 11);
    print(" ", b.items, " ", b.tag);
}
