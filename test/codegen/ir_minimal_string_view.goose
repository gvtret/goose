// A string literal is a u8[:] view of read-only bytes: it initializes a
// slice, slices like any other view, prints as raw bytes, and iterates.
fn main() {
    let text = "hello, world";
    print(text);
    print(text.len);
    print(text[..5]);
    print(text[7..]);
    var sum = 0;
    for c in text { sum += c; }
    print(sum);
    let s: const u8[:] = "abc";
    print(s[1..]);
    print(s.len);
}
