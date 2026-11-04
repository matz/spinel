# A Struct's or Data's `new` defined under a condition: the call runs the
# def where it was made and the builtin constructor where it was not.
S1 = Struct.new(:a)
def S1.new(a) = :mine if ARGV.size < 5
p S1.new(1)

S2 = Struct.new(:a)
def S2.new(a) = :mine if ARGV.size > 5
p S2.new(1)

D1 = Data.define(:x)
def D1.new(x) = :mine if ARGV.size < 5
p D1.new(1)
D2 = Data.define(:y)
def D2.new(y) = :mine if ARGV.size > 5
p D2.new(2)

S3 = Struct.new(:a)
class T3 < S3; end
def S3.new(a) = :mine if ARGV.size > 5
p S3.new(1), T3.new(2)
S6 = Struct.new(:a)
class T6 < S6; end
def S6.new(a) = :mine if ARGV.size < 5
p S6.new(1), T6.new(2)

S4 = Struct.new(:a)
def S4.new(a) = :mine if ARGV.size < 5
def mk4 = S4.new(1)
p mk4
k = S4
p k.new(2)

S5 = Struct.new(:a)
def S5.new(a) = a * 2 if ARGV.size < 5
x = S5.new(21)
p x + 1
p [S5.new(1), S5.new(2)]
