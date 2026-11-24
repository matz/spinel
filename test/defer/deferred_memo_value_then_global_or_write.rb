# Compiled with --defer-refusals: a memo `@m ||= v` whose value the compiler
# refuses is deferred as a statement, and the frozen-receiver guard the
# ivar's `||=` had set around its value was left in place by the rollback.
# Every global `||=` emitted after it took that guard, naming a `self` the
# top level has none of, and the C did not compile. The globals below are
# written as before; the memo raises where it runs.
# spinel: defer-refusals: 1:start [1, 2]
# spinel: defer-message: eval of a runtime string
class Memo
  def initialize; @m = nil; end
  def m = (@m ||= eval("1 + 1"))
end
puts "start"
($g ||= []) << 1
($g ||= []) << 2
p $g
x = Memo.new
x.freeze
x.m
