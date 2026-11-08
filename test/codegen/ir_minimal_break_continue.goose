recursive fn sum_range(lo: i64, hi: i64, skip: i64) -> i64 {
    var s: i64 = 0;
    var i: i64 = 0;
    while i < hi {
        i += 1;
        if i == skip { continue; }
        if i > hi - 2 { break; }
        s += i + lo;
    }
    return s;
}
fn main() {
    print(sum_range(1, 10, 4));
}
