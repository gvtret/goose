enum Choice { One, Two }
fn build(c: Choice.One) -> i64 { return 11; }
fn build(c: Choice.Two) -> i64 { return 21; }
fn main() {
    var c: Choice = Choice.One {};
    print(build(c));
    c = Choice.Two {};
    print(build(c));
}
