// A continue in a range loop steps the counter; a u8[] local and a
// u8[>..] start from a string literal; a growable array prints its live
// bytes; a global increment reaches the global; assert passes.
var touched = 0;
fn bump() -> i64 { touched++; return 3; }
fn main() {
    var s = 0;
    for i in 1..6 {
        if i % 2 == 0 { continue; }
        s += i;
    }
    print(s);
    let word: u8[] = "hey";
    print(word, " ", word.len);
    var pool: u8[>..] = "ab";
    pool.push(99);
    pool.push(100);
    print(pool);
    print(touched);
    bump();
    bump();
    print(touched);
    assert(touched == 2);
    assert(pool.len == 4);
    print("done");
}
