// A growable array of a narrow element (u8) pushes, indexes and reports
// its length as an element count.
fn main() {
    var b: u8[>..] = [];
    b.push(65);
    b.push(66);
    print(b.len);
    print(b[0]);
    print(b[1]);
}
