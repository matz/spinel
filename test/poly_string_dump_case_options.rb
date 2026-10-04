# A String or Symbol read from a boxed slot answers dump and undump, and the
# case mappings given options, as a typed one does: it raised NoMethodError
# naming String
x = ["ab", 1][0]
p x.upcase(:ascii), x.downcase(:ascii), x.capitalize(:ascii), x.swapcase(:ascii)
p x.upcase(:turkic), x.downcase(:fold), x.upcase(:turkic, :lithuanian)
begin; x.upcase(:bogus); rescue => e; p e.class, e.message; end
begin; x.upcase(:ascii, :turkic); rescue => e; p e.class, e.message; end
y = [:ab, 1][0]
p y.upcase(:ascii), y.swapcase(:ascii)
z = [5, "s"][0]
begin; z.upcase(:ascii); rescue NoMethodError => e; p e.message; end
begin; z.upcase(:bogus); rescue NoMethodError => e; p e.message; end
w = [+"hé\n\"", 1][0]
p w.dump, w.dump.undump == w
begin; ["x", 1][0].undump; rescue => e; p e.class; end
