# A method that ends in an operator write to a local a proc captures answers
# the value the write gave, as in CRuby: the local is a heap cell, and the
# statement form the write took left the method answering nothing.
def plus
  v = 5
  pr = proc { v }
  v += 1
end
p plus

def times_param(n)
  pr = -> { n }
  n *= 3
end
p times_param(4)

def str_append
  s = +"ab"
  pr = proc { s }
  s += "c"
end
p str_append

def float_minus
  f = 2.5
  pr = proc { f }
  f -= 1
end
p float_minus

def plain
  v = 9
  v += 1
end
p plain

def written_in_proc
  v = 1
  pr = proc { v += 10 }
  pr.call
  v += 1
end
p written_in_proc

def nilable_cell(c)
  v = c ? 5 : nil
  pr = proc { v }
  v = v.to_i
  v += 1
end
p nilable_cell(true)

def nilled_in_proc(c)
  v = 5
  pr = proc { v = nil }
  pr.call if c
  v = 1
  v += 1
end
p nilled_in_proc(true)

# a plain Integer or Float cell in a method that answers nil on another path
def plain_cell_nil_path(c)
  v = 1
  pr = proc { v }
  return nil unless c
  v += 1
end
p plain_cell_nil_path(true), plain_cell_nil_path(false)
def float_cell_nil_path(c)
  f = 1.5
  pr = proc { f }
  return nil unless c
  f += 1
end
p float_cell_nil_path(true), float_cell_nil_path(false)
