struct P { x: i32, y: i32 }
fn first(ps: P[:]) -> i64 { return ps[0].x + ps.len; }
fn main() {
    var ps: P[>..] = [P { 1, 1 }, P { 2, 2 }];
    print(first(ps[..]));
}
