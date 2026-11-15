// A fixed int-array global holds its elements in thread-local storage:
// its literal initializer lands there when the program starts, and print,
// index, .len and copy see the run.
var h2:    i64[2] = [4, 5];
var bytes: u8[3]  = "abc";
var d64:   i64[]  = [1, 2, 3];

fn main() {
    print(h2, " ", h2.len, " ", h2[1]);
    print(bytes);
    print(d64, " ", d64.len);
    print(copy(h2), " ", copy(d64));
    h2[0] = 40;
    print(" ", h2[0], " ", h2);
}
