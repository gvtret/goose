var counter: i64 = 5;

recursive fn bump(n: i64) -> i64 {
    counter += n;
    return counter;
}

fn main() {
    bump(2);
    bump(3);
    print(counter);
}
