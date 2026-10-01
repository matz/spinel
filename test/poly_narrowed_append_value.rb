# `x << y` on a boxed local an `is_a?(String)` guard narrows, where its
# value is used (a block's or a method's last line, an operand) or in a
# chain: the append is made through the box, as in CRuby, so a String the
# box shares is the caller's own. The String form wrote the new value back
# into the unboxed read, which is no lvalue, and the C build stopped.

X = "x" * 4

def f1(a) = (a << X if a.is_a?(String))
s = +"a"; p f1(s), s; p f1(1)
def f2(a) = (a.is_a?(String) ? (a << X) : a)
s = +"a"; p f2(s), s, f2(2)

def y1(v) = (yield(1); yield(v))
s = +"a"; y1(s) { |a| a << X if a.is_a?(String) }; p s
s = +"a"; r = []; y1(s) { |a| r << (a << X if a.is_a?(String)) }; p s, r
s = +"a"; y1(s) { |a| if a.is_a?(String) then a << X end }; p s

# a method that calls its named block and yields to it as well
def y2(v, &b) = (b.call(1); yield(v); b.call(v))
s = +"a"; y2(s) { |a| a << X if a.is_a?(String) }; p s
def y3(v, &b) = (b.call(1); yield(v))
s = +"a"; y3(s) { |a| a << X if a.is_a?(String); nil }; p s

# a chain, in value and in statement position, and a local
def y4(v) = (yield(v); nil)
s = +"a"; y4(s) { |a| (a << X << "!") if a.is_a?(String) }; p s
s = +"a"; y4(s) { |a| a << X << "!" if a.is_a?(String); 0 }; p s
y4(2) { |a| a }
x = [1, +"q"][1]
t0 = (x << X if x.is_a?(String))
p t0, x

# every String mutator a narrowed box takes, in value and in statement
# position: the String arm's write-back stopped the C build for each
def m1(a) = (a.concat(X) if a.is_a?(String))
def m2(a) = (a.prepend("<") if a.is_a?(String))
def m3(a) = (a.replace("zz") if a.is_a?(String))
def m4(a) = (a.clear if a.is_a?(String))
def m5(a) = (a.gsub!("b", "B") if a.is_a?(String))
def m6(a) = (a.sub!("b", "B") if a.is_a?(String))
def m7(a) = (a.upcase! if a.is_a?(String))
def m8(a) = (a.downcase! if a.is_a?(String))
def m9(a) = (a.capitalize! if a.is_a?(String))
def m10(a) = (a.swapcase! if a.is_a?(String))
def m11(a) = (a.strip! if a.is_a?(String))
def m12(a) = (a.lstrip! if a.is_a?(String))
def m13(a) = (a.rstrip! if a.is_a?(String))
def m14(a) = (a.chomp! if a.is_a?(String))
def m15(a) = (a.chop! if a.is_a?(String))
def m16(a) = (a.squeeze! if a.is_a?(String))
def m17(a) = (a.tr!("b", "c") if a.is_a?(String))
def m18(a) = (a.delete!("b") if a.is_a?(String))
def m19(a) = (a.tr_s!("b", "c") if a.is_a?(String))
def m20(a) = (a.delete_prefix!("a") if a.is_a?(String))
def m21(a) = (a.delete_suffix!("b") if a.is_a?(String))
def m22(a) = (a.reverse! if a.is_a?(String))
def m23(a) = (a.succ! if a.is_a?(String))
def m24(a) = (a.next! if a.is_a?(String))
def m25(a) = (a[0] = "Z" if a.is_a?(String))
def m26(a) = (a.insert(0, "<") if a.is_a?(String))
def m27(a) = (a.slice!(0) if a.is_a?(String))
def m28(a) = (a.setbyte(0, 65) if a.is_a?(String))
s = +" abb\n"; t = +"Ab"; p [m1(s), s, m1(1), m1(t), t]
s = +" abb\n"; t = +"Ab"; p [m2(s), s, m2(1), m2(t), t]
s = +" abb\n"; t = +"Ab"; p [m3(s), s, m3(1), m3(t), t]
s = +" abb\n"; t = +"Ab"; p [m4(s), s, m4(1), m4(t), t]
s = +" abb\n"; t = +"Ab"; p [m5(s), s, m5(1), m5(t), t]
s = +" abb\n"; t = +"Ab"; p [m6(s), s, m6(1), m6(t), t]
s = +" abb\n"; t = +"Ab"; p [m7(s), s, m7(1), m7(t), t]
s = +" abb\n"; t = +"Ab"; p [m8(s), s, m8(1), m8(t), t]
s = +" abb\n"; t = +"Ab"; p [m9(s), s, m9(1), m9(t), t]
s = +" abb\n"; t = +"Ab"; p [m10(s), s, m10(1), m10(t), t]
s = +" abb\n"; t = +"Ab"; p [m11(s), s, m11(1), m11(t), t]
s = +" abb\n"; t = +"Ab"; p [m12(s), s, m12(1), m12(t), t]
s = +" abb\n"; t = +"Ab"; p [m13(s), s, m13(1), m13(t), t]
s = +" abb\n"; t = +"Ab"; p [m14(s), s, m14(1), m14(t), t]
s = +" abb\n"; t = +"Ab"; p [m15(s), s, m15(1), m15(t), t]
s = +" abb\n"; t = +"Ab"; p [m16(s), s, m16(1), m16(t), t]
s = +" abb\n"; t = +"Ab"; p [m17(s), s, m17(1), m17(t), t]
s = +" abb\n"; t = +"Ab"; p [m18(s), s, m18(1), m18(t), t]
s = +" abb\n"; t = +"Ab"; p [m19(s), s, m19(1), m19(t), t]
s = +" abb\n"; t = +"Ab"; p [m20(s), s, m20(1), m20(t), t]
s = +" abb\n"; t = +"Ab"; p [m21(s), s, m21(1), m21(t), t]
s = +" abb\n"; t = +"Ab"; p [m22(s), s, m22(1), m22(t), t]
s = +" abb\n"; t = +"Ab"; p [m23(s), s, m23(1), m23(t), t]
s = +" abb\n"; t = +"Ab"; p [m24(s), s, m24(1), m24(t), t]
s = +" abb\n"; t = +"Ab"; p [m25(s), s, m25(1), m25(t), t]
s = +" abb\n"; t = +"Ab"; p [m26(s), s, m26(1), m26(t), t]
s = +" abb\n"; t = +"Ab"; p [m27(s), s, m27(1), m27(t), t]
s = +" abb\n"; t = +"Ab"; p [m28(s), s, m28(1), m28(t), t]
def st1(a) = (a[0] = "Z" if a.is_a?(String); 0)
def st2(a) = (a.insert(0, "<") if a.is_a?(String); 0)
def st3(a) = (a.slice!(0) if a.is_a?(String); 0)
def st4(a) = (a.setbyte(0, 65) if a.is_a?(String); 0)
s = +" abb"; p [st1(s), s, st1(1)]
s = +" abb"; p [st2(s), s, st2(1)]
s = +" abb"; p [st3(s), s, st3(1)]
s = +" abb"; p [st4(s), s, st4(1)]
