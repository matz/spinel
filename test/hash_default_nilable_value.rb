# Hash#default= takes an Integer or a Float that may be nil: nil sets a nil
# default, a number that number
def int_default(d = nil)
  h = {}
  h.default = d
  h
end

def float_default(d = nil)
  h = {"a" => 1.0}
  h.default = d
  h
end

def counts(d = nil)
  h = {"a" => 1}
  h.default = d
  h
end

p int_default[:x]
p int_default(3)[:x]
p float_default["b"]
p float_default(2.5)["b"]
p counts["b"]
p counts(7)["b"]
p counts(7)["a"]
def set_and_return(h, d = nil) = (h.default = d)
p set_and_return({})
p set_and_return({"a" => 1}, 5)
