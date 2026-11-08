// Growing arrays initialize from element literals ([e1, e2, ...]) and
// the fill form ([v; n], the value evaluated once), then keep pushing
// past the initialized counts.
fn main() {
    var a: i64[>..] = [3, 12, 27];
    a.push(a[1] + a[2]);
    print(a.len);
    print(a[0]);
    print(a[1]);
    print(a[3]);
    var b: i64[>..] = [5; 3];
    print(b.len);
    print(b[2]);
    var c: i64[>..] = [];
    print(c.len);
}
