// Integer operators beyond arithmetic: bitwise and/or/xor/not, shifts,
// and their compound assignment forms (the shift count masks by 63).
fn main() {
    print(1 + 2);
    print(6 & 3, " ", 6 | 3, " ", 6 ^ 3, " ", ~6);
    print(1 << 5, " ", -17 >> 2, " ", 7 >> 1);
    var x = 5;
    x &= 3;
    print(x);
    x |= 8;
    print(x);
    x ^= 1;
    print(x);
    x <<= 3;
    print(x);
    x >>= 1;
    print(x);
    print(1 << 64, " ", 1 << 65);
}
