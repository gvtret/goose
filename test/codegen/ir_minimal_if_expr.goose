fn sign(x: i64) -> i64 {
    if x > 0 { 1 } else { -1 }
}
fn main() {
    print(sign(3));
    print(sign(-3));
}
