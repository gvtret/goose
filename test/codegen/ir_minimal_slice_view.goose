// A local slice is a view of a run of an int array: a checked range, its
// length, element reads and writes, re-slicing, iteration, and printing as
// [a, b, c] (raw bytes for u8 elements). A range past the end aborts.
fn main() {
    var a: i64[6] = [10, 20, 30, 40, 50, 60];
    var s: i64[:] = a[1..4];
    print(s.len, " ", s[0], " ", s[2]);
    s[1] = 25;
    print(a[2], " ", s[1]);
    var t: i64[:] = s[1..3];
    print(t.len, " ", t[0], " ", t[1]);
    var sum = 0;
    for x in t { sum += x; }
    print(sum);
    print(t);
    print(a);
    var u: u8[3] = [65, 66, 67];
    var v: u8[:] = u[..];
    print(v);
    print(v.len);
    print(a[2..]);
    // A slice local re-pointed in the body: the next pass takes the element
    // at its index in the view it points at now (section 5.2).
    var w: i64[:] = a[0..2];
    for x in w {
        print(x);
        w = a[4..6];
    }
    print(a[0..7]);
    print("not reached");
}
