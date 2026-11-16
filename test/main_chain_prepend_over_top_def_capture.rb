# An Object prepend of a module with a method of the name answers ahead of a top-level def of it from the statement that
# runs it, and a capture or a `send` there finds the prepended method: the top-level ask (CT_TOP) keeps meaning "the
# top-level functions", and the calls that ask main's whole chain switch on what it answers.
module P
  def shout = :p
end
def shout = :top
p shout
m0 = method(:shout)
Object.prepend P
p shout
m = method(:shout)
p m.call
p m.owner
p m0.call
p 1 + 1
