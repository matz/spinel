# A block-local named like a stored enumerator's variable, in another method,
# does not stop the enumerator's `each` block from being lowered: the name is
# no parameter of the scope that holds the enumerator.
def other
  [1].each { |;e| p e }
end
other
e = [1, 2, 3].map
r = e.each { |x| x * 2 }
p r
