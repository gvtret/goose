// A growable i64 array on a real data stack (M3 first cut): push, index
// read and len against a region reserved through gs_reserve_region.
fn main() {
    var a: i64[>..] = [];
    a.push(3);
    a.push(12);
    a.push(27);
    print(a.len);
    print(a[0]);
    print(a[1]);
    print(a[2]);
    a.push(a[1] + a[2]);
    print(a[3]);
}
