struct P { x: i64, y: i64 }
fn main() {
    var ps: P[>..] = [];
    ps.push(P { 1, 2 });
    ps.push(P { 3, 4 });
    print(ps, " ", ps.len, " ", ps[1].y);
    ps[0].x = 10;
    print(" ", ps[0]);
}
