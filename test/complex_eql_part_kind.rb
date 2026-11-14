# A boxed Complex's eql?, its Hash-key eql? and its hash compare the kind of
# each part, as CRuby does: Complex(1, 0) is == to Complex(1.0, 0), not eql?.
require "set"
P = Struct.new(:a)
p P.new(Complex(1, 0)).eql?(P.new(Complex(1.0, 0)))
p P.new(Complex(1, 0)).eql?(P.new(Complex(1, 0)))
p [Complex(1,0),"s"].first.eql?(Complex(1.0,0))
p [Complex(1,0),"s"].first.eql?(Complex(1,0.0))
p [Complex(1,0),"s"].first.eql?(Complex(1,0))
p({Complex(1,0)=>1}.key?(Complex(1.0,0)))
p({Complex(1,0)=>1}.key?(Complex(1,0)))
p Complex(1,0).eql?(Complex(1.0,0))
p Complex(1,2).eql?(Complex(1,2.0))
p Complex(1,2).eql?(Complex(1,2))
p [Complex(1,0), Complex(1.0,0), Complex(1,0)].uniq
p [[Complex(1,0)]].eql?([[Complex(1.0,0)]])
h = {}; h[Complex(1,0)] = :a; h[Complex(1.0,0)] = :b; p h.size
p Complex(1,0).hash == Complex(1.0,0).hash
p [Complex(1,0), "x"].include?(Complex(1.0, 0))
a = [Complex(1, 0), Complex(1.0, 0), Complex(1, 0.0), Complex(1, 0)]
p a.uniq
p a.tally
p a.group_by { |z| z }.keys
p Set.new(a).size
p a.to_h { |z| [z, z.real.class] }
p({Complex(1, 0) => 1}.eql?({Complex(1.0, 0) => 1}))
p({Complex(1, 0) => 1} == {Complex(1.0, 0) => 1})
p [Complex(1, 0)].eql?([Complex(1, 0.0)])
p [Complex(1, 0)] == [Complex(1, 0.0)]
x = Complex(2, 3) + Complex(1, 0)
y = Complex(3.0, 3)
p [x, "s"].first.eql?(y)
p [x, "s"].first.eql?(Complex(3, 3))
p [x, y].uniq.size
p (x - Complex(0.5, 0)).eql?(Complex(2.5, 3))
p [Complex(-0.0, 1.0), Complex(0.0, 1.0)].uniq.size
p({Complex(2, 3) => :k}[Complex(2, 3)])
p({Complex(2, 3) => :k}[Complex(2.0, 3)])
D = Data.define(:z)
p D.new(Complex(1, 0)).eql?(D.new(Complex(1.0, 0)))
p D.new(Complex(1, 0)).eql?(D.new(Complex(1, 0)))
p D.new(Complex(1, 0)) == D.new(Complex(1.0, 0))
p [Complex(1,0), Complex(1.0, 0)].index(Complex(1.0, 0))
# a String's parts keep their kinds
p ["1.0+2i".to_c, 1].first.eql?(Complex(1.0, 2))
p({"1.0+2i".to_c => 1}.key?(Complex(1.0, 2)))
p ["1.0+2i".to_c, Complex(1.0, 2)].uniq.size
p Complex("1.5").eql?(Complex(1.5, 0))
p "1.0+2i".to_c, "1+2.5i".to_c, "3e2i".to_c, "2-1.0i".to_c, "7".to_c, Complex("1+2i")
p "1+2i".to_c.eql?(Complex(1, 2))
# the same object is eql? to itself, even with a NaN part
a = [Complex(Float::NAN, 0), 1]
p a[0].eql?(a[0])
# a Complex divided by an Integer: a part it does not divide is a Rational
p (Complex(5, 9) / 2).eql?(Complex(Rational(5, 2), Rational(9, 2)))
p Complex(4, 8) / 2, (Complex(4, 8) / 2).real.class
p Complex(1.5, 2) / 2
# Marshal keeps each part's kind
s = Marshal.dump(Complex(1, 2))
z = Marshal.load(s)
p z, z.real.class, z.eql?(Complex(1, 2)), z.hash == Complex(1, 2).hash
w = Marshal.load(Marshal.dump(Complex(1.5, 2)))
p w, w.eql?(Complex(1.5, 2)), w.imaginary.class
p Marshal.load(Marshal.dump([Complex(3, 0.5)]))
p Marshal.load(Marshal.dump(Complex(-3, 4.0)))
p Marshal.load(Marshal.dump([Complex(1, 0), Complex(1.0, 0)])).uniq.size
h = Marshal.load(Marshal.dump({Complex(1, 0) => :i, Complex(1.0, 0) => :f}))
p h, h[Complex(1, 0)], h[Complex(1.0, 0)]
p Marshal.dump(Complex(1, 2)).bytesize, Marshal.dump(Complex(1, 2.5)).bytesize
