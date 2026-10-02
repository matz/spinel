# A yield site whose block gives an Array or a Hash, where the program
# reopened that class with its own method of a builtin's name, answers the
# reopen's value beside sites the builtin answers.

class Hash
  def first = :hfirst
end
class Array
  alias orig_dup dup
  def dup = :adup
end

def f = yield.first
p f { {a: 1} }
p f { [4, 5] }
p f { ["q"] }

def d = yield.dup
p d { [1, 2] }
p d { "s" }
p d { 5 }

# an alias taken before the reopen still runs the builtin
def o = yield.orig_dup
p o { [7, 8] }
p o { ["r"] }
