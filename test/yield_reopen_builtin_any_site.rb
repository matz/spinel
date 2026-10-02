# A yield site whose class reopens a builtin name answers the reopen, and
# the other sites, which the per-site table does not cover, answer their own
# builtin's value rather than the reopen's slot or the reopen itself.

class Array
  def size = "arr"
  def first = "afirst"
  def [](i) = :aidx
end
class Integer
  def succ = "isucc"
end
class Hash
  def length = 100
end

def sz = yield.size
p sz { [1, 2, 3] }
p sz { "hello" }
p sz { {a: 1} }

def fi = yield.first
p fi { [1, 2] }
p fi { "ab".chars }
p fi { {a: 1} }

def ix = yield[1]
p ix { [10, 20] }
p ix { "xyz" }
p ix { {1 => :one} }

def sc = yield.succ
p sc { 1 }
p sc { "a" }

# a link past the reopened one is typed per site as well
def ln = yield.length + 1
p ln { {a: 1} }
p ln { [1, 2] }
p ln { "abc" }

def st = yield.size.to_s
p st { [1] }
p st { "hey" }
