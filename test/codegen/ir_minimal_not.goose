recursive fn check(x: i64) -> i64 {
    if !(x == 4) { return 6; }
    return 0;
}
fn main() {
    print(check(3));
}
