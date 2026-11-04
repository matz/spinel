# find_index(v) on a yield receiver whose call sites differ in kind: an Array
# site beside a Hash site. The Hash site's Enumerable find_index answers a
# boxed Integer-or-nil, and the Array site's typed index used to unwrap that
# answer to a plain Integer before the yield site boxed it again, so the
# generated C passed an sp_int where an sp_oint was expected and did not
# compile. Each site must answer what CRuby answers: the index or nil.
def w
  v = yield.find_index(2)
  p v
end

def u
  v = yield.find_index(["a", 1])
  p v
end

w { [3, 1, 2] }
w { ["b", "a"] }
w { {a: 1} }
w { {b: 2} }
u { [["x", 0], ["a", 1]] }
u { {a: 1} }
u { ["b", "a"] }
