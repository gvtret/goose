// A static-capacity limited array (`T[..k]`) is a fixed-slot value whose
// count its operations move: a literal or a string literal sets it, push
// adds an element behind a capacity check that aborts, pop takes the last
// one off (an empty one aborts), pop_n drops several at once, append
// copies a source in behind the same capacity check, index reads and
// stores check the count, .len and .cap read the header, a for names each
// element, and print renders the run.

fn twice(n: i64) -> i64 { return n * 2; }

fn main() {
    var a: i64[..4] = [1, 2];
    var e: i64[..2] = [];
    print(a, " ", a.len, " ", a.cap, " ", e.len);
    a.push(twice(3));
    print(a);
    a.push(9);
    print(a, " ", a.len);
    let x = a.pop();
    print(x, " ", a);
    a.pop();
    print(a);
    a[1] = 7;
    print(a);
    var i = 1;
    print(a[i]);
    a[i] += 1;
    print(a);
    let s = a[0..2];
    print(s, " ", s.len);
    a.append(s);
    print(a, " ", a.len);
    var t: i64[..6] = [5, 6];
    t.append(a);
    print(t, " ", t.len);
    for v in t { print(v); }
    t.pop_n(2);
    print(t, " ", t.len);
    var h: u8[..8] = "hi";
    h.append("!");
    print(h, " ", h.len);
    e.push(1);
    e.push(2);
    e.push(3);
}
