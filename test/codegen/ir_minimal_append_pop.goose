// append copies a fixed array's, a slice's or a string literal's elements
// onto a growable array's top and adds their count to the length; pop takes
// the last element off in value and statement position, and an empty pop
// aborts.
var g: i64[>..] = [];

fn main() {
    var a: i64[>..] = [1];
    let f: i64[3] = [2, 3, 4];
    a.append(f);
    print(a);
    let s = a[1..3];
    a.append(s);
    print(a);
    var b: u8[>..] = "ab";
    b.append("cd");
    print(b);
    let x = a.pop();
    print(x, " ", a);
    a.pop();
    print(a);
    g.append(f);
    print(g);
    print(g.pop(), " ", g.len);
    let q: i64[>..] = [];
    q.pop();
}
