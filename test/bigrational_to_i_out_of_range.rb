# spinel: not-cruby -- raise and wrap give a RangeError where CRuby answers the Bignum (promote_bigrational_to_i.rb is CRuby's)
# raise/wrap only: a Bignum-numerator Rational whose integer value is wider
# than the machine word. The sp_int slot cannot carry it, so it is the same
# loud RangeError a boxed Bignum's #to_i gives, pending #2024 -- where it
# used to saturate silently, and answer nil for a negative value because
# INTPTR_MIN is the nil sentinel. promote answers the Bignum; see
# promote_bigrational_to_i.rb.
br = Rational(2**70, 3)
nb = Rational(-(2**70), 3)
ex = Rational(2**70, 1)
[br, nb, ex].each do |r|
  begin
    r.to_i
    puts "no raise"
  rescue RangeError => e
    puts "to_i: #{e.message}"
  end
end
begin; br.to_int; rescue RangeError => e; puts "to_int: #{e.message}"; end
puts "numerator: #{br.numerator}"
begin; Integer(br); rescue RangeError => e; puts "Integer: #{e.message}"; end
pb = [Rational(2**70, 3), nil][0]
begin; pb.to_i; rescue RangeError => e; puts "boxed to_i: #{e.message}"; end
