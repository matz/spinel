# Two loops in one body set the same constant to classes of different
# parents: CRuby replaces the first with a warning, so OK's superclass is
# B. Class definitions in their place would reopen the first class instead.
# The first loop runs where it stands; the second's const_set names a
# constant that already holds a class there, which a class definition in
# its place would reopen, so it stays as written, which Spinel refuses.
module HTTPStatus
  class A; end
  class B; end
  T1 = { 1 => "OK" }
  T2 = { 2 => "OK" }
  T1.each { |c, n| const_set(n, Class.new(A)) }
  T2.each { |c, n| const_set(n, Class.new(B)) }
end
p HTTPStatus::OK.superclass
