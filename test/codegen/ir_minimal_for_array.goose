// Iteration over a growable array names its elements in order, the
// optional second binder (the index) counts from zero, and the count is
// re-read per pass, so pushes made during the walk are seen by later
// passes (the walk extends over a push).
fn main() {
    var xs: i64[>..] = [40, 50];
    var sum = 0;
    for x in xs {
        sum += x;
        if x == 50 { xs.push(60); }
    }
    print(sum);
    for x, i in xs {
        print(x);
        print(i);
    }
}
