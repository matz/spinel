# A require_relative is a call in the file's own tree (#9898): text a
# String, `%q()` or an `=begin` comment holds is not one, and where a call
# stands is read from the nodes around it, not from the indentation of the
# text spliced in ahead of it. The line scanner took each of these for a
# require (or misplaced one).
s = "first line
require_relative 'require_relative_tree/never'
last line"
puts s
puts %q(require_relative "require_relative_tree/never")
=begin
require_relative "require_relative_tree/b_value"
=end
puts "start"
require_relative "require_relative_tree/b_value"
puts B_VALUE
if true
  require_relative "require_relative_tree/late"
  require_relative "require_relative_tree/after_late"
end
p AFTER_LATE
# Two on one line: the second file loads after the first, which it reads.
require_relative "require_relative_tree/s_a"; require_relative "require_relative_tree/s_b"
p SB
# A value-position require after one inlined in the same `if` body goes
# after that file, not ahead of the `if`.
if true
  require_relative "require_relative_tree/i_c"
  y = require_relative "require_relative_tree/i_d"
  p y, ID.new.hi, IDV
end
# The same in the other arm of an if/else.
if ARGV.size > 5
  require_relative "require_relative_tree/e_c"
else
  z = require_relative "require_relative_tree/e_d"
  p z, ED.new.hi
end
# A require in a modifier's condition runs first.
require_relative "require_relative_tree/m_a" if require_relative("require_relative_tree/m_b")
p [MA, MB]
# A do-while body runs before its condition.
begin
  require_relative "require_relative_tree/w_a"
end while require_relative("require_relative_tree/w_b") && false
p [WA, WB]
# A method called on the literal applies to the call's value; the literal
# is the path.
require_relative "require_relative_tree/f_z".freeze
p FZ
