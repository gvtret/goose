// clear() drops a growable array's count to zero and resets its stack top
// to the element region's base, and a static-capacity limited array's
// count to zero (its slots stay reserved).
var g: i64[>..] = [1, 2, 3];

fn main() {
    var a: i64[>..] = [1, 2, 3, 4];
    a.push(5);
    a.clear();
    print(a.len, " ", a);
    a.push(7);
    a.push(8);
    print(a);
    g.clear();
    print(g.len, " ", g);
    g.append([9, 10]);
    print(g);
    var l: i64[..4] = [1, 2, 3];
    l.clear();
    print(l.len, " ", l);
    l.push(6);
    print(l);
}
