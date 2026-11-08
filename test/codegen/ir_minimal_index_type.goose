// A cast into an index or bound type checks against the array's live
// count in every build: `in` must be below it, `upto` at most equal to
// it, and a value past it is the indexing error (abort, with whatever
// printed before the fail kept).
var xs: i64[>..] = [];
var ys: i64[>..] = [];
type Id = u32 in xs;
type Cap = u32 upto ys;
fn main() {
    xs.push(9);
    ys.push(7);
    print(xs.len);
    print(0 as Id);
    print(1 as Cap);
    print(9 as Id);
    print("unreachable");
}
